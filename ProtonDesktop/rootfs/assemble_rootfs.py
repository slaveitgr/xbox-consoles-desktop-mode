#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Assemble two real Wine64 base layers from the frozen Debian package lock.

Offline by default. --download permits only exact hash/size-locked .deb inputs.
No Wine, package manager, maintainer script or guest executable is run.
"""
from __future__ import annotations
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath
import argparse
import datetime
import email.utils
import hashlib
import io
import json
import os
import re
import shutil
import stat
import struct
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile
import zlib

HERE = Path(__file__).resolve().parent
MAX_FILE = 1024 ** 3
MAX_TOTAL = 2 * 1024 ** 3
DISK_RESERVE = 512 * 1024 ** 2
MAX_ENTRIES = 200000

class AssemblyError(RuntimeError): pass

def require(condition, message):
    if not condition: raise AssemblyError(message)

def file_digest(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        while chunk := stream.read(1024 * 1024): digest.update(chunk)
    return digest.hexdigest()

def guest_path(name, *, absolute=False):
    require(isinstance(name, str) and '\0' not in name and '\\' not in name, 'Invalid archive path')
    require(absolute or not name.startswith('/'), 'Absolute archive member path: ' + name)
    parts = []
    for part in name.split('/'):
        if part in ('', '.'): continue
        require(part != '..', 'Archive path traversal: ' + name)
        require('(_colon_)' not in part and '(_at_)' not in part, 'Unrepresentable escaped guest name: ' + name)
        parts.append(part)
    return '/' + '/'.join(parts)

def link_target(path, target):
    require(target and '\0' not in target and '\\' not in target, 'Invalid virtual link target')
    stack = [] if target.startswith('/') else path.strip('/').split('/')[:-1]
    for part in target.split('/'):
        if part in ('', '.'): continue
        if part == '..':
            require(bool(stack), 'Virtual link escapes guest root: ' + path)
            stack.pop()
        else: stack.append(part)
    return '/' + '/'.join(stack)

@dataclass
class Node:
    kind: str
    mode: int
    size: int = 0
    digest: str | None = None
    blob: Path | None = None
    target: str | None = None
    origins: list[dict] = field(default_factory=list)

class Filesystem:
    def __init__(self, blobs):
        self.blobs = Path(blobs); self.blobs.mkdir(parents=True, exist_ok=True)
        self.nodes = {}
        self.total = 0
    def add(self, path, node):
        require(len(self.nodes) < MAX_ENTRIES or path in self.nodes, 'Too many rootfs entries')
        previous = self.nodes.get(path)
        if previous:
            same = (previous.kind, previous.mode, previous.size, previous.digest, previous.target) == \
                   (node.kind, node.mode, node.size, node.digest, node.target)
            require(same, 'Conflicting package payload at ' + path + ': ' +
                    repr(previous.origins) + ' versus ' + repr(node.origins))
            previous.origins.extend(origin for origin in node.origins if origin not in previous.origins)
            return
        self.nodes[path] = node
    def blob(self, stream, size):
        require(0 <= size <= MAX_FILE, 'Archive member is too large')
        require(self.total + size <= MAX_TOTAL, 'Rootfs uncompressed byte limit exceeded')
        self.total += size
        digest = hashlib.sha256()
        fd, temporary = tempfile.mkstemp(prefix='member-', dir=self.blobs)
        try:
            with os.fdopen(fd, 'wb') as output:
                remaining = size
                while remaining:
                    chunk = stream.read(min(1024 * 1024, remaining))
                    require(bool(chunk), 'Truncated tar member data')
                    output.write(chunk); digest.update(chunk); remaining -= len(chunk)
            checksum = digest.hexdigest(); target = self.blobs / checksum
            if target.exists(): Path(temporary).unlink()
            else: os.replace(temporary, target)
            return checksum, target
        finally:
            if Path(temporary).exists(): Path(temporary).unlink()
    def generated(self, path, data, mode=0o644, reason='explicit app configuration'):
        checksum, blob = self.blob(io.BytesIO(data), len(data))
        self.add(path, Node('file', mode, len(data), checksum, blob, origins=[{'generated':reason}]))
    def resolve(self, path):
        require(path.startswith('/'),'Virtual lookup requires an absolute guest path')
        path = link_target('/',path)
        for _ in range(64):
            parts = path.strip('/').split('/')
            changed = False
            for length in range(1, len(parts) + 1):
                prefix = '/' + '/'.join(parts[:length]); node = self.nodes.get(prefix)
                if node and node.kind == 'symlink':
                    path = link_target(prefix, node.target)
                    if length < len(parts): path = path.rstrip('/') + '/' + '/'.join(parts[length:])
                    changed = True; break
            if not changed: return path, self.nodes.get(path)
        raise AssemblyError('Virtual link cycle or excessive depth: ' + path)
    def finish_links(self):
        for path, node in self.nodes.items():
            if node.kind != 'hardlink': continue
            _, target = self.resolve(node.target)
            seen = {path}
            while target and target.kind == 'hardlink':
                require(target.target not in seen, 'Archive hardlink cycle')
                seen.add(target.target); _, target = self.resolve(target.target)
            require(target and target.kind == 'file', 'Unresolved/non-file archive hardlink: ' + path)
            node.kind = 'file'; node.size = target.size; node.digest = target.digest; node.blob = target.blob
            node.mode = target.mode
            for origin in node.origins: origin['archive_hardlink_target'] = node.target
            node.target = None
        for path in self.nodes:
            parts = path.strip('/').split('/')
            for length in range(1, len(parts)):
                parent = self.nodes.get('/' + '/'.join(parts[:length]))
                require(parent is None or parent.kind == 'directory', 'Payload descends through non-directory: ' + path)

class ArMemberReader:
    def __init__(self, stream, start, size):
        self.stream = stream; self.remaining = size; stream.seek(start)
    def read(self, size=-1):
        size = self.remaining if size < 0 else min(size, self.remaining)
        data = self.stream.read(size); self.remaining -= len(data)
        return data

def ar_members(stream):
    stream.seek(0); require(stream.read(8) == b'!<arch>\n', 'Not a Debian ar archive')
    total = os.fstat(stream.fileno()).st_size
    offset = 8; members = {}
    while offset < total:
        stream.seek(offset); header = stream.read(60)
        require(len(header) == 60 and header[58:60] == b'`\n', 'Malformed ar member header')
        name = header[:16].decode('ascii').rstrip(' /')
        require(name in ('debian-binary','control.tar','control.tar.gz','control.tar.xz','control.tar.zst',
                         'data.tar','data.tar.gz','data.tar.xz','data.tar.zst'), 'Unsupported ar member: ' + name)
        require(name not in members, 'Duplicate ar member: ' + name)
        try: size = int(header[48:58].decode('ascii').strip())
        except ValueError as error: raise AssemblyError('Invalid ar member length') from error
        start = offset + 60
        require(0 <= size <= total - start, 'Truncated ar member')
        members[name] = (start, size); offset = start + size + (size & 1)
    require(offset == total and 'debian-binary' in members, 'Malformed Debian archive layout')
    start, size = members['debian-binary']; stream.seek(start)
    require(size == 4 and stream.read(size) == b'2.0\n', 'Unsupported Debian archive version')
    data = [name for name in members if name.startswith('data.tar')]
    require(len(data) == 1, 'Expected exactly one Debian data archive')
    return data[0], members[data[0]]

def ingest_deb(path, package, filesystem):
    path = Path(path)
    require(path.stat().st_size == package['bytes'], 'Package size mismatch: ' + package['package'])
    require(file_digest(path) == package['sha256'], 'Package SHA256 mismatch: ' + package['package'])
    origin = {key:package[key] for key in ('package','version','source','source_version','sha256')}
    with path.open('rb') as stream:
        name, (start, length) = ar_members(stream)
        try:
            with tarfile.open(fileobj=ArMemberReader(stream, start, length), mode='r|*') as archive:
                count = 0
                for member in archive:
                    count += 1; require(count <= MAX_ENTRIES, 'Too many archive entries')
                    target = guest_path(member.name)
                    if target == '/':
                        require(member.isdir(), 'Non-directory archive root'); continue
                    mode = member.mode & 0o7777
                    if member.isdir(): node = Node('directory', mode)
                    elif member.isfile():
                        data = archive.extractfile(member)
                        require(data is not None, 'Missing regular tar member data')
                        checksum, blob = filesystem.blob(data, member.size)
                        node = Node('file', mode, member.size, checksum, blob)
                    elif member.issym():
                        link_target(target, member.linkname)
                        node = Node('symlink', mode, target=member.linkname)
                    elif member.islnk(): node = Node('hardlink', mode, target=guest_path(member.linkname, absolute=True))
                    else: raise AssemblyError('Unsupported special tar member: ' + target)
                    node.origins.append(dict(origin)); filesystem.add(target, node)
        except (tarfile.TarError, OSError, EOFError) as error:
            raise AssemblyError('Cannot parse ' + name + ' for ' + package['package'] + ': ' + str(error)) from error

def acquire(package, cache, download=False):
    require(re.fullmatch(r'[0-9a-f]{64}', package['sha256']) is not None, 'Invalid locked package digest')
    require(0 < package['bytes'] <= MAX_FILE, 'Invalid locked package byte count')
    require(package['url'] == 'https://deb.debian.org/debian/' + package['filename'] and
            package['filename'].startswith('pool/') and '..' not in PurePosixPath(package['filename']).parts,
            'Package URL is outside the frozen Debian pool')
    path = Path(cache) / (package['sha256'] + '.deb'); path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        require(path.stat().st_size == package['bytes'] and file_digest(path) == package['sha256'],
                'Cached package hash/size mismatch: ' + package['package'])
        return path
    require(download, 'Package missing from offline cache: ' + package['package'] + ' (use --download explicitly)')
    fd, temporary = tempfile.mkstemp(prefix='download-', dir=path.parent)
    try:
        require(shutil.disk_usage(path.parent).free >= package['bytes'] + DISK_RESERVE, 'Insufficient package-cache disk space with 512 MiB reserve')
        checksum = hashlib.sha256(); total = 0
        with os.fdopen(fd, 'wb') as output, urllib.request.urlopen(package['url'], timeout=90) as response:
            while chunk := response.read(1024 * 1024):
                total += len(chunk); require(total <= package['bytes'], 'Download exceeds locked package size')
                checksum.update(chunk); output.write(chunk)
        require(total == package['bytes'] and checksum.hexdigest() == package['sha256'],
                'Downloaded package hash/size mismatch: ' + package['package'])
        os.replace(temporary, path)
    finally:
        if Path(temporary).exists(): Path(temporary).unlink()
    return path

def add_configuration(fs):
    for path, mode in [('/tmp',0o1777),('/run',0o755),('/run/user',0o755),('/run/user/1000',0o700),
                       ('/home',0o755),('/home/username',0o755),('/var',0o755),('/var/tmp',0o1777),('/etc',0o755)]:
        if path not in fs.nodes: fs.add(path, Node('directory', mode, origins=[{'generated':'writable guest parent'}]))
    fs.generated('/etc/passwd', b'root:x:0:0:root:/root:/bin/false\nusername:x:1000:1000:ProtonDesktop:/home/username:/bin/false\n')
    fs.generated('/etc/group', b'root:x:0:\nusername:x:1000:\n')
    fs.generated('/etc/nsswitch.conf', b'passwd: files\ngroup: files\nhosts: files\n')
    fs.generated('/etc/hosts', b'127.0.0.1 localhost ProtonDesktop\n::1 localhost\n')
    fs.generated('/etc/hostname', b'ProtonDesktop\n')
    fs.generated('/etc/ld.so.cache', b'', reason='empty cache; explicit default ELF library paths only')
    _, utc = fs.resolve('/usr/share/zoneinfo/Etc/UTC')
    require(utc is not None and utc.kind == 'file', 'Pinned tzdata UTC file missing')
    fs.add('/etc/localtime', Node('file', utc.mode, utc.size, utc.digest, utc.blob,
                               origins=[dict(origin, staged_from='/usr/share/zoneinfo/Etc/UTC') for origin in utc.origins]))

def encode_zip_path(path, node):
    raw = path.lstrip('/')
    require(not raw.endswith('.link'), 'Literal .link pathname is not representable by pinned FsZip: ' + path)
    name = raw.replace(':','(_colon_)').replace('@','(_at_)')
    if node.kind == 'symlink': name += '.link'
    elif node.kind == 'directory': name += '/'
    return name

def manifest_nodes(fs):
    result = []
    for path, node in sorted(fs.nodes.items()):
        record = {'guest_path':path,'kind':node.kind,'mode':oct(node.mode),'bytes':node.size,
                  'sha256':node.digest,'link_target':node.target,'origins':node.origins,
                  'zip_entry':encode_zip_path(path,node)}
        result.append(record)
    return result

def write_layers(fs, output, epoch):
    seen = {}
    for path, node in fs.nodes.items():
        name = encode_zip_path(path, node).rstrip('/')
        require(name not in seen or seen[name] == path, 'Encoded ZIP pathname collision: ' + path)
        seen[name] = path
    stamp = datetime.datetime.fromtimestamp(epoch, datetime.timezone.utc)
    require(1980 <= stamp.year <= 2107, 'SOURCE_DATE_EPOCH is outside ZIP date range')
    date = (stamp.year,stamp.month,stamp.day,stamp.hour,stamp.minute,stamp.second // 2 * 2)
    results = []
    for layer in ('glibc-rootfs64.zip','wine64.zip'):
        target = Path(output) / layer
        with zipfile.ZipFile(target, 'w', zipfile.ZIP_DEFLATED, compresslevel=6, allowZip64=True) as archive:
            for path, node in sorted(fs.nodes.items()):
                is_wine = path == '/root' or path.startswith('/root/') or path == '/usr' or path.startswith('/usr/')
                if (layer == 'wine64.zip') != is_wine: continue
                name = encode_zip_path(path,node); info = zipfile.ZipInfo(name, date)
                info.create_system = 3
                filetype = stat.S_IFDIR if node.kind == 'directory' else stat.S_IFREG
                info.external_attr = ((filetype | node.mode) << 16) | (0x10 if node.kind == 'directory' else 0)
                info.compress_type = zipfile.ZIP_DEFLATED
                if node.kind == 'directory': archive.writestr(info, b'')
                elif node.kind == 'symlink': archive.writestr(info, node.target.encode('utf-8'))
                else:
                    require(node.kind == 'file', 'Unfinished archive hardlink')
                    with node.blob.open('rb') as source, archive.open(info,'w',force_zip64=True) as dest:
                        shutil.copyfileobj(source,dest,1024*1024)
        results.append({'file':layer,'bytes':target.stat().st_size,'sha256':file_digest(target)})
    return results

def read_elf_dependencies(data):
    require(data[:6] == b'\x7fELF\x02\x01' and len(data) >= 64, 'Expected little-endian ELF64')
    header = struct.unpack_from('<16sHHIQQQIHHHHHH',data)
    require(header[2] == 62, 'Expected AMD64 ELF guest')
    phoff, phsize, count = header[5], header[9], header[10]
    require(phsize == 56 and count <= 65536 and phoff + count * phsize <= len(data), 'Invalid ELF program headers')
    segments = [struct.unpack_from('<IIQQQQQQ',data,phoff+i*phsize) for i in range(count)]
    for seg in segments: require(seg[2] + seg[5] <= len(data), 'ELF segment exceeds file')
    def raw_string(offset, limit):
        require(0 <= offset < len(data) and offset < limit <= len(data), 'Invalid ELF string offset')
        end = data.find(b'\0',offset,limit); require(end != -1,'Unterminated ELF string')
        return data[offset:end].decode('utf-8')
    interp = None; needed_offsets = []; strtab = None; strsize = None; paths = []
    for seg in segments:
        if seg[0] == 3: interp = raw_string(seg[2],seg[2]+seg[5])
        if seg[0] != 2: continue
        require(seg[5] % 16 == 0,'Invalid ELF dynamic segment')
        for offset in range(seg[2],seg[2]+seg[5],16):
            tag, value = struct.unpack_from('<qQ',data,offset)
            if tag == 0: break
            if tag == 1: needed_offsets.append(value)
            elif tag == 5: strtab = value
            elif tag == 10: strsize = value
            elif tag in (15,29): paths.append(value)
    if not needed_offsets and not paths: return interp,[],[]
    require(strtab is not None and strsize is not None,'ELF dynamic string table missing')
    mapped = next((seg[2]+strtab-seg[3] for seg in segments if seg[0] == 1 and
                   seg[3] <= strtab and strtab + strsize <= seg[3] + seg[5]),None)
    require(mapped is not None,'Unmapped ELF dynamic string table')
    needed = [raw_string(mapped+offset,mapped+strsize) for offset in needed_offsets]
    search = [raw_string(mapped+offset,mapped+strsize) for offset in paths]
    return interp,needed,search

def check_elf_closure(fs, plan):
    seeds = ['/usr/lib/wine/wine64','/usr/lib/wine/wineserver64']
    seeds += [path for path,node in fs.nodes.items() if path.startswith('/usr/lib/x86_64-linux-gnu/wine/x86_64-unix/') and
              path.endswith('.so') and node.kind in ('file','symlink')]
    defaults = ['/lib/x86_64-linux-gnu','/usr/lib/x86_64-linux-gnu','/lib64','/usr/lib64','/lib','/usr/lib']
    missing=[]; pending=[]; errors=[]; checked=set(); queue=list(seeds)
    for soname in plan['dlopen_libraries']:
        found = next((path+'/'+soname for path in defaults if fs.resolve(path+'/'+soname)[1] is not None),None)
        if found: queue.append(found)
        else: missing.append({'requested_by':'explicit dlopen list','library':soname})
    while queue:
        path = queue.pop(0)
        try:
            canonical,node=fs.resolve(path)
            require(node and node.kind == 'file','Missing/non-file ELF seed/dependency: '+path)
            if canonical in checked: continue
            checked.add(canonical)
            data=node.blob.read_bytes()
            interp,needed,search=read_elf_dependencies(data)
            if interp: queue.append(interp)
            directories=[]
            for item in search:
                for directory in item.split(':'):
                    directory=directory.replace('${ORIGIN}',str(PurePosixPath(canonical).parent)).replace('$ORIGIN',str(PurePosixPath(canonical).parent))
                    require(directory.startswith('/') and '$' not in directory,'Unsupported dynamic search token/path: '+directory)
                    directories.append(directory)
            for soname in needed:
                candidates = [soname] if soname.startswith('/') else [directory+'/'+soname for directory in directories+defaults]
                found=next((candidate for candidate in candidates if fs.resolve(candidate)[1] is not None),None)
                if found: queue.append(found)
                else:
                    present = sorted(candidate for candidate,node in fs.nodes.items() if
                        PurePosixPath(candidate).name == soname and node.kind in ('file','symlink'))
                    if present:
                        pending.append({'requested_by':canonical,'library':soname,'present_paths':present,
                            'reason':'present outside qualified static search paths; runtime Wine loader/load-order resolution not proven'})
                        queue.extend(present)
                    else: missing.append({'requested_by':canonical,'library':soname})
        except (AssemblyError,UnicodeError,struct.error) as error: errors.append({'path':path,'error':str(error)})
    return {'checked':True,'elf_files_checked':len(checked),'seed_count':len(seeds),'missing':missing,'runtime_resolution_pending':pending,'errors':errors,
            'scope':'static PT_INTERP/DT_NEEDED and explicit dlopen presence; private-library resolution is separately unqualified, no Wine execution'}

def notice_gaps(fs, packages):
    gaps=[]
    for package in packages:
        path='/usr/share/doc/'+package['package']+'/copyright'
        try:
            canonical,node=fs.resolve(path)
            if not node or node.kind != 'file':
                gaps.append({'package':package['package'],'missing':path}); continue
            text=node.blob.read_text(errors='replace')
            for license_path in set(re.findall(r'/usr/share/common-licenses/[A-Za-z0-9.+-]+',text)):
                license_path=license_path.rstrip('.')
                if fs.resolve(license_path)[1] is None: gaps.append({'package':package['package'],'missing':license_path})
        except AssemblyError as error: gaps.append({'package':package['package'],'error':str(error)})
    return gaps

def publish_file(source, target):
    source, target = Path(source), Path(target)
    require(shutil.disk_usage(target.parent).free >= source.stat().st_size + DISK_RESERVE,
            'Insufficient output disk space with 512 MiB reserve')
    fd, temporary = tempfile.mkstemp(prefix='publish-',dir=target.parent)
    try:
        with os.fdopen(fd,'wb') as dest,source.open('rb') as stream:
            shutil.copyfileobj(stream,dest,1024*1024)
            dest.flush(); os.fsync(dest.fileno())
        require(file_digest(temporary) == file_digest(source),'Published artifact copy checksum mismatch')
        os.replace(temporary,target)
    finally:
        if Path(temporary).exists(): Path(temporary).unlink()

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--download',action='store_true',help='Explicitly acquire exact core package .deb payloads into ignored .cache')
    parser.add_argument('--output',type=Path,default=HERE/'.cache'/'assembled')
    parser.add_argument('--work-dir',type=Path,help='Existing scratch parent (for example /dev/shm); owns only a private temporary child')
    args=parser.parse_args()
    output=args.output.resolve(); cache=(HERE/'.cache').resolve()
    ram_output = output.parent == Path('/dev/shm') and output.name.startswith('protondesktop-rootfs-')
    require(output.is_relative_to(cache) or ram_output,'Output must remain under rootfs/.cache or a private /dev/shm/protondesktop-rootfs-* directory')
    output.mkdir(mode=0o700,parents=True,exist_ok=True)
    report_path=output/'assembly-report.json'
    report_path.write_text(json.dumps({'status':'incomplete','rootfs_built':False,'wine_executed':False})+'\n')
    try:
        subprocess.run([sys.executable,str(HERE/'verify_metadata_lock.py'),'--no-report'],check=True)
        lock=json.loads((HERE/'debian-bookworm.lock.json').read_text())
        plan=json.loads((HERE/'assembly-plan.json').read_text())
        packages=[item for item in lock['packages'] if item.get('core_depends_closure')]
        require(len(packages)==125,'Core package lock changed; review assembler input selection')
        epoch=int(email.utils.parsedate_to_datetime(lock['distribution']['release_date']).timestamp())
        work_parent = args.work_dir.resolve() if args.work_dir else cache
        require(work_parent.is_dir(),'Scratch parent must already exist')
        with tempfile.TemporaryDirectory(prefix='assembly-work-',dir=work_parent) as temporary:
            work=Path(temporary); fs=Filesystem(work/'blobs')
            for number,package in enumerate(packages,1):
                print(f'[{number}/{len(packages)}] {package["package"]} {package["version"]}',flush=True)
                package_path=acquire(package,cache/'packages',args.download)
                ingest_deb(package_path,package,fs)
            fs.finish_links(); add_configuration(fs)
            closure=check_elf_closure(fs,plan); notices=notice_gaps(fs,packages)
            manifest={'schema':1,'backend_commit':plan['backend_commit'],'package_lock_sha256':file_digest(HERE/'debian-bookworm.lock.json'),
                'source_packages':lock['sources'],'entries':manifest_nodes(fs),'host_symlinks_created':False,
                'archive_hardlinks_materialized_as_regular_files':True,'zip_guest_modes_preserved':True,
                'runtime_permission_enforcement_qualified':False,'wine_executed':False}
            (work/'payload-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
            layers=write_layers(fs,work,epoch)
            for name in ['payload-manifest.json',*[item['file'] for item in layers]]: publish_file(work/name,output/name)
            result={'status':'built_unqualified','rootfs_built':True,'wine_executed':False,'prepared_prefix_created':False,
                'xbox_tested':False,'boot_qualified':False,'redistribution_qualified':False,
                'core_package_count':len(packages),'optional_alsa_plugins_included':False,
                'package_lock_sha256':file_digest(HERE/'debian-bookworm.lock.json'),
                'layers':layers,'payload_manifest_sha256':file_digest(output/'payload-manifest.json'),
                'entries':len(fs.nodes),'elf_closure':closure,'copyright_or_common_license_gaps':notices,
                'ld_so_cache':'explicitly empty; default library paths and recorded RPATH/RUNPATH only',
                'reproducibility':{'source_date_epoch':epoch,'python':sys.version.split()[0],'zlib':zlib.ZLIB_VERSION,'independent_rebuild_compared':False},
                'missing_implicit_essential_packages_not_added':['base-files common-license payload; not present in frozen lock']}
            report_path.write_text(json.dumps(result,indent=2)+'\n')
            print(json.dumps({key:result[key] for key in ['status','rootfs_built','core_package_count','entries','layers','wine_executed']},indent=2))
            if closure['missing'] or closure['runtime_resolution_pending'] or closure['errors']:
                print('Static ELF closure has unresolved items; artifacts are inspection-only.',file=sys.stderr); return 2
            return 0
    except BaseException as error:
        report_path.write_text(json.dumps({'status':'failed','error':str(error),'rootfs_built':False,'wine_executed':False},indent=2)+'\n')
        raise
if __name__=='__main__': raise SystemExit(main())

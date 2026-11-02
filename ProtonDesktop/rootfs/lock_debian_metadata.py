#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Refresh signed Debian metadata and exact package/source references only.

Never downloads .deb files, source archives, Wine/rootfs binaries, or executes
installers, rootfs tools or Wine. A refresh intentionally changes the lock.
"""
from pathlib import Path
import argparse
import datetime
import hashlib
import json
import lzma
import re
import subprocess
import urllib.request

HERE = Path(__file__).resolve().parent
BASE = 'https://deb.debian.org/debian'
KEYS = {
    'archive-key-12.asc': 'B8B80B5B623EAB6AD8775C45B7C5D7D6350947F8',
    'release-12.asc': '4D64FEC119C2029067D6E791F8D2585B8783D481',
    'archive-key-13.asc': '04B54C3CDCA79751B16BC6B5225629DF75B188BD',
}
SEEDS = ['wine64', 'libwine', 'fonts-wine', 'libc6', 'libgcc-s1',
    'libfreetype6', 'libfontconfig1', 'fontconfig-config', 'libx11-6', 'libx11-data',
    'libxcb1', 'libxext6', 'libxrender1', 'libxcursor1', 'libxfixes3',
    'libxcomposite1', 'libxi6', 'libxinerama1', 'libxxf86vm1', 'libxrandr2',
    'fonts-liberation2', 'tzdata', 'libasound2', 'libasound2-data']
OPTIONAL = ['libasound2-plugins']

def sha(data): return hashlib.sha256(data).hexdigest()
def fetch(url, maximum=20000000):
    with urllib.request.urlopen(url, timeout=90) as response:
        data = response.read(maximum + 1)
    if len(data) > maximum: raise RuntimeError('metadata size limit exceeded: ' + url)
    return data

def paragraphs(text):
    for block in text.split('\n\n'):
        fields = {}
        key = None
        for line in block.splitlines():
            if line.startswith((' ', '\t')) and key:
                fields[key] += '\n' + line[1:]
            elif ':' in line:
                key, value = line.split(':', 1)
                fields[key] = value.strip()
        if fields: yield fields

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--refresh', action='store_true', required=True,
        help='Explicitly fetch metadata and replace package/source locks')
    parser.parse_args()
    cache = HERE / '.cache'; cache.mkdir(exist_ok=True)
    output = HERE / 'metadata'; output.mkdir(exist_ok=True)
    home = cache / 'gnupg'; home.mkdir(mode=0o700, exist_ok=True)
    key_records = []
    for name, fingerprint in KEYS.items():
        url = 'https://ftp-master.debian.org/keys/' + name
        data = fetch(url, 100000)
        path = output / name; path.write_bytes(data)
        listing = subprocess.check_output(['gpg', '--batch', '--no-options', '--homedir', str(home),
            '--with-colons', '--show-keys', str(path)], text=True)
        actual = re.findall(r'^fpr:::::::::([A-F0-9]+):', listing, re.M)[0]
        if actual != fingerprint: raise RuntimeError('Debian primary key fingerprint mismatch')
        subprocess.run(['gpg', '--batch', '--no-options', '--homedir', str(home), '--import', str(path)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        key_records.append({'url':url, 'primary_fingerprint':fingerprint, 'sha256':sha(data)})
    keyring = cache / 'debian-bookworm.gpg'
    keyring.write_bytes(subprocess.check_output(['gpg', '--batch', '--no-options', '--homedir', str(home),
                                                '--export', *KEYS.values()]))
    release_url = BASE + '/dists/bookworm/InRelease'
    inrelease = fetch(release_url, 1000000)
    release_path = output / 'InRelease'; release_path.write_bytes(inrelease)
    verified = subprocess.run(['gpgv', '--keyring', str(keyring), '--status-fd', '1', str(release_path)],
        capture_output=True, text=True)
    (output / 'signature-verification.txt').write_text(verified.stdout + verified.stderr)
    if verified.returncode: raise RuntimeError('Debian InRelease signature verification failed')
    release = inrelease.decode('utf-8')
    signed = release.split('\n\n',1)[1].split('-----BEGIN PGP SIGNATURE-----')[0]
    fields = next(paragraphs(signed))
    if fields.get('Codename') != 'bookworm': raise RuntimeError('Unexpected Debian codename')
    hashes = {}
    for line in fields['SHA256'].splitlines():
        if not line.strip(): continue
        digest, size, name = line.split()
        hashes[name] = (digest, int(size))
    index_records = []
    indexes = {}
    for name in ['main/binary-amd64/Packages.xz', 'main/source/Sources.xz']:
        digest, size = hashes[name]
        directory = name.rsplit('/',1)[0]
        url = BASE + '/dists/bookworm/' + directory + '/by-hash/SHA256/' + digest
        data = fetch(url)
        if len(data) != size or sha(data) != digest: raise RuntimeError('Signed index checksum mismatch')
        (cache / name.rsplit('/',1)[1]).write_bytes(data)
        indexes[name] = list(paragraphs(lzma.decompress(data).decode('utf-8')))
        index_records.append({'path':name,'url':url,'sha256':digest,'bytes':size})
    packages = {p['Package']:p for p in indexes['main/binary-amd64/Packages.xz']}
    sources = {p['Package']:p for p in indexes['main/source/Sources.xz']}
    # Lock every declared Depends/Pre-Depends edge of the explicit core seeds.
    # This is deterministic metadata resolution, not an apt installation or
    # validation of maintainer-script effects, conflicts or dlopen ELF closure.
    chosen = set(SEEDS)
    queue = list(SEEDS)
    edges = []
    while queue:
        owner = queue.pop(0)
        for field in ('Pre-Depends', 'Depends'):
            for group in filter(None, (part.strip() for part in packages[owner].get(field, '').split(','))):
                alternatives = []
                for token in group.split('|'):
                    match = re.fullmatch(r'([a-z0-9+.-]+)(?::(?:any|native|amd64))?(?:\s*\((<<|<=|=|>=|>>)\s*([^()]+)\))?', token.strip())
                    if not match: raise RuntimeError('Unsupported dependency syntax: ' + token)
                    name, operation, version = match.groups()
                    if name not in packages: continue
                    if operation and subprocess.run(['dpkg', '--compare-versions', packages[name]['Version'], operation, version],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
                        continue
                    alternatives.append(name)
                if not alternatives: raise RuntimeError('No concrete compatible dependency candidate: ' + owner + ' -> ' + group)
                dependency = next((name for name in alternatives if name in chosen), alternatives[0])
                edges.append({'package':owner,'field':field,'relation':group,'selected':dependency})
                if dependency not in chosen:
                    chosen.add(dependency); queue.append(dependency)
    selected = []
    selected_sources = {}
    for name in sorted(chosen | set(OPTIONAL)):
        package = packages[name]
        source_field = package.get('Source', name)
        source_name = source_field.split()[0]
        source_version = source_field.split('(',1)[1].rstrip(')') if '(' in source_field else package['Version']
        source = sources[source_name]
        if source['Version'] != source_version:
            raise RuntimeError('Binary/source version mismatch for ' + name)
        selected.append({'package':name,'version':package['Version'],'architecture':package['Architecture'],
            'filename':package['Filename'],'url':BASE+'/'+package['Filename'], 'sha256':package['SHA256'],
            'bytes':int(package['Size']),'source':source_name,'source_version':source_version,
            'depends':package.get('Depends',''),'pre_depends':package.get('Pre-Depends',''),
            'recommends':package.get('Recommends',''),'optional_audio':name in OPTIONAL,'explicit_core_seed':name in SEEDS,'core_depends_closure':name in chosen})
        archives = []
        for line in source['Checksums-Sha256'].splitlines():
            if not line.strip(): continue
            digest, size, filename = line.split()
            archives.append({'filename':filename,'sha256':digest,'bytes':int(size),
                             'url':BASE+'/'+source['Directory']+'/'+filename})
        selected_sources[source_name] = {'source':source_name,'version':source['Version'],
            'directory':source['Directory'],'archives':archives,
            'copyright_reference':'https://metadata.ftp-master.debian.org/changelogs/main/' +
                (source_name[:4] if source_name.startswith('lib') else source_name[0]) + '/' +
                source_name + '/' + source_name + '_' + source['Version'] + '_copyright'}
    lock = {'schema':1,'created_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'purpose':'metadata-only declared Depends/Pre-Depends closure of explicit core seeds, plus optional audio-plugin reference; not an apt install simulation or ELF closure',
        'core_seeds':SEEDS,'optional_seeds':OPTIONAL,'dependency_choices':edges,
        'distribution':{'codename':fields['Codename'],'suite':fields['Suite'],
            'version':fields.get('Version'),'release_date':fields['Date'],
            'inrelease_url':release_url,'inrelease_sha256':sha(inrelease)},
        'trust':{'fingerprint_source':'https://ftp-master.debian.org/keys.html',
            'bootstrap':'Official HTTPS keys checked against explicitly recorded Debian fingerprints; no preinstalled distro trust anchor used',
            'keys':key_records,'gpgv_verified':True},
        'indexes':index_records,'packages':selected,
        'sources':sorted(selected_sources.values(), key=lambda p:p['source']),
        'wine_binaries_downloaded':False,'source_archives_downloaded':False,
        'rootfs_built':False,'wine_executed':False}
    (HERE / 'debian-bookworm.lock.json').write_text(json.dumps(lock, indent=2)+'\n')
    print(json.dumps({'release':lock['distribution']['version'], 'signed_metadata_verified':True,
                      'locked_binary_packages':len(selected),'core_seeds':len(SEEDS),'source_packages':len(selected_sources),
                      'wine64_version':packages['wine64']['Version'],
                      'libc6_version':packages['libc6']['Version'],
                      'binaries_downloaded':False},indent=2))
if __name__ == '__main__': main()

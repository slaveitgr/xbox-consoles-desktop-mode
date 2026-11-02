# SPDX-License-Identifier: GPL-2.0-only
"""Self-authored archive/ELF metadata fixtures; never run a guest executable."""
from pathlib import Path
import hashlib
import importlib.util
import io
import json
import lzma
import struct
import sys
import tarfile
import tempfile
import unittest
import zipfile

spec=importlib.util.spec_from_file_location('assemble_rootfs',Path(__file__).with_name('assemble_rootfs.py'))
tool=importlib.util.module_from_spec(spec); sys.modules[spec.name]=tool; spec.loader.exec_module(tool)

def make_tar(entries):
    buffer=io.BytesIO()
    with tarfile.open(fileobj=buffer,mode='w',format=tarfile.PAX_FORMAT) as archive:
        for kind,name,value in entries:
            item=tarfile.TarInfo(name); item.mode=0o755 if kind=='directory' else 0o644
            if kind=='file': item.size=len(value); archive.addfile(item,io.BytesIO(value))
            else:
                item.type={'directory':tarfile.DIRTYPE,'symlink':tarfile.SYMTYPE,'hardlink':tarfile.LNKTYPE,'fifo':tarfile.FIFOTYPE}[kind]
                if kind in ('symlink','hardlink'): item.linkname=value
                archive.addfile(item)
    return lzma.compress(buffer.getvalue())

def ar_member(name,data):
    header=(name+'/').ljust(16)+'0'.ljust(12)+'0'.ljust(6)+'0'.ljust(6)+'100644'.ljust(8)+str(len(data)).ljust(10)+'`\n'
    return header.encode('ascii')+data+(b'\n' if len(data)%2 else b'')

def write_deb(directory,entries,name='fixture',version='1'):
    data=b'!<arch>\n'+ar_member('debian-binary',b'2.0\n')+ar_member('control.tar.xz',make_tar([]))+ar_member('data.tar.xz',make_tar(entries))
    checksum=hashlib.sha256(data).hexdigest(); path=Path(directory)/(name+'.deb'); path.write_bytes(data)
    package={'package':name,'version':version,'source':name,'source_version':version,'bytes':len(data),'sha256':checksum,
             'filename':'pool/main/f/fixture/'+name+'_1_all.deb','url':'https://deb.debian.org/debian/pool/main/f/fixture/'+name+'_1_all.deb'}
    return path,package

def elf(needed=None,interp=None,bad_strings=False):
    data=bytearray(0x400); base=0x400000
    strings=b'\0'+(needed.encode()+b'\0' if needed else b'')
    count=1+(1 if needed else 0)+(1 if interp else 0)
    data[:64]=struct.pack('<16sHHIQQQIHHHHHH',b'\x7fELF\x02\x01\x01'+b'\0'*9,3,62,1,base,64,0,0,64,56,count,0,0,0)
    ph=[(1,5,0,base,base,len(data),len(data),0x1000)]
    if needed:
        dynamic=[(1,1),(5,base+(0x1000 if bad_strings else 0x300)),(10,len(strings)),(0,0)]
        ph.append((2,4,0x200,base+0x200,base+0x200,len(dynamic)*16,len(dynamic)*16,8))
        for index,(tag,value) in enumerate(dynamic): struct.pack_into('<qQ',data,0x200+index*16,tag,value)
        data[0x300:0x300+len(strings)]=strings
    if interp:
        raw=interp.encode()+b'\0'; data[0x380:0x380+len(raw)]=raw
        ph.append((3,4,0x380,base+0x380,base+0x380,len(raw),len(raw),1))
    for index,segment in enumerate(ph): struct.pack_into('<IIQQQQQQ',data,64+index*56,*segment)
    return bytes(data)

class AssemblyTests(unittest.TestCase):
    def setUp(self):
        self.temporary=tempfile.TemporaryDirectory(); self.root=Path(self.temporary.name)
        self.fs=tool.Filesystem(self.root/'blobs')
    def tearDown(self): self.temporary.cleanup()
    def ingest(self,entries,name='fixture'):
        path,package=write_deb(self.root,entries,name); tool.ingest_deb(path,package,self.fs); return path,package
    def test_real_ar_tar_and_virtual_drive_link_encoding(self):
        self.ingest([('directory','usr',None),('file','usr/lib/self-authored.txt',b'not an executable'),
            ('symlink','home/username/.wine/dosdevices/c:','../drive_c'),
            ('symlink','lib64/loader','/usr/lib/self-authored.txt')])
        self.fs.finish_links(); out=self.root/'out'; out.mkdir(); tool.write_layers(self.fs,out,1783764997)
        with zipfile.ZipFile(out/'glibc-rootfs64.zip') as archive:
            self.assertEqual(archive.read('home/username/.wine/dosdevices/c(_colon_).link'),b'../drive_c')
            self.assertEqual(archive.read('lib64/loader.link'),b'/usr/lib/self-authored.txt')
        with zipfile.ZipFile(out/'wine64.zip') as archive:
            self.assertEqual(archive.read('usr/lib/self-authored.txt'),b'not an executable')
            self.assertEqual((archive.getinfo('usr/lib/self-authored.txt').external_attr>>16)&0o7777,0o644)
        self.assertFalse(any(path.is_symlink() for path in self.root.rglob('*')))
    def test_package_hash_and_size_before_parsing(self):
        path,p=write_deb(self.root,[('file','safe',b'x')])
        with self.assertRaisesRegex(tool.AssemblyError,'size mismatch'): tool.ingest_deb(path,dict(p,bytes=p['bytes']+1),self.fs)
        with self.assertRaisesRegex(tool.AssemblyError,'SHA256 mismatch'): tool.ingest_deb(path,dict(p,sha256='0'*64),self.fs)
        self.assertEqual(self.fs.nodes,{})
    def test_archive_parent_traversal_rejected(self):
        with self.assertRaisesRegex(tool.AssemblyError,'traversal'): self.ingest([('file','../../outside',b'bad')])
        self.assertFalse((self.root.parent/'outside').exists())
    def test_absolute_and_backslash_member_paths_rejected(self):
        for name in ('/outside','folder\\outside'):
            with self.subTest(name=name),self.assertRaises(tool.AssemblyError): self.ingest([('file',name,b'bad')])
    def test_symlink_above_guest_root_rejected(self):
        with self.assertRaisesRegex(tool.AssemblyError,'escapes guest root'): self.ingest([('symlink','safe/link','../../../host')])
    def test_hardlink_parent_traversal_rejected(self):
        with self.assertRaisesRegex(tool.AssemblyError,'traversal'): self.ingest([('hardlink','safe','../../host')])
    def test_payload_below_virtual_symlink_is_not_followed_on_host(self):
        self.ingest([('symlink','redirect','/tmp'),('file','redirect/surprise',b'bad')])
        with self.assertRaisesRegex(tool.AssemblyError,'non-directory'): self.fs.finish_links()
        self.assertFalse((self.root/'redirect').exists())
    def test_unrelated_package_collision_is_error(self):
        self.ingest([('file','usr/shared',b'first')],'first')
        with self.assertRaisesRegex(tool.AssemblyError,'Conflicting package payload'):
            self.ingest([('file','usr/shared',b'second')],'second')
        self.assertEqual(self.fs.nodes['/usr/shared'].blob.read_bytes(),b'first')
    def test_identical_shared_payload_retains_both_origins(self):
        self.ingest([('file','usr/shared',b'same')],'first'); self.ingest([('file','usr/shared',b'same')],'second')
        self.assertEqual([p['package'] for p in self.fs.nodes['/usr/shared'].origins],['first','second'])
    def test_hardlink_materializes_verified_package_bytes(self):
        self.ingest([('file','usr/source',b'own fixture'),('hardlink','usr/alias','usr/source')])
        self.fs.finish_links(); self.assertEqual(self.fs.nodes['/usr/alias'].blob.read_bytes(),b'own fixture')
        self.assertEqual(self.fs.nodes['/usr/alias'].origins[0]['archive_hardlink_target'],'/usr/source')
    def test_unresolved_hardlink_and_loop_rejected(self):
        self.ingest([('hardlink','a','b'),('hardlink','b','a')])
        with self.assertRaisesRegex(tool.AssemblyError,'cycle'): self.fs.finish_links()
    def test_device_fifo_rejected_without_host_creation(self):
        with self.assertRaisesRegex(tool.AssemblyError,'special tar'): self.ingest([('fifo','device',None)])
        self.assertFalse((self.root/'device').exists())
    def test_escaped_or_literal_link_names_fail_closed(self):
        with self.assertRaisesRegex(tool.AssemblyError,'escaped guest name'): self.ingest([('file','already(_colon_)encoded',b'x')])
        self.ingest([('file','literal.link',b'not a link')]); out=self.root/'out';out.mkdir()
        with self.assertRaisesRegex(tool.AssemblyError,'Literal .link'): tool.write_layers(self.fs,out,1783764997)
        self.assertEqual(list(out.iterdir()),[])
    def test_zip_repeat_bytes_and_epoch_are_deterministic(self):
        self.ingest([('file','usr/b',b'b'),('file','lib/a',b'a')]); self.fs.finish_links()
        first=self.root/'one';second=self.root/'two';first.mkdir();second.mkdir()
        a=tool.write_layers(self.fs,first,1783764997);b=tool.write_layers(self.fs,second,1783764997)
        self.assertEqual(a,b)
        with zipfile.ZipFile(first/'wine64.zip') as archive:
            self.assertEqual(archive.infolist()[0].date_time,(2026,7,11,10,16,36))
    def test_offline_acquisition_and_cache_corruption(self):
        path,p=write_deb(self.root,[('file','own',b'text')]);cache=self.root/'cache';cache.mkdir()
        with self.assertRaisesRegex(tool.AssemblyError,'offline cache'): tool.acquire(p,cache)
        cached=cache/(p['sha256']+'.deb');cached.write_bytes(path.read_bytes())
        self.assertEqual(tool.acquire(p,cache),cached)
        cached.write_bytes(b'broken')
        with self.assertRaisesRegex(tool.AssemblyError,'Cached package'): tool.acquire(p,cache)
    def test_ar_truncation_and_invalid_magic(self):
        path,p=write_deb(self.root,[('file','own',b'x')])
        for raw in (b'not ar',path.read_bytes()[:-9]):
            broken=self.root/'broken';broken.write_bytes(raw)
            with broken.open('rb') as stream,self.assertRaises(tool.AssemblyError): tool.ar_members(stream)
    def test_elf_interp_and_dynamic_needed_metadata(self):
        self.assertEqual(tool.read_elf_dependencies(elf('libown.so','/lib64/ld-own.so')),
                         ('/lib64/ld-own.so',['libown.so'],[]))
        with self.assertRaisesRegex(tool.AssemblyError,'Unmapped ELF'): tool.read_elf_dependencies(elf('libown.so',bad_strings=True))
    def test_real_static_dependency_closure_and_missing_library(self):
        self.fs.generated('/usr/lib/wine/wine64',elf('libown.so'))
        self.fs.generated('/usr/lib/wine/wineserver64',elf())
        first=tool.check_elf_closure(self.fs,{'dlopen_libraries':[]})
        self.assertEqual(first['missing'],[{'requested_by':'/usr/lib/wine/wine64','library':'libown.so'}])
        self.fs.generated('/lib/x86_64-linux-gnu/libown.so',elf())
        second=tool.check_elf_closure(self.fs,{'dlopen_libraries':[]})
        self.assertEqual(second['missing'],[]);self.assertEqual(second['errors'],[])
        self.assertEqual(second['elf_files_checked'],3)

    def test_present_private_library_is_not_reported_missing_or_silently_qualified(self):
        self.fs.generated('/usr/lib/wine/wine64',elf('ntdll.so'))
        self.fs.generated('/usr/lib/wine/wineserver64',elf())
        private='/usr/lib/x86_64-linux-gnu/wine/x86_64-unix/ntdll.so'
        self.fs.generated(private,elf())
        result=tool.check_elf_closure(self.fs,{'dlopen_libraries':[]})
        self.assertEqual(result['missing'],[])
        self.assertEqual(result['runtime_resolution_pending'][0]['present_paths'],[private])
        self.assertEqual(result['elf_files_checked'],3)
    def test_guest_lookup_normalizes_in_root_parent_components_only(self):
        self.fs.generated('/lib/own',b'own')
        self.assertEqual(self.fs.resolve('/lib/child/../own')[1].blob.read_bytes(),b'own')
        with self.assertRaisesRegex(tool.AssemblyError,'escapes guest root'): self.fs.resolve('/../../host')

if __name__=='__main__': unittest.main()

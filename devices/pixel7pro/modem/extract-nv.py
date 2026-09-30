#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Supports the direct/inline files and directories in this phone's snapshots.
# Refuses unsupported directory layouts. dump.f2fs may modify its image, so
# it sees disposable copies only. Work output contains private NV: never commit.
# Correct inline offset: i_addr + i_extra_isize + one reserved u32. The local
# dump.f2fs 1.16 export omitted extra attributes for inline regular files.
from pathlib import Path
import os,re,struct,subprocess,shutil
import argparse
ap=argparse.ArgumentParser(description='Extract private F2FS NV snapshots from backup COPIES; never mounts a partition. Set LD_LIBRARY_PATH externally if dump.f2fs needs it.')
ap.add_argument('backups',type=Path)
ap.add_argument('output',type=Path)
ap.add_argument('--dump-f2fs',required=True,type=Path)
a=ap.parse_args()
if a.output.exists():ap.error('output must not exist')
a.output.mkdir(parents=True,mode=0o700)
private=a.output.resolve();dest=private/'nv-snapshot';dest.mkdir(mode=0o700)
tool=Path(os.path.abspath(a.dump_f2fs));env=os.environ.copy()
for part in ['efs','efs_backup','modem_userdata']:
 work=private/('extract-'+part);work.mkdir(mode=0o700,exist_ok=True)
 image=work/(part+'-copy.img')
 if not image.exists():subprocess.run(['cp','--reflink=auto',str(a.backups/f'{part}.img'),str(image)],check=True)
 seen=set()
 def dump(ino):
  r=subprocess.run([str(tool),'-i',f'0x{ino:x}',str(image)],cwd=work,env=env,input='Y\n',text=True,capture_output=True,timeout=20)
  (work/f'inode-{ino:x}.log').write_text(r.stdout+r.stderr)
  if r.returncode:raise RuntimeError('dump failed')
  return r.stdout
 def walk(ino,target):
  if ino in seen:return
  seen.add(ino); s=dump(ino)
  mode=int(re.search(r'i_mode\s*\[0x\s*([0-9a-fA-F]+)',s)[1],16)
  if mode & 0xf000 == 0x4000:
   target.mkdir(parents=True,mode=0o700,exist_ok=True)
   if any(int(v,16) for v in re.findall(r'i_nid\[\d+\]\s*\[0x\s*([0-9a-fA-F]+)',s)):raise RuntimeError('indirect directory not implemented')
   words={int(i,16):int(v,16) for i,v in re.findall(r'i_addr\[0x([0-9a-fA-F]+)\]\s*\[0x\s*([0-9a-fA-F]+)',s)}
   inline=int(re.search(r'i_inline\s*\[0x\s*([0-9a-fA-F]+)',s)[1],16)
   if inline & 4:
    extra=int(re.search(r'i_extra_isize\s*\[0x\s*([0-9a-fA-F]+)',s)[1],16)
    size=int(re.search(r'i_size\s*\[0x\s*([0-9a-fA-F]+)',s)[1],16)
    raw=bytearray(923*4)
    for i,v in words.items():struct.pack_into('<I',raw,i*4,v)
    data=bytes(raw[extra+4:extra+4+size]);count=size*8//153
    entry_offset=size-count*19;filename_offset=entry_offset+count*11
    regions=[(data,count,entry_offset,filename_offset)]
   else:
    regions=[]
    with image.open('rb') as f:
     for block in words.values():
      if not block or block==0xffffffff:continue
      f.seek(block*4096);data=f.read(4096)
      if len(data)!=4096:raise RuntimeError('invalid directory block')
      regions.append((data,214,30,2384))
   for data,count,entry_offset,filename_offset in regions:
    skip_until=0
    for slot in range(count):
     if slot<skip_until:continue
     if not (data[slot//8]>>(slot%8)&1):continue
     _,child,length,kind=struct.unpack_from('<IIHB',data,entry_offset+slot*11)
     if not 0<length<=255:raise RuntimeError('invalid dentry')
     skip_until=slot+(length+7)//8
     name=data[filename_offset+slot*8:filename_offset+slot*8+length].decode()
     if name in ['.','..']:continue
     if '/' in name or '\0' in name:raise RuntimeError('invalid name')
     walk(child,target/name)
  elif mode & 0xf000 == 0x8000:
   inline=int(re.search(r'i_inline\s*\[0x\s*([0-9a-fA-F]+)',s)[1],16)
   if inline & 2:
    extra=int(re.search(r'i_extra_isize\s*\[0x\s*([0-9a-fA-F]+)',s)[1],16)
    size=int(re.search(r'i_size\s*\[0x\s*([0-9a-fA-F]+)',s)[1],16)
    words={int(i,16):int(v,16) for i,v in re.findall(r'i_addr\[0x([0-9a-fA-F]+)\]\s*\[0x\s*([0-9a-fA-F]+)',s)}
    raw=bytearray(923*4)
    for i,v in words.items():struct.pack_into('<I',raw,i*4,v)
    if size>len(raw)-extra-4:raise RuntimeError('oversize inline file')
    target.parent.mkdir(parents=True,mode=0o700,exist_ok=True)
    target.write_bytes(raw[extra+4:extra+4+size]);target.chmod(0o600)
    return
   # dump.f2fs names each recovered inode under lost_found/.
   matches=[p for p in work.rglob(target.name) if p.is_file() and p!=target]
   if len(matches)!=1:raise RuntimeError('dump output missing or ambiguous for '+target.name)
   target.parent.mkdir(parents=True,mode=0o700,exist_ok=True)
   shutil.copyfile(matches[0],target);target.chmod(0o600)
  else:raise RuntimeError('unexpected inode type')
 walk(3,dest/part)
 print(part,'extracted',sum(p.is_file() for p in (dest/part).rglob('*')),'files')

import hashlib
for part,name in [('efs','nv_normal.bin'),('efs','nv_protected.bin'),('efs_backup','nv_protected.bak')]:
 p=dest/part/name
 if hashlib.md5(p.read_bytes()+b'Samsung_SIT_RIL').hexdigest().encode()!=p.with_name(name+'.md5').read_bytes():
  raise RuntimeError('Stock salted NV checksum mismatch: '+name)
print('All three stock salted NV checksums verified')

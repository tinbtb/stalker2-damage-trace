from pathlib import Path
import argparse,struct,hashlib,json
R=Path(__file__).resolve().parent
import pefile
parser=argparse.ArgumentParser(description='Optional local profile regeneration; never required by CI.')
parser.add_argument('exe',type=Path,help='Path to your legally installed, audited Steam v2.1.0 executable')
exe=parser.parse_args().exe
expected=json.loads((R/'profile.json').read_text())['exe_sha256']
if hashlib.sha256(exe.read_bytes()).hexdigest()!=expected:
    raise SystemExit('Refusing an unaudited executable; new versions require a fresh reverse-engineering audit.')
p=pefile.PE(str(exe),fast_load=True)
d=p.OPTIONAL_HEADER.DATA_DIRECTORY[3]
funcs=list(struct.iter_unpack('<III',p.get_data(d.VirtualAddress,d.Size)))
sites={'APPLY':0xc4c814,'ARMOR':0xc4f34e,'ROLL':0xc4f3fc,'BONE':0xc4fe59,'HEALTH':0x53765e}
relocs=[];d=p.OPTIONAL_HEADER.DATA_DIRECTORY[5];off=0
while off<d.Size:
    page,size=struct.unpack('<II',p.get_data(d.VirtualAddress+off,8))
    if size<8:break
    for (v,) in struct.iter_unpack('<H',p.get_data(d.VirtualAddress+off+8,size-8)):
        if v>>12:relocs.append(page+(v&4095))
    off+=size
R.joinpath('src').mkdir(exist_ok=True,parents=True)
lines=['/* Generated from the audited v2.1.0 snapshot. */','#pragma once',f'#define EXPECTED_EXE_SHA256 "{hashlib.sha256(exe.read_bytes()).hexdigest()}"']
manifest=[]
for n,a in sites.items():
    start,end,_=next((f for f in funcs if f[0]==a),(a,0xc4f45e,0) if n=='ROLL' else (0,0,0))
    assert start==a and end>a,n
    assert not any(start<=r<end for r in relocs)
    digest=hashlib.sha256(p.get_data(a,end-a)).digest()
    lines += [f'#define RVA_{n} 0x{a:x}u',f'#define SIZE_{n} {end-a}u',f'static const unsigned char HASH_{n}[32]={{'+','.join(str(b) for b in digest)+'};']
    manifest.append(dict(name=n,rva=hex(a),size=end-a,sha256=digest.hex()))
license_text=(R/'third_party/minhook/LICENSE.txt').read_text()
lines += ['static const char third_party_license[] = '+json.dumps(license_text)+';']
(R/'src/profile.h').write_text('\n'.join(lines)+'\n')
(R/'profile.json').write_text(json.dumps(dict(exe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),functions=manifest),indent=2)+'\n')
print(json.dumps(manifest,indent=2))

"""Verify native build evidence and package only the two installable mod files."""
from pathlib import Path
import hashlib,json,zipfile
import pefile
R=Path(__file__).resolve().parent
out=R/'dist';out.mkdir(exist_ok=True)
asi=R/'build/DamageTrace.asi'
p=pefile.PE(str(asi),fast_load=True);p.parse_data_directories(directories=[0,1])
imports=[e.dll.decode() for e in p.DIRECTORY_ENTRY_IMPORT]
exports=[e.name.decode() for e in p.DIRECTORY_ENTRY_EXPORT.symbols if e.name]
assert p.FILE_HEADER.Machine==0x8664 and p.FILE_HEADER.Characteristics&0x2000
assert {'InitializeASI','DamageTrace_ThirdPartyLicense'}<=set(exports)
assert all(n.lower() in ('kernel32.dll','bcrypt.dll') or n.lower().startswith('api-ms-win-crt-') for n in imports),imports
guard=(R/'build/DamageTrace.log').read_text()
assert 'UNSUPPORTED:' in guard and 'ACTIVE:' not in guard
analysis=json.loads((R/'build/selftest_analysis/summary.json').read_text())
assert analysis['armor_checks']=={'match':5} and analysis['observed_kills']==1
archive=out/'DamageTrace_v0.2.1.zip'
with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED) as z:
    z.write(asi,'DamageTrace.asi');z.write(R/'DamageTrace.ini','DamageTrace.ini')
with zipfile.ZipFile(archive) as z:
    assert z.namelist()==['DamageTrace.asi','DamageTrace.ini'] and z.testzip() is None
manifest=dict(version='0.2.1',asi_sha256=hashlib.sha256(asi.read_bytes()).hexdigest(),zip_sha256=hashlib.sha256(archive.read_bytes()).hexdigest(),supported_exe_sha256=json.loads((R/'profile.json').read_text())['exe_sha256'],zip_entries=['DamageTrace.asi','DamageTrace.ini'],imports=imports,exports=exports,tests='passed')
(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps(manifest,indent=2))

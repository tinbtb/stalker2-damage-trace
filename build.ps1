param([string]$Zig='',[string]$Python='python')
$ErrorActionPreference='Stop'
if(!$Zig){$Zig=Join-Path $PSScriptRoot '.tools/zig-x86_64-windows-0.15.2/zig.exe'}
$Zig=(Resolve-Path -LiteralPath $Zig).Path
if((& $Zig version).Trim() -ne '0.15.2'){throw 'Requires Zig 0.15.2'}
$env:ZIG_LIB_DIR=Join-Path (Split-Path $Zig -Parent) 'lib'
$build=Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Force $build | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR=Join-Path $build 'zig-cache'
$env:ZIG_LOCAL_CACHE_DIR=Join-Path $build 'zig-local-cache'
Push-Location $PSScriptRoot
try {
 $sources=@('src/damage_trace.c','third_party/minhook/src/buffer.c','third_party/minhook/src/hook.c','third_party/minhook/src/trampoline.c','third_party/minhook/src/hde/hde64.c')
 $flags=@('-target','x86_64-windows-gnu','-std=c11','-O2','-g0','-Wall','-Wextra','-Wno-cast-function-type','-Wno-unused-function','-Isrc','-Ithird_party/minhook/include','-lbcrypt','-lkernel32')
 & $Zig cc @sources @flags -shared -o build/DamageTrace.asi
 if($LASTEXITCODE){throw 'ASI compilation failed'}
 & $Python scripts/normalize_pe.py build/DamageTrace.asi
 if($LASTEXITCODE){throw 'PE normalization failed'}
 & $Zig cc @sources @flags -DTRACE_TEST -o build/selftest.exe
 if($LASTEXITCODE){throw 'Native test compilation failed'}
 Push-Location $build
 try { & ./selftest.exe; if($LASTEXITCODE){throw 'Native tests failed'} } finally {Pop-Location}
 & $Python test_analysis.py
 if($LASTEXITCODE){throw 'Trace analysis tests failed'}
 & $Zig cc -target x86_64-windows-gnu -O2 guard_host.c -o build/guard_host.exe
 if($LASTEXITCODE){throw 'Guard-host compilation failed'}
 Push-Location $build
 try { & ./guard_host.exe; if($LASTEXITCODE){throw 'Unsupported-host test failed'} } finally {Pop-Location}
 & $Python package.py
 if($LASTEXITCODE){throw 'Package verification failed'}
} finally {Pop-Location}

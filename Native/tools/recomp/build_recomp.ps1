# Builds a game package in recomp mode on Windows: the game recompiled from the user's ROM by
# N64Recomp, on com.recomp.n64's recomp runtime, published as the library the game's addon links.
#
#   build_recomp.ps1 -Package <Packages\com.recomp.<game>> -Rom <your .z64> [-Decomp <dir>] [-Config RelWithDebInfo]
#
# The game package provides Recomp\ (recomp.<region>.toml, its symbol files, CMakeLists.txt calling
# n64port_recomp_game). Steps, each skipped when its output is up to date:
#   1. N64Recomp.exe from the vendored source (ThirdParty\N64Recomp)    -> Native\build\n64recomp
#   2. the C: N64Recomp with the game's toml, this ROM and output folder -> <game>\Native\build\recomp\funcs
#      then the loop preemption points (add_loop_checks.py)
#   3. the game library (the game's Recomp\CMakeLists.txt)              -> <game>\Native\build\recomp-<Config>
#   4. published as Lib\<the library package.json links on Windows>, with Lib\<lib>.mode = recomp
#      and the stamp header in Source\Generated bumped, so the editor relinks the addon.
# The generated C and the library hold game code made from your ROM: they stay in build\ and
# Lib\, which are git-ignored. Called by the editor (N64 Recomp Target Options, Build mode Recomp).
param(
    [Parameter(Mandatory = $true)][string]$Package,
    [string]$Rom = '',
    [string]$Decomp = '',
    [string]$Config = 'RelWithDebInfo'
)
$ErrorActionPreference = 'Continue'
$n64 = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$Package = (Resolve-Path $Package -ErrorAction Stop).Path
$recompDir = Join-Path $Package 'Recomp'
$outDir = Join-Path $Package 'Native\build\recomp'
$funcs = Join-Path $outDir 'funcs'
function Fwd([string]$p) { return $p -replace '\\', '/' }

# The game's config: one recomp.<region>.toml in Recomp\
$tomls = @(Get-ChildItem $recompDir -Filter 'recomp.*.toml' -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^recomp\.[A-Za-z0-9]+\.toml$' -and $_.Name -notmatch '\.(dev|gen|trig)\.toml$' })
if ($tomls.Count -eq 0) { throw "no Recomp\recomp.<region>.toml in $Package" }
if ($tomls.Count -gt 1) { Write-Host "several configs in Recomp\, using $($tomls[0].Name)" }
$toml = $tomls[0].FullName

# Visual Studio's x64 environment, clang, CMake and Ninja (as the decomp build uses)
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -property installationPath
if (-not $vs) { throw 'Visual Studio (with the C++ workload and its Clang tools) is needed' }
if (-not $env:VSCMD_VER) {
    $vars = & "$env:SystemRoot\System32\cmd.exe" /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>&1 && set"
    if (-not $vars) { throw 'could not import the Visual Studio environment (vcvars64.bat)' }
    foreach ($line in $vars) {
        if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
    }
}
$llvm = "$vs\VC\Tools\Llvm\x64\bin"
$cmake = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
foreach ($tool in @("$llvm\clang.exe", $cmake, $ninja)) {
    if (-not (Test-Path $tool)) { throw "missing $tool (install the Visual Studio C++ Clang tools and CMake)" }
}
$python = if ($Decomp -and (Test-Path (Join-Path $Decomp '.venv-win\Scripts\python.exe'))) { Join-Path $Decomp '.venv-win\Scripts\python.exe' } else { 'python' }

# 1. N64Recomp
$rcBuild = Join-Path $n64 'Native\build\n64recomp'
$rcExe = Join-Path $rcBuild 'N64Recomp.exe'
if (-not (Test-Path $rcExe)) {
    Write-Host 'building N64Recomp (once)'
    & $cmake -S (Join-Path $n64 'ThirdParty\N64Recomp') -B $rcBuild -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" `
        "-DCMAKE_C_COMPILER=$llvm\clang-cl.exe" "-DCMAKE_CXX_COMPILER=$llvm\clang-cl.exe" -DCMAKE_BUILD_TYPE=Release | Out-Null
    & $cmake --build $rcBuild --target N64RecompCLI
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $rcExe)) { throw 'building N64Recomp failed' }
}

# 2. The C. The game's toml with its paths made absolute, this ROM and this output folder.
New-Item -ItemType Directory -Force $funcs | Out-Null
$lines = foreach ($line in Get-Content $toml) {
    if ($line -match '^\s*(\w+_path)\s*=\s*"(.*)"\s*$') {
        $key = $Matches[1]; $value = $Matches[2]
        if ($key -eq 'output_func_path') { $value = Fwd $funcs }
        elseif ($key -eq 'rom_file_path' -and $Rom) { $value = Fwd $Rom }
        elseif (-not [System.IO.Path]::IsPathRooted($value)) { $value = Fwd ([System.IO.Path]::GetFullPath((Join-Path $recompDir $value))) }
        "$key = `"$value`""
    } else { $line }
}
$genToml = Join-Path $outDir 'recomp.gen.toml'
$text = ($lines -join "`n") + "`n"
if (-not (Test-Path $genToml) -or (Get-Content $genToml -Raw) -ne $text) { Set-Content -Path $genToml -Value $text -NoNewline }
$romUsed = (Select-String -Path $genToml -Pattern '^rom_file_path = "(.*)"').Matches[0].Groups[1].Value
if (-not (Test-Path $romUsed)) { throw "ROM not found: $romUsed (set the ROM in the N64 Recomp Target Options)" }
$magic = [System.IO.File]::ReadAllBytes($romUsed)[0..3]
if (-not ($magic[0] -eq 0x80 -and $magic[1] -eq 0x37 -and $magic[2] -eq 0x12 -and $magic[3] -eq 0x40)) {
    throw "$romUsed is not a big-endian .z64 ROM (convert a .v64 / .n64 dump to .z64 first)"
}

$stamp = Join-Path $outDir 'funcs.stamp'
$inputs = @($genToml, $romUsed, $rcExe) + @(Get-ChildItem $recompDir -Filter '*.toml' | ForEach-Object FullName)
$stale = -not (Test-Path $stamp) -or -not (Test-Path (Join-Path $funcs 'funcs.h'))
if (-not $stale) {
    $made = (Get-Item $stamp).LastWriteTimeUtc
    $stale = @($inputs | Where-Object { (Get-Item $_).LastWriteTimeUtc -gt $made }).Count -gt 0
}
if ($stale) {
    Write-Host "recompiling $(Split-Path $romUsed -Leaf) with $(Split-Path $toml -Leaf)"
    Remove-Item $stamp -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $funcs 'funcs_*.c') -ErrorAction SilentlyContinue
    Push-Location $outDir
    & $rcExe $genToml
    $code = $LASTEXITCODE
    Pop-Location
    if ($code -ne 0) { throw "N64Recomp failed ($code)" }
    & $python (Join-Path $n64 'Native\tools\recomp\add_loop_checks.py') $funcs
    if ($LASTEXITCODE -ne 0) { throw 'add_loop_checks.py failed (Python 3 is needed)' }
    Set-Content -Path $stamp -Value (Get-Date -Format o)
}

# 3. The game library
$buildDir = Join-Path $Package "Native\build\recomp-$Config"
$configure = @('-S', $recompDir, '-B', $buildDir, '-G', 'Ninja', "-DCMAKE_MAKE_PROGRAM=$ninja",
    "-DCMAKE_C_COMPILER=$llvm\clang.exe", "-DCMAKE_CXX_COMPILER=$llvm\clang++.exe", "-DCMAKE_BUILD_TYPE=$Config",
    "-DN64PORT_ROOT=$(Fwd (Join-Path $n64 'Native'))", "-DN64RECOMP_GENERATED=$(Fwd $funcs)", "-DN64RECOMP_ROM=$(Fwd $romUsed)")
if ($Decomp) { $configure += "-DN64RECOMP_DECOMP_DIR=$(Fwd $Decomp)" }
& $cmake @configure | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'cmake configure failed (see the game package''s Recomp\CMakeLists.txt)' }
& $cmake --build $buildDir
if ($LASTEXITCODE -ne 0) { throw 'build failed' }

# 4. Publish under the name the game's addon links on Windows (package.json)
$libs = @(Get-ChildItem $buildDir -Filter '*_recomp.lib')
if ($libs.Count -ne 1) { throw "expected one <game>_recomp.lib in $buildDir" }
$manifest = Get-Content (Join-Path $Package 'package.json') -Raw | ConvertFrom-Json
$libName = @($manifest.nativePerPlatform.Windows.extraLibs)[0]
if (-not $libName) { throw 'package.json has no nativePerPlatform.Windows.extraLibs to publish as' }
$libDir = Join-Path $Package 'Lib'
New-Item -ItemType Directory -Force $libDir | Out-Null
Copy-Item $libs[0].FullName (Join-Path $libDir $libName) -Force
Set-Content -Path (Join-Path $libDir "$libName.mode") -Value 'recomp' -NoNewline
$hash = (Get-FileHash $libs[0].FullName -Algorithm SHA1).Hash
foreach ($header in @(Get-ChildItem (Join-Path $Package 'Source\Generated') -Filter '*LibStamp.h' -ErrorAction SilentlyContinue)) {
    $old = Get-Content $header.FullName -Raw
    if ($old -match '#define (\w+) "') {
        $new = "// Generated by Native/build.ps1 - changes whenever $libName changes.`n#define $($Matches[1]) `"$hash`"`n"
        if ($old -ne $new) { Set-Content -Path $header.FullName -Value $new -NoNewline }
    }
}
Write-Host "published $libName (recomp mode, from $(Split-Path $romUsed -Leaf))"

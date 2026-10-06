# Refreshes the vendored N64Recomp snapshot in ThirdParty/N64Recomp from GitHub.
#
#   .\update_n64recomp.ps1                  # latest main
#   .\update_n64recomp.ps1 -Ref <commit>    # a specific commit or tag
#
# Clones N64Recomp with its submodules into a temporary folder, copies only what its CMake
# builds (plus every license), and rewrites N64Recomp/VERSION.txt. Review the diff, rebuild,
# and update THIRD_PARTY.md if a dependency or license changed.
param([string]$Ref = 'main')
$ErrorActionPreference = 'Stop'
if (-not $PSScriptRoot) { throw 'run this script from its file (PSScriptRoot is empty)' }
$dest = Join-Path $PSScriptRoot 'N64Recomp'
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("n64recomp-" + [guid]::NewGuid().ToString('N'))
# The two folders this script deletes: never anything but these.
if ($dest -notlike '*\ThirdParty\N64Recomp') { throw "unexpected destination $dest" }
if ($tmp -notlike '*\n64recomp-*') { throw "unexpected temp folder $tmp" }

git clone --recursive https://github.com/N64Recomp/N64Recomp.git $tmp
if ($LASTEXITCODE -ne 0) { throw 'clone failed' }
git -C $tmp checkout $Ref
git -C $tmp submodule update --init --recursive
if ($LASTEXITCODE -ne 0) { throw "checkout of $Ref failed" }

# What N64Recomp's CMakeLists.txt uses (fmt's CMake also lists its README and ChangeLog).
$keep = @(
    'CMakeLists.txt', 'LICENSE', 'README.md', 'include', 'src', 'LiveRecomp', 'RSPRecomp', 'RecompModTool',
    'OfflineModRecomp', 'RecompModMerger',
    'lib/rabbitizer/LICENSE', 'lib/rabbitizer/README.md', 'lib/rabbitizer/include', 'lib/rabbitizer/cplusplus/include',
    'lib/rabbitizer/cplusplus/src', 'lib/rabbitizer/src', 'lib/rabbitizer/tables',
    'lib/fmt/CMakeLists.txt', 'lib/fmt/LICENSE', 'lib/fmt/README.md', 'lib/fmt/ChangeLog.md', 'lib/fmt/include',
    'lib/fmt/src', 'lib/fmt/support',
    'lib/tomlplusplus/CMakeLists.txt', 'lib/tomlplusplus/LICENSE', 'lib/tomlplusplus/README.md',
    'lib/tomlplusplus/include', 'lib/tomlplusplus/cmake', 'lib/tomlplusplus/toml.hpp',
    'lib/ELFIO/LICENSE.txt', 'lib/ELFIO/README.md', 'lib/ELFIO/elfio',
    'lib/sljit/LICENSE', 'lib/sljit/README', 'lib/sljit/sljit_src'
)
if (Test-Path $dest) { Remove-Item -LiteralPath $dest -Recurse -Force }
foreach ($p in $keep) {
    $from = Join-Path $tmp $p
    if (-not (Test-Path $from)) { throw "upstream no longer has $p: update the keep list" }
    $to = Join-Path $dest $p
    New-Item -ItemType Directory -Force (Split-Path $to) | Out-Null
    Copy-Item -LiteralPath $from -Destination $to -Recurse
}

$lines = @("N64Recomp vendored snapshot (trimmed). Update with ThirdParty/update_n64recomp.ps1.",
           "fetched: $(Get-Date -Format yyyy-MM-dd)",
           "N64Recomp  https://github.com/N64Recomp/N64Recomp  $(git -C $tmp rev-parse HEAD)")
foreach ($line in (git -C $tmp submodule status)) {
    $parts = $line.Trim().TrimStart('-', '+') -split '\s+'
    $url = git -C $tmp config -f .gitmodules --get "submodule.$($parts[1]).url"
    $lines += "$(Split-Path $parts[1] -Leaf)  $url  $($parts[0])"
}
Set-Content -Path (Join-Path $dest 'VERSION.txt') -Value $lines
Remove-Item -LiteralPath $tmp -Recurse -Force
Write-Host "Updated ThirdParty/N64Recomp:"
Get-Content (Join-Path $dest 'VERSION.txt')

# Makes a new N64 game package from com.recomp.n64's template (Templates/game): a data-only
# package (Recomp/ config + symbols, game.json, a few lines of Source/ naming the game) that
# Set Up Game recompiles from the player's ROM.
#
#   new_game.ps1 -Project <project folder> -Id mk64 -Title "Mario Kart 64 (US)" [-Region us] [-Name MarioKart64]
#
#   -Id      short lower-case id: the package becomes Packages/com.recomp.<id>, the library <id>.lib
#   -Title   shown to players
#   -Region  ROM region code used in file names (us, eu, jp, ...)
#   -Name    C++ name prefix (the node is <Name>Player); default: the title's letters and digits
# Called by the editor (Tools > Recomp > N64 > New Game Package).
param(
    [Parameter(Mandatory = $true)][string]$Project,
    [Parameter(Mandatory = $true)][string]$Id,
    [Parameter(Mandatory = $true)][string]$Title,
    [string]$Region = 'us',
    [string]$Name = ''
)
$ErrorActionPreference = 'Stop'
$template = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\Templates\game')).Path
$Project = (Resolve-Path $Project).Path

if ($Id -notmatch '^[a-z][a-z0-9]*$') { throw "the id must be lower-case letters and digits, starting with a letter: '$Id'" }
if ($Region -notmatch '^[a-z0-9]+$') { throw "the region must be lower-case letters and digits: '$Region'" }
if ($Title -match '["\\]') { throw 'the title cannot contain quotes or backslashes' }
if (-not $Name) {
    $words = [regex]::Matches($Title, '[A-Za-z0-9]+') | ForEach-Object { $_.Value.Substring(0, 1).ToUpper() + $_.Value.Substring(1) }
    $Name = -join $words
}
if ($Name -notmatch '^[A-Za-z_][A-Za-z0-9_]*$') { $Name = 'Game' + ($Name -replace '[^A-Za-z0-9_]', '') }

$package = "com.recomp.$Id"
$target = Join-Path $Project "Packages\$package"
if (Test-Path $target) { throw "$target already exists" }

$tokens = [ordered]@{
    '{{PACKAGE}}' = $package
    '{{ENTRY}}'   = 'PolyphasePlugin_GetDesc_' + ($package -replace '[^A-Za-z0-9_]', '_')
    '{{STAMP}}'   = ($Id.ToUpper() + '_LIB_STAMP')
    '{{TITLE}}'   = $Title
    '{{NAME}}'    = $Name
    '{{REGION}}'  = $Region
    '{{ID}}'      = $Id
}
function Fill([string]$text) {
    foreach ($key in $tokens.Keys) { $text = $text.Replace($key, $tokens[$key]) }
    return $text
}

$utf8 = New-Object System.Text.UTF8Encoding($false)
foreach ($file in Get-ChildItem $template -Recurse -File -Force) {
    $relative = Fill ($file.FullName.Substring($template.Length + 1))
    $out = Join-Path $target $relative
    New-Item -ItemType Directory -Force (Split-Path $out -Parent) | Out-Null
    [System.IO.File]::WriteAllText($out, (Fill ([System.IO.File]::ReadAllText($file.FullName))), $utf8)
}
Write-Host "made $package ($Title): Packages\$package"
Write-Host "next: put the game's symbols in Recomp\$Id.$Region.syms.toml, then Tools > Recomp > N64 > Set Up Game"

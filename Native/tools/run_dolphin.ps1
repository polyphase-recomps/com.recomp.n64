# Run a standalone console build (host/main_ogc.c) in Dolphin with a private user folder and
# print what it logged. Only the Dolphin process started here is ever stopped.
#
#   .\run_dolphin.ps1 -Elf build\Wii\ssb64_host.elf -Pack ..\Assets\ssb64.n64pak -Frames 300
#
# The user folder holds the SD card sync folder (sdsync\n64port\game.n64pak, frames.txt), the
# log (Logs\dolphin.log) and frame dumps (Dump\Frames) when -DumpFrames is given.
param(
    [Parameter(Mandatory = $true)][string]$Elf,
    [string]$Pack = '',
    [int]$Frames = 300,
    [int]$DumpEvery = 0,      # write the picture to the log every N frames (see dolphin_frames.py)
    [int]$Fuzz = 0,           # random input seed (same sequence as host/main.c --fuzz)
    [int]$VerboseFrom = 0,    # log every frame and backend detail from this frame on
    [int]$TimeoutSec = 180,
    [string]$UserDir = 'P:\temp\dolphin_n64port',
    [string]$Dolphin = "$env:USERPROFILE\Documents\Dolphin-x64\Dolphin.exe",
    [switch]$DumpFrames
)
$ErrorActionPreference = 'Stop'
$Elf = (Resolve-Path $Elf).Path
$cfg = Join-Path $UserDir 'Config'
$sd = Join-Path $UserDir 'sdsync\n64port'
New-Item -ItemType Directory -Force $cfg, $sd, (Join-Path $UserDir 'Wii') | Out-Null
if ($Pack) {
    $dst = Join-Path $sd 'game.n64pak'
    $src = Get-Item (Resolve-Path $Pack).Path
    if (-not (Test-Path $dst) -or (Get-Item $dst).Length -ne $src.Length -or (Get-Item $dst).LastWriteTime -lt $src.LastWriteTime) {
        Copy-Item $src.FullName $dst -Force
    }
}
Set-Content (Join-Path $sd 'frames.txt') $Frames
Set-Content (Join-Path $sd 'dump.txt') $DumpEvery
Set-Content (Join-Path $sd 'fuzz.txt') $Fuzz
Set-Content (Join-Path $sd 'verbose.txt') $VerboseFrom
$u = $UserDir -replace '\\', '/'
$dump = if ($DumpFrames) { 'True' } else { 'False' }
Set-Content (Join-Path $cfg 'Dolphin.ini') @"
[General]
WiiSDCardPath = $u/Wii/sd.raw
WiiSDCardSyncFolder = $u/sdsync/
[Core]
WiiSDCard = True
WiiSDCardEnableFolderSync = True
WiiSDCardAllowWrites = True
[Interface]
ConfirmStop = False
UsePanicHandlers = False
OnScreenDisplayMessages = False
[Analytics]
PermissionAsked = True
Enabled = False
[AutoUpdate]
UpdateTrack =
[Movie]
DumpFrames = $dump
DumpFramesSilent = True
[DSP]
Backend = No Audio Output
"@
Set-Content (Join-Path $cfg 'Logger.ini') @"
[Options]
Verbosity = 4
WriteToFile = True
WriteToConsole = False
WriteToWindow = False
[Logs]
OSREPORT = True
OSREPORT_HLE = True
"@
Set-Content (Join-Path $cfg 'GFX.ini') @"
[Settings]
DumpFramesAsImages = True
InternalResolutionFrameDumps = False
[Hardware]
VSync = False
[Hacks]
EFBToTextureEnable = False
EFBAccessEnable = True
"@
$log = Join-Path $UserDir 'Logs\dolphin.log'
Remove-Item $log -ErrorAction SilentlyContinue
Remove-Item (Join-Path $UserDir 'Dump\Frames\*') -Recurse -ErrorAction SilentlyContinue

$proc = Start-Process -FilePath $Dolphin -ArgumentList @('-u', "`"$UserDir`"", '-b', '-e', "`"$Elf`"") -PassThru
$deadline = (Get-Date).AddSeconds($TimeoutSec)
while ((Get-Date) -lt $deadline -and -not $proc.HasExited) {
    Start-Sleep -Seconds 2
    if ((Test-Path $log) -and (Select-String -Path $log -Pattern 'n64port: stopped|n64port: could not boot|Exception \(' -Quiet)) {
        Start-Sleep -Seconds 2
        break
    }
}
if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
if (Test-Path $log) {
    Get-Content $log | Where-Object { $_ -notmatch 'FRAME \d+ \d+ \d+ ' } | ForEach-Object { $_ -replace '^.*?N\[OSREPORT[^\]]*\]: ', '' }
} else {
    'no log written'
}

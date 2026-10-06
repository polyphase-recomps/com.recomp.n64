# Run the standalone 3DS runner (host/main_ctr.c) in Azahar and photograph its window.
#
#   .\run_azahar.ps1 -Dsx ..\..\..\com.recomp.ssb64\Native\build\3DS-gpu\ssb64_host.3dsx -Pack <ssb64.n64pak>
#        [-Frames 3000] [-Fuzz 33] [-ShotEvery 8] [-TimeoutSec 300] [-Out P:\temp\azahar]
#
# Uses the user's Azahar install and its emulated SD card (%APPDATA%\Azahar\sdmc): the runner,
# the pack and run.txt go to sdmc:/n64port; the log is sdmc:/n64port/log.txt. Azahar's own
# settings are not touched. Only the Azahar process started here is stopped.
# Frames read back on the 3DS side are not reliable under Azahar, so pictures are taken of the
# emulator window instead (shot_NN.png every -ShotEvery seconds).
param(
    [Parameter(Mandatory)][string]$Dsx,
    [string]$Pack = '',
    [int]$Frames = 3000,
    [int]$Fuzz = 0,
    [int]$ShotEvery = 8,
    [int]$TimeoutSec = 300,
    [string]$Extra = '',
    [string]$Out = 'P:\temp\azahar'
)
$ErrorActionPreference = 'Stop'
$sd = Join-Path $env:APPDATA 'Azahar\sdmc\n64port'
$exe = 'C:\Program Files\Azahar\azahar.exe'
New-Item -ItemType Directory -Force $sd, $Out | Out-Null
Copy-Item $Dsx (Join-Path $sd 'runner.3dsx') -Force
if ($Pack) { Copy-Item $Pack (Join-Path $sd 'ssb64.n64pak') -Force }
$cfg = @("frames=$Frames")
if ($Fuzz) { $cfg += "fuzz=$Fuzz" }
if ($Extra) { $cfg += $Extra -split ';' }
Set-Content (Join-Path $sd 'run.txt') $cfg

Add-Type -AssemblyName System.Drawing
if (-not ('AzWin' -as [type])) {
    Add-Type @"
using System; using System.Runtime.InteropServices;
public class AzWin { [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
public struct RECT { public int Left, Top, Right, Bottom; } }
"@
}
function Shot($p, $path) {
    $p.Refresh()
    $r = New-Object AzWin+RECT
    if (-not [AzWin]::GetWindowRect($p.MainWindowHandle, [ref]$r)) { return }
    $bmp = New-Object System.Drawing.Bitmap ($r.Right - $r.Left), ($r.Bottom - $r.Top)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
    $bmp.Save($path)
    $g.Dispose(); $bmp.Dispose()
}

$start = Get-Date
$log = Join-Path $sd 'log.txt'
$p = Start-Process -FilePath $exe -ArgumentList "`"$(Join-Path $sd 'runner.3dsx')`"" -PassThru
$n = 0
$next = $start.AddSeconds($ShotEvery)
while (((Get-Date) - $start).TotalSeconds -lt $TimeoutSec -and -not $p.HasExited) {
    Start-Sleep -Milliseconds 500
    if ((Get-Date) -ge $next) {
        Shot $p (Join-Path $Out ('shot_{0:D2}.png' -f $n)); $n++
        $next = $next.AddSeconds($ShotEvery)
    }
    $f = Get-Item $log -ErrorAction SilentlyContinue
    if ($f -and $f.LastWriteTime -gt $start -and (Get-Content $log -Raw) -match 'stopped after|boot failed|game stopped|FATAL') { break }
}
if (-not $p.HasExited) { Stop-Process -Id $p.Id }
Get-Content $log -ErrorAction SilentlyContinue | Select-Object -Last 12
"$n screenshots in $Out"

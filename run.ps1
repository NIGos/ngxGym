# ngxhost -- stage a run folder and launch one scenario in a fresh process.
#
# Fresh process per scenario is load-bearing, not tidiness. The add-on is full of
# one-way process-lifetime latches -- a refusal that never clears, "said once" log
# guards, a readiness flag that only goes false -- so two scenarios in one process
# are not two independent results.
#
# Everything the run needs is copied into run\<name>\ so nothing reaches back into a
# game folder or the repo. The add-on writes its log beside its own module, which is
# why the log lands in the run folder and not somewhere shared.
#
#   .\run.ps1 -Scenario phase0

[CmdletBinding()]
param(
    [string] $Scenario = 'phase0',
    [int]    $Frames   = 300,
    # 310.7.129.0, which has the DLAA enum member. Point this at Red Dead
    # Redemption 2's copy to stage 2.2.10.0, which does not -- that is the whole of
    # the "old snippet" scenario, a file copy rather than a feature.
    [string] $Snippet  = 'D:\SteamLibrary\steamapps\common\Baldurs Gate 3\bin\nvngx_dlss.dll',
    [switch] $NoSnippet
)

$ErrorActionPreference = 'Stop'
$root   = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe    = Join-Path $root 'bin\ngxhost.exe'
$addon  = 'C:\Users\quali\Documents\Code Projects\Misc\ngxbridge\dlss5-bridge.addon64'
$shade  = 'C:\ProgramData\ReShade\ReShade64.dll'

foreach ($p in @($exe, $addon, $shade)) {
    if (-not (Test-Path $p)) { Write-Error "missing: $p"; exit 2 }
}

$run = Join-Path $root "run\$Scenario"
if (Test-Path $run) { Remove-Item -Recurse -Force $run }
New-Item -ItemType Directory -Force $run | Out-Null

Copy-Item $exe   $run
Copy-Item $addon $run
# Named d3d11.dll, because that is the proxy ReShade needs to wrap the device a
# D3D11 add-on subscribes to. If phase 0 registers but sees no device events, try
# dxgi.dll instead -- one line here, not a redesign.
Copy-Item $shade (Join-Path $run 'd3d11.dll')
if (-not $NoSnippet) {
    if (Test-Path $Snippet) { Copy-Item $Snippet (Join-Path $run 'nvngx_dlss.dll') }
    else { Write-Warning "snippet not found, running without one: $Snippet" }
    # The neural-rendering model, which is a different file from the
    # super-resolution snippet and is what the DLSS 5 add-on loads. Phase 0 does not
    # need it, but the add-on says so on every run and a warning nobody can act on
    # trains people to ignore warnings.
    $nr = Join-Path (Split-Path -Parent $Snippet) 'nvngx_dlssnr.dll'
    if (Test-Path $nr) { Copy-Item $nr $run }
}

# The consumer under test. Absent, the bridge runs and delivers to nobody, which is
# a valid phase-0 result and a useless phase-1 one.
$dlss5 = Join-Path (Split-Path -Parent $Snippet) 'renodx-dlss5.addon64'
if (Test-Path $dlss5) { Copy-Item $dlss5 $run }
else { Write-Warning "no renodx-dlss5.addon64 beside the snippet: the bridge will mirror to nobody." }

# ReShade needs to be told where add-ons live and not to show a tutorial to nobody.
@"
[GENERAL]
PresetPath=.\default.ini
[ADDON]
AddonPath=.
[OVERLAY]
TutorialProgress=4
"@ | Set-Content -Path (Join-Path $run 'ReShade.ini') -Encoding ASCII

@"
vk_mirror=1
synth_after=0
source=auto
stage=3
mode=2
skip_game=1
flags=-1
"@ | Set-Content -Path (Join-Path $run 'dlss5-bridge.cfg') -Encoding ASCII

$log = Join-Path $run 'dlss5-bridge.log'
Write-Host "run folder: $run"

$p = Start-Process -FilePath (Join-Path $run 'ngxhost.exe') -ArgumentList $Frames `
                   -WorkingDirectory $run -PassThru -Wait -NoNewWindow
Write-Host "host exit: $($p.ExitCode)"

# The verdict for phase 0, and only for phase 0: did the add-on attach at all.
if (-not (Test-Path $log)) {
    Write-Host "FAIL: no dlss5-bridge.log -- ReShade did not load, or DllMain returned FALSE." -ForegroundColor Red
    exit 1
}
$txt = Get-Content $log -Raw
$m = [regex]::Match($txt, 'registered for ReShade effect events at add-on API (\d+)')
if (-not $m.Success) {
    Write-Host "FAIL: the add-on wrote a log but never registered with ReShade." -ForegroundColor Red
    Get-Content $log -TotalCount 25
    exit 1
}
$api = [int]$m.Groups[1].Value
Write-Host "PASS: add-on registered at ReShade add-on API $api" -ForegroundColor Green
if ($api -lt 10) {
    Write-Host "  but below the synth floor of 10, so the synthetic path is unreachable here." -ForegroundColor Yellow
}
Get-Content $log -TotalCount 12

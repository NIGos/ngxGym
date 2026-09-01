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
    [switch] $NoSnippet,
    # Run with no ReShade effects enabled, so reshade_begin_effects never ticks.
    [switch] $NoEffects
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
# Clear the CONTENTS rather than the folder. Removing the directory itself fails
# whenever anything holds a handle on it -- a shell sitting in it, an explorer
# window -- and a stale lock is not a reason to refuse to run a test.
if (Test-Path $run) {
    Get-ChildItem -Path $run -Force -Recurse | Remove-Item -Force -Recurse -ErrorAction SilentlyContinue
}
New-Item -ItemType Directory -Force $run | Out-Null
$stale = Get-ChildItem -Path $run -Force -ErrorAction SilentlyContinue
if ($stale) { Write-Warning "run folder not fully cleared: $($stale.Count) item(s) left behind" }

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

# ReShade needs to be told where add-ons and effects live, and not to show a
# tutorial to nobody.
#
# The effect matters more than it looks. reshade_begin_effects only fires when
# ReShade actually runs something, and the add-on's source-latch release needs
# thirty of those ticks -- so a run folder with no effects enabled can never
# release the latch, whatever the game does. -NoEffects reproduces that state
# deliberately; the default reproduces a normal user's.
if (-not $NoEffects) {
    Copy-Item (Join-Path $root 'reshade-fx') (Join-Path $run 'fx') -Recurse
    $effectLines = "EffectSearchPaths=.\fx`nTextureSearchPaths=.\fx"
    $presetBody  = "[ngxhost_probe.fx]`n`nTechniques=ngxhost_probe@ngxhost_probe.fx`n"
} else {
    $effectLines = ""
    $presetBody  = "Techniques=`n"
}
$presetBody | Set-Content -Path (Join-Path $run 'default.ini') -Encoding ASCII

@"
[GENERAL]
PresetPath=.\default.ini
$effectLines
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

# A scenario file if one exists by that name, otherwise a plain frame count. The
# file is copied in so the run folder is self-contained and a scenario edited
# mid-suite cannot change what a finished run did.
$scfile = Join-Path $root "scenarios\$Scenario.txt"
if (Test-Path $scfile) {
    Copy-Item $scfile $run
    $hostArg = "$Scenario.txt"
    Write-Host "scenario: $Scenario.txt"
} else {
    $hostArg = "$Frames"
}

# Redirected to files, not inherited. A crash inside a loaded DLL can take the
# process down before anything reaches a console, and "host exit: -1073740791" with
# no output is not a bug report. host.out survives the process either way.
$outf = Join-Path $run 'host.out'
$errf = Join-Path $run 'host.err'
$p = Start-Process -FilePath (Join-Path $run 'ngxhost.exe') -ArgumentList $hostArg `
                   -WorkingDirectory $run -PassThru -Wait -NoNewWindow `
                   -RedirectStandardOutput $outf -RedirectStandardError $errf
if (Test-Path $outf) { Get-Content $outf }
Write-Host "host exit: $($p.ExitCode)"
if ($p.ExitCode -ne 0) {
    $hex = '0x{0:X8}' -f ([int64]$p.ExitCode -band 0xFFFFFFFFL)
    Write-Host "FAIL: the host exited $($p.ExitCode) ($hex)." -ForegroundColor Red
    if ((Test-Path $errf) -and (Get-Item $errf).Length -gt 0) { Get-Content $errf }
    Write-Host "  host.out and dlss5-bridge.log are in $run" -ForegroundColor Yellow
    exit 1
}

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
Write-Host "registered at ReShade add-on API $api" -ForegroundColor Green
if ($api -lt 10) {
    Write-Host "  below the synth floor of 10, so the synthetic path is unreachable here." -ForegroundColor Yellow
}

# The verdict proper. Registration only proves the stage is up; what matters is
# whether the bridge built its own feature and delivered a frame. Structure, not
# wording: a count and a shape, so rephrasing a log line does not fail a run.
$ready = [regex]::Matches($txt, 'feature ready: render (\d+)x(\d+) -> output (\d+)x(\d+)')
$deliv = [regex]::Matches($txt, 'frame (\d+) delivered')
$stood = [regex]::Match($txt, 'does nothing for the rest of this session|disabled\. Game rendering is untouched')

if ($stood.Success) {
    Write-Host "FAIL: the bridge stood down. The line above it in the log says why." -ForegroundColor Red
    ($txt -split "`n" | Select-String -Pattern 'does nothing for the rest|disabled\.' -Context 2,0) | Select-Object -First 1
    exit 1
}
if ($ready.Count -eq 0) {
    Write-Host "FAIL: the bridge never built a feature. It attached and did nothing." -ForegroundColor Red
    exit 1
}
if ($deliv.Count -eq 0) {
    Write-Host "FAIL: a feature was built but no frame was ever delivered." -ForegroundColor Red
    exit 1
}

# One "feature ready" per shape the host presented, and the shapes themselves. NOT a
# delivered-frame count: that line is periodic in the add-on -- every 1800 frames --
# so a scenario in 300-frame segments prints it once and a verdict keyed on it reads
# "1" however well the run went. The first version of this check did exactly that.
Write-Host ("PASS: {0} feature build(s), no stand-down" -f $ready.Count) -ForegroundColor Green
foreach ($m in $ready) {
    Write-Host ("       {0}x{1} -> {2}x{3}" -f `
        $m.Groups[1].Value, $m.Groups[2].Value, $m.Groups[3].Value, $m.Groups[4].Value)
}

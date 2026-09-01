# ngxGym -- stage a run folder and launch one scenario in a fresh process.
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
    # Divide every 'frames N' in the scenario by this. See the block below.
    [int]    $Scale = 1,
    [string] $Snippet  = 'D:\SteamLibrary\steamapps\common\Baldurs Gate 3\bin\nvngx_dlss.dll',
    [switch] $NoSnippet,
    # Run with no ReShade effects enabled, so reshade_begin_effects never ticks.
    [switch] $NoEffects
)

$ErrorActionPreference = 'Stop'
$root   = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe    = Join-Path $root 'bin\ngxGym.exe'
$addon  = 'C:\Users\quali\Documents\Code Projects\Misc\ngxbridge\dlss5-bridge.addon64'
$shade  = 'C:\ProgramData\ReShade\ReShade64.dll'

foreach ($p in @($exe, $addon, $shade)) {
    if (-not (Test-Path $p)) { Write-Error "missing: $p"; exit 2 }
}

$run = Join-Path $root "run\$Scenario"
# Clear the CONTENTS rather than the folder. Removing the directory itself fails
# whenever anything holds a handle on it -- a shell sitting in it, an explorer
# window -- so the contents go individually and a leftover is a hard failure below.
if (Test-Path $run) {
    Get-ChildItem -Path $run -Force -Recurse | Remove-Item -Force -Recurse -ErrorAction SilentlyContinue
}
New-Item -ItemType Directory -Force $run | Out-Null
$stale = Get-ChildItem -Path $run -Force -ErrorAction SilentlyContinue
if ($stale) {
    # A hard failure, and exit 2 like the missing-prerequisite gate above, so a
    # suite loop can tell "your machine is wrong" from "the add-on is wrong". This
    # was a warning, under a comment saying a stale lock is not a reason to refuse
    # to run a test -- which is exactly backwards for a harness whose failure mode
    # is proving nothing. A locked dlss5-bridge.cfg silently runs the PREVIOUS
    # run stage, mode and flags.
    Write-Host "FAIL: run folder not cleared, $($stale.Count) item(s) locked. Close any tail or editor on $run." -ForegroundColor Red
    exit 2
}

Copy-Item $exe   $run
Copy-Item $addon $run
# Named d3d11.dll, because that is the proxy ReShade needs to wrap the device a
# D3D11 add-on subscribes to. If phase 0 registers but sees no device events, try
# dxgi.dll instead -- one line here, not a redesign.
Copy-Item $shade (Join-Path $run 'd3d11.dll')
if (-not $NoSnippet) {
    # Resolved once and guarded. Split-Path throws on a path whose drive does not
    # exist, so pointing -Snippet at a deliberately absent file -- which is how you
    # test what the add-on does with no super-resolution snippet at all -- used to
    # kill the runner instead of staging one file short.
    # Both halves guarded: Split-Path is happy to hand back a parent for a drive
    # that does not exist, and Join-Path then throws on it.
    $snipDir = $null
    try { $snipDir = Split-Path -Parent $Snippet -EA Stop } catch { $snipDir = $null }
    if ($snipDir) {
        $ok = $false
        try { $ok = Test-Path -LiteralPath $snipDir -EA Stop } catch { $ok = $false }
        if (-not $ok) { $snipDir = $null }
    }
    if (Test-Path $Snippet) { Copy-Item $Snippet (Join-Path $run 'nvngx_dlss.dll') }
    else { Write-Warning "snippet not found, running without one: $Snippet" }
    # The neural-rendering model, which is a different file from the
    # super-resolution snippet and is what the DLSS 5 add-on loads. Phase 0 does not
    # need it, but the add-on says so on every run and a warning nobody can act on
    # trains people to ignore warnings.
    if ($snipDir) {
        $nr = Join-Path $snipDir 'nvngx_dlssnr.dll'
        if (Test-Path $nr) { Copy-Item $nr $run }
    }
}

# The consumer under test. Absent, the bridge runs and delivers to nobody, which is
# a valid phase-0 result and a useless phase-1 one.
$dlss5 = if ($snipDir) { Join-Path $snipDir 'renodx-dlss5.addon64' } else { $null }
if ($dlss5 -and (Test-Path $dlss5)) { Copy-Item $dlss5 $run }
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
    $presetBody  = "[ngxGym_probe.fx]`n`nTechniques=ngxGym_probe@ngxGym_probe.fx`n"
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
# A scenario may state configuration of its own with "# cfg: key=value" lines.
# Appended after the block written above, so they win: the add-on's parser takes
# the last value for a key. It exists because gating a behaviour behind a key
# would otherwise make the scenario that proves the behaviour untestable --
# dlss-off needs synth=1 to reach the source-latch release, and that key is off
# by default on purpose.
if ($Scenario -and (Test-Path $scfile)) {
    $extra = @(Select-String -Path $scfile -Pattern '^#\s*cfg:\s*(.+)$' |
               ForEach-Object { $_.Matches[0].Groups[1].Value.Trim() })
    if ($extra.Count -gt 0) {
        Add-Content -Path (Join-Path $run 'dlss5-bridge.cfg') -Value $extra -Encoding ASCII
        Write-Host ("scenario config: " + ($extra -join ', '))
    }
}
# Fast mode. Frames are what the suite spends its wall clock on: 20,950 of them
# per backend, and omissions alone is 9,500. Nothing in a contract check needs
# that many -- the counts are large so a real title has time to settle, and this
# host settles in tens of frames.
#
# So -Scale divides every "frames N" in the COPIED scenario, never the original,
# with a floor that keeps each step long enough to build a feature and deliver.
# A scenario whose behaviour is driven by the WALL CLOCK rather than by steps
# opts out with a "# nofast" line, because scaling it does not shorten the test,
# it breaks it: dlss-off waits five seconds and thirty presents for the source
# latch to be released, and a quarter of the frames is a quarter of the seconds.
#
# The scale is printed on every run that uses it. A fast run must never be
# mistaken for a full one in a log somebody reads later.
if ($Scale -gt 1 -and $Scenario -and (Test-Path $scfile)) {
    $staged = Join-Path $run "$Scenario.txt"
    if (Select-String -Path $scfile -Pattern '^#\s*nofast\b' -Quiet) {
        Write-Host "scale: $Scenario opts out with # nofast, running it in full" -ForegroundColor DarkGray
    } else {
        $kept = 0
        $cut  = 0
        $out = Get-Content $staged | ForEach-Object {
            if ($_ -match '^\s*frames\s+(\d+)\s*$') {
                $was = [int]$Matches[1]
                # Never longer than it was. exclusive's steps are 50 frames each,
                # and a bare floor of 120 turned a 150-frame scenario into 360.
                $now = [Math]::Min($was, [Math]::Max(120, [int][Math]::Floor($was / $Scale)))
                $kept += $now; $cut += $was
                "frames $now"
            } else { $_ }
        }
        Set-Content -Path $staged -Value $out -Encoding ASCII
        Write-Host ("scale /{0}: {1} frames instead of {2}" -f $Scale, $kept, $cut) -ForegroundColor DarkGray
    }
}



# Redirected to files, not inherited. A crash inside a loaded DLL can take the
# process down before anything reaches a console, and "host exit: -1073740791" with
# no output is not a bug report. host.out survives the process either way.
$outf = Join-Path $run 'host.out'
$errf = Join-Path $run 'host.err'
$p = Start-Process -FilePath (Join-Path $run 'ngxGym.exe') -ArgumentList $hostArg `
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

# A recorded crash is a FAIL, and the exit code does not cover it: the add-on's own
# handler catches the fault and the host still returns 0. Three run folders on this
# disk held "### CRASH RECORDED ###" under a printed PASS before this existed.
if ($txt -match '### CRASH RECORDED ###') {
    Write-Host "FAIL: the add-on recorded a crash." -ForegroundColor Red
    ($txt -split "`n" | Select-String -Pattern 'CRASH RECORDED' -Context 0,5) | Select-Object -First 1
    exit 1
}
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
# What the D3D11 bridge ACTUALLY prints when it gives up. The previous pattern
# matched zero lines across ten stored logs while two of them contained
# 'stopped: the DLSS feature could not be created.' -- so 'no stand-down' was never
# a check, it was a sentence. The Vulkan alternative is kept for run-vk.ps1's sake.
$stood = [regex]::Match($txt, 'stopped: .+The game renders normally|does nothing for the rest of this session')

if ($stood.Success) {
    Write-Host "FAIL: the bridge stood down. The line above it in the log says why." -ForegroundColor Red
    ($txt -split "`n" | Select-String -Pattern 'stopped: |does nothing for the rest' -Context 2,0) | Select-Object -First 1
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

# Did the mirror keep going? Every gate above is satisfied by the bridge starting up
# during the leading "frames" segment that every scenario opens with -- so an add-on
# that delivered nothing from the first mode change onward passed all seven. The
# add-on logs a cumulative mirrored counter every 600 frames; if it has not moved
# between the first and last, nothing was mirrored after the opening segment.
# Scenarios shorter than ~1200 frames produce fewer than two of these lines and stay
# covered by the exit-code and crash gates only.
$mir = [regex]::Matches($txt, 'd3d12 (\d+)/\d+ \(')
if ($mir.Count -ge 2 -and
    [int]$mir[$mir.Count-1].Groups[1].Value -le [int]$mir[0].Groups[1].Value) {
    Write-Host "FAIL: the mirror stopped advancing ($($mir[0].Groups[1].Value) -> $($mir[$mir.Count-1].Groups[1].Value) over $($mir.Count) samples)." -ForegroundColor Red
    exit 1
}

# What this particular scenario says its own log must contain. Opt-in: a scenario
# with no "# expect:" line is unaffected, and a scenario that has one states the
# thing it exists to prove instead of leaving it to a reader.
if (Test-Path $scfile) {
    # Both spellings: a shared expectation and this backend's own. The two logs
    # word the same event differently, so a single list would either be so vague
    # it proves nothing or fail on the other half.
    foreach ($m in (Select-String -Path $scfile -Pattern '^#\s*expect(-d3d11)?:\s*(.+)$')) {
        $e = $m.Matches[0].Groups[2].Value.Trim()
        if ($txt -notmatch [regex]::Escape($e)) {
            Write-Host "FAIL: the scenario expects '$e' in the log and it is not there." -ForegroundColor Red
            exit 1
        }
        Write-Host "  expect ok: $e" -ForegroundColor DarkGray
    }
}

Write-Host ("PASS: {0} feature build(s), no stand-down" -f $ready.Count) -ForegroundColor Green
foreach ($m in $ready) {
    Write-Host ("       {0}x{1} -> {2}x{3}" -f `
        $m.Groups[1].Value, $m.Groups[2].Value, $m.Groups[3].Value, $m.Groups[4].Value)
}

# Explicitly, and this is not decoration. Falling off the end leaves $LASTEXITCODE
# holding whatever a previous command left, so a caller looping over scenarios reads
# a stale value and every run looks like the last one's outcome. A test runner whose
# success cannot be detected is not a runner.
# What the DLSS 5 add-on did with the contract. The bridge's own verdict proves
# it BUILT one and delivered frames; it cannot prove anybody consumed them, and
# "the bridge is fine and the picture is unchanged" is the report that costs the
# most time to triage.
#
# renodx writes its own lines into ReShade.log, so they are read from there.
# Attaching and evaluating at least once is the gate. The workset pool running
# out afterwards is NOT: it happens on every run of every scenario on both
# backends, and it happens identically under a 1.3.0 bridge built from its own
# tag -- measured 2026-09-01 by staging that binary into a run folder. It is the
# neighbour add-on's own state and this suite must not report it as a bridge
# defect. It is counted and printed so a change in it is visible.
$rs = Join-Path $run 'ReShade.log'
if (Test-Path $rs) {
    $rt = Get-Content $rs -Raw
    if ($rt -match 'DLSS5 Generic') {
        $made = ([regex]::Matches($rt, 'feature 18 created')).Count
        $ran  = ([regex]::Matches($rt, 'inline feature 18 evaluation succeeded')).Count
        $dry  = ([regex]::Matches($rt, 'NR workset pool exhausted')).Count
        if ($made -eq 0) {
            Write-Host "FAIL: the DLSS 5 add-on loaded and never created its NR feature. The bridge built a contract nobody consumed." -ForegroundColor Red
            ($rt -split "`n" | Select-String -Pattern 'DLSS5 Generic' | Select-Object -Last 4)
            exit 1
        }
        if ($ran -eq 0) {
            Write-Host "FAIL: the DLSS 5 add-on created its NR feature and never evaluated it." -ForegroundColor Red
            ($rt -split "`n" | Select-String -Pattern 'DLSS5 Generic' | Select-Object -Last 4)
            exit 1
        }
        Write-Host ("NR: feature created {0}x, evaluated {1}x, workset pool exhausted {2}x" -f $made, $ran, $dry) -ForegroundColor Green
        if ($dry -gt 0) {
            Write-Host "    the exhaustion is the neighbour add-on's own and reproduces under a 1.3.0 bridge; it is counted, not failed." -ForegroundColor DarkGray
        }
    }
}

exit 0

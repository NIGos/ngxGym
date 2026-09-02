# ngxGym -- stage and run the Vulkan host.
#
# Separate from run.ps1 for one reason that is not cosmetic. On Vulkan ReShade is an
# implicit layer registered machine-wide and gated PER EXECUTABLE PATH by
# C:\ProgramData\ReShade\ReShadeApps.ini. A folder per scenario would need a
# registration per scenario, so the Vulkan host has one fixed home -- run\vk -- and
# scenarios differ only in what is passed to it.
#
# -Register appends this exe's path to that machine-wide list, once, keeping a
# timestamped backup. It is the documented mechanism and there is no other, but it
# touches a file outside this repository, so it is opt-in rather than automatic.
#
#   .\run-vk.ps1 -Register        # first time only
#   .\run-vk.ps1 -Frames 300

[CmdletBinding()]
param(
    [int]    $Frames  = 300,
    [string] $Scenario = '',
    [switch] $Register,
    [switch] $Unregister,
    # Enable VK_LAYER_KHRONOS_validation for this run. It is the only oracle this
    # half has: the add-on makes two premises about a game's images it cannot check
    # from inside -- that they carry VK_IMAGE_USAGE_TRANSFER_SRC_BIT, and that they
    # are in VK_IMAGE_LAYOUT_GENERAL at the evaluate -- and both are written into its
    # source as ASSUMED, NOT MEASURED. Validation reports either by name, instantly.
    # Do not take focus; see the block further down.
    [switch] $Background,
    [switch] $Validate,
    # Divide every 'frames N' in the scenario by this. See the block below.
    [int]    $Scale = 1,
    [string] $Snippet = 'D:\SteamLibrary\steamapps\common\Baldurs Gate 3\bin\nvngx_dlss.dll'
)

$ErrorActionPreference = 'Stop'
$root  = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe   = Join-Path $root 'bin\ngxGym-vk.exe'
$addon = 'C:\Users\quali\Documents\Code Projects\Misc\ngxbridge\dlss5-bridge.addon64'
$apps  = 'C:\ProgramData\ReShade\ReShadeApps.ini'
$run   = Join-Path $root 'run\vk'
$target = Join-Path $run 'ngxGym-vk.exe'

if (-not (Test-Path $exe)) { Write-Error "missing: $exe (build.cmd skips it without the Vulkan SDK)"; exit 2 }

function Get-Apps {
    if (-not (Test-Path $apps)) { return @() }
    $line = (Get-Content $apps -Raw) -split "`r?`n" | Where-Object { $_ -match '^\s*Apps\s*=' } | Select-Object -First 1
    if (-not $line) { return @() }
    return ($line -replace '^\s*Apps\s*=', '').Split(',') | Where-Object { $_.Trim() -ne '' }
}
function Set-Apps($list) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    Copy-Item $apps "$apps.$stamp.bak"
    Set-Content -Path $apps -Value ("Apps=" + ($list -join ',')) -Encoding UTF8
    Write-Host "ReShadeApps.ini updated, backup at $apps.$stamp.bak" -ForegroundColor Yellow
}

if ($Unregister) {
    $list = Get-Apps | Where-Object { $_ -ne $target }
    Set-Apps $list
    Write-Host "removed: $target"
    exit 0
}

if ($Register) {
    $list = Get-Apps
    if ($list -contains $target) { Write-Host "already registered: $target" }
    else { Set-Apps ($list + $target); Write-Host "registered: $target" }
}

if (Test-Path $run) {
    Get-ChildItem -Path $run -Force -Recurse | Remove-Item -Force -Recurse -ErrorAction SilentlyContinue
}
New-Item -ItemType Directory -Force $run | Out-Null

Copy-Item $exe   $run
Copy-Item $addon $run
# No d3d11.dll proxy here: on Vulkan ReShade arrives as a layer, not as a DLL beside
# the executable. If a proxy shows up in this folder something is wrong.
if (Test-Path $Snippet) {
    Copy-Item $Snippet (Join-Path $run 'nvngx_dlss.dll')
    $nr = Join-Path (Split-Path -Parent $Snippet) 'nvngx_dlssnr.dll'
    if (Test-Path $nr) { Copy-Item $nr $run }
    # Any renodx-dlss5*.addon64: a re-download lands as 'renodx-dlss5 (2).addon64'
    # and staging only the exact name silently drops the consumer, which makes the
    # NR check below pass by never running.
    $d5 = Get-ChildItem (Split-Path -Parent $Snippet) -Filter 'renodx-dlss5*.addon64' -EA SilentlyContinue | Select-Object -First 1
    if ($d5) { Copy-Item $d5.FullName (Join-Path $run 'renodx-dlss5.addon64') }
}

Copy-Item (Join-Path $root 'reshade-fx') (Join-Path $run 'fx') -Recurse
"[ngxGym_probe.fx]`n`nTechniques=ngxGym_probe@ngxGym_probe.fx`n" |
    Set-Content -Path (Join-Path $run 'default.ini') -Encoding ASCII
@"
[GENERAL]
PresetPath=.\default.ini
EffectSearchPaths=.\fx
TextureSearchPaths=.\fx
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

Write-Host "run folder: $run"
Write-Host "registered in ReShadeApps.ini: $((Get-Apps) -contains $target)"

if ($Validate) {
    $env:VK_INSTANCE_LAYERS = 'VK_LAYER_KHRONOS_validation'
    $env:VK_LOADER_LAYERS_ENABLE = 'VK_LAYER_KHRONOS_validation'
    Write-Host "validation layer enabled for this run" -ForegroundColor Cyan
} else {
    Remove-Item Env:VK_INSTANCE_LAYERS -ErrorAction SilentlyContinue
    Remove-Item Env:VK_LOADER_LAYERS_ENABLE -ErrorAction SilentlyContinue
}

$outf = Join-Path $run 'host.out'
$errf = Join-Path $run 'host.err'
$hostArg = "$Frames"
$scfile = Join-Path $root "scenarios\$Scenario.txt"
if ($Scenario -and (Test-Path $scfile)) {
    Copy-Item $scfile $run
    $hostArg = "$Scenario.txt"
    Write-Host "scenario: $Scenario.txt"
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
# The add-on replaces a settings file from another version with its defaults at
# attach, which would silently drop every "# cfg:" line above. So the seeded file
# is stamped "keep" -- unless the scenario says "# cfg-old-file", which is how the
# replacement itself is tested.
$cfgPath = Join-Path $run 'dlss5-bridge.cfg'
if (-not ($Scenario -and (Test-Path $scfile) -and (Select-String -Path $scfile -Pattern '^#\s*cfg-old-file' -Quiet))) {
    Set-Content -Path $cfgPath -Value (@('# dlss5-bridge keep') + @(Get-Content $cfgPath)) -Encoding ASCII
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


# Run without taking the screen: the window is created unactivated and sent to
# the back, so a suite can run while somebody works. Not minimised -- a
# minimised window has a 0x0 client area and the Vulkan half refuses to build a
# swapchain for one, correctly. Exclusive fullscreen is refused in this mode,
# because it takes the display whatever anybody wants.
if ($Background) { $env:NGXGYM_BACKGROUND = '1' } else { $env:NGXGYM_BACKGROUND = '0' }

$p = Start-Process -FilePath $target -ArgumentList $hostArg -WorkingDirectory $run `
                   -PassThru -Wait -NoNewWindow -RedirectStandardOutput $outf -RedirectStandardError $errf
if (Test-Path $outf) { Get-Content $outf }
Write-Host "host exit: $($p.ExitCode)"
if ($p.ExitCode -ne 0) {
    $hex = '0x{0:X8}' -f ([int64]$p.ExitCode -band 0xFFFFFFFFL)
    Write-Host "FAIL: the host exited $($p.ExitCode) ($hex)." -ForegroundColor Red
    if ((Test-Path $errf) -and (Get-Item $errf).Length -gt 0) { Get-Content $errf | Select-Object -First 20 }
    exit 1
}

$log = Join-Path $run 'dlss5-bridge.log'
if (-not (Test-Path $log)) {
    Write-Host "FAIL: no dlss5-bridge.log -- the ReShade layer did not attach to this exe." -ForegroundColor Red
    Write-Host "  Run once with -Register, and check the Apps= line in $apps" -ForegroundColor Yellow
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
    exit 1
}
Write-Host "registered at ReShade add-on API $($m.Groups[1].Value)" -ForegroundColor Green

# The verdict proper. "The layer attached" was the whole verdict until 2026-09-01,
# and it passed a run in which the mirror died at the first resize and never came
# back -- the defect this file exists to catch. Structure, not wording: counts and
# a shape, so rephrasing a log line does not fail a run.
$built = [regex]::Matches($txt, 'building the D3D12 feature and its Vulkan import for (\d+)x(\d+) -> (\d+)x(\d+)')
$recorded = [regex]::Matches($txt, 'frame \d+ recorded')
$stood = [regex]::Match($txt, 'does nothing for the rest of this session')
if ($stood.Success) {
    Write-Host "FAIL: the mirror stood down. The line it printed says why." -ForegroundColor Red
    ($txt -split "`n" | Select-String -Pattern 'does nothing for the rest' -Context 2,0) | Select-Object -First 1
    exit 1
}
# Which source held the session decides which shape the verdict takes. The
# substitute contract logs none of the mirror's lines -- no import build, no
# re-arm -- so a run it carried used to fail here as "the mirror never built a
# feature". It has its own evidence: the cumulative delivered count, which every
# source writes every 600 frames, and which the check further down requires to
# keep rising after the last runtime teardown.
$synth = $txt -match '\[synth\] arming'
$down = [regex]::Matches($txt, 'the game is destroying the effect runtime')
$rearm = [regex]::Matches($txt, 'the mirror is armed again')
if ($synth) {
    if (([regex]::Matches($txt, 'frames delivered so far')).Count -eq 0) {
        Write-Host "FAIL: the substitute contract armed and never reached 600 delivered frames." -ForegroundColor Red
        exit 1
    }
} else {
    if ($built.Count -eq 0) {
        Write-Host "FAIL: the mirror never built a D3D12 feature. It attached and did nothing." -ForegroundColor Red
        exit 1
    }
    if ($recorded.Count -eq 0) {
        Write-Host "FAIL: a feature was built but no frame was ever recorded into the game's command buffer." -ForegroundColor Red
        exit 1
    }

    # ReShade destroys and recreates its effect runtime on every swapchain change, so
    # a scenario with display steps tears the mirror down once per step. Each teardown
    # has to be followed by a build, or the mirror recovered in name only.
    if ($down.Count -gt 1 -and $rearm.Count -lt ($down.Count - 1)) {
        Write-Host ("FAIL: {0} runtime teardown(s) but only {1} re-arm(s) -- the mirror did not come back." -f $down.Count, $rearm.Count) -ForegroundColor Red
        exit 1
    }
    if ($rearm.Count -gt 0 -and $built.Count -lt ($rearm.Count + 1)) {
        Write-Host ("FAIL: {0} re-arm(s) but only {1} build(s) -- it re-armed and never rebuilt." -f $rearm.Count, $built.Count) -ForegroundColor Red
        exit 1
    }
}

# What this particular scenario says its own log must contain. Opt-in, as on the
# Did whatever was delivering keep delivering after the LAST runtime teardown?
# The mirror counter above is sampled first-to-last over the whole run, so a
# source that delivered for a thousand frames and then stopped at the first
# mode change still shows movement. The add-on logs a cumulative delivered count
# every 600 frames from every source, mirror or substitute; if the log has a
# runtime being recreated, the samples after the last recreation must rise.
# Fewer than two samples after it means the tail was too short to say, and the
# run stays covered by the gates above -- give the scenario a longer tail.
$lastRt = $txt.LastIndexOf('created an effect runtime on')
if ($lastRt -ge 0) {
    $after = [regex]::Matches($txt.Substring($lastRt), '(\d+) frames delivered so far')
    if ($after.Count -ge 2 -and
        [int]$after[$after.Count-1].Groups[1].Value -le [int]$after[0].Groups[1].Value) {
        Write-Host "FAIL: delivery stopped after the last runtime teardown ($($after[0].Groups[1].Value) -> $($after[$after.Count-1].Groups[1].Value) over $($after.Count) samples)." -ForegroundColor Red
        exit 1
    }
}

# D3D11 side: a scenario with no "# expect:" line is unaffected.
if ($Scenario -and (Test-Path $scfile)) {
    foreach ($e in (Select-String -Path $scfile -Pattern '^#\s*expect(-vk)?:\s*(.+)$')) {
        $want = $e.Matches[0].Groups[2].Value.Trim()
        if ($txt -notmatch [regex]::Escape($want)) {
            Write-Host "FAIL: the scenario expects '$want' in the log and it is not there." -ForegroundColor Red
            exit 1
        }
        Write-Host "  expect ok: $want" -ForegroundColor DarkGray
    }
    # "# expect-after: A :: B": some B after the LAST A. What "# expect:" cannot
    # say -- that a thing happened again after a teardown, not only before it.
    foreach ($m in (Select-String -Path $scfile -Pattern '^#\s*expect-after:\s*(.+?)\s*::\s*(.+)$')) {
        $a = $m.Matches[0].Groups[1].Value.Trim(); $b = $m.Matches[0].Groups[2].Value.Trim()
        $ia = $txt.LastIndexOf($a)
        if ($ia -lt 0) {
            Write-Host "FAIL: the scenario expects '$b' after '$a', and '$a' is not in the log at all." -ForegroundColor Red
            exit 1
        }
        if ($txt.IndexOf($b, $ia) -lt 0) {
            Write-Host "FAIL: the scenario expects '$b' after the last '$a', and it is not there." -ForegroundColor Red
            exit 1
        }
        Write-Host "  expect ok: $b after the last $a" -ForegroundColor DarkGray
    }
}

Write-Host ("PASS: {0} feature build(s), {1} re-arm(s) after {2} runtime teardown(s), no stand-down" -f `
            $built.Count, $rearm.Count, $down.Count) -ForegroundColor Green
foreach ($b in $built) {
    Write-Host ("       {0}x{1} -> {2}x{3}" -f `
        $b.Groups[1].Value, $b.Groups[2].Value, $b.Groups[3].Value, $b.Groups[4].Value)
}

if ($Validate) {
    # Validation writes to stderr. Reported as counted, deduplicated VUIDs rather
    # than a wall of text: the same barrier fires once per frame and a thousand
    # copies of one message is not a thousand findings.
    $v = @()
    if (Test-Path $errf) { $v += Get-Content $errf }
    if (Test-Path $outf) { $v += Get-Content $outf }
    # ONE match per line, not -AllMatches. Every validation message names its VUID
    # twice, in the header and again in the spec URL, so -AllMatches doubled every
    # count this block printed.
    $vuids = $v | Select-String -Pattern 'Validation (Error|Warning): \[ (?<v>VUID-[A-Za-z0-9-]+)' |
             ForEach-Object { $_.Matches[0].Groups['v'].Value } |
             Group-Object | Sort-Object Count -Descending
    # A VUID that is always there proves nothing, and a report that lists it beside
    # a new one buries the new one. These four were measured on 2026-09-01 and each
    # has an owner that is not a defect this suite can act on:
    #
    #   the two ReShade ones  -- present with the add-on's Vulkan work switched
    #                            off entirely (vk_mirror=0) and absent when the
    #                            ReShade layer is disabled with
    #                            VK_LOADER_LAYERS_DISABLE=VK_LAYER_reshade, which
    #                            is how they were attributed
    #   the two event ones    -- the mirror's park: a command buffer waits on an
    #                            event a worker thread host-sets after the submit.
    #                            See the header of vkmirror.inc for why no
    #                            arrangement of these calls avoids them
    #
    # Anything else is new and printed as such.
    $known = @{
        'VUID-vkGetPrivateData-objectHandle-09498'   = 'ReShade layer'
        'VUID-vkQueueSubmit-pSignalSemaphores-00067' = 'ReShade layer'
        'VUID-vkSetEvent-event-09543'                = "the mirror's park, inherent"
        'VUID-vkCmdWaitEvents-srcStageMask-01158'    = "the mirror's park, inherent"
    }
    $fresh = @($vuids | Where-Object { -not $known.ContainsKey($_.Name) })
    if ($fresh.Count -gt 0) {
        Write-Host "validation: $($fresh.Count) VUID(s) NOT on the known list" -ForegroundColor Red
        foreach ($g in $fresh) { Write-Host ("  {0,6}x  {1}" -f $g.Count, $g.Name) -ForegroundColor Red }
    } elseif ($vuids) {
        Write-Host "validation: nothing new; $($vuids.Count) known VUID(s)" -ForegroundColor Green
        foreach ($g in $vuids) { Write-Host ("  {0,6}x  {1}  ({2})" -f $g.Count, $g.Name, $known[$g.Name]) }
    } else {
        Write-Host "validation: clean -- no VUID reported" -ForegroundColor Green
    }
}

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

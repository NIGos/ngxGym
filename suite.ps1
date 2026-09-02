# ngxGym -- run every scenario on both backends and print one table.
#
#   .\suite.ps1                 both backends, every scenario
#   .\suite.ps1 -Only vk        one backend
#   .\suite.ps1 -Repeat 4       four times each, for the intermittent ones
#   .\suite.ps1 -Validate       Vulkan runs also enable the Khronos layer
#
# The exit code is the verdict: 0 only if every run passed. A green table and a
# non-zero exit would be the same mistake the per-scenario runners already made
# once, where a caller read a stale $LASTEXITCODE.
[CmdletBinding()]
param(
    [ValidateSet('both','d3d11','vk')] [string] $Only = 'both',
    [int]    $Frames = 120,
    # A contract-shaped pass: every scenario, every backend, a fraction of the
    # frames. For a change that is not about frame cadence or a latch, which is
    # most of them. dlss-off still runs in full -- it opts out with # nofast,
    # because its latch release is measured in seconds rather than in steps.
    # Pass -Background through to every run: no window takes focus.
    [switch] $Background,
    [switch] $Fast,
    [int]    $Repeat = 1,
    [switch] $Validate
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$scenarios = Get-ChildItem (Join-Path $root 'scenarios') -Filter *.txt |
             Sort-Object Name | ForEach-Object { $_.BaseName }

$backends = @()
if ($Only -ne 'vk')    { $backends += 'd3d11' }
if ($Only -ne 'd3d11') { $backends += 'vk' }

$rows = @()
foreach ($b in $backends) {
    $runner = Join-Path $root ($b -eq 'vk' ? 'run-vk.ps1' : 'run.ps1')
    foreach ($s in $scenarios) {
        # "# d3d11-only": a scenario using a verb the Vulkan host does not have.
        if ($b -eq 'vk' -and (Select-String -Path (Join-Path $root "scenarios/$s.txt") -Pattern '^#\s*d3d11-only' -Quiet)) { continue }
        if ($b -eq 'd3d11' -and (Select-String -Path (Join-Path $root "scenarios/$s.txt") -Pattern '^#\s*vk-only' -Quiet)) { continue }
        $pass = 0; $fail = 0; $why = ''
        for ($i = 0; $i -lt $Repeat; ++$i) {
            $args = @{ Frames = $Frames; Scenario = $s }
            if ($Fast) { $args['Scale'] = 8 }
            if ($Background) { $args['Background'] = $true }
            if ($Validate -and $b -eq 'vk') { $args['Validate'] = $true }
            # A runner that dies of a script error never reaches its own exit, and
            # $LASTEXITCODE would still hold the previous runner's 0. Preset it, and
            # treat an exception as the failure it is.
            $global:LASTEXITCODE = -1
            try { $out = & $runner @args *>&1 } catch { $out = "runner error: $_"; $global:LASTEXITCODE = -1 }
            if ($LASTEXITCODE -eq 0) { $pass++ }
            else {
                $fail++
                if ($why -eq '') {
                    $line = $out | Select-String 'FAIL:' | Select-Object -First 1
                    if ($line) { $why = ($line.ToString() -replace '^\s*FAIL:\s*','').Trim() }
                }
            }
        }
        # A step the host refused is not a failure, and it is not nothing either.
        if ($fail -eq 0) { $nskip = @($out | Select-String 'SKIP:').Count; if ($nskip -gt 0) { $why = "$nskip step(s) skipped" } }
        $rows += [pscustomobject]@{ Backend = $b; Scenario = $s; Pass = $pass; Fail = $fail; Why = $why }
        $tag = $fail -eq 0 ? 'ok  ' : 'FAIL'
        $colour = $fail -eq 0 ? 'Green' : 'Red'
        Write-Host ('{0}  {1,-6} {2,-10} {3}/{4}  {5}' -f $tag, $b, $s, $pass, ($pass + $fail), $why) -ForegroundColor $colour
    }
}

Write-Host ''
$rows | Format-Table Backend, Scenario, Pass, Fail -AutoSize
# The consumer check, once, on D3D11. Two runs of one scenario that differ only
# in whether the DLSS 5 add-on is staged: the bridge's own output hash must
# differ too. It is the only check here that measures a NEIGHBOUR rather than
# this project, and it is the one that would have caught the 2026-09-01 report
# in a minute instead of an afternoon.
if ($Only -ne 'vk') {
    Write-Host ''
    Write-Host 'consumer check: does the DLSS 5 add-on change the picture?'
    $hashes = @{}
    # Three legs: without, without again, with. Two without-consumer hashes that
    # differ mean the instrument is not stable and "different" proves nothing.
    foreach ($with in @($false, 'again', $true)) {
        $a = @{ Scenario = 'consumer'; Frames = $Frames }
        if ($Background) { $a['Background'] = $true }
        if ($with -ne $true) { $a['NoConsumer'] = $true }
        $global:LASTEXITCODE = -1
        try { & (Join-Path $root 'run.ps1') @a *>&1 | Out-Null } catch { $global:LASTEXITCODE = -1 }
        if ($LASTEXITCODE -ne 0) { Write-Host ("  FAIL: the {0} run exited {1}." -f ($with -eq $true ? 'with-consumer' : 'without-consumer'), $LASTEXITCODE) -ForegroundColor Red; $rows += [pscustomobject]@{ Backend='d3d11'; Scenario='consumer'; Pass=0; Fail=1; Why="runner exit $LASTEXITCODE" }; $hashes = $null; break }
        $rsl = Join-Path $root 'run/consumer/ReShade.log'
        if ($with -eq $true -and -not ((Test-Path $rsl) -and (Select-String -Path $rsl -Pattern 'DLSS5 Generic' -Quiet))) { Write-Host '  FAIL: the with-consumer run has no DLSS 5 add-on in it.' -ForegroundColor Red; $rows += [pscustomobject]@{ Backend='d3d11'; Scenario='consumer'; Pass=0; Fail=1; Why='consumer not staged' }; $hashes = $null; break }
        # Read from the LOG, not from the runner's output. Write-Host does not
        # go down the pipeline, and capturing it is how this check silently
        # compared two empty strings the first time it ran.
        $cl = Join-Path $root 'run/consumer/dlss5-bridge.log'
        $line = if (Test-Path $cl) { Select-String -Path $cl -Pattern 'output hash after evaluate' | Select-Object -First 1 } else { $null }
        $hashes[$with] = if ($line) { [regex]::Match($line.ToString(), '([0-9A-F]{16})').Groups[1].Value } else { '' }
        Write-Host ('  {0,-14} {1}' -f ($with -eq $true ? 'with consumer' : 'without'), $hashes[$with])
    }
    if ($null -eq $hashes) { }
    elseif ($hashes[$false] -ne $hashes['again']) {
        Write-Host '  FAIL: two runs without the consumer gave different hashes, so the instrument is not stable and no comparison is possible.' -ForegroundColor Red
        $rows += [pscustomobject]@{ Backend='d3d11'; Scenario='consumer'; Pass=0; Fail=1; Why='hash unstable' }
    }
    elseif (-not $hashes[$true] -or -not $hashes[$false]) {
        Write-Host '  FAIL: no output hash. The scenario must carry # cfg: hash_out=1.' -ForegroundColor Red
        $rows += [pscustomobject]@{ Backend='d3d11'; Scenario='consumer'; Pass=0; Fail=1; Why='no hash' }
    } elseif ($hashes[$true] -eq $hashes[$false]) {
        Write-Host '  FAIL: identical. The DLSS 5 add-on attached and wrote nothing -- the picture is the same as with no add-on at all.' -ForegroundColor Red
        $rows += [pscustomobject]@{ Backend='d3d11'; Scenario='consumer'; Pass=0; Fail=1; Why='consumer wrote nothing' }
    } else {
        Write-Host '  ok: the consumer changed the output.' -ForegroundColor Green
        $rows += [pscustomobject]@{ Backend='d3d11'; Scenario='consumer'; Pass=1; Fail=0; Why='' }
    }
}

# The brightness check, once, on D3D11. The neural pass may change the
# picture; it must not change its brightness by a factor. The bridge logs the
# mean of its output per feature build, and the brightness scenario builds
# three: scRGB float, HDR10 and 8-bit SDR. Compared with and without the DLSS 5
# add-on staged, per format; a luma ratio outside [0.5, 2] fails. An HDR frame
# the neural pass overexposed (dlss5-bridge #10) is what this is for.
if ($Only -ne 'vk') {
    Write-Host ''
    Write-Host 'brightness check: does the DLSS 5 add-on keep the picture at its brightness?'
    $means = @{}
    $bwhy = 'brightness moved'
    foreach ($with in @($true, $false)) {
        $a = @{ Scenario = 'brightness'; Frames = $Frames }
        if ($Background) { $a['Background'] = $true }
        if (-not $with) { $a['NoConsumer'] = $true }
        $global:LASTEXITCODE = -1
        try { & (Join-Path $root 'run.ps1') @a *>&1 | Out-Null } catch { $global:LASTEXITCODE = -1 }
        if ($LASTEXITCODE -ne 0) { Write-Host ("  FAIL: the {0} run exited {1}." -f ($with ? 'with-consumer' : 'without-consumer'), $LASTEXITCODE) -ForegroundColor Red; $means = $null; $bwhy = "runner exit $LASTEXITCODE"; break }
        $rsl = Join-Path $root 'run/brightness/ReShade.log'
        if ($with -and -not ((Test-Path $rsl) -and (Select-String -Path $rsl -Pattern 'DLSS5 Generic' -Quiet))) { Write-Host '  FAIL: the with-consumer run has no DLSS 5 add-on in it.' -ForegroundColor Red; $means = $null; $bwhy = 'consumer not staged'; break }
        $cl = Join-Path $root 'run/brightness/dlss5-bridge.log'
        $m = @{}
        if (Test-Path $cl) {
            foreach ($l in (Select-String -Path $cl -Pattern 'output mean after evaluate \d+: .*luma=([0-9.]+) \(([^)]+)\)')) {
                $m[$l.Matches[0].Groups[2].Value] = [double]$l.Matches[0].Groups[1].Value
            }
        }
        $means[$with] = $m
    }
    $fail = $false
    if ($null -eq $means) { $fail = $true; $means = @{ $true = @{}; $false = @{} } }
    foreach ($fmt in $means[$false].Keys) {
        if (-not $means[$true].ContainsKey($fmt)) { Write-Host ('  {0,-24} no reading with the consumer' -f $fmt) -ForegroundColor Red; $fail = $true; continue }
        $w = $means[$true][$fmt]; $wo = $means[$false][$fmt]
        $ratio = if ($wo -gt 0) { $w / $wo } else { 0 }
        $ok = $ratio -ge 0.5 -and $ratio -le 2.0
        Write-Host ('  {0,-24} luma without {1:F4}  with {2:F4}  ratio {3:F2}' -f $fmt, $wo, $w, $ratio) -ForegroundColor ($ok ? 'Green' : 'Red')
        if (-not $ok) { $fail = $true }
    }
    if ($means[$false].Count -eq 0) { Write-Host '  FAIL: no output mean. The scenario must carry # cfg: hash_out=1.' -ForegroundColor Red; $fail = $true }
    # Four presentations -- float, scRGB, HDR10, SDR -- or the check compared
    # fewer than it claims. HDR10 is refused on a display not in HDR mode, and
    # that refusal used to shrink the set in silence.
    elseif ($means[$false].Count -lt 4) { Write-Host ('  FAIL: {0} presentation(s) measured, 4 expected (float, scRGB, HDR10, SDR). Is the display in HDR mode?' -f $means[$false].Count) -ForegroundColor Red; $fail = $true; $bwhy = 'fewer than 4 presentations' }
    $rows += [pscustomobject]@{ Backend='d3d11'; Scenario='brightness'; Pass=($fail ? 0 : 1); Fail=($fail ? 1 : 0); Why=($fail ? $bwhy : '') }
}

$bad = @($rows | Where-Object { $_.Fail -gt 0 })
if ($bad.Count -gt 0) {
    Write-Host ('{0} of {1} scenario runs failed.' -f ($bad | Measure-Object Fail -Sum).Sum,
                (($rows | Measure-Object Pass -Sum).Sum + ($rows | Measure-Object Fail -Sum).Sum)) -ForegroundColor Red
    exit 1
}
Write-Host ('all {0} scenario run(s) passed.{1}' -f ($rows | Measure-Object Pass -Sum).Sum,
            $(if ($Fast) { ' FAST: a fraction of the frames, so a cadence or latch regression is not covered, and the mirror advancing check never applies on D3D11.' } else { '' })) -ForegroundColor Green
exit 0

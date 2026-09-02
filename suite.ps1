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
        $pass = 0; $fail = 0; $why = ''
        for ($i = 0; $i -lt $Repeat; ++$i) {
            $args = @{ Frames = $Frames; Scenario = $s }
            if ($Fast) { $args['Scale'] = 8 }
            if ($Background) { $args['Background'] = $true }
            if ($Validate -and $b -eq 'vk') { $args['Validate'] = $true }
            $out = & $runner @args 2>&1
            if ($LASTEXITCODE -eq 0) { $pass++ }
            else {
                $fail++
                if ($why -eq '') {
                    $line = $out | Select-String 'FAIL:' | Select-Object -First 1
                    if ($line) { $why = ($line.ToString() -replace '^\s*FAIL:\s*','').Trim() }
                }
            }
        }
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
    foreach ($with in @($true, $false)) {
        $a = @{ Scenario = 'consumer'; Frames = $Frames }
        if ($Background) { $a['Background'] = $true }
        if (-not $with) { $a['NoConsumer'] = $true }
        & (Join-Path $root 'run.ps1') @a 2>&1 | Out-Null
        # Read from the LOG, not from the runner's output. Write-Host does not
        # go down the pipeline, and capturing it is how this check silently
        # compared two empty strings the first time it ran.
        $cl = Join-Path $root 'run/consumer/dlss5-bridge.log'
        $line = if (Test-Path $cl) { Select-String -Path $cl -Pattern 'output hash after evaluate' | Select-Object -First 1 } else { $null }
        $hashes[$with] = if ($line) { [regex]::Match($line.ToString(), '([0-9A-F]{16})').Groups[1].Value } else { '' }
        Write-Host ('  {0,-14} {1}' -f ($with ? 'with consumer' : 'without'), $hashes[$with])
    }
    if (-not $hashes[$true] -or -not $hashes[$false]) {
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
    foreach ($with in @($true, $false)) {
        $a = @{ Scenario = 'brightness'; Frames = $Frames }
        if ($Background) { $a['Background'] = $true }
        if (-not $with) { $a['NoConsumer'] = $true }
        & (Join-Path $root 'run.ps1') @a 2>&1 | Out-Null
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
    foreach ($fmt in $means[$false].Keys) {
        if (-not $means[$true].ContainsKey($fmt)) { Write-Host ('  {0,-24} no reading with the consumer' -f $fmt) -ForegroundColor Red; $fail = $true; continue }
        $w = $means[$true][$fmt]; $wo = $means[$false][$fmt]
        $ratio = if ($wo -gt 0) { $w / $wo } else { 0 }
        $ok = $ratio -ge 0.5 -and $ratio -le 2.0
        Write-Host ('  {0,-24} luma without {1:F4}  with {2:F4}  ratio {3:F2}' -f $fmt, $wo, $w, $ratio) -ForegroundColor ($ok ? 'Green' : 'Red')
        if (-not $ok) { $fail = $true }
    }
    if ($means[$false].Count -eq 0) { Write-Host '  FAIL: no output mean. The scenario must carry # cfg: hash_out=1.' -ForegroundColor Red; $fail = $true }
    $rows += [pscustomobject]@{ Backend='d3d11'; Scenario='brightness'; Pass=($fail ? 0 : 1); Fail=($fail ? 1 : 0); Why=($fail ? 'brightness moved' : '') }
}

$bad = @($rows | Where-Object { $_.Fail -gt 0 })
if ($bad.Count -gt 0) {
    Write-Host ('{0} of {1} scenario runs failed.' -f ($bad | Measure-Object Fail -Sum).Sum,
                (($rows | Measure-Object Pass -Sum).Sum + ($rows | Measure-Object Fail -Sum).Sum)) -ForegroundColor Red
    exit 1
}
Write-Host ('all {0} scenario run(s) passed.{1}' -f ($rows | Measure-Object Pass -Sum).Sum,
            $(if ($Fast) { ' FAST: a fraction of the frames, so a cadence or latch regression is not covered.' } else { '' })) -ForegroundColor Green
exit 0

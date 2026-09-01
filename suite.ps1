# ngxhost -- run every scenario on both backends and print one table.
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
        $pass = 0; $fail = 0; $why = ''
        for ($i = 0; $i -lt $Repeat; ++$i) {
            $args = @{ Frames = $Frames; Scenario = $s }
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
$bad = @($rows | Where-Object { $_.Fail -gt 0 })
if ($bad.Count -gt 0) {
    Write-Host ('{0} of {1} scenario runs failed.' -f ($bad | Measure-Object Fail -Sum).Sum,
                (($rows | Measure-Object Pass -Sum).Sum + ($rows | Measure-Object Fail -Sum).Sum)) -ForegroundColor Red
    exit 1
}
Write-Host ('all {0} scenario run(s) passed.' -f ($rows | Measure-Object Pass -Sum).Sum) -ForegroundColor Green
exit 0

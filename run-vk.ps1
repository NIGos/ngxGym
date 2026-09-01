# ngxhost -- stage and run the Vulkan host.
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
    [switch] $Validate,
    [string] $Snippet = 'D:\SteamLibrary\steamapps\common\Baldurs Gate 3\bin\nvngx_dlss.dll'
)

$ErrorActionPreference = 'Stop'
$root  = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe   = Join-Path $root 'bin\ngxhost-vk.exe'
$addon = 'C:\Users\quali\Documents\Code Projects\Misc\ngxbridge\dlss5-bridge.addon64'
$apps  = 'C:\ProgramData\ReShade\ReShadeApps.ini'
$run   = Join-Path $root 'run\vk'
$target = Join-Path $run 'ngxhost-vk.exe'

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
    $d5 = Join-Path (Split-Path -Parent $Snippet) 'renodx-dlss5.addon64'
    if (Test-Path $d5) { Copy-Item $d5 $run }
}

Copy-Item (Join-Path $root 'reshade-fx') (Join-Path $run 'fx') -Recurse
"[ngxhost_probe.fx]`n`nTechniques=ngxhost_probe@ngxhost_probe.fx`n" |
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
Write-Host "PASS: the layer attached and the add-on registered at API $($m.Groups[1].Value)" -ForegroundColor Green

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

exit 0

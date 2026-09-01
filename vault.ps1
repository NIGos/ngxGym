# ngxGym -- the neighbour vault.
#
# Every file this suite stages that this project does not build: the DLSS 5
# add-on, the NGX super-resolution snippet, the neural-rendering snippet. They
# are the variables an A/B needs and the ones nobody keeps a copy of.
#
# The evening of 2026-09-01 went into looking inside this add-on for a change
# that turned out to be a neighbour: two builds of the same DLSS 5 add-on,
# 1,732,608 and 1,703,424 bytes, BOTH declaring version 0.2026.0828.0517, one in
# each game folder. The newer one made its own panel say active while the picture
# did not change. Nothing on disk recorded that two builds existed, and the older
# one survived only because it had not been overwritten yet.
#
#   .\vault.ps1                                  list what is kept
#   .\vault.ps1 -Add <path> -Label "renodx new"  take a copy, hash it, record it
#   .\vault.ps1 -Use <label-or-sha> -To <folder> deploy it for an A/B
#   .\vault.ps1 -Scan <folder>                   record everything stageable in a folder
#
# The BINARIES are not tracked by git -- they are other people's, one of them is
# 165 MB, and a private mirror of somebody else's build is their call and not a
# side effect of a test runner. vault.tsv IS tracked: it is the record of what
# was on this machine when a measurement was taken, which is the part that has to
# survive.
[CmdletBinding()]
param(
    [string] $Add,
    [string] $Label,
    [string] $Use,
    [string] $To,
    [string] $Scan
)

$ErrorActionPreference = 'Stop'
$root  = Split-Path -Parent $MyInvocation.MyCommand.Path
$store = Join-Path $root 'vault'
$index = Join-Path $root 'vault.tsv'
New-Item -ItemType Directory -Force $store | Out-Null
if (-not (Test-Path $index)) {
    "sha256`tbytes`tmtime`tname`tlabel`tfirst_seen" | Set-Content $index -Encoding UTF8
}

function Read-Index { Import-Csv $index -Delimiter "`t" }

function Add-One([string] $path, [string] $label) {
    if (-not (Test-Path $path)) { Write-Warning "not found: $path"; return }
    $f    = Get-Item $path
    $sha  = (Get-FileHash $path -Algorithm SHA256).Hash
    $rows = Read-Index
    if ($rows | Where-Object { $_.sha256 -eq $sha }) {
        Write-Host ("already kept: {0}  {1}" -f $sha.Substring(0,12), $f.Name) -ForegroundColor DarkGray
        return
    }
    # Stored under the hash, so two builds of one filename cannot overwrite each
    # other -- which is the whole failure this exists for.
    $kept = Join-Path $store ("{0}-{1}{2}" -f $f.BaseName, $sha.Substring(0,12), $f.Extension)
    Copy-Item $f.FullName $kept
    $line = "{0}`t{1}`t{2:yyyy-MM-dd HH:mm}`t{3}`t{4}`t{5:yyyy-MM-dd HH:mm}" -f `
            $sha, $f.Length, $f.LastWriteTime, $f.Name, $label, (Get-Date)
    Add-Content $index $line -Encoding UTF8
    Write-Host ("kept {0}  {1,10:N0} bytes  {2}" -f $sha.Substring(0,12), $f.Length, $f.Name) -ForegroundColor Green
}

if ($Scan) {
    foreach ($p in 'renodx-dlss5*.addon64','nvngx_dlss.dll','nvngx_dlssnr.dll','nvngx_dlssd.dll') {
        Get-ChildItem $Scan -Filter $p -EA SilentlyContinue |
            ForEach-Object { Add-One $_.FullName ($Label ? $Label : (Split-Path $Scan -Leaf)) }
    }
    exit 0
}

if ($Add) { Add-One $Add $Label; exit 0 }

if ($Use) {
    if (-not $To) { Write-Error "-Use needs -To <folder>"; exit 2 }
    $rows = Read-Index
    $hit = $rows | Where-Object { $_.label -eq $Use -or $_.sha256 -like "$Use*" } | Select-Object -First 1
    if (-not $hit) { Write-Error "nothing in the vault matches '$Use'"; exit 2 }
    $f = Get-Item $hit.name -EA SilentlyContinue
    $stem = [IO.Path]::GetFileNameWithoutExtension($hit.name)
    $ext  = [IO.Path]::GetExtension($hit.name)
    $kept = Join-Path $store ("{0}-{1}{2}" -f $stem, $hit.sha256.Substring(0,12), $ext)
    if (-not (Test-Path $kept)) { Write-Error "recorded but not on disk: $kept"; exit 2 }
    $dst = Join-Path $To $hit.name
    Copy-Item $kept $dst -Force
    Write-Host ("deployed {0} -> {1}" -f $hit.sha256.Substring(0,12), $dst) -ForegroundColor Green
    Write-Host ("  {0}  {1,10:N0} bytes  label '{2}'" -f $hit.name, [int]$hit.bytes, $hit.label)
    exit 0
}

$rows = @(Read-Index)
if ($rows.Count -eq 0) { Write-Host "vault is empty. Try: .\vault.ps1 -Scan '<game folder>'"; exit 0 }
Write-Host ("{0} file(s) kept in {1}" -f $rows.Count, $store)
$rows | Sort-Object name, first_seen |
    Format-Table @{n='sha';e={$_.sha256.Substring(0,12)}}, @{n='bytes';e={[int]$_.bytes}}, mtime, name, label -AutoSize
exit 0

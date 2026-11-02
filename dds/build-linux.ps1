<#
.SYNOPSIS
  Build and check the Linux DDS libraries in a wslc python:3.12-slim container.

.DESCRIPTION
  Runs dds/build-linux.sh with this checkout mounted (keep the checkout off
  OneDrive: wslc cannot read OneDrive placeholders). With -Reference, the
  given ddss libdds.so is mounted too and cross-checked against.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File dds\build-linux.ps1
  powershell -ExecutionPolicy Bypass -File dds\build-linux.ps1 -Reference C:\path\ddss-libdds.so
#>
param(
  [string]$Image = "python:3.12-slim",
  [string]$Reference = ""
)
$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$wslc = "C:\Program Files\WSL\wslc.exe"
if (! (Test-Path $wslc)) { $wslc = "wslc" }

$mounts = @("-v", "${repoRoot}:/src/bsc")
$refArg = ""
if ($Reference) {
  $refDir = Split-Path -Parent (Resolve-Path $Reference)
  $refName = Split-Path -Leaf $Reference
  $mounts += @("-v", "${refDir}:/ref")
  $refArg = "/ref/$refName"
}
Write-Host "Building the Linux DDS libraries in $Image via wslc ..."
# sed strips CR so scripts checked out on Windows still run under bash.
& $wslc run --rm @mounts -w /src/bsc $Image bash -lc `
  "sed -i 's/\r$//' build.sh dds/build-linux.sh; bash dds/build-linux.sh $refArg"
if ($LASTEXITCODE -ne 0) { throw "Linux build/check failed (exit $LASTEXITCODE)" }
Get-Item (Join-Path $repoRoot "build\libdds-single.so") | Format-List FullName, Length, LastWriteTime

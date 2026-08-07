# SPDX-License-Identifier: MIT
# MultiplayerEvolved: tools/make_testing_zip.ps1
#
# Builds the archive handed to testers: the public release, plus the collector address.
#
# WHY IT IS A SEPARATE ARCHIVE
#
# The repository and its releases are public. An address published there is readable by
# anyone who finds it, which means anyone could read the testers' reports, and floodable by
# anyone who wants to, which means a free collector fills with somebody else's traffic and
# the reports being waited for are pushed out of it.
#
# So the address lives in testing/report.url, which is not in the repository, and this makes
# one archive with it in. That archive is handed to the people testing rather than published.
#
# THIS IS TEMPORARY
#
# It goes when the testing period does, along with the rest of the sharing.

$ErrorActionPreference = 'Stop'

$root    = Split-Path -Parent $PSScriptRoot
$out     = Join-Path $root 'build'
$stage   = Join-Path $out 'testing_package'
$urlFile = Join-Path $root 'testing\report.url'
$archive = Join-Path $out 'MultiplayerEvolved-testing.zip'

if (-not (Test-Path $urlFile)) {
    Write-Error "No testing\report.url. Put the collector address in it, one https line."
}
$url = (Get-Content $urlFile -First 1).Trim()
if (-not $url.StartsWith('https://')) {
    Write-Error "The collector address must be https. A log crossing the network in the clear is not something to arrange by accident."
}

foreach ($name in 'MultiplayerEvolved.dll', 'version.dll') {
    if (-not (Test-Path (Join-Path $out $name))) {
        Write-Error "$name is missing from build\. Run build.bat first."
    }
}

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path (Join-Path $stage 'MultiplayerEvolved') -Force | Out-Null

Copy-Item (Join-Path $out 'MultiplayerEvolved.dll') $stage
Copy-Item (Join-Path $out 'version.dll') $stage
Copy-Item (Join-Path $root 'data\*') (Join-Path $stage 'MultiplayerEvolved') -Recurse -Force
Copy-Item $urlFile (Join-Path $stage 'MultiplayerEvolved\report.url')

# The symbol descriptor is the one file whose absence is not obvious until the engine
# binding quietly falls back to its built in defaults, so it is checked by name.
if (-not (Get-ChildItem (Join-Path $stage 'MultiplayerEvolved\symbols') -Filter *.json -ErrorAction SilentlyContinue)) {
    Write-Error "No symbol descriptor was staged; this archive would install a mod that falls back to built in defaults."
}

if (Test-Path $archive) { Remove-Item $archive -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $archive -Force

Write-Host ""
Write-Host "=== Packaged $archive ==="
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead($archive)
foreach ($entry in $zip.Entries) { '  {0}  {1} bytes' -f $entry.FullName, $entry.Length }
$zip.Dispose()
Write-Host ""
Write-Host "Reports go to $url"
Write-Host "Hand this archive to the people testing. Do not attach it to a release."

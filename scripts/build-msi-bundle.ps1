<#
.SYNOPSIS
  Wrap the MSI cpack just built in a Burn bootstrapper .exe.

.DESCRIPTION
  The reason this exists at all is the icon: a .msi cannot carry one, so the
  file people download is this .exe. See cmake/bundle.wxs.in for the long
  version.

  Run after `cpack`, against the same build directory. CMake has already written
  bundle.wxs there with the version, the paths and the MSI's name in it, so all
  that is left is candle and light.

  Nothing here is conditional on CI: it is the same two commands on a desktop,
  which is the point of it being a script rather than eight lines of YAML.
#>
[CmdletBinding()]
param(
    # The build directory cpack was run against, e.g. build/release.
    [Parameter(Mandatory = $true)][string]$BuildDir
)

$ErrorActionPreference = "Stop"

$buildPath = (Resolve-Path $BuildDir).Path
$source = Join-Path $buildPath "bundle.wxs"
if (-not (Test-Path $source)) {
    throw "No bundle.wxs in $buildPath. It is written by cmake/Packaging.cmake at configure time, so this build was configured before the bundle existed, or is not a Windows build."
}

# The MSI is named in bundle.wxs, but light.exe reports a missing SourceFile as
# a resolution error several lines deep. Said here instead, where the cause --
# cpack has not run, or its MSI step failed -- is the obvious one.
# @() so that one match is still an array: Get-ChildItem returns a bare FileInfo
# for a single file, and indexing that is a trap worth not setting.
$msi = @(Get-ChildItem -Path $buildPath -Filter "CutReel-*.msi" -File)
if ($msi.Count -ne 1) {
    throw "Expected exactly one CutReel-*.msi in $buildPath, found $($msi.Count). Run cpack first."
}

# The .exe takes the MSI's name, so the two sit together in a release and it is
# obvious they are the same build.
$output = Join-Path $buildPath ($msi[0].BaseName + ".exe")
$object = Join-Path $buildPath "bundle.wixobj"

# WixBalExtension is what BootstrapperApplicationRef and the bal: namespace come
# from; without it candle fails on the element rather than on the missing
# extension, which reads like the file is wrong.
Write-Host "candle: $source"
& candle.exe -nologo -ext WixBalExtension -out $object $source
if ($LASTEXITCODE -ne 0) { throw "candle.exe failed with $LASTEXITCODE" }

Write-Host "light: $output"
& light.exe -nologo -ext WixBalExtension -out $output $object
if ($LASTEXITCODE -ne 0) { throw "light.exe failed with $LASTEXITCODE" }

if (-not (Test-Path $output)) {
    throw "light.exe reported success but wrote no $output"
}
Write-Host "wrote $output ($((Get-Item $output).Length) bytes)"

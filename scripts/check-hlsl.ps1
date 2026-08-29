# Compile every shader bundle's HLSL with fxc, the way the D3D11 backend will.
#
#   pwsh scripts/check-hlsl.ps1 -BuildDir build/windows-debug
#
# Why this exists. A .qsb bundle carries the same shader in several languages,
# and qsb generates all of them from one GLSL source without compiling any. So a
# construct that is legal in GLSL, SPIR-V and Metal but not in HLSL builds
# cleanly, ships, and then fails at runtime -- on the one platform none of us
# develops on -- when QRhi hands the HLSL to fxc and the pipeline will not
# build.
#
# That happened: `colour[index] = ...` in composite.frag, a dynamic index used
# as an l-value, which fxc rejects with X3500 because a vec3 local is a register
# rather than addressable memory. It reached us as thirty failing GPU tests
# whose only symptom was "cannot create a graphics pipeline". This check turns
# the same mistake into a build failure naming the file, the line and the error.
#
# Windows only: fxc comes from the Windows SDK and there is no other compiler
# that agrees with it about what SM5 accepts.

[CmdletBinding()]
param(
    # The configured build tree, e.g. build/windows-debug. Searched for *.qsb.
    [Parameter(Mandatory = $true)]
    [string] $BuildDir
)

function Find-Program {
    param([string] $Name, [string[]] $Fallbacks)

    $found = Get-Command $Name -ErrorAction SilentlyContinue
    if ($found) { return $found.Source }
    foreach ($path in $Fallbacks) {
        if ($path -and (Test-Path $path)) { return $path }
    }
    return $null
}

# qsb ships in Qt's bin directory, which install-qt-action puts on PATH.
$qsbFallbacks = @()
if ($env:QT_ROOT_DIR) { $qsbFallbacks += (Join-Path $env:QT_ROOT_DIR 'bin\qsb.exe') }
if ($env:Qt6_DIR)     { $qsbFallbacks += (Join-Path $env:Qt6_DIR '..\..\..\bin\qsb.exe') }
$qsb = Find-Program -Name 'qsb.exe' -Fallbacks $qsbFallbacks
if (-not $qsb) {
    Write-Host "::error::qsb.exe not found. It ships with Qt; put Qt's bin directory on PATH."
    exit 1
}

# fxc comes from the Windows SDK. vcvars usually puts it on PATH; if it has not,
# the newest x64 one under the Kits directory is the same compiler.
$fxc = Find-Program -Name 'fxc.exe' -Fallbacks @()
if (-not $fxc) {
    $kits = 'C:\Program Files (x86)\Windows Kits\10\bin'
    if (Test-Path $kits) {
        $candidate = Get-ChildItem -Path $kits -Recurse -Filter 'fxc.exe' -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -like '*\x64\*' } |
            Sort-Object -Property FullName -Descending |
            Select-Object -First 1
        if ($candidate) { $fxc = $candidate.FullName }
    }
}
if (-not $fxc) {
    Write-Host "::error::fxc.exe not found. It ships with the Windows SDK."
    exit 1
}

Write-Host "qsb: $qsb"
Write-Host "fxc: $fxc"

# -Force, because the bundles live in a directory called .qsb and a check that
# quietly found nothing would be worse than no check at all.
$bundles = @(Get-ChildItem -Path $BuildDir -Recurse -Force -Filter '*.qsb' -ErrorAction SilentlyContinue)
if ($bundles.Count -eq 0) {
    # A silent pass here would be a check that stopped checking. If the build
    # tree holds no shaders, either the path is wrong or the build did not run.
    Write-Host "::error::No .qsb files under '$BuildDir'. Build first, or check the path."
    exit 1
}

$scratch = Join-Path ([System.IO.Path]::GetTempPath()) 'zaro-hlsl'
New-Item -ItemType Directory -Force -Path $scratch | Out-Null

$failures = 0
foreach ($bundle in $bundles) {
    # The stage decides the profile, and the repository's shaders say which they
    # are in their names. Anything else is not ours to judge.
    $target = $null
    if     ($bundle.Name -like '*.vert.qsb') { $target = 'vs_5_0' }
    elseif ($bundle.Name -like '*.frag.qsb') { $target = 'ps_5_0' }
    if (-not $target) {
        Write-Host "skipped $($bundle.Name): not named for a stage"
        continue
    }

    $hlsl = Join-Path $scratch ($bundle.BaseName + '.hlsl')
    # 50 is the version QRhi's D3D11 backend asks the bundle for.
    $extracted = & $qsb --extract 'hlsl,50' -o $hlsl $bundle.FullName 2>&1
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $hlsl)) {
        # qsb's own words, not a guess at them: "no HLSL in this bundle" and
        # "that is not an option I know" both land here and want different
        # fixes.
        Write-Host "::error::could not take HLSL 50 out of $($bundle.Name)"
        foreach ($line in $extracted) { Write-Host "  $line" }
        $failures++
        continue
    }

    $object = Join-Path $scratch ($bundle.BaseName + '.fxo')
    $output = & $fxc /nologo /T $target /E main /Fo $object $hlsl 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "::error::$($bundle.Name) does not compile as $target"
        foreach ($line in $output) { Write-Host "  $line" }
        $failures++
    }
    else {
        Write-Host "ok  $($bundle.Name) ($target)"
    }
}

if ($failures -gt 0) {
    Write-Host "$failures shader(s) will not compile with fxc; the D3D11 backend will refuse them."
    exit 1
}

Write-Host "All $($bundles.Count) shader bundle(s) compile as SM5."
exit 0

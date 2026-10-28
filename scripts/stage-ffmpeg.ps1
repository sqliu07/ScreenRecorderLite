param(
    [Parameter(Mandatory = $true)][string]$DependencyMetadata,
    [Parameter(Mandatory = $true)][string]$ProbeExecutable,
    [Parameter(Mandatory = $true)][string]$PackageDirectory,
    [Parameter(Mandatory = $true)][string]$SourceArchive
)
$ErrorActionPreference = 'Stop'

$dependency = Get-Content $DependencyMetadata -Raw | ConvertFrom-Json
$triplet = [string]$dependency.triplet
if ($triplet -notmatch '^[a-zA-Z0-9_-]+$' -or -not $dependency.installedRoot -or -not $dependency.vcpkgRoot) {
    throw 'The CMake dependency metadata is incomplete.'
}
$information = (& $ProbeExecutable --dependency-info | Out-String)
if ($LASTEXITCODE -ne 0) { throw 'The FFmpeg dependency probe failed.' }
$runtime = $information | ConvertFrom-Json
$libraries = @($runtime.libraries)
$expected = @('avformat', 'avcodec', 'avutil')
if ($libraries.Count -ne $expected.Count -or
    @(Compare-Object $expected @($libraries | ForEach-Object name)).Count -ne 0) {
    throw 'Expected avformat, avcodec and avutil dependency information.'
}
foreach ($library in $libraries) {
    if ($library.version -notmatch '^\d+\.\d+\.\d+$' -or
        $library.license -notmatch 'LGPL|Lesser General Public License' -or
        $library.configuration -match '--enable-(gpl|nonfree)\b' -or
        $library.configuration -notmatch '--enable-shared\b') {
        throw "The $($library.name) runtime must be a versioned, shared LGPL build without GPL/nonfree features."
    }
}
if (@($libraries.configuration | Sort-Object -Unique).Count -ne 1) {
    throw 'The FFmpeg runtime libraries were built with different configurations.'
}

$notice = Join-Path $dependency.installedRoot "$triplet/share/ffmpeg/copyright"
if (-not (Test-Path $notice -PathType Leaf)) { throw "Missing FFmpeg license notice: $notice" }
$buildDirectory = Join-Path $dependency.vcpkgRoot "buildtrees/ffmpeg/$triplet-rel"
$buildScript = Join-Path $buildDirectory 'build.sh'
if (-not (Test-Path $buildScript -PathType Leaf)) {
    throw 'The exact FFmpeg source and build recipe are required. Disable vcpkg binary-cache restoration for release builds.'
}
$sourceMatch = Select-String -Path $buildScript -Pattern '^PATH_TO_SRC_DIR="([^"]+)"$'
if (-not $sourceMatch) { throw 'Cannot locate the FFmpeg source used by this build.' }
$sourceDirectory = $sourceMatch.Matches[0].Groups[1].Value
if (-not (Test-Path (Join-Path $sourceDirectory 'configure') -PathType Leaf)) {
    throw "The corresponding FFmpeg source tree is unavailable: $sourceDirectory"
}

$licenses = Join-Path $PackageDirectory 'licenses'
New-Item -ItemType Directory -Force $licenses | Out-Null
Copy-Item $notice (Join-Path $licenses 'FFmpeg-LICENSE.txt')
$runtime | ConvertTo-Json -Depth 6 | Set-Content -Encoding UTF8 (Join-Path $licenses 'FFmpeg-build.json')

$sourceStage = Join-Path ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($SourceArchive))) (
    'ffmpeg-source-' + [guid]::NewGuid().ToString('N'))
try {
    New-Item -ItemType Directory -Force $sourceStage | Out-Null
    Copy-Item $sourceDirectory (Join-Path $sourceStage 'ffmpeg') -Recurse
    $reproduction = Join-Path $sourceStage 'build-information'
    New-Item -ItemType Directory -Force $reproduction | Out-Null
    foreach ($file in @('vcpkg.json', 'CMakePresets.json')) {
        Copy-Item (Join-Path $PSScriptRoot "../$file") $reproduction
    }
    Copy-Item (Join-Path $PSScriptRoot '../cmake/triplets') (Join-Path $reproduction 'triplets') -Recurse
    Copy-Item $DependencyMetadata (Join-Path $reproduction 'srl-dependency.json')
    Copy-Item $buildScript $reproduction
    foreach ($name in @('cflags.rsp', 'ldflags.rsp', 'config.h', 'config_components.h', 'ffbuild/config.log', 'ffbuild/config.mak')) {
        $file = Join-Path $buildDirectory $name
        if (Test-Path $file -PathType Leaf) { Copy-Item $file $reproduction }
    }
    Copy-Item (Join-Path $licenses 'FFmpeg-build.json') $reproduction
    Copy-Item $notice (Join-Path $reproduction 'FFmpeg-LICENSE.txt')
    @"
The ffmpeg/ folder contains the exact patched source tree used for these DLLs.
The generated build.sh records the configure flags; the response files and
configuration records capture the compiler settings. Absolute paths refer to
the build machine. The manifest baseline identifies the port and patch set.

Install vcpkg at the builtin-baseline in vcpkg.json, then install that manifest
with triplet $triplet and --overlay-triplets pointing to the included triplets
folder. Build FFmpeg as shared libraries and retain the static MSVC runtime.
The application uses avformat, avcodec and avutil; no FFmpeg executable is used.
"@ | Set-Content -Encoding UTF8 (Join-Path $reproduction 'README.txt')
    & tar.exe -czf $SourceArchive -C $sourceStage .
    if ($LASTEXITCODE -ne 0) { throw 'Could not archive the corresponding FFmpeg source.' }
} finally {
    if (Test-Path $sourceStage) { Remove-Item $sourceStage -Recurse -Force }
}

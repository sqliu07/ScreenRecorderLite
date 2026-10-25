param(
    [Parameter(Mandatory = $true)][string]$FfmpegExecutable,
    [Parameter(Mandatory = $true)][string]$InstalledRoot,
    [Parameter(Mandatory = $true)][string]$VcpkgRoot,
    [Parameter(Mandatory = $true)][string]$PackageDirectory,
    [Parameter(Mandatory = $true)][string]$SourceArchive
)
$ErrorActionPreference = 'Stop'

$version = (& $FfmpegExecutable -version 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0) { throw 'The bundled FFmpeg executable cannot run.' }
if ($version -match '--enable-(gpl|nonfree)\b') {
    throw 'Release packaging requires the LGPL FFmpeg build from vcpkg; GPL/nonfree overrides are not accepted.'
}
$license = (& $FfmpegExecutable -L 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0 -or $license -notmatch 'GNU Lesser General Public\s+License') {
    throw 'The bundled FFmpeg executable does not identify itself as an LGPL build.'
}

$notice = Join-Path $InstalledRoot 'x64-windows-static/share/ffmpeg/copyright'
if (-not (Test-Path $notice)) { throw "Missing FFmpeg license notice: $notice" }
$buildDirectory = Join-Path $VcpkgRoot 'buildtrees/ffmpeg/x64-windows-static-rel'
$buildScript = Join-Path $buildDirectory 'build.sh'
if (-not (Test-Path $buildScript)) {
    throw 'The exact FFmpeg source and build recipe are required. Disable vcpkg binary-cache restoration for release builds.'
}
$sourceMatch = Select-String -Path $buildScript -Pattern '^PATH_TO_SRC_DIR="([^"]+)"$'
if (-not $sourceMatch) { throw 'Cannot locate the FFmpeg source used by this build.' }
$sourceDirectory = $sourceMatch.Matches[0].Groups[1].Value
if (-not (Test-Path (Join-Path $sourceDirectory 'configure'))) {
    throw "The corresponding FFmpeg source tree is unavailable: $sourceDirectory"
}

$licenses = Join-Path $PackageDirectory 'licenses'
New-Item -ItemType Directory -Force $licenses | Out-Null
Copy-Item $notice (Join-Path $licenses 'FFmpeg-LICENSE.txt')
$version + "`n" + $license | Set-Content -Encoding UTF8 (Join-Path $licenses 'FFmpeg-build.txt')
@'
This application runs FFmpeg as a separate process to mux MP4 audio tracks.
FFmpeg is licensed under the GNU Lesser General Public License (LGPL).
The exact patched source and build information are supplied in the
FFmpeg-corresponding-source.tar.gz asset alongside this portable ZIP.
See licenses/FFmpeg-LICENSE.txt and licenses/FFmpeg-build.txt.
FFmpeg project: https://ffmpeg.org/
'@ | Set-Content -Encoding UTF8 (Join-Path $PackageDirectory 'THIRD_PARTY_NOTICES.txt')

$sourceStage = Join-Path ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($SourceArchive))) (
    'ffmpeg-source-' + [guid]::NewGuid().ToString('N'))
try {
    New-Item -ItemType Directory -Force $sourceStage | Out-Null
    Copy-Item $sourceDirectory (Join-Path $sourceStage 'ffmpeg') -Recurse
    $reproduction = Join-Path $sourceStage 'build-information'
    New-Item -ItemType Directory -Force $reproduction | Out-Null
    Copy-Item (Join-Path $PSScriptRoot '../vcpkg.json') $reproduction
    Copy-Item (Join-Path $PSScriptRoot '../CMakePresets.json') $reproduction
    Copy-Item (Join-Path $PSScriptRoot '../cmake/triplets') (Join-Path $reproduction 'triplets') -Recurse
    Copy-Item $buildScript $reproduction
    foreach ($name in @('cflags.rsp', 'ldflags.rsp', 'ffbuild/config.log')) {
        $file = Join-Path $buildDirectory $name
        if (Test-Path $file) { Copy-Item $file $reproduction }
    }
    Copy-Item (Join-Path $licenses 'FFmpeg-build.txt') $reproduction
    Copy-Item $notice (Join-Path $reproduction 'FFmpeg-LICENSE.txt')
    @'
The ffmpeg/ folder contains the patched source tree used for these binaries.
The generated build.sh records the actual configure flags. Response files and
config.log record the compiler settings. Their absolute paths refer to the CI runner.
To reproduce dependency selection, install vcpkg and use the included vcpkg.json
with triplet x64-windows-static and --overlay-triplets pointing to the included
triplets directory. The manifest's builtin-baseline pins the port and patch set.
'@ | Set-Content -Encoding UTF8 (Join-Path $reproduction 'README.txt')
    & tar.exe -czf $SourceArchive -C $sourceStage .
    if ($LASTEXITCODE -ne 0) { throw 'Could not archive the corresponding FFmpeg source.' }
} finally {
    if (Test-Path $sourceStage) { Remove-Item $sourceStage -Recurse -Force }
}

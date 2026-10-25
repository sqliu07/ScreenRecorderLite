param(
    [string]$BuildDirectory = 'out/build/windows',
    [string]$InstalledRoot = 'out/vcpkg_installed',
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [Parameter(Mandatory = $true)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $VcpkgRoot) { throw 'VCPKG_ROOT is required to collect the corresponding FFmpeg source.' }
if (-not [IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory = Join-Path $repo $BuildDirectory }
if (-not [IO.Path]::IsPathRooted($InstalledRoot)) { $InstalledRoot = Join-Path $repo $InstalledRoot }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
$stage = Join-Path $OutputDirectory ('stage-' + [guid]::NewGuid().ToString('N'))
try {
    New-Item -ItemType Directory $stage | Out-Null
    foreach ($name in @('ScreenRecorderLite.exe', 'ffmpeg.exe')) {
        $binary = Join-Path $BuildDirectory "Release/$name"
        if (-not (Test-Path $binary -PathType Leaf)) { throw "Missing release component: $binary" }
        Copy-Item $binary $stage
    }
    Get-ChildItem (Join-Path $BuildDirectory 'Release') -Filter '*.dll' | Copy-Item -Destination $stage
    foreach ($name in @('README.md', 'README.en.md', 'LICENSE', 'THIRD_PARTY_NOTICES.md')) {
        Copy-Item (Join-Path $repo $name) $stage
    }
    $sourceArchive = Join-Path $OutputDirectory 'FFmpeg-corresponding-source.tar.gz'
    & (Join-Path $PSScriptRoot 'stage-ffmpeg.ps1') `
        -FfmpegExecutable (Join-Path $stage 'ffmpeg.exe') `
        -InstalledRoot $InstalledRoot -VcpkgRoot $VcpkgRoot `
        -PackageDirectory $stage -SourceArchive $sourceArchive

    $archive = Join-Path $OutputDirectory 'ScreenRecorderLite-win-x64.zip'
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $archive -Force
    foreach ($file in @($archive, $sourceArchive)) {
        $hash = (Get-FileHash $file -Algorithm SHA256).Hash.ToLowerInvariant()
        "$hash  $([IO.Path]::GetFileName($file))" | Set-Content -Encoding ascii "$file.sha256"
    }
    Write-Output "Portable package: $archive"
    Write-Output "FFmpeg source: $sourceArchive"
} finally {
    if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
}

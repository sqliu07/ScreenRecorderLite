param(
    [string]$BuildDirectory = 'out/build/windows',
    [Parameter(Mandatory = $true)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not [IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory = Join-Path $repo $BuildDirectory }
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$metadataPath = Join-Path $BuildDirectory 'srl-dependency.json'
if (-not (Test-Path $metadataPath -PathType Leaf)) { throw "Missing CMake dependency metadata: $metadataPath" }
$dependency = Get-Content $metadataPath -Raw | ConvertFrom-Json
$runtimeFiles = @($dependency.runtimeFiles)
if ($runtimeFiles.Count -ne 3) { throw 'Expected exactly three FFmpeg runtime DLLs in the CMake dependency metadata.' }
$runtimeNames = @($runtimeFiles | ForEach-Object { [IO.Path]::GetFileName($_) })
if (@($runtimeNames | Sort-Object -Unique).Count -ne $runtimeNames.Count -or
    @($runtimeNames | Where-Object { $_ -notmatch '^(avformat|avcodec|avutil)(-\d+)?\.dll$' }).Count -ne 0 -or
    @($runtimeNames | ForEach-Object { $_ -replace '(-\d+)?\.dll$', '' } | Sort-Object -Unique).Count -ne 3) {
    throw 'The runtime metadata must contain one DLL each for avformat, avcodec and avutil.'
}
$releaseDirectory = Join-Path $BuildDirectory 'Release'
$probe = Join-Path $releaseDirectory 'srl_probe.exe'
if (-not (Test-Path $probe -PathType Leaf)) { throw 'Release packaging requires the dependency probe (SRL_BUILD_PROBE=ON).' }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
$stage = Join-Path $OutputDirectory ('stage-' + [guid]::NewGuid().ToString('N'))
try {
    New-Item -ItemType Directory $stage | Out-Null
    $application = Join-Path $releaseDirectory 'ScreenRecorderLite.exe'
    if (-not (Test-Path $application -PathType Leaf)) { throw "Missing release component: $application" }
    Copy-Item $application $stage
    foreach ($runtimeFile in $runtimeFiles) {
        $binary = Join-Path $releaseDirectory ([IO.Path]::GetFileName($runtimeFile))
        if (-not (Test-Path $runtimeFile -PathType Leaf) -or -not (Test-Path $binary -PathType Leaf)) {
            throw "Missing FFmpeg runtime component: $binary"
        }
        if ((Get-FileHash $binary -Algorithm SHA256).Hash -ne (Get-FileHash $runtimeFile -Algorithm SHA256).Hash) {
            throw "The built FFmpeg runtime differs from the configured SDK: $binary"
        }
        Copy-Item $binary $stage
    }
    foreach ($name in @('README.md', 'README.en.md', 'LICENSE', 'THIRD_PARTY_NOTICES.md')) {
        Copy-Item (Join-Path $repo $name) $stage
    }
    $sourceArchive = Join-Path $OutputDirectory 'FFmpeg-corresponding-source.tar.gz'
    $probeDirectory = Join-Path $stage '.dependency-check'
    $previousPath = $env:PATH
    try {
        New-Item -ItemType Directory $probeDirectory | Out-Null
        Copy-Item $probe $probeDirectory
        foreach ($name in $runtimeNames) { Copy-Item (Join-Path $stage $name) $probeDirectory }
        $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
        & (Join-Path $PSScriptRoot 'stage-ffmpeg.ps1') `
            -DependencyMetadata $metadataPath `
            -ProbeExecutable (Join-Path $probeDirectory 'srl_probe.exe') `
            -PackageDirectory $stage -SourceArchive $sourceArchive
    } finally {
        $env:PATH = $previousPath
        if (Test-Path $probeDirectory) { Remove-Item $probeDirectory -Recurse -Force }
    }

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

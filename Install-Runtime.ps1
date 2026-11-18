[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
Add-Type -AssemblyName System.IO.Compression.FileSystem
$stage = Join-Path ([IO.Path]::GetTempPath()) ('rtx-video-runtime-' + [Guid]::NewGuid().ToString('N'))
$payload = Join-Path $stage 'payload'
$null = New-Item -ItemType Directory -Path $payload -Force

function Get-VerifiedArchive($url, $digest) {
    $archive = Join-Path $stage $digest
    Write-Host ('Downloading ' + ([Uri]$url).Host)
    $client = New-Object Net.WebClient
    try { $client.DownloadFile($url, $archive) } finally { $client.Dispose() }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $digest) {
        throw ('Checksum mismatch: ' + $url)
    }
    return $archive
}

function Install-ArchiveFiles($url, $digest, $mapping) {
    $archive = Get-VerifiedArchive $url $digest
    $zip = [IO.Compression.ZipFile]::OpenRead($archive)
    try {
        foreach ($source in $mapping.Keys) {
            $entries = @($zip.Entries | Where-Object {
                $_.FullName -eq $source -or $_.FullName.EndsWith('/' + $source, [StringComparison]::OrdinalIgnoreCase)
            })
            if ($entries.Count -ne 1) { throw ('Expected one archive member: ' + $source) }
            $target = Join-Path $payload $mapping[$source]
            $null = [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entries[0], $target, $true)
        }
    } finally { $zip.Dispose() }
    [IO.File]::Delete($archive)
}

try {
    $sdr = @{}
    foreach ($name in 'sdr2hdr_cli.exe','ffmpeg.exe','ffprobe.exe','nvngx_vsr.dll','nvngx_truehdr.dll') {
        $sdr[$name] = 'bin/' + $name
    }
    Install-ArchiveFiles 'https://github.com/zizhenliu0427/sdr2hdr/releases/download/v1.1.0/sdr2hdr_v1.1.zip' '41015cd3155e97a52da99d2d039ced87bde8af828bef41f430f31eff9055548d' $sdr
    Install-ArchiveFiles 'https://files.pythonhosted.org/packages/59/df/e7c3a360be4f7b93cee39271b792669baeb3846c58a4df6dfcf187a7ffab/nvidia_cuda_runtime_cu12-12.9.79-py3-none-win_amd64.whl' '8e018af8fa02363876860388bd10ccb89eb9ab8fb0aa749aaf58430a9f7c4891' @{
        'nvidia/cuda_runtime/bin/cudart64_12.dll'='bin/cudart64_12.dll'
    }
    $nvenc = Get-VerifiedArchive 'https://github.com/rigaya/NVEnc/releases/download/9.41/NVEncC_9.41_x64.7z' '718ce2084d45e56706149bb9cf5d96a5ea697c4c5f75a2f256ebf79918259da2'
    $encodeDir = Join-Path $payload 'bin/encode'
    $null = [IO.Directory]::CreateDirectory($encodeDir)
    $members = @('NVEncC64.exe','NVEncNVSDKNGX.dll','nvngx_truehdr.dll','nvngx_vsr.dll',
        'msvcp140.dll','vcruntime140.dll','vcruntime140_1.dll','avcodec-63.dll','avformat-63.dll',
        'avutil-61.dll','swresample-7.dll','avfilter-12.dll','avdevice-63.dll','libass-9.dll',
        'libplacebo-360.dll','nvrtc64_112_0.dll','nvrtc-builtins64_118.dll','cudart64_110.dll')
    & "$env:SystemRoot\System32\tar.exe" -xf $nvenc -C $encodeDir @members
    if ($LASTEXITCODE -ne 0) { throw 'Could not extract the NVEncC runtime with Windows tar.exe.' }
    foreach ($name in $members) {
        if (!(Test-Path -LiteralPath (Join-Path $encodeDir $name) -PathType Leaf)) { throw ('Missing component: ' + $name) }
    }
    [IO.File]::Delete($nvenc)
    $vfx = @{}
    foreach ($name in 'cudart64_12.dll','nppc64_12.dll','nppial64_12.dll','nppicc64_12.dll',
        'nppidei64_12.dll','nppif64_12.dll','nppig64_12.dll','nppim64_12.dll','nppist64_12.dll',
        'nppitc64_12.dll','NVCVImage.dll','nvngxruntime.dll','nvVFXVideoFrameGeneration.dll','NVVideoEffects.dll') {
        $vfx[('nvvfx/libs/' + $name)] = 'bin/vfg/' + $name
    }
    Install-ArchiveFiles 'https://pypi.nvidia.com/nvidia-vfx/nvidia_vfx-0.2.0.0-cp312-abi3-win_amd64.whl' 'cebaa12411f7e22b39293205c5c4d07b8073d14ca1e0e7727ee4375207a36445' $vfx
    $shared = @{}
    foreach ($name in 'avcodec-62.dll','avformat-62.dll','avutil-60.dll','swresample-6.dll') {
        $shared[('bin/' + $name)] = 'bin/vfg/' + $name
    }
    Install-ArchiveFiles 'https://github.com/GyanD/codexffmpeg/releases/download/8.1.2/ffmpeg-8.1.2-full_build-shared.zip' '274923c68904a9b76c73b908f57923dafba81155856cd742138515ded570d066' $shared
    # Only commit verified files after every dependency has been downloaded.
    foreach ($file in Get-ChildItem -LiteralPath $payload -Recurse -File) {
        $relative = $file.FullName.Substring($payload.Length+1)
        $target = Join-Path $PSScriptRoot $relative
        $null = [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))
        [IO.File]::Copy($file.FullName, $target, $true)
    }
    Write-Host 'Ready. Open RTX Video Studio.exe.'
} finally {
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
    $resolved = [IO.Path]::GetFullPath($stage)
    if ([IO.Path]::GetDirectoryName($resolved) -eq $tempRoot -and [IO.Path]::GetFileName($resolved) -match '^rtx-video-runtime-[0-9a-f]{32}$') {
        Remove-Item -LiteralPath $resolved -Recurse -Force -ErrorAction SilentlyContinue
    }
}

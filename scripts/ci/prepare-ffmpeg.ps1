[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Destination,
    [string]$ArchivePath
)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$dependency = (Get-Content "$root/third_party/versions.json" -Raw | ConvertFrom-Json).ffmpeg
New-Item -ItemType Directory -Force $Destination | Out-Null
$Destination = (Resolve-Path $Destination).Path
if (-not $ArchivePath) {
    $ArchivePath = Join-Path $Destination "ffmpeg-$($dependency.version)-full_build-shared.7z"
    Invoke-WebRequest -Uri $dependency.archive -OutFile $ArchivePath
}
if ((Get-FileHash -LiteralPath $ArchivePath -Algorithm SHA256).Hash -ne $dependency.sha256) {
    throw 'FFmpeg archive SHA-256 mismatch; refusing to extract or execute it.'
}
# Windows' bundled bsdtar supports 7z; no extra package manager is needed.
& tar -xf $ArchivePath -C $Destination
if ($LASTEXITCODE -ne 0) { throw 'FFmpeg archive extraction failed.' }
$bin = Join-Path $Destination "ffmpeg-$($dependency.version)-full_build-shared/bin"
$executable = Join-Path $bin 'ffmpeg.exe'
if (-not (Test-Path -LiteralPath $executable)) { throw 'FFmpeg CLI is missing from the archive.' }
& $executable -hide_banner -version
if ($LASTEXITCODE -ne 0) { throw 'FFmpeg CLI could not start.' }
$env:PATH = "$bin;$env:PATH"
if ($env:GITHUB_PATH) { Add-Content -LiteralPath $env:GITHUB_PATH -Value $bin -Encoding utf8 }
Write-Host "Verified FFmpeg $($dependency.version): $executable"

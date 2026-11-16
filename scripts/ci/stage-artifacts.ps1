[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('Core', 'Gui')][string]$Kind,
    [Parameter(Mandatory)][ValidateSet('Release', 'Debug')][string]$Configuration,
    [Parameter(Mandatory)][string]$Root,
    [string]$BuildDirectory = 'build'
)
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path $Root).Path
if (-not [IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory = Join-Path $Root $BuildDirectory }
$destination = Join-Path $Root "out/ci-package/$Configuration"
if (Test-Path -LiteralPath $destination) {
    throw "Artifact staging directory already exists: $destination. Use a fresh build checkout."
}
New-Item -ItemType Directory -Force $destination | Out-Null
$bin = Join-Path $BuildDirectory "bin/$Configuration"
if ($Kind -eq 'Core') {
    New-Item -ItemType Directory -Force "$destination/lib", "$destination/bin" | Out-Null
    Copy-Item -LiteralPath "$BuildDirectory/$Configuration/livewallpaper.lib" -Destination "$destination/lib/"
    Copy-Item -LiteralPath "$Root/include" -Destination $destination -Recurse
    Get-ChildItem -LiteralPath $bin -Filter '*.dll' -File |
        Copy-Item -Destination "$destination/bin/"
    Copy-Item -LiteralPath "$bin/livewallpaper_example.exe" -Destination "$destination/bin/"
} else {
    if (-not (Test-Path -LiteralPath "$bin/lwallpaper.exe")) { throw 'GUI executable is missing.' }
    Get-ChildItem -LiteralPath $bin |
        Where-Object { $_.Name -notlike '*tests*' -and $_.Extension -ne '.pdb' } |
        Copy-Item -Destination $destination -Recurse
}
Copy-Item -LiteralPath "$Root/LICENSE" -Destination $destination
Copy-Item -LiteralPath "$Root/readme.md" -Destination "$destination/README.md"
New-Item -ItemType Directory -Force "$destination/licenses/ffmpeg", "$destination/licenses/SDL3" | Out-Null
Copy-Item -LiteralPath "$Root/third_party/ffmpeg/LICENSE" -Destination "$destination/licenses/ffmpeg/"
Copy-Item -LiteralPath "$Root/third_party/SDL3/LICENSE.txt" -Destination "$destination/licenses/SDL3/"
Copy-Item -LiteralPath "$Root/third_party/versions.json" -Destination $destination
Write-Host "Staged $Kind $Configuration artifacts at $destination"

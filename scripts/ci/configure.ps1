[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('Core', 'Gui')][string]$Kind,
    [Parameter(Mandatory)][string]$Root,
    [string]$QtRoot,
    [string]$BuildDirectory = 'build'
)
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path $Root).Path
if (-not [IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory = Join-Path $Root $BuildDirectory }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer/vswhere.exe is missing.' }
$instances = @(& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json |
    ConvertFrom-Json)
if ($LASTEXITCODE -ne 0 -or $instances.Count -ne 1) {
    throw 'Cannot locate an installed Visual Studio C++ toolchain.'
}
$instance = $instances[0]
$major = ([version]$instance.installationVersion).Major
$generator = switch ($major) {
    17 { 'Visual Studio 17 2022' }
    18 { 'Visual Studio 18 2026' }
    default { throw "Unsupported Visual Studio major version: $major" }
}
$arguments = @('-S', $Root, '-B', $BuildDirectory, '-G', $generator, '-A', 'x64',
    "-DCMAKE_GENERATOR_INSTANCE=$($instance.installationPath)", '-DBUILD_TESTING=ON')
if ($Kind -eq 'Gui') {
    if (-not $QtRoot) { throw 'GUI configuration requires -QtRoot.' }
    $arguments += @("-DQt6_ROOT=$QtRoot", '-DLWALLPAPER_DEPLOY_RUNTIME=ON')
} else {
    $arguments += '-DLIVEWALLPAPER_BUILD_EXAMPLES=ON'
}
Write-Host "Using $generator at $($instance.installationPath)"
& cmake @arguments
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }

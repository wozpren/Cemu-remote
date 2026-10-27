# SPDX-License-Identifier: GPL-3.0-only
param(
    [ValidateSet('plugin', 'cemu', 'all')][string]$Component = 'all',
    [ValidateSet('Release', 'RelWithDebInfo', 'Debug')][string]$Configuration = 'Release',
    [ValidateRange(1, 64)][int]$Jobs = 8,
    [switch]$Direct,
    [switch]$UseSystemTools,
    [switch]$UsePowerShellDownloads,
    [switch]$FreshConfigure,
    [switch]$ConfigureOnly
)
$ErrorActionPreference = 'Stop'
$pluginRoot = Split-Path $PSScriptRoot -Parent
$repoRoot = Split-Path (Split-Path $pluginRoot -Parent) -Parent
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $visualStudio) { throw 'Visual Studio C++ tools are unavailable.' }
& (Join-Path $visualStudio 'Common7/Tools/Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation -NoLogo
$cmake = Join-Path $visualStudio 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$ninja = Join-Path $visualStudio 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
if (-not (Test-Path -LiteralPath $cmake)) { $cmake = (Get-Command cmake.exe).Source }
if (-not (Test-Path -LiteralPath $ninja)) { $ninja = (Get-Command ninja.exe).Source }
$vcpkg = Join-Path $repoRoot 'dependencies/vcpkg/vcpkg.exe'
if (-not (Test-Path -LiteralPath $vcpkg)) { throw 'Run dependencies/vcpkg/bootstrap-vcpkg.bat -disableMetrics first.' }
$logDirectory = Join-Path $repoRoot "out/build-logs/gamepad-$Configuration"
New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
$savedEnvironment = @{}
$processVariables = @('HTTP_PROXY', 'HTTPS_PROXY', 'ALL_PROXY', 'NO_PROXY', 'GIT_CONFIG_COUNT', 'GIT_CONFIG_KEY_0',
    'GIT_CONFIG_VALUE_0', 'GIT_CONFIG_KEY_1', 'GIT_CONFIG_VALUE_1', 'VCPKG_DISABLE_METRICS',
    'VCPKG_MAX_CONCURRENCY', 'VCPKG_VISUAL_STUDIO_PATH', 'VCPKG_FORCE_SYSTEM_BINARIES', 'PATH', 'X_VCPKG_ASSET_SOURCES', 'VSLANG')
foreach ($name in $processVariables) { $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
function Invoke-BuildCMake {
    param([string]$LogName, [string[]]$Arguments)
    & $cmake @Arguments 2>&1 | Tee-Object -FilePath (Join-Path $logDirectory $LogName)
    if ($LASTEXITCODE -ne 0) { throw "CMake failed; see $logDirectory/$LogName" }
}
try {
    $env:VCPKG_DISABLE_METRICS = '1'
    $env:VCPKG_MAX_CONCURRENCY = [string]$Jobs
    $env:VCPKG_VISUAL_STUDIO_PATH = $visualStudio
    # CMake/Ninja must recognize cl's include prefix to track header changes.
    # A fixed diagnostic language avoids corrupting that prefix across code pages.
    $env:VSLANG = '1033'
    if ($UseSystemTools) {
        $env:VCPKG_FORCE_SYSTEM_BINARIES = '1'
        $env:PATH = (Split-Path $cmake -Parent) + ';' + (Split-Path $ninja -Parent) + ';' + $env:PATH
        $sevenZip = Join-Path $env:ProgramFiles '7-Zip'
        if (Test-Path -LiteralPath (Join-Path $sevenZip '7z.exe')) { $env:PATH = $sevenZip + ';' + $env:PATH }
    }
    if ($UsePowerShellDownloads) {
        $powerShell = Join-Path $PSHOME 'pwsh.exe'
        if (-not (Test-Path -LiteralPath $powerShell)) { throw 'PowerShell 7 is required for this download mode.' }
        $fetchScript = Join-Path $PSScriptRoot 'download-vcpkg-asset.ps1'
        $env:X_VCPKG_ASSET_SOURCES = 'clear;x-script,"' + $powerShell + '" -NoProfile -File "' +
            $fetchScript + '" -Url {url} -Destination {dst} -Sha512 {sha512};x-block-origin'
    }
    if ($Direct) {
        # Only child build processes bypass the machine's proxy configuration.
        foreach ($name in @('HTTP_PROXY', 'HTTPS_PROXY', 'ALL_PROXY')) { [Environment]::SetEnvironmentVariable($name, $null, 'Process') }
        # vcpkg may re-read the Windows system proxy after these are removed.
        $env:NO_PROXY = '*'
        $env:GIT_CONFIG_COUNT = '2'
        $env:GIT_CONFIG_KEY_0 = 'http.proxy'
        $env:GIT_CONFIG_VALUE_0 = ''
        $env:GIT_CONFIG_KEY_1 = 'https.proxy'
        $env:GIT_CONFIG_VALUE_1 = ''
    }
    $common = @('-G', 'Ninja', "-DCMAKE_MAKE_PROGRAM=$ninja", "-DCMAKE_BUILD_TYPE=$Configuration",
        "-DCMAKE_PROJECT_INCLUDE=$PSScriptRoot/msvc-dependencies.cmake",
        "-DCMAKE_TOOLCHAIN_FILE=$repoRoot/dependencies/vcpkg/scripts/buildsystems/vcpkg.cmake",
        '-DVCPKG_TARGET_TRIPLET=x64-windows-static', '-DCMAKE_POLICY_VERSION_MINIMUM=3.5')
    if ($FreshConfigure) { $common += '--fresh' }
    if ($Component -in @('plugin', 'all')) {
        $pluginBuild = Join-Path $pluginRoot "build/$Configuration"
        Invoke-BuildCMake 'plugin-configure.log' (@('-S', $pluginRoot, '-B', $pluginBuild) + $common)
        if (-not $ConfigureOnly) {
            Invoke-BuildCMake 'plugin-build.log' @('--build', $pluginBuild, '--parallel', [string]$Jobs)
            Invoke-BuildCMake 'plugin-install.log' @('--install', $pluginBuild, '--prefix', "$pluginRoot/out/gamepad-stage")
        }
    }
    if ($Component -in @('cemu', 'all')) {
        $cemuBuild = Join-Path $repoRoot "out/build/GamePad-$Configuration"
        # The two projects pin different dependency baselines. Keep temporary
        # package trees separate so their installs can run concurrently.
        $cemuInstallOptions = "-DVCPKG_INSTALL_OPTIONS=--x-buildtrees-root=$repoRoot/out/vcpkg-cemu-buildtrees;--x-packages-root=$repoRoot/out/vcpkg-cemu-packages"
        Invoke-BuildCMake 'cemu-configure.log' (@('-S', $repoRoot, '-B', $cemuBuild,
            '-DENABLE_GAMEPAD_EXTENSION=ON', '-DENABLE_VULKAN=ON', $cemuInstallOptions) + $common)
        if (-not $ConfigureOnly) {
            Invoke-BuildCMake 'cemu-build.log' @('--build', $cemuBuild, '--target', 'CemuBin', '--parallel', [string]$Jobs)
        }
    }
}
finally {
    foreach ($name in $processVariables) { [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process') }
}

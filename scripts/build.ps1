#requires -version 5.1
[CmdletBinding()]
param(
    [string]$OutputDirectory,
    [ValidateSet('Application', 'UnitTests', 'ExplorerIntegration')]
    [string]$Target = 'Application',
    [switch]$UpdateRelease
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

if ($UpdateRelease -and $Target -ne 'Application') {
    throw '-UpdateRelease 仅支持 Application 构建目标。'
}

function Get-LatestVersionedDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$ParentPath,
        [Parameter(Mandatory = $true)][scriptblock]$IsUsable
    )

    if (-not (Test-Path -LiteralPath $ParentPath -PathType Container)) {
        return $null
    }

    return Get-ChildItem -LiteralPath $ParentPath -Directory |
        Where-Object {
            $parsedVersion = $null
            [version]::TryParse($_.Name, [ref]$parsedVersion) -and (& $IsUsable $_.FullName)
        } |
        Sort-Object { [version]$_.Name } -Descending |
        Select-Object -First 1
}

$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $projectRoot 'build'
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)

$targetFiles = @{
    Application = @('src\CopyPathMenu.cpp', 'copy-path.exe', 'WINDOWS')
    UnitTests = @('tests\CopyPathMenuTests.cpp', 'CopyPathMenuTests.exe', 'CONSOLE')
    ExplorerIntegration = @('tests\ExplorerIntegration.cpp', 'ExplorerIntegration.exe', 'CONSOLE')
}
$targetSettings = $targetFiles[$Target]
$sourcePath = Join-Path $projectRoot $targetSettings[0]
if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
    throw "找不到源文件：$sourcePath"
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    throw '找不到 vswhere.exe；请安装包含“使用 C++ 的桌面开发”组件的 Visual Studio Build Tools。'
}

$visualStudioPath = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
if ([string]::IsNullOrWhiteSpace($visualStudioPath)) {
    throw '找不到 Visual C++ x64 编译工具；请安装 Visual Studio Build Tools 的“使用 C++ 的桌面开发”组件。'
}

$toolsetDirectory = Get-LatestVersionedDirectory `
    -ParentPath (Join-Path $visualStudioPath 'VC\Tools\MSVC') `
    -IsUsable { param($path) Test-Path -LiteralPath (Join-Path $path 'bin\Hostx64\x64\cl.exe') -PathType Leaf }
if ($null -eq $toolsetDirectory) {
    throw "Visual Studio 安装中找不到 x64 cl.exe：$visualStudioPath"
}

$windowsKitsRoot = $null
$installedRoots = 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots'
if (Test-Path -LiteralPath $installedRoots) {
    $windowsKitsRoot = (Get-Item -LiteralPath $installedRoots).GetValue('KitsRoot10')
}
if ([string]::IsNullOrWhiteSpace($windowsKitsRoot)) {
    $windowsKitsRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
}

$sdkDirectory = Get-LatestVersionedDirectory `
    -ParentPath (Join-Path $windowsKitsRoot 'Include') `
    -IsUsable {
        param($path)
        $version = Split-Path -Path $path -Leaf
        (Test-Path -LiteralPath (Join-Path $path 'um\windows.h') -PathType Leaf) -and
        (Test-Path -LiteralPath (Join-Path $windowsKitsRoot "Lib\$version\um\x64\ole32.lib") -PathType Leaf)
    }
if ($null -eq $sdkDirectory) {
    throw "找不到可用的 Windows 10/11 x64 SDK：$windowsKitsRoot"
}

$sdkVersion = $sdkDirectory.Name
$compilerPath = Join-Path $toolsetDirectory.FullName 'bin\Hostx64\x64\cl.exe'
$rcPath = Join-Path $windowsKitsRoot "bin\$sdkVersion\x64\rc.exe"
if (-not (Test-Path -LiteralPath $rcPath -PathType Leaf)) {
    $rcSdkDir = Get-LatestVersionedDirectory `
        -ParentPath (Join-Path $windowsKitsRoot 'bin') `
        -IsUsable { param($path) Test-Path -LiteralPath (Join-Path $path 'x64\rc.exe') -PathType Leaf }
    if ($null -ne $rcSdkDir) {
        $rcPath = Join-Path $rcSdkDir.FullName 'x64\rc.exe'
    }
}

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$exePath = Join-Path $OutputDirectory $targetSettings[1]
$objectPath = Join-Path $OutputDirectory ($Target + '.obj')
$resPath = Join-Path $OutputDirectory ($Target + '.res')
$rcSourcePath = Join-Path $projectRoot 'src\CopyPathMenu.rc'

$compilerArguments = @(
    '/nologo',
    '/std:c++17',
    '/permissive-',
    '/EHsc',
    '/O2',
    '/W4',
    '/MT',
    '/utf-8',
    '/DUNICODE',
    '/D_UNICODE',
    "/Fe:$exePath",
    "/Fo:$objectPath",
    $sourcePath,
    ('/I' + (Join-Path $toolsetDirectory.FullName 'include')),
    ('/I' + (Join-Path $sdkDirectory.FullName 'ucrt')),
    ('/I' + (Join-Path $sdkDirectory.FullName 'shared')),
    ('/I' + (Join-Path $sdkDirectory.FullName 'um')),
    ('/I' + (Join-Path $sdkDirectory.FullName 'winrt')),
    '/link',
    ('/SUBSYSTEM:' + $targetSettings[2]),
    '/OPT:REF',
    '/OPT:ICF',
    '/DYNAMICBASE',
    '/HIGHENTROPYVA',
    '/NXCOMPAT',
    ('/LIBPATH:' + (Join-Path $toolsetDirectory.FullName 'lib\x64')),
    ('/LIBPATH:' + (Join-Path $windowsKitsRoot "Lib\$sdkVersion\ucrt\x64")),
    ('/LIBPATH:' + (Join-Path $windowsKitsRoot "Lib\$sdkVersion\um\x64")),
    'ole32.lib',
    'oleaut32.lib',
    'shell32.lib',
    'user32.lib',
    'uuid.lib',
    'uuid.lib',
    'propsys.lib',
    'advapi32.lib',
    'comctl32.lib',
    'gdi32.lib'
)

try {
    if ($Target -eq 'Application' -and (Test-Path -LiteralPath $rcSourcePath -PathType Leaf)) {
        if (-not (Test-Path -LiteralPath $rcPath -PathType Leaf)) {
            throw "找不到 Windows SDK 资源编译器 rc.exe：$rcPath"
        }
        $rcArguments = @(
            '/nologo',
            "/fo$resPath",
            ('/I' + (Join-Path $sdkDirectory.FullName 'um')),
            ('/I' + (Join-Path $sdkDirectory.FullName 'shared')),
            ('/I' + (Join-Path $projectRoot 'src')),
            $rcSourcePath
        )
        $rcOutput = & $rcPath @rcArguments 2>&1
        if ($LASTEXITCODE -ne 0) {
            $rcOutput | ForEach-Object { Write-Output $_ }
            throw "RC 资源编译失败，退出码：$LASTEXITCODE"
        }
        $compilerArguments += $resPath
    }

    $compileOutput = & $compilerPath @compilerArguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        $compileOutput | ForEach-Object { Write-Output $_ }
        throw "C++ 编译失败，退出码：$LASTEXITCODE"
    }

    $artifact = Get-Item -LiteralPath $exePath
    $sha256 = (Get-FileHash -LiteralPath $exePath -Algorithm SHA256).Hash.ToUpperInvariant()

    if ($UpdateRelease) {
        $releaseDir = Join-Path $projectRoot 'release'
        New-Item -ItemType Directory -Path $releaseDir -Force | Out-Null
        $releaseExe = Join-Path $releaseDir 'copy-path.exe'
        $checksumFile = Join-Path $releaseDir 'SHA256SUMS.txt'
        Copy-Item -LiteralPath $exePath -Destination $releaseExe -Force
        $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
        [IO.File]::WriteAllText($checksumFile, ("$sha256  copy-path.exe`r`n"), $utf8NoBom)
    }

    [pscustomobject]@{
        ExePath = $artifact.FullName
        Length = $artifact.Length
        Sha256 = $sha256
        LastWriteTime = $artifact.LastWriteTime
        Compiler = $compilerPath
        WindowsSdkVersion = $sdkVersion
        ReleaseUpdated = [bool]$UpdateRelease
    }
}
finally {
    if (Test-Path -LiteralPath $objectPath -PathType Leaf) {
        Remove-Item -LiteralPath $objectPath -Force
    }
    if (Test-Path -LiteralPath $resPath -PathType Leaf) {
        Remove-Item -LiteralPath $resPath -Force
    }
}

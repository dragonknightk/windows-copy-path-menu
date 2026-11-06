#requires -version 5.1
<#
.SYNOPSIS
    运行 CopyPathMenu 全部自动化测试与发布包完整性核验。
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildScript = Join-Path $PSScriptRoot 'build.ps1'
$installTests = Join-Path $projectRoot 'tests\Install.Tests.ps1'
$releaseExe = Join-Path $projectRoot 'release\copy-path.exe'
$checksumFile = Join-Path $projectRoot 'release\SHA256SUMS.txt'

Write-Output '=== [1/3] 编译并运行 C++ 源码与 COM 契约回归测试 ==='
$unitBuild = & $buildScript -Target UnitTests
& $unitBuild.ExePath
if ($LASTEXITCODE -ne 0) {
    throw "C++ 源码回归测试失败，退出码：$LASTEXITCODE"
}

Write-Output ''
Write-Output '=== [2/3] 运行安装、升级、防篡改与事务回滚测试 ==='
& $installTests

Write-Output ''
Write-Output '=== [3/3] 核验发布包 SHA-256、版本资源与命令行接口 ==='
if (-not (Test-Path -LiteralPath $releaseExe -PathType Leaf)) {
    throw "缺少发布程序：$releaseExe"
}
if (-not (Test-Path -LiteralPath $checksumFile -PathType Leaf)) {
    throw "缺少校验文件：$checksumFile"
}
$checksumLine = ([IO.File]::ReadAllText($checksumFile, [Text.Encoding]::UTF8) -replace '(\r?\n)?$', '')
if ($checksumLine -cnotmatch '^(?<hash>[0-9A-Fa-f]{64})[ \t]+\*?copy-path\.exe$') {
    throw "SHA256SUMS.txt 格式无效：$checksumFile"
}
$expectedHash = $Matches['hash'].ToUpperInvariant()
$actualHash = (Get-FileHash -LiteralPath $releaseExe -Algorithm SHA256).Hash.ToUpperInvariant()
if ($actualHash -cne $expectedHash) {
    throw "发布程序 SHA-256 不匹配：期望 $expectedHash，实际 $actualHash"
}

$versionInfo = (Get-Item -LiteralPath $releaseExe).VersionInfo
if ($versionInfo.ProductName -ne 'DragonKnightK.CopyPathMenu') {
    throw "发布程序缺少预期的 ProductName 版本资源：$($versionInfo.ProductName)"
}

& $releaseExe --version | Out-Null
if ($LASTEXITCODE -ne 0) {
    throw "copy-path.exe --version 退出码异常：$LASTEXITCODE"
}

Write-Output "通过：release\copy-path.exe SHA-256 ($actualHash) 与版本资源核验完成。"
Write-Output ''
Write-Output '全部自动化测试与发布包核验均已通过！'

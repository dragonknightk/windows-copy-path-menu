#requires -version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$CandidateExe,
    [Parameter(Mandatory = $true)][switch]$Interactive,
    [ValidateRange(1, 6)][int]$Case,
    [string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) { $OutputDirectory = Join-Path $projectRoot 'build' }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if (-not $Interactive) { throw '本测试需要外部操作员逐个点击真实 Explorer 菜单，请传入 -Interactive。' }
$CandidateExe = (Resolve-Path -LiteralPath $CandidateExe).Path
if (-not (Test-Path -LiteralPath $CandidateExe -PathType Leaf)) { throw "找不到候选程序：$CandidateExe" }
# 使用现有菜单注册和已打开的 C:\Windows 视图；测试不安装候选程序。
& (Join-Path $PSScriptRoot 'build.ps1') -Target ExplorerIntegration -OutputDirectory $OutputDirectory
if (-not $?) { throw 'Explorer 集成测试编译失败。' }
$helper = Join-Path $OutputDirectory 'ExplorerIntegration.exe'
$helperArguments = @($CandidateExe, '--interactive')
if ($PSBoundParameters.ContainsKey('Case')) { $helperArguments += @('--case', [string]$Case) }
& $helper @helperArguments
if ($LASTEXITCODE -ne 0) { throw "Explorer 真实菜单集成测试失败，退出码：$LASTEXITCODE" }

#requires -version 5.1
# 使用真实临时文件和内存注册表替身，不执行安装入口、不访问产品注册表。
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$installer = Join-Path (Split-Path $PSScriptRoot -Parent) 'scripts\install.ps1'
$tokens = $null
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($installer, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count -ne 0) { throw ($parseErrors | Out-String) }
$functionNames = @('Assert-AvailableForCurrentProduct', 'Assert-ExistingInstalledExeBelongsToProduct', 'Backup-InstalledExe', 'Restore-PreviousState')
foreach ($name in $functionNames) {
    $definition = $ast.Find({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name }, $true)
    if ($null -eq $definition) { throw "未找到安装函数：$name" }
    . ([scriptblock]::Create($definition.Extent.Text))
}
# 覆盖入口中的实际失败处理块，防止空回滚结果在 StrictMode 下触发二次异常。
$installTry = $ast.EndBlock.Statements | Where-Object { $_ -is [Management.Automation.Language.TryStatementAst] } | Select-Object -Last 1
$failureBody = $installTry.CatchClauses[0].Body.Extent.Text
$failureHandler = [scriptblock]::Create($failureBody.Substring(1, $failureBody.Length - 2))
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('CopyPathMenu-InstallTest-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
$installDirectory = Join-Path $testRoot 'installed'
$temporaryDirectory = Join-Path $testRoot 'backup'
New-Item -ItemType Directory -Path $installDirectory, $temporaryDirectory | Out-Null
$installedExe = Join-Path $installDirectory 'copy-path.exe'
$clsidPath = Join-Path $testRoot 'registry-clsid'
$localServerPath = Join-Path $clsidPath 'LocalServer32'
$ownerValueName = 'CopyPathMenuOwner'
$productOwner = 'DragonKnightK.CopyPathMenu'
$clsid = '{7E4DC989-2ABA-458A-94D3-A350A352C32E}'
$registry = @{}
$oldContents = '已安装的旧版本'
$registrySnapshots = @()
$stagedExe = $null
$fileSnapshot = $null
$installDirectoryExisted = $true
$mutationStarted = $true
$passed = 0
function Test-Path {
    param([string]$LiteralPath, [string]$PathType)
    if ($LiteralPath -eq $clsidPath -or $LiteralPath -eq $localServerPath) { return $registry.ContainsKey($LiteralPath) }
    if ($PathType) { return Microsoft.PowerShell.Management\Test-Path -LiteralPath $LiteralPath -PathType $PathType }
    return Microsoft.PowerShell.Management\Test-Path -LiteralPath $LiteralPath
}
function Get-RegistryValue {
    param([string]$Path, [string]$Name = '')
    return $registry[$Path][$Name]
}
function Reset-OldInstallation {
    [IO.File]::WriteAllText($installedExe, $oldContents)
    $script:oldHash = (Get-FileHash -LiteralPath $installedExe -Algorithm SHA256).Hash
    $script:releaseHash = 'A' * 64
    $script:registry = @{}
    $script:registry[$clsidPath] = @{ CopyPathMenuOwner = $productOwner; CopyPathMenuSha256 = $oldHash }
    $script:registry[$localServerPath] = @{ '' = ('"' + $installedExe + '"'); ServerExecutable = $installedExe }
}
function Assert-Gate {
    param([string]$Name, [bool]$ShouldPass, [scriptblock]$Arrange)
    Reset-OldInstallation
    & $Arrange
    $failure = $null
    try { Assert-AvailableForCurrentProduct -Path $clsidPath; Assert-ExistingInstalledExeBelongsToProduct } catch { $failure = $_ }
    if ($ShouldPass -and $null -ne $failure) { throw "$Name 意外拒绝：$failure" }
    if (-not $ShouldPass -and $null -eq $failure) { throw "$Name 未拒绝无效安装" }
    $script:passed++
    Write-Output "通过：$Name"
}
try {
    Assert-Gate '旧版本升级' $true { }
    Assert-Gate '相同版本重装' $true { $script:releaseHash = $oldHash }
    Assert-Gate '首次安装' $true { Remove-Item -LiteralPath $installedExe; $script:registry = @{} }
    Assert-Gate '缺少 CLSID' $false { $registry.Remove($clsidPath) }
    Assert-Gate '缺少所有者' $false { $registry[$clsidPath].Remove($ownerValueName) }
    Assert-Gate '其他所有者' $false { $registry[$clsidPath][$ownerValueName] = 'Other.Product' }
    Assert-Gate '缺少哈希记录' $false { $registry[$clsidPath].Remove('CopyPathMenuSha256') }
    Assert-Gate '哈希记录格式无效' $false { $registry[$clsidPath]['CopyPathMenuSha256'] = 'invalid' }
    Assert-Gate '哈希记录不匹配' $false { $registry[$clsidPath]['CopyPathMenuSha256'] = 'B' * 64 }
    Assert-Gate '旧文件被篡改' $false { [IO.File]::WriteAllText($installedExe, '篡改内容') }
    Assert-Gate '缺少服务器注册' $false { $registry.Remove($localServerPath) }
    Assert-Gate '服务器命令不匹配' $false { $registry[$localServerPath][''] = 'other.exe' }
    Assert-Gate '服务器路径不匹配' $false { $registry[$localServerPath]['ServerExecutable'] = 'other.exe' }
    Assert-Gate '部署路径是目录' $false { Remove-Item -LiteralPath $installedExe; New-Item -ItemType Directory -Path $installedExe | Out-Null }
    Remove-Item -LiteralPath $installedExe

    Reset-OldInstallation
    Assert-ExistingInstalledExeBelongsToProduct
    Backup-InstalledExe
    [IO.File]::WriteAllText($installedExe, '升级后的版本')
    $stagedExe = Join-Path $installDirectory 'pending.staged'
    [IO.File]::WriteAllText($stagedExe, '未完成暂存')
    # 注册表导入替身模拟已登记旧版本随备份一起恢复。
    $registry[$clsidPath]['CopyPathMenuSha256'] = $releaseHash
    $registrySnapshots = @([PSCustomObject]@{ Path = (Join-Path $testRoot 'registry-written'); Exists = $true; BackupFile = 'memory.reg' })
    function Invoke-RegExe {
        param([string]$Arguments)
        if ($Arguments -ne 'import "memory.reg"') { throw "意外导入参数：$Arguments" }
        $registry[$clsidPath]['CopyPathMenuSha256'] = $oldHash
        return [PSCustomObject]@{ ExitCode = 0 }
    }
    $failure = $null
    try { try { throw '模拟升级注册失败' } catch { & $failureHandler } } catch { $failure = $_.Exception.Message }
    if ($failure -ne 'CopyPathMenu 安装失败：模拟升级注册失败；已完成回滚。') { throw "升级失败处理结果异常：$failure" }
    if ([IO.File]::ReadAllText($installedExe) -cne $oldContents) { throw '回滚未恢复旧版本文件' }
    if (Test-Path -LiteralPath $stagedExe) { throw '回滚未清理暂存文件' }
    Assert-ExistingInstalledExeBelongsToProduct
    $passed++
    Write-Output '通过：升级失败恢复旧文件、旧哈希登记及原始错误'

    function Invoke-RegExe { param([string]$Arguments) return [PSCustomObject]@{ ExitCode = 1 } }
    $failure = $null
    try { try { throw '模拟升级注册失败' } catch { & $failureHandler } } catch { $failure = $_.Exception.Message }
    if ($failure -notlike '*模拟升级注册失败；回滚失败：*reg.exe import 退出码：1*') { throw "未保留回滚失败信息：$failure" }
    $passed++
    Write-Output '通过：升级失败时报告注册表恢复错误'
    Write-Output "安装回归测试通过：$passed 项。"
}
finally {
    $resolvedRoot = [IO.Path]::GetFullPath($testRoot)
    $tempPrefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $resolvedRoot.StartsWith($tempPrefix, [StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path $resolvedRoot -Leaf) -notlike 'CopyPathMenu-InstallTest-*') { throw '测试临时目录越界，停止清理' }
    Remove-Item -LiteralPath $resolvedRoot -Recurse -Force
}

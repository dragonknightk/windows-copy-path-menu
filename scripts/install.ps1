#requires -version 5.1
<#
.SYNOPSIS
    安装机器级 CopyPathMenu。

.DESCRIPTION
    此脚本仅部署已封存的 release\copy-path.exe，并只写入带有
    CopyPathMenuOwner=DragonKnightK.CopyPathMenu 标记的产品专属注册表键及开机自启项。
#>

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$projectRoot = Split-Path -Path $PSScriptRoot -Parent
$productName = 'CopyPathMenu'
$productOwner = 'DragonKnightK.CopyPathMenu'
$verbName = 'DragonKnightK.CopyPathMenu'
$ownerValueName = 'CopyPathMenuOwner'
$clsid = '{7E4DC989-2ABA-458A-94D3-A350A352C32E}'
$releaseExe = Join-Path -Path $projectRoot -ChildPath 'release\copy-path.exe'
$checksumFile = Join-Path -Path $projectRoot -ChildPath 'release\SHA256SUMS.txt'
$installDirectory = Join-Path -Path $env:ProgramFiles -ChildPath $productName
$installedExe = Join-Path -Path $installDirectory -ChildPath 'copy-path.exe'
$classesRoot = 'HKLM:\Software\Classes'
$runKeyPath = 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Run'
$runValueName = $productOwner
$logDirectory = Join-Path -Path $env:ProgramData -ChildPath $productName
$transcriptStarted = $false
$temporaryDirectory = $null
$stagedExe = $null
$replaceBackupFile = $null
$mutationStarted = $false
$installDirectoryExisted = $false
$registrySnapshots = @()
$runValueSnapshot = $null
$fileSnapshot = $null
$releaseHash = $null

function Assert-ElevatedNativePowerShell {
    if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) {
        throw '在 64 位 Windows 上必须使用 C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe 运行此脚本；32 位 PowerShell 会写入错误的注册表视图。'
    }

    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw '需要以管理员身份运行此安装脚本。'
    }
}

function Get-RegistryValue {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [string]$Name = ''
    )

    return (Get-Item -LiteralPath $Path).GetValue($Name)
}

function Set-RegistryString {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Value,
        [string]$Name = ''
    )

    if ([string]::IsNullOrEmpty($Name)) {
        Set-Item -LiteralPath $Path -Value $Value
    }
    else {
        New-ItemProperty -LiteralPath $Path -Name $Name -Value $Value -PropertyType String -Force | Out-Null
    }
}

function Invoke-RegExe {
    param([Parameter(Mandatory = $true)][string]$Arguments)

    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = Join-Path -Path $env:WINDIR -ChildPath 'System32\reg.exe'
    $startInfo.Arguments = $Arguments
    $startInfo.UseShellExecute = $false
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.CreateNoWindow = $true

    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        throw '无法启动 reg.exe。'
    }

    $standardOutput = $process.StandardOutput.ReadToEnd()
    $standardError = $process.StandardError.ReadToEnd()
    $process.WaitForExit()
    return [PSCustomObject]@{
        ExitCode = $process.ExitCode
        StandardOutput = $standardOutput
        StandardError = $standardError
    }
}

function Assert-ReleasePackage {
    if (-not (Test-Path -LiteralPath $releaseExe -PathType Leaf)) {
        throw "找不到封存的发布文件：$releaseExe"
    }
    if (-not (Test-Path -LiteralPath $checksumFile -PathType Leaf)) {
        throw "找不到 SHA-256 校验文件：$checksumFile"
    }

    $checksumContent = [IO.File]::ReadAllText($checksumFile, [Text.Encoding]::UTF8)
    $checksumLine = $checksumContent -replace '(\r?\n)?$', ''
    if ($checksumLine -cnotmatch '^(?<hash>[0-9A-Fa-f]{64})[ \t]+\*?copy-path\.exe$') {
        throw "SHA-256 校验文件格式无效：$checksumFile"
    }

    $expectedHash = $Matches['hash'].ToUpperInvariant()
    $actualHash = (Get-FileHash -LiteralPath $releaseExe -Algorithm SHA256).Hash.ToUpperInvariant()
    if ($actualHash -ne $expectedHash) {
        throw "封存的发布文件 SHA-256 校验失败：$releaseExe"
    }

    $script:releaseHash = $expectedHash
}

function Assert-ExistingInstalledExeBelongsToProduct {
    if (-not (Test-Path -LiteralPath $installedExe)) {
        return
    }
    if (-not (Test-Path -LiteralPath $installedExe -PathType Leaf)) {
        throw "部署文件路径不是文件：$installedExe"
    }

    # 升级时以已安装版本的登记信息确认归属，新发布文件已单独校验。
    if (-not (Test-Path -LiteralPath $clsidPath)) {
        throw "部署文件存在但缺少产品 CLSID：$installedExe"
    }
    if ((Get-RegistryValue -Path $clsidPath -Name $ownerValueName) -ne $productOwner) {
        throw "部署文件存在但 CLSID 不属于 $productOwner：$installedExe"
    }
    $registeredHash = Get-RegistryValue -Path $clsidPath -Name 'CopyPathMenuSha256'
    if ($registeredHash -isnot [string] -or $registeredHash -cnotmatch '^[0-9A-F]{64}$') {
        throw "当前产品 CLSID 的 SHA-256 标记无效：$clsidPath"
    }
    $localServerPath = Join-Path -Path $clsidPath -ChildPath 'LocalServer32'
    if (-not (Test-Path -LiteralPath $localServerPath)) {
        throw "当前产品缺少 LocalServer32 注册：$clsidPath"
    }
    if ((Get-RegistryValue -Path $localServerPath) -ne ('"' + $installedExe + '"') -or
        (Get-RegistryValue -Path $localServerPath -Name 'ServerExecutable') -ne $installedExe) {
        throw "当前产品 LocalServer32 与部署文件路径不匹配：$installedExe"
    }

    $installedHash = (Get-FileHash -LiteralPath $installedExe -Algorithm SHA256).Hash.ToUpperInvariant()
    if ($installedHash -cne $registeredHash) {
        throw "部署文件与当前产品登记的 SHA-256 不匹配：$installedExe"
    }
}

function Stop-CurrentProductProcesses {
    $runningProcesses = Get-CimInstance -ClassName Win32_Process | Where-Object {
        $_.ExecutablePath -eq $installedExe
    }
    foreach ($runningProcess in $runningProcesses) {
        Stop-Process -Id $runningProcess.ProcessId -Force
        Write-Output "已停止当前程序进程：PID $($runningProcess.ProcessId)"
    }
}

function Assert-AvailableForCurrentProduct {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }
    if ((Get-RegistryValue -Path $Path -Name $ownerValueName) -ne $productOwner) {
        throw "注册表目标已存在且不属于 $productOwner：$Path"
    }
}

function Backup-RegistryState {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][int]$Index
    )

    $exists = Test-Path -LiteralPath $Path
    $backupFile = $null
    if ($exists) {
        $backupFile = Join-Path -Path $temporaryDirectory -ChildPath ('registry-{0:D2}.reg' -f $Index)
        $nativePath = $Path -replace '^HKLM:\\', 'HKLM\'
        $result = Invoke-RegExe -Arguments ('export "{0}" "{1}" /y' -f $nativePath, $backupFile)
        if ($result.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $backupFile -PathType Leaf)) {
            throw "无法导出注册表备份：$Path"
        }
    }

    $script:registrySnapshots += [PSCustomObject]@{
        Path = $Path
        Exists = $exists
        BackupFile = $backupFile
    }
}

function Backup-RunValueState {
    $existingValue = $null
    if (Test-Path -LiteralPath $runKeyPath) {
        $existingValue = (Get-Item -LiteralPath $runKeyPath).GetValue($runValueName, $null)
    }
    $script:runValueSnapshot = [PSCustomObject]@{
        Exists = ($null -ne $existingValue)
        Value = [string]$existingValue
    }
}

function Backup-InstalledExe {
    if (Test-Path -LiteralPath $installedExe) {
        if (-not (Test-Path -LiteralPath $installedExe -PathType Leaf)) {
            throw "部署文件路径不是文件：$installedExe"
        }

        $backupFile = Join-Path -Path $temporaryDirectory -ChildPath 'installed-copy-path.exe'
        Copy-Item -LiteralPath $installedExe -Destination $backupFile -Force
        $script:fileSnapshot = [PSCustomObject]@{ Exists = $true; BackupFile = $backupFile }
    }
    else {
        $script:fileSnapshot = [PSCustomObject]@{ Exists = $false; BackupFile = $null }
    }
}

function Restore-PreviousState {
    $rollbackErrors = @()
    if ($null -ne $stagedExe -and (Test-Path -LiteralPath $stagedExe -PathType Leaf)) {
        try {
            Remove-Item -LiteralPath $stagedExe -Force
        }
        catch {
            $rollbackErrors += "无法清理暂存部署文件 $stagedExe：$($_.Exception.Message)"
        }
    }
    foreach ($snapshot in ($registrySnapshots | Sort-Object { $_.Path.Length } -Descending)) {
        try {
            if (Test-Path -LiteralPath $snapshot.Path) {
                Remove-Item -LiteralPath $snapshot.Path -Recurse -Force
            }
        }
        catch {
            $rollbackErrors += "无法删除本次写入的注册表键 $($snapshot.Path)：$($_.Exception.Message)"
        }
    }

    foreach ($snapshot in $registrySnapshots) {
        if ($snapshot.Exists) {
            try {
                $result = Invoke-RegExe -Arguments ('import "{0}"' -f $snapshot.BackupFile)
                if ($result.ExitCode -ne 0) {
                    throw "reg.exe import 退出码：$($result.ExitCode)"
                }
            }
            catch {
                $rollbackErrors += "无法恢复注册表键 $($snapshot.Path)：$($_.Exception.Message)"
            }
        }
    }

    if ((Test-Path -LiteralPath 'variable:runValueSnapshot') -and $null -ne $runValueSnapshot) {
        try {
            if ($runValueSnapshot.Exists) {
                New-Item -Path $runKeyPath -Force | Out-Null
                Set-RegistryString -Path $runKeyPath -Name $runValueName -Value $runValueSnapshot.Value
            }
            elseif (Test-Path -LiteralPath $runKeyPath) {
                if ($null -ne (Get-Item -LiteralPath $runKeyPath).GetValue($runValueName, $null)) {
                    Remove-ItemProperty -LiteralPath $runKeyPath -Name $runValueName -Force
                }
            }
        }
        catch {
            $rollbackErrors += "无法恢复开机自启注册表项 $($runKeyPath)\$($runValueName)：$($_.Exception.Message)"
        }
    }

    try {
        if ($fileSnapshot.Exists) {
            Copy-Item -LiteralPath $fileSnapshot.BackupFile -Destination $installedExe -Force
        }
        elseif (Test-Path -LiteralPath $installedExe -PathType Leaf) {
            Remove-Item -LiteralPath $installedExe -Force
        }
    }
    catch {
        $rollbackErrors += "无法恢复部署文件 $installedExe：$($_.Exception.Message)"
    }

    if (-not $installDirectoryExisted -and (Test-Path -LiteralPath $installDirectory -PathType Container)) {
        try {
            if ((Get-ChildItem -LiteralPath $installDirectory -Force | Measure-Object).Count -eq 0) {
                Remove-Item -LiteralPath $installDirectory -Force
            }
        }
        catch {
            $rollbackErrors += "无法清理本次创建的安装目录 $installDirectory：$($_.Exception.Message)"
        }
    }

    return $rollbackErrors
}

function New-DelegatedCopyPathVerb {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$MenuText
    )

    New-Item -Path $Path -Force | Out-Null
    Set-RegistryString -Path $Path -Value $MenuText
    Set-RegistryString -Path $Path -Name 'MUIVerb' -Value $MenuText
    Set-RegistryString -Path $Path -Name 'Position' -Value 'Top'
    Set-RegistryString -Path $Path -Name 'MultiSelectModel' -Value 'Player'
    Set-RegistryString -Path $Path -Name $ownerValueName -Value $productOwner

    $commandPath = Join-Path -Path $Path -ChildPath 'command'
    New-Item -Path $commandPath -Force | Out-Null
    Set-RegistryString -Path $commandPath -Value ''
    Set-RegistryString -Path $commandPath -Name 'DelegateExecute' -Value $clsid
}

function Notify-ShellAssociationChanged {
    try {
        if (-not ('CopyPathMenu.NativeMethods' -as [type])) {
            Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace CopyPathMenu {
    public static class NativeMethods {
        [DllImport("shell32.dll")]
        public static extern void SHChangeNotify(uint eventId, uint flags, IntPtr item1, IntPtr item2);
    }
}
'@
        }
        [CopyPathMenu.NativeMethods]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)
    }
    catch {
        Write-Warning "安装已完成，但无法通知资源管理器刷新关联：$($_.Exception.Message)"
    }
}

try {
    Assert-ElevatedNativePowerShell
    Assert-ReleasePackage

    $menuText = ([char]0x590D).ToString() + ([char]0x5236).ToString() + ([char]0x8DEF).ToString() + ([char]0x5F84).ToString()
    $allFilesystemVerb = Join-Path -Path $classesRoot -ChildPath ("AllFilesystemObjects\shell\$verbName")
    $driveVerb = Join-Path -Path $classesRoot -ChildPath ("Drive\shell\$verbName")
    $backgroundVerb = Join-Path -Path $classesRoot -ChildPath ("Directory\Background\shell\$verbName")
    $clsidPath = Join-Path -Path $classesRoot -ChildPath ("CLSID\$clsid")
    $registryPaths = @($allFilesystemVerb, $driveVerb, $backgroundVerb, $clsidPath)

    foreach ($registryPath in $registryPaths) {
        Assert-AvailableForCurrentProduct -Path $registryPath
    }

    $installDirectoryExisted = Test-Path -LiteralPath $installDirectory
    if ($installDirectoryExisted -and -not (Test-Path -LiteralPath $installDirectory -PathType Container)) {
        throw "安装目录路径不是目录：$installDirectory"
    }
    Assert-ExistingInstalledExeBelongsToProduct

    New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
    $logFile = Join-Path -Path $logDirectory -ChildPath ('install-{0:yyyyMMdd-HHmmss}-{1}.log' -f (Get-Date), [Guid]::NewGuid().ToString('N'))
    Start-Transcript -LiteralPath $logFile -Append
    $transcriptStarted = $true

    $temporaryDirectory = Join-Path -Path ([IO.Path]::GetTempPath()) -ChildPath ('CopyPathMenu-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $temporaryDirectory -Force | Out-Null
    Backup-InstalledExe
    Backup-RunValueState
    $index = 0
    foreach ($registryPath in $registryPaths) {
        Backup-RegistryState -Path $registryPath -Index $index
        $index++
    }

    if ($fileSnapshot.Exists) {
        Stop-CurrentProductProcesses
    }
    $mutationStarted = $true
    New-Item -ItemType Directory -Path $installDirectory -Force | Out-Null
    $stagedExe = Join-Path -Path $installDirectory -ChildPath ('copy-path.exe.' + [Guid]::NewGuid().ToString('N') + '.staged')
    Copy-Item -LiteralPath $releaseExe -Destination $stagedExe -Force
    $stagedHash = (Get-FileHash -LiteralPath $stagedExe -Algorithm SHA256).Hash.ToUpperInvariant()
    if ($stagedHash -cne $releaseHash) {
        throw "暂存部署文件 SHA-256 校验失败：$stagedExe"
    }
    if ($fileSnapshot.Exists) {
        $replaceBackupFile = Join-Path -Path $temporaryDirectory -ChildPath 'replace-backup.exe'
        [IO.File]::Replace($stagedExe, $installedExe, $replaceBackupFile)
        Remove-Item -LiteralPath $replaceBackupFile -Force
    }
    else {
        [IO.File]::Move($stagedExe, $installedExe)
    }

    foreach ($registryPath in $registryPaths) {
        if (Test-Path -LiteralPath $registryPath) {
            Remove-Item -LiteralPath $registryPath -Recurse -Force
        }
    }

    $localServerPath = Join-Path -Path $clsidPath -ChildPath 'LocalServer32'
    New-Item -Path $localServerPath -Force | Out-Null
    Set-RegistryString -Path $clsidPath -Name $ownerValueName -Value $productOwner
    Set-RegistryString -Path $clsidPath -Name 'CopyPathMenuSha256' -Value $releaseHash
    Set-RegistryString -Path $localServerPath -Value ('"' + $installedExe + '"')
    Set-RegistryString -Path $localServerPath -Name 'ServerExecutable' -Value $installedExe
    New-DelegatedCopyPathVerb -Path $allFilesystemVerb -MenuText $menuText
    New-DelegatedCopyPathVerb -Path $driveVerb -MenuText $menuText

    New-Item -Path $backgroundVerb -Force | Out-Null
    Set-RegistryString -Path $backgroundVerb -Value $menuText
    Set-RegistryString -Path $backgroundVerb -Name 'MUIVerb' -Value $menuText
    Set-RegistryString -Path $backgroundVerb -Name 'Position' -Value 'Top'
    Set-RegistryString -Path $backgroundVerb -Name $ownerValueName -Value $productOwner
    $backgroundCommandPath = Join-Path -Path $backgroundVerb -ChildPath 'command'
    New-Item -Path $backgroundCommandPath -Force | Out-Null
    Set-RegistryString -Path $backgroundCommandPath -Value ('"' + $installedExe + '" --copy "%V"')

    New-Item -Path $runKeyPath -Force | Out-Null
    Set-RegistryString -Path $runKeyPath -Name $runValueName -Value ('"' + $installedExe + '" --hotkey')

    Notify-ShellAssociationChanged
    try {
        Start-Process -FilePath $installedExe -ArgumentList '--hotkey' -WindowStyle Hidden
        Write-Output '已启动后台快捷键监听服务（默认快捷键：Alt + C，可运行 set-hotkey.bat 自定义）。'
    }
    catch {
        Write-Warning "安装已完成，但未能立即启动快捷键监听进程：$($_.Exception.Message)"
    }
    Write-Output "安装完成：$installedExe"
    Write-Output "安装日志：$logFile"
}
catch {
    $failureMessage = $_.Exception.Message
    if ($mutationStarted) {
        $rollbackErrors = @(Restore-PreviousState)
        if ($rollbackErrors.Count -gt 0) {
            $failureMessage += '；回滚失败：' + ($rollbackErrors -join '；')
        }
        else {
            $failureMessage += '；已完成回滚。'
        }
    }
    throw "CopyPathMenu 安装失败：$failureMessage"
}
finally {
    if ($null -ne $stagedExe -and (Test-Path -LiteralPath $stagedExe -PathType Leaf)) {
        Remove-Item -LiteralPath $stagedExe -Force -ErrorAction SilentlyContinue
    }
    if ($null -ne $replaceBackupFile -and (Test-Path -LiteralPath $replaceBackupFile -PathType Leaf)) {
        Remove-Item -LiteralPath $replaceBackupFile -Force -ErrorAction SilentlyContinue
    }
    if ($null -ne $temporaryDirectory -and (Test-Path -LiteralPath $temporaryDirectory)) {
        Remove-Item -LiteralPath $temporaryDirectory -Recurse -Force -ErrorAction SilentlyContinue
    }
    if ($transcriptStarted) {
        Stop-Transcript | Out-Null
    }
}

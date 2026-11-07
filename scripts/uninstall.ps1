#requires -version 5.1
<#
.SYNOPSIS
    卸载机器级 CopyPathMenu。

.DESCRIPTION
    此脚本仅删除带有 CopyPathMenuOwner=DragonKnightK.CopyPathMenu 标记的产品专属
    注册表键、开机自启项、快捷键配置键，以及 C:\Program Files\CopyPathMenu\copy-path.exe。
#>

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$productName = 'CopyPathMenu'
$productOwner = 'DragonKnightK.CopyPathMenu'
$verbName = 'DragonKnightK.CopyPathMenu'
$ownerValueName = 'CopyPathMenuOwner'
$clsid = '{7E4DC989-2ABA-458A-94D3-A350A352C32E}'
$installDirectory = Join-Path -Path $env:ProgramFiles -ChildPath $productName
$installedExe = Join-Path -Path $installDirectory -ChildPath 'copy-path.exe'
$classesRoot = 'HKLM:\Software\Classes'
$runKeyPath = 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Run'
$logDirectory = Join-Path -Path $env:ProgramData -ChildPath $productName
$transcriptStarted = $false

function Assert-ElevatedNativePowerShell {
    if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) {
        throw '在 64 位 Windows 上必须使用 C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe 运行此脚本；32 位 PowerShell 会写入错误的注册表视图。'
    }

    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw '需要以管理员身份运行此卸载脚本。'
    }
}

function Get-RegistryValue {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [string]$Name = ''
    )

    return (Get-Item -LiteralPath $Path).GetValue($Name)
}

function Assert-OwnedByCurrentProduct {
    param([Parameter(Mandatory = $true)][string]$Path)

    if ((Get-RegistryValue -Path $Path -Name $ownerValueName) -ne $productOwner) {
        throw "注册表目标存在但不属于 $productOwner，已停止卸载：$Path"
    }
}

function Assert-InstalledExeOwnedByCurrentProduct {
    param([switch]$RequireInstalledExe)

    if (-not (Test-Path -LiteralPath $installedExe)) {
        if ($RequireInstalledExe) {
            throw "部署文件在最终复核时不存在：$installedExe"
        }
        return $false
    }
    if (-not (Test-Path -LiteralPath $installedExe -PathType Leaf)) {
        throw "部署文件路径不是文件：$installedExe"
    }
    if (-not (Test-Path -LiteralPath $clsidPath)) {
        throw "部署文件存在但缺少当前产品 CLSID：$installedExe"
    }
    if ((Get-RegistryValue -Path $clsidPath -Name $ownerValueName) -ne $productOwner) {
        throw "部署文件存在但 CLSID 不属于 $productOwner：$installedExe"
    }

    $registeredHash = Get-RegistryValue -Path $clsidPath -Name 'CopyPathMenuSha256'
    if ($registeredHash -isnot [string] -or $registeredHash -cnotmatch '^[0-9A-F]{64}$') {
        throw "当前产品 CLSID 的 SHA-256 标记无效：$clsidPath"
    }
    $installedHash = (Get-FileHash -LiteralPath $installedExe -Algorithm SHA256).Hash.ToUpperInvariant()
    if ($installedHash -cne $registeredHash) {
        throw "部署文件不属于当前产品：$installedExe"
    }

    return $true
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
        Write-Warning "卸载已完成，但无法通知资源管理器刷新关联：$($_.Exception.Message)"
    }
}

try {
    Assert-ElevatedNativePowerShell

    $allFilesystemVerb = Join-Path -Path $classesRoot -ChildPath ("AllFilesystemObjects\shell\$verbName")
    $driveVerb = Join-Path -Path $classesRoot -ChildPath ("Drive\shell\$verbName")
    $backgroundVerb = Join-Path -Path $classesRoot -ChildPath ("Directory\Background\shell\$verbName")
    $clsidPath = Join-Path -Path $classesRoot -ChildPath ("CLSID\$clsid")
    $registryPaths = @($allFilesystemVerb, $driveVerb, $backgroundVerb, $clsidPath)

    foreach ($registryPath in $registryPaths) {
        if (Test-Path -LiteralPath $registryPath) {
            Assert-OwnedByCurrentProduct -Path $registryPath
        }
    }
    $installedExeExists = Assert-InstalledExeOwnedByCurrentProduct

    New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
    $logFile = Join-Path -Path $logDirectory -ChildPath ('uninstall-{0:yyyyMMdd-HHmmss}-{1}.log' -f (Get-Date), [Guid]::NewGuid().ToString('N'))
    Start-Transcript -LiteralPath $logFile -Append
    $transcriptStarted = $true

    if ($installedExeExists) {
        Stop-CurrentProductProcesses
        Assert-InstalledExeOwnedByCurrentProduct -RequireInstalledExe | Out-Null
        Remove-Item -LiteralPath $installedExe -Force
        Write-Output "已删除部署文件：$installedExe"
    }

    foreach ($registryPath in $registryPaths) {
        if (Test-Path -LiteralPath $registryPath) {
            Remove-Item -LiteralPath $registryPath -Recurse -Force
            Write-Output "已删除产品专属注册表键：$registryPath"
        }
    }

    if (Test-Path -LiteralPath $runKeyPath) {
        if ($null -ne (Get-Item -LiteralPath $runKeyPath).GetValue($productOwner, $null)) {
            Remove-ItemProperty -LiteralPath $runKeyPath -Name $productOwner -Force
            Write-Output "已删除开机自启注册表项：$runKeyPath [$productOwner]"
        }
    }

    $configPaths = @(
        "HKCU:\Software\$productOwner",
        "HKLM:\Software\$productOwner"
    )
    foreach ($configPath in $configPaths) {
        if (Test-Path -LiteralPath $configPath) {
            Remove-Item -LiteralPath $configPath -Recurse -Force
            Write-Output "已删除快捷键配置注册表键：$configPath"
        }
    }

    if (Test-Path -LiteralPath $installDirectory -PathType Container) {
        if ((Get-ChildItem -LiteralPath $installDirectory -Force | Measure-Object).Count -eq 0) {
            Remove-Item -LiteralPath $installDirectory -Force
            Write-Output "已删除空安装目录：$installDirectory"
        }
        else {
            Write-Output "安装目录含有其他文件，已保留：$installDirectory"
        }
    }

    Notify-ShellAssociationChanged
    Write-Output '卸载完成。诊断日志保留在 ProgramData\CopyPathMenu。'
    Write-Output "卸载日志：$logFile"
}
catch {
    throw "CopyPathMenu 卸载失败：$($_.Exception.Message)"
}
finally {
    if ($transcriptStarted) {
        Stop-Transcript | Out-Null
    }
}

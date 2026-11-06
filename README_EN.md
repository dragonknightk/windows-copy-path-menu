# Windows Context Menu "Copy Path" (CopyPathMenu)

![Platform](https://img.shields.io/badge/Platform-Windows%2010%20%2F%2011%20x64-0078D6?logo=windows)
![Language](https://img.shields.io/badge/Language-C%2B%2B17%20%7C%20Win32%20COM-00599C?logo=cplusplus)
![Dependencies](https://img.shields.io/badge/Dependencies-Zero%20(Native%20Win32)-success)
![License](https://img.shields.io/badge/License-MIT-green)

[简体中文](README.md) | **English**

A native Windows File Explorer context menu extension (**`复制路径`** / Copy Path) purpose-built for **Windows 10** (and fully compatible with the **Windows 11** classic context menu).

Built with pure C++17 and an out-of-process Windows Shell COM local server (`DelegateExecute`), it lets you right-click to copy the full physical paths of **files, folders, drive roots, and folder backgrounds** in a single click—**without holding `Shift`**, **without unwanted double quotes**, and with **multi-selection strictly matching File Explorer's visual display order**, zero console window flashes, and zero background resident processes.

---

## Why CopyPathMenu? (Pain Points Solved)

Copying file or directory paths is one of the most frequent operations in daily development and office workflows. However, both Windows's built-in "Copy as path" and common registry script workarounds suffer from glaring flaws:

1. **No More Awkward `Shift + Right-Click` on Windows 10**
   - Windows 10's default right-click menu does **not** include a "Copy path" option—you must hold down `Shift` on the keyboard while right-clicking every single time to reveal the hidden "Copy as path" item. CopyPathMenu places the command right at the top of the primary context menu for effortless one-handed access.
2. **Clean, Unquoted Paths—No Forced Double Quotes `""`**
   - Both Windows 10 (`Shift + Right-Click -> Copy as path`) and Windows 11 (`Copy as path`) forcibly wrap every path in double quotes (e.g., `"D:\Project\main.cpp"`). When pasting into a terminal, browser address bar, config file, or chat window, you have to manually strip the quotes every time. CopyPathMenu outputs **clean, unquoted paths**, separated by standard `CRLF` (`\r\n`) on multi-select, with no trailing newline.
3. **Right-Click on Empty Folder Background to Copy Current Directory**
   - Windows's built-in "Copy as path" only works when selecting a specific file or folder item; it cannot copy the current folder path when right-clicking on the empty background of an open folder. CopyPathMenu supports right-clicking directly on **any folder background (including drive roots like `C:\` and `D:\`)** to copy the current directory path.
4. **Fixes Windows Explorer's Infamous Multi-Select "Focus Rotation" Order Bug**
   - **A well-known Windows Shell bug**: If you select 5 files (`file1.txt` through `file5.txt`) in File Explorer and right-click on `file3.txt` to copy their paths, Windows's default `IShellItemArray` enumerates in a loop starting from the focused item—producing `file3, file4, file5, file1, file2`! It gets even worse when sorting descending or by Date Modified.
   - **Our Solution**: CopyPathMenu queries the caller window's `IFolderView` (`SVGIO_ALLVIEW | SVGIO_FLAG_VIEWORDER`) via `IObjectWithSite` and matches the original selection snapshot using canonical Shell item identity (`SICHINT_CANONICAL`). **No matter which selected item you right-click on, and regardless of ascending/descending sort, Date Modified sort, or grouping, the copied paths are always 100% identical to the visual order you see in File Explorer!**
5. **Far Superior to `cmd /c echo %1 | clip` Registry Scripts**
   - Common registry tutorials that invoke `cmd` or `powershell` via `shell\...\command` flash an ugly **black console window** on every click, and when **multiple files are selected**, Explorer spawns dozens of concurrent processes fighting over the clipboard—leaving only a single path (plus an unwanted trailing newline). CopyPathMenu uses the native Windows GUI subsystem and receives the entire selection batch via COM `IShellItemArray`, performing **one atomic clipboard write** with zero console flashes, zero dropped items, and immediate process exit upon completion.

### Feature Comparison

| Feature | **CopyPathMenu (This Project)** | **Win10 Built-in (`Shift+Right-Click`)** | **Win11 Built-in (`Copy as path`)** | **Registry Scripts (`cmd/clip`)** |
| :--- | :---: | :---: | :---: | :---: |
| **Direct Right-Click (No `Shift` Key)** | **Yes (Pinned to Top)** | No (Requires holding `Shift`) | Yes | Yes |
| **Clean Paths (No Surrounding `""`)** | **Unquoted** | Forced `"..."` | Forced `"..."` | Depends on script |
| **Folder Background Right-Click** | **Yes (Including Drive Roots)** | No | No | Breaks on drive roots (`%V` `\"` bug) |
| **Multi-Select Preserves Visual Order** | **100% Exact (Focus-Independent)** | Rotated by right-clicked focus item | Rotated by right-clicked focus item | Random race condition order |
| **Multi-Select Atomic Copy** | **Single Atomic Clipboard Write** | Yes | Yes | Processes overwrite each other |
| **No Console Flash / No Trailing Newline** | **No Flash, No Trailing Newline** | No Flash | No Flash | Flashes console, `echo` adds newline |
| **Background Resource Usage** | **Zero (Exits Immediately)** | Built-in | Built-in | Spawns console host per item |
| **Transactional Install & Rollback** | **SHA-256 + Ownership Guard + Rollback** | - | - | Manual `.reg` edits, prone to leftovers |

---

## Behavior & Output Format

Selecting multiple files or folders and clicking **`复制路径`** writes the following to the clipboard:

```text
D:\Example\file1.txt
D:\Example\file2.txt
D:\Example\folder
```

### Supported Context Menu Locations
- **Single or Multiple Files / Folders** (`AllFilesystemObjects`)
- **Disk Drives** (`Drive`, e.g., right-clicking `C:` or `D:` copies `C:\` or `D:\`)
- **Folder Empty Background** (`Directory\Background`, copies the current directory path)

### Format & Atomicity Guarantees
- Paths are separated by standard Windows `CRLF` (`\r\n`).
- **No double quotes** are added, and **no trailing newline** is appended after the last path.
- CJK characters, spaces, and arbitrary Unicode characters are preserved verbatim (`CF_UNICODETEXT`).
- **Strict Visual Order**: Multi-selected paths match the originating File Explorer view order (respecting current sort columns and grouping), unaffected by which item has focus.
- **Snapshot Isolation**: Captures the selection snapshot at menu invocation time; subsequent selection changes cannot leak unselected items into the output, and duplicate items in the original selection are preserved without forced deduplication.
- **All-or-Nothing Atomicity**: If any selected item cannot resolve to a physical filesystem path, or if the caller view cannot be obtained during multi-select, the entire operation aborts cleanly without clearing or partially overwriting the clipboard.

---

## Quick Installation & Uninstallation

### Option 1: One-Click Batch Scripts (Recommended)

After downloading or cloning the repository, no manual commands are needed:

- **Install / Upgrade**: Double-click **[`install.bat`](install.bat)** in the project root and click "Yes" on the UAC prompt. Takes effect immediately without restarting Windows or File Explorer.
- **Uninstall**: Double-click **[`uninstall.bat`](uninstall.bat)** in the project root to cleanly remove the program and all registered keys.

### Option 2: PowerShell Commands

You can also run the following commands in PowerShell from the project root:

**Install / Upgrade:**
```powershell
$scriptPath = (Resolve-Path -LiteralPath '.\scripts\install.ps1').Path
Start-Process `
    -FilePath (Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe') `
    -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$scriptPath`"") `
    -Verb RunAs `
    -Wait
```

**Uninstall:**
```powershell
$scriptPath = (Resolve-Path -LiteralPath '.\scripts\uninstall.ps1').Path
Start-Process `
    -FilePath (Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe') `
    -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$scriptPath`"") `
    -Verb RunAs `
    -Wait
```

### Deployment & Safety Design
- **Install Path**: `C:\Program Files\CopyPathMenu\copy-path.exe`
- **Diagnostic Logs**: `C:\ProgramData\CopyPathMenu\`
- **Strict Ownership Isolation**: The install and uninstall scripts only touch registry keys marked with `CopyPathMenuOwner=DragonKnightK.CopyPathMenu`. If an existing key or binary at the target path does not belong to this product or fails integrity verification, the script aborts before making any changes.
- **Transactional Backup & Automatic Rollback**: Before installing or upgrading, `install.ps1` verifies `release\SHA256SUMS.txt` and backs up existing files and registry keys. If any step fails, it automatically rolls back to the pre-installation state.

---

## Architecture & How It Works

For files, folders, and drives, File Explorer invokes our out-of-process COM local server via `DelegateExecute`, passing both the selection array and the caller window site:

```text
Explorer
  -> DelegateExecute ({7E4DC989-2ABA-458A-94D3-A350A352C32E})
  -> copy-path.exe -Embedding
  -> IObjectWithSelection::SetSelection (Captures selected items snapshot)
  -> IObjectWithSite::SetSite           (Captures caller Explorer site)
  -> IExecuteCommand::Execute
  -> Queries caller IFolderView::Items(SVGIO_ALLVIEW | SVGIO_FLAG_VIEWORDER)
  -> Matches items via IShellItem::Compare(SICHINT_CANONICAL) in exact visual order
  -> Writes CF_UNICODETEXT to Clipboard (with busy-retry loop)
  -> Drains references and exits process automatically
```

- **Solving Explorer's Focus-Rotation Bug**: By default, Windows Shell's `IShellItemArray` enumerates multi-selected items starting from the focused item and wrapping around. CopyPathMenu obtains the caller's `IServiceProvider` via `SetSite`, queries `SID_SFolderView` (falling back to `SID_STopLevelBrowser`), enumerates view items in visual order, and matches them against the selection snapshot using canonical Shell identity.
- **Folder Background & Drive Root Handling**: Folder backgrounds (`Directory\Background`) do not have multi-select semantics and pass the current directory via `--copy "%V"`. CopyPathMenu specifically normalizes the trailing `\"` escape quirk produced when Explorer expands `"%V"` on drive roots (such as `D:\` becoming `D:"` in `CommandLineToArgvW`), while also implementing `IExecuteCommand::SetDirectory` as a COM fallback.
- **Product Identifiers**:
  - Product Owner: `DragonKnightK.CopyPathMenu`
  - COM CLSID: `{7E4DC989-2ABA-458A-94D3-A350A352C32E}`

---

## Directory Structure

```text
├── install.bat                 One-click install/upgrade batch launcher (auto-elevates)
├── uninstall.bat               One-click uninstall batch launcher (auto-elevates)
├── README.md                   Documentation (Simplified Chinese)
├── README_EN.md                Documentation (English)
├── src/
│   ├── CopyPathMenu.cpp        Native C++17 source (COM local server & CLI handler)
│   └── CopyPathMenu.rc         Windows version resource definition (VERSIONINFO)
├── scripts/
│   ├── build.ps1               x64 build and release packaging script
│   ├── install.ps1             Machine-wide transactional installer & upgrader
│   ├── uninstall.ps1           Machine-wide safe uninstaller
│   ├── test.ps1                Unified automated test & release verification runner
│   └── test-explorer.ps1       Interactive live File Explorer integration test driver
├── tests/
│   ├── CopyPathMenuTests.cpp   C++ unit & COM contract regression tests (15 cases)
│   ├── ExplorerIntegration.cpp Live Explorer view & context menu integration helper
│   └── Install.Tests.ps1       Install, upgrade, tamper-check & rollback tests (16 cases)
├── release/
│   ├── copy-path.exe           Prebuilt x64 release binary (statically linked CRT, zero deps)
│   └── SHA256SUMS.txt          SHA-256 checksum of the release binary
├── docs/
│   └── 测试说明.md              Detailed test specifications & verification records
└── LICENSE                     MIT License
```

---

## Building from Source & Running Tests

### Prerequisites
- Windows 10 / 11 x64
- Visual Studio 2022 (or Visual Studio Build Tools 2022) with the **"Desktop development with C++"** workload and Windows 10/11 SDK installed.

### Build
`scripts\build.ps1` automatically locates the latest x64 MSVC compiler (`cl.exe`) and Windows SDK resource compiler (`rc.exe`) via `vswhere.exe`, producing a standalone native Windows binary with statically linked CRT (`/MT`) and embedded version info:

```powershell
# Standard build (outputs to build\copy-path.exe)
.\scripts\build.ps1

# Maintainer release build (updates release\copy-path.exe and recalculates release\SHA256SUMS.txt)
.\scripts\build.ps1 -UpdateRelease
```

### Automated Tests
The repository includes a comprehensive automated test suite (using in-memory clipboard interception and mock registry doubles so your real clipboard and system registry are never touched):

```powershell
# Run all 15 C++ unit tests, 16 installer/rollback tests, and release package checks
.\scripts\test.ps1
```

Or run individual test targets manually:
```powershell
.\scripts\build.ps1 -Target UnitTests
.\build\CopyPathMenuTests.exe
.\tests\Install.Tests.ps1
```

For details on the interactive live File Explorer integration test suite, see **[测试说明](docs/测试说明.md)**.

---

## License

Licensed under the [MIT License](LICENSE).

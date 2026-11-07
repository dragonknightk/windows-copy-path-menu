# Windows Context Menu & Hotkey "Copy Path" (CopyPathMenu)

![Platform](https://img.shields.io/badge/Platform-Windows%2010%20%2F%2011%20x64-0078D6?logo=windows)
![Language](https://img.shields.io/badge/Language-C%2B%2B17%20%7C%20Win32%20COM-00599C?logo=cplusplus)
![Dependencies](https://img.shields.io/badge/Dependencies-Zero%20(Native%20Win32)-success)
![License](https://img.shields.io/badge/License-MIT-green)

[简体中文](README.md) | **English**

A native Windows File Explorer **"Copy Path"** enhancement purpose-built for **Windows 10 / 11**, offering both a **top-level right-click context menu item** and an instant **`Alt + C` keyboard shortcut**.

Built with pure C++17 and Windows Shell COM (`DelegateExecute` + `IFolderView`), it lets you copy the full physical paths of **files, folders, drive roots, and the currently open folder**—either by right-clicking or pressing **`Alt + C` (customizable)**—**without holding `Shift`**, **without unwanted double quotes**, and with **multi-selection strictly matching File Explorer's visual display order**, with zero console window flashes.

---

## Why CopyPathMenu? (Pain Points Solved)

Copying file or directory paths (especially when feeding file paths to AI coding assistants) is one of the most frequent operations in daily workflows. However, both Windows's built-in "Copy as path" and common registry script workarounds suffer from glaring flaws:

1. **Instant `Alt + C` Keyboard Shortcut + Top-Level Context Menu**
   - **One-Press Shortcut (New)**: Select one or more files/folders in File Explorer and press **`Alt + C`** to copy all their paths immediately; if **no item is selected**, pressing **`Alt + C`** smartly copies the **path of the currently open folder**!
   - **Explorer-Foreground-Only Activation (Zero Global Conflict)**: Powered by `SetWinEventHook` foreground window event awareness and dynamic `RegisterHotKey`, the shortcut is **only registered while a File Explorer window is in the foreground**. As soon as you switch to a browser, IDE, terminal, or chat app, the hotkey is immediately unregistered so it never interferes with other applications. You can also customize or disable the shortcut anytime via the native GUI dialog (**[`set-hotkey.bat`](set-hotkey.bat)**).
   - **No More Awkward `Shift + Right-Click` on Windows 10**: Also places **`复制路径`** right at the top of the primary right-click context menu for effortless one-handed mouse access.
2. **Clean, Unquoted Paths—No Forced Double Quotes `""`**
   - Both Windows 10 (`Shift + Right-Click -> Copy as path`) and Windows 11 (`Ctrl + Shift + C`) forcibly wrap every path in double quotes (e.g., `"D:\Project\main.cpp"`). When pasting into a terminal, browser address bar, config file, or AI prompt, you have to manually strip the quotes every time. CopyPathMenu outputs **clean, unquoted paths**, separated by standard `CRLF` (`\r\n`) on multi-select, with no trailing newline.
3. **Copy Current Folder Path via Empty Selection (`Alt + C`) or Folder Background Right-Click**
   - Windows's built-in "Copy as path" only works when selecting a specific file or folder item; it cannot copy the current folder path when nothing is selected. CopyPathMenu lets you either **press `Alt + C` with nothing selected** or **right-click directly on any folder background (including drive roots like `C:\` and `D:\`)** to copy the current directory path.
4. **Fixes Windows Explorer's Infamous Multi-Select "Focus Rotation" Order Bug**
   - **A well-known Windows Shell bug**: If you select 5 files (`file1.txt` through `file5.txt`) in File Explorer and right-click on `file3.txt` to copy their paths, Windows's default `IShellItemArray` enumerates in a loop starting from the focused item—producing `file3, file4, file5, file1, file2`! It gets even worse when sorting descending or by Date Modified.
   - **Our Solution**: CopyPathMenu queries the active Explorer window/tab's `IFolderView` (`SVGIO_ALLVIEW | SVGIO_FLAG_VIEWORDER`) and matches the selection snapshot using canonical Shell item identity (`SICHINT_CANONICAL`). **Whether triggered by `Alt + C` or right-click, and regardless of ascending/descending sort, Date Modified sort, or grouping, the copied paths are always 100% identical to the visual order you see in File Explorer!**
5. **Far Superior to `cmd /c echo %1 | clip` Registry Scripts**
   - Common registry tutorials that invoke `cmd` or `powershell` via `shell\...\command` flash an ugly **black console window** on every click, and when **multiple files are selected**, Explorer spawns dozens of concurrent processes fighting over the clipboard—leaving only a single path (plus an unwanted trailing newline). CopyPathMenu uses the native Windows GUI subsystem and performs **one atomic clipboard write** with zero console flashes and zero dropped items.

### Feature Comparison

| Feature | **CopyPathMenu (This Project)** | **Win10 Built-in (`Shift+Right-Click`)** | **Win11 Built-in (`Ctrl+Shift+C`)** | **Registry Scripts (`cmd/clip`)** |
| :--- | :---: | :---: | :---: | :---: |
| **Keyboard Shortcut in Explorer** | **Yes (Default `Alt+C`, Customizable)** | No | Fixed `Ctrl+Shift+C` (Not customizable) | No |
| **Direct Right-Click (No `Shift` Key)** | **Yes (Pinned to Top)** | No (Requires holding `Shift`) | Yes | Yes |
| **Clean Paths (No Surrounding `""`)** | **Unquoted** | Forced `"..."` | Forced `"..."` | Depends on script |
| **Copy Current Folder (Empty Selection / Background)** | **Yes (`Alt+C` or Background Right-Click)** | No | No | Breaks on drive roots (`%V` `\"` bug) |
| **Multi-Select Preserves Visual Order** | **100% Exact (Focus-Independent)** | Rotated by right-clicked focus item | Rotated by right-clicked focus item | Random race condition order |
| **Multi-Select Atomic Copy** | **Single Atomic Clipboard Write** | Yes | Yes | Processes overwrite each other |
| **No Console Flash / No Trailing Newline** | **No Flash, No Trailing Newline** | No Flash | No Flash | Flashes console, `echo` adds newline |
| **Transactional Install & Rollback** | **SHA-256 + Ownership Guard + Rollback** | - | - | Manual `.reg` edits, prone to leftovers |

---

## Behavior & Output Format

Selecting multiple files or folders and pressing **`Alt + C`** (or right-clicking **`复制路径`**) writes the following to the clipboard:

```text
D:\Example\file1.txt
D:\Example\file2.txt
D:\Example\folder
```

### Supported Trigger Methods
1. **Keyboard Shortcut (Default `Alt + C`)**:
   - **1 or more files/folders selected** in File Explorer: copies all selected paths in exact visual order.
   - **0 items selected** in File Explorer: automatically copies the path of the currently open folder.
   - Supports both Windows 10 File Explorer and Windows 11 multi-tab File Explorer.
2. **Right-Click Context Menu (`复制路径`)**:
   - **Single or Multiple Files / Folders** (`AllFilesystemObjects`)
   - **Disk Drives** (`Drive`, e.g., right-clicking `C:` or `D:` copies `C:\` or `D:\`)
   - **Folder Empty Background** (`Directory\Background`, copies the current directory path)

### Customizing or Disabling the Shortcut
- **GUI Configuration Dialog (Recommended)**: Double-click **[`set-hotkey.bat`](set-hotkey.bat)** in the project root (or run `"C:\Program Files\CopyPathMenu\copy-path.exe" --config`). Press your desired key combination directly in the hotkey box (e.g., `Ctrl + Shift + C`, `Alt + C`, `Ctrl + Alt + F9`) and click **Save**—the running background listener hot-reloads immediately without restarting Windows. You can also restore the default (`Alt + C`) or disable the shortcut at any time.
- **Command-Line Configuration**:
  ```powershell
  # Print current hotkey configuration
  & "C:\Program Files\CopyPathMenu\copy-path.exe" --get-hotkey

  # Change hotkey to Ctrl+Shift+C (hot-reloads immediately)
  & "C:\Program Files\CopyPathMenu\copy-path.exe" --set-hotkey "Ctrl+Shift+C"

  # Restore default (Alt+C)
  & "C:\Program Files\CopyPathMenu\copy-path.exe" --set-hotkey default

  # Disable hotkey (keep right-click menu only)
  & "C:\Program Files\CopyPathMenu\copy-path.exe" --set-hotkey disable
  ```

### Format & Atomicity Guarantees
- Paths are separated by standard Windows `CRLF` (`\r\n`).
- **No double quotes** are added, and **no trailing newline** is appended after the last path.
- CJK characters, spaces, and arbitrary Unicode characters are preserved verbatim (`CF_UNICODETEXT`).
- **Strict Visual Order**: Multi-selected paths match the active File Explorer view order (respecting current sort columns and grouping), unaffected by which item has focus.
- **All-or-Nothing Atomicity**: If any selected item cannot resolve to a physical filesystem path, or if the caller view cannot be obtained during multi-select, the entire operation aborts cleanly without clearing or partially overwriting the clipboard.

---

## Quick Installation & Uninstallation

### Option 1: One-Click Batch Scripts (Recommended)

After downloading or cloning the repository, no manual commands are needed:

- **Install / Upgrade**: Double-click **[`install.bat`](install.bat)** in the project root and click "Yes" on the UAC prompt. Takes effect immediately (both context menu and `Alt + C` shortcut) without restarting Windows or File Explorer.
- **Configure Shortcut**: Double-click **[`set-hotkey.bat`](set-hotkey.bat)** in the project root to open the native shortcut configuration dialog.
- **Uninstall**: Double-click **[`uninstall.bat`](uninstall.bat)** in the project root to cleanly remove the program, auto-start entry, and all registered keys.

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

1. **Right-Click Context Menu (`DelegateExecute` Out-of-Process COM Server)**:
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
   ```
2. **Keyboard Shortcut (`SetWinEventHook` Foreground Awareness + Dynamic `RegisterHotKey`)**:
   ```text
   copy-path.exe --hotkey (Zero-polling event-driven listener)
     -> SetWinEventHook (EVENT_SYSTEM_FOREGROUND .. EVENT_SYSTEM_MINIMIZEEND)
     -> Dynamically calls RegisterHotKey ONLY when CabinetWClass / ExploreWClass is foreground
     -> Immediately calls UnregisterHotKey when switching to any other app
     -> On WM_HOTKEY:
     -> Enumerates CLSID_ShellWindows and locates the active Explorer window & active Win11 tab IFolderView
     -> If selection count > 0 (SVGIO_SELECTION): copies selected paths in exact visual order
     -> If selection count == 0: copies the currently open folder path via IFolderView::GetFolder
   ```

- **Product Identifiers**:
  - Product Owner: `DragonKnightK.CopyPathMenu`
  - COM CLSID: `{7E4DC989-2ABA-458A-94D3-A350A352C32E}`

---

## Directory Structure

```text
├── install.bat                 One-click install/upgrade batch launcher (auto-elevates)
├── uninstall.bat               One-click uninstall batch launcher (auto-elevates)
├── set-hotkey.bat              One-click shortcut configuration GUI launcher
├── README.md                   Documentation (Simplified Chinese)
├── README_EN.md                Documentation (English)
├── src/
│   ├── CopyPathMenu.cpp        Native C++17 source (COM server, hotkey listener & GUI config)
│   ├── CopyPathMenu.rc         Windows version & manifest resource definition
│   └── CopyPathMenu.manifest   ComCtl32 v6 visual styles & PerMonitorV2 DPI awareness manifest
├── scripts/
│   ├── build.ps1               x64 build and release packaging script
│   ├── install.ps1             Machine-wide transactional installer & upgrader
│   ├── uninstall.ps1           Machine-wide safe uninstaller
│   ├── test.ps1                Unified automated test & release verification runner
│   └── test-explorer.ps1       Interactive live File Explorer integration test driver
├── tests/
│   ├── CopyPathMenuTests.cpp   C++ unit, COM contract & hotkey regression tests (19 cases)
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
`scripts\build.ps1` automatically locates the latest x64 MSVC compiler (`cl.exe`) and Windows SDK resource compiler (`rc.exe`) via `vswhere.exe`, producing a standalone native Windows binary with statically linked CRT (`/MT`) and embedded version/DPI manifest resources:

```powershell
# Standard build (outputs to build\copy-path.exe)
.\scripts\build.ps1

# Maintainer release build (updates release\copy-path.exe and recalculates release\SHA256SUMS.txt)
.\scripts\build.ps1 -UpdateRelease
```

### Automated Tests
The repository includes a comprehensive automated test suite (using in-memory clipboard interception and mock registry doubles so your real clipboard and system registry are never touched):

```powershell
# Run all 19 C++ unit tests, 16 installer/rollback tests, and release package checks
.\scripts\test.ps1
```

For details on the interactive live File Explorer integration test suite, see **[测试说明](docs/测试说明.md)**.

---

## License

Licensed under the [MIT License](LICENSE).

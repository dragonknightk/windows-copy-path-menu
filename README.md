# Windows 右键菜单 & 快捷键“复制路径” (CopyPathMenu)

![Platform](https://img.shields.io/badge/Platform-Windows%2010%20%2F%2011%20x64-0078D6?logo=windows)
![Language](https://img.shields.io/badge/Language-C%2B%2B17%20%7C%20Win32%20COM-00599C?logo=cplusplus)
![Dependencies](https://img.shields.io/badge/Dependencies-Zero%20(Native%20Win32)-success)
![License](https://img.shields.io/badge/License-MIT-green)

**简体中文** | [English](README_EN.md)

专为 **Windows 10 / 11** 打造的原生级文件资源管理器 **`复制路径`** 增强工具，同时提供 **右键菜单一键复制** 与 **`Alt + C` 键盘快捷键秒级复制** 两种交互方式。

基于纯正 C++17 与 Windows Shell 进程外 COM 本地服务器（`DelegateExecute` + `IFolderView`）开发，**无需按住 `Shift` 键**，无论通过**鼠标右键**还是直接按下 **`Alt + C`（支持自定义）**，均可一键复制**文件、文件夹、磁盘根目录、当前打开目录**的完整物理路径；输出**不带多余双引号**，多选时**严格遵循资源管理器当前视觉排列顺序**，无黑框闪烁。

---

## 为什么选择本项目？（直击痛点）

在日常开发（尤其是向 AI 助手提供文件路径）与办公中，复制文件或目录路径是最高频的操作之一。但无论是 Windows 系统自带功能，还是网上流传的注册表脚本教程，都存在明显缺陷：

1. **`Alt + C` 快捷键直达 + 右键置顶菜单（双剑合璧）**
   - **快捷键秒复制（新特性）**：在资源管理器中选中一个或多个文件/文件夹，直接按 **`Alt + C`** 即可复制全部路径；如果当前**未选中任何文件**，按 **`Alt + C`** 会智能复制**当前打开文件夹本身的路径**！
   - **仅在资源管理器前台激活，零全局冲突**：采用 `SetWinEventHook` 前台焦点事件感知 + 动态 `RegisterHotKey` 技术——**只有当文件资源管理器处于前台活动窗口时才占用快捷键**；一旦切换到浏览器、IDE、终端或聊天软件，快捷键立即释放，绝不干扰任何其他软件！并提供可视化设置窗口（双击 **[`set-hotkey.bat`](set-hotkey.bat)**）随时更改或禁用快捷键。
   - **告别 Win10 繁琐的 `Shift + 右键`**：同时保留置于右键菜单顶部的 **`复制路径`** 菜单项，习惯纯鼠标操作时单手一点即得。
2. **纯净路径输出，拒绝强制双引号 `""`**
   - 无论是 Win10 的 `Shift + 右键 -> 复制为路径` 还是 Win11 的 `Ctrl + Shift + C`，系统都会强制在路径首尾加上双引号（如 `"D:\Project\main.cpp"`）。粘贴到终端、浏览器地址栏、配置文件或 AI 对话框时，每次都要手动删引号。本项目输出**纯净无引号路径**，多选时以标准 `CRLF` 分隔，且末尾绝不附加多余换行。
3. **支持“文件夹空白处”右键或空选快捷键复制当前目录**
   - 系统自带的复制路径只能在选中具体文件/文件夹时触发；当你身处某个已打开的文件夹内想要复制当前目录路径时，系统菜单无能为力。本项目支持直接在**文件夹空白处（含 `C:\`、`D:\` 等磁盘根目录空白处）右键**，或在**未选中文件时直接按 `Alt + C`** 复制当前目录路径。
4. **攻克 Windows 资源管理器多选“焦点乱序”顽疾**
   - **你可能遇到过这个系统级 Bug**：在资源管理器中选中 `file1.txt` 到 `file5.txt` 共 5 个文件，如果你在 `file3.txt` 上点击右键复制路径，Windows 原生接口会从焦点项开始循环枚举，导致复制出来的顺序变成 `file3, file4, file5, file1, file2`；当视图按修改日期或降序排列时更是混乱。
   - **本项目的解决方案**：直接向资源管理器活动窗口/标签页查询 `IFolderView`（`SVGIO_ALLVIEW | SVGIO_FLAG_VIEWORDER`），并以 Shell 对象的规范身份（`SICHINT_CANONICAL`）匹配选中项快照——**无论通过右键菜单还是 `Alt + C` 快捷键，无论当前按名称升序/降序、按修改时间排序还是分组显示，剪贴板里的路径顺序永远与你肉眼看到的列表顺序 100% 一致！**
5. **吊打网上常见的 `cmd /c echo %1 | clip` 注册表脚本**
   - 网上教程常用的 `command` 命令行写法，不仅每次点击会**闪现黑框（CMD/PowerShell 窗口）**，而且在**多选文件时会被系统并发拉起数十个进程争抢剪贴板**，最终导致剪贴板内容被覆盖、只剩一条路径。本项目采用原生 GUI 子系统与 COM 批量接收选择，多选再多文件也只进行**一次原子剪贴板写入**，零黑框、不丢项。

### 方案全方位对比

| 特性维度 | **本项目 (CopyPathMenu)** | **Win10 自带 (`Shift+右键`)** | **Win11 自带 (`Ctrl+Shift+C`)** | **传统注册表脚本 (`cmd/clip`)** |
| :--- | :---: | :---: | :---: | :---: |
| **键盘快捷键一键复制** | **支持（默认 `Alt+C`，可自定义）** | 不支持 | 固定 `Ctrl+Shift+C`（不可改） | 不支持 |
| **直接右键触发（免按 Shift）** | **支持（置顶显示）** | 不支持（需按住 Shift） | 支持 | 支持 |
| **纯净路径（不带首尾双引号）** | **不带双引号** | 强制加 `"..."` | 强制加 `"..."` | 取决于写法 |
| **未选中文件/空白处复制当前目录** | **支持（快捷键空选 & 空白处右键）** | 不支持 | 不支持 | 盘符根目录 `%V` 易转义出错 |
| **多选保持资源管理器真实视觉顺序** | **严格一致（不受焦点项影响）** | 受右键焦点项旋转打乱 | 受右键焦点项旋转打乱 | 多进程并发乱序 |
| **多选一次性完整复制** | **一次原子写入剪贴板** | 支持 | 支持 | 多进程互相覆盖，通常只剩 1 条 |
| **无黑框闪烁 / 无末尾多余换行** | **无黑框、无末尾多余换行** | 无黑框 | 无黑框 | 闪黑框、`echo` 附带末尾换行 |
| **安装升级回滚与安全校验** | **SHA-256 + 所有权隔离 + 自动回滚** | - | - | 手工改注册表，易残留或误删 |

---

## 效果与行为规范

选中多个文件或文件夹后，按下 **`Alt + C`** 或右键点击 **`复制路径`**，剪贴板内容形如：

```text
D:\Example\file1.txt
D:\Example\file2.txt
D:\Example\folder
```

### 支持的使用方式
1. **键盘快捷键（默认 `Alt + C`）**：
   - 在文件资源管理器窗口中**选中 1 个或多个文件/文件夹**后按 `Alt + C`：按当前视图视觉顺序复制所有选中项的完整路径。
   - 在文件资源管理器窗口中**未选中任何项**时按 `Alt + C`：自动复制当前打开文件夹的路径。
   - 完美支持 Windows 10 资源管理器及 Windows 11 多标签页（Multi-Tab）资源管理器。
2. **鼠标右键菜单（`复制路径`）**：
   - **单个或多个文件 / 文件夹**（`AllFilesystemObjects`）
   - **磁盘驱动器**（`Drive`，如右键 `C:`、`D:` 复制 `C:\`、`D:\`）
   - **文件夹内部空白处**（`Directory\Background`，复制当前所在目录路径）

### 自定义或禁用快捷键
- **图形界面设置（推荐）**：直接双击项目根目录下的 **[`set-hotkey.bat`](set-hotkey.bat)**（或运行 `"C:\Program Files\CopyPathMenu\copy-path.exe" --config`），在弹出的原生设置窗口中直接按下想要的新组合键（如 `Ctrl + Shift + C`、`Alt + C`、`Ctrl + Alt + F9` 等），点击**“保存并立即生效”**即可热重载生效，无需重启电脑。也可以随时点击“恢复默认 (Alt+C)”或“禁用快捷键”。
- **命令行设置**：
  ```powershell
  # 查看当前快捷键配置
  & "C:\Program Files\CopyPathMenu\copy-path.exe" --get-hotkey

  # 修改快捷键为 Ctrl+Shift+C（立即热重载生效）
  & "C:\Program Files\CopyPathMenu\copy-path.exe" --set-hotkey "Ctrl+Shift+C"

  # 恢复默认 Alt+C
  & "C:\Program Files\CopyPathMenu\copy-path.exe" --set-hotkey default

  # 禁用快捷键（仅保留右键菜单）
  & "C:\Program Files\CopyPathMenu\copy-path.exe" --set-hotkey disable
  ```

### 格式与原子性约定
- 路径之间使用标准 Windows 换行符 `CRLF`（`\r\n`）。
- **不添加双引号**，最后一条路径后**不添加末尾换行符**。
- 中文、空格及任意 Unicode 字符原样保留（`CF_UNICODETEXT`）。
- **严格遵循视觉顺序**：多选路径与当前资源管理器视图排列一致，遵循当前排序和分组，不受鼠标焦点项位置影响。
- **全有或全无（原子失败保护）**：任一选中项无法取得物理文件系统路径，或多选时无法取得调用方视图，整次操作直接中止，绝不清空原有剪贴板或写入残缺列表。

---

## 快速安装与卸载

### 方式一：双击一键安装 / 卸载 / 设置快捷键（推荐）

下载本项目后，无需手动输入任何命令：

- **安装 / 升级**：直接双击项目根目录下的 **[`install.bat`](install.bat)**，在弹出的管理员权限提示中点击“是”即可完成安装并立即启动快捷键服务（无需重启电脑或资源管理器）。
- **设置快捷键**：直接双击项目根目录下的 **[`set-hotkey.bat`](set-hotkey.bat)** 即可打开可视化快捷键设置窗口（默认快捷键为 `Alt + C`）。
- **卸载**：直接双击项目根目录下的 **[`uninstall.bat`](uninstall.bat)** 即可干净卸载。

### 方式二：通过 PowerShell 命令执行

也可以在项目根目录的 PowerShell 中执行以下命令：

**安装 / 升级：**
```powershell
$scriptPath = (Resolve-Path -LiteralPath '.\scripts\install.ps1').Path
Start-Process `
    -FilePath (Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe') `
    -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$scriptPath`"") `
    -Verb RunAs `
    -Wait
```

**卸载：**
```powershell
$scriptPath = (Resolve-Path -LiteralPath '.\scripts\uninstall.ps1').Path
Start-Process `
    -FilePath (Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe') `
    -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$scriptPath`"") `
    -Verb RunAs `
    -Wait
```

### 部署与安全机制说明
- **安装位置**：`C:\Program Files\CopyPathMenu\copy-path.exe`
- **诊断日志**：`C:\ProgramData\CopyPathMenu\`
- **零误伤所有权隔离**：安装与卸载脚本仅处理带有 `CopyPathMenuOwner=DragonKnightK.CopyPathMenu` 标记的产品专属注册表项与同名自启项。若遇到同名但归属不明的注册表项或被篡改的程序文件，脚本会在写入前立即中止。
- **事务级备份与自动回滚**：安装/升级前自动核对 `release\SHA256SUMS.txt`，并对现有文件和注册表键创建快照备份；若部署中途发生任何异常，自动回滚恢复至修改前状态。

---

## 技术实现原理

1. **右键菜单路径（进程外 COM `DelegateExecute`）**：
   ```text
   Explorer
     -> DelegateExecute ({7E4DC989-2ABA-458A-94D3-A350A352C32E})
     -> copy-path.exe -Embedding
     -> IObjectWithSelection::SetSelection (获取选中项快照)
     -> IObjectWithSite::SetSite           (获取调用方窗口站点)
     -> IExecuteCommand::Execute
     -> 查询调用方 IFolderView::Items(SVGIO_ALLVIEW | SVGIO_FLAG_VIEWORDER)
     -> 按 IShellItem::Compare(SICHINT_CANONICAL) 匹配还原真实视觉顺序
     -> 写入剪贴板 CF_UNICODETEXT (带占用重试机制)
   ```
2. **快捷键路径（前台事件感知 `SetWinEventHook` + 动态 `RegisterHotKey`）**：
   ```text
   copy-path.exe --hotkey (零轮询事件驱动监听)
     -> SetWinEventHook (EVENT_SYSTEM_FOREGROUND .. EVENT_SYSTEM_MINIMIZEEND)
     -> 前台窗口为 CabinetWClass / ExploreWClass 时动态 RegisterHotKey
     -> 切换到其他应用窗口时立即 UnregisterHotKey (绝不占用全局热键)
     -> 按下快捷键触发 WM_HOTKEY
     -> 通过 CLSID_ShellWindows 枚举并匹配当前活动 Explorer 窗口及 Win11 活动标签页 IFolderView
     -> 若有选中项 (SVGIO_SELECTION > 0)：按视觉顺序复制全部选中路径
     -> 若无选中项 (SVGIO_SELECTION == 0)：通过 IFolderView::GetFolder 复制当前打开文件夹路径
   ```

- **元数据标识**：
  - 产品标识：`DragonKnightK.CopyPathMenu`
  - COM CLSID：`{7E4DC989-2ABA-458A-94D3-A350A352C32E}`

---

## 目录结构

```text
├── install.bat                 双击一键安装/升级引导脚本（自动提权）
├── uninstall.bat               双击一键卸载引导脚本（自动提权）
├── set-hotkey.bat              双击打开快捷键可视化设置窗口
├── README.md                   项目主页说明文档（简体中文）
├── README_EN.md                项目说明文档（English）
├── src/
│   ├── CopyPathMenu.cpp        原生 C++17 核心源码（COM 服务器、快捷键守护与 GUI 设置窗口）
│   ├── CopyPathMenu.rc         Windows 应用程序版本与清单资源定义
│   └── CopyPathMenu.manifest   ComCtl32 v6 现代视觉样式与 PerMonitorV2 高分屏 DPI 感知清单
├── scripts/
│   ├── build.ps1               x64 自动化构建与发布包更新脚本
│   ├── install.ps1             机器级事务安装与升级脚本
│   ├── uninstall.ps1           机器级安全卸载脚本
│   ├── test.ps1                一键运行全部自动化回归测试与发布包核验
│   └── test-explorer.ps1       真实 Explorer 窗口交互式集成测试驱动脚本
├── tests/
│   ├── CopyPathMenuTests.cpp   C++ 源码、COM 契约与快捷键逻辑回归测试（19 项用例）
│   ├── ExplorerIntegration.cpp Explorer 真实视图与菜单集成测试辅助程序
│   └── Install.Tests.ps1       安装、升级、防篡改与回滚测试（16 项用例）
├── release/
│   ├── copy-path.exe           预编译 x64 发布程序（静态链接运行库，零外部依赖）
│   └── SHA256SUMS.txt          发布程序 SHA-256 校验值
├── docs/
│   └── 测试说明.md              详细测试策略、验收标准与验证记录
└── LICENSE                     MIT 开源协议
```

---

## 从源码构建与测试

### 环境要求
- Windows 10 / 11 x64
- Visual Studio 2022（或 Visual Studio Build Tools 2022），勾选 **“使用 C++ 的桌面开发”** 组件及 Windows 10/11 SDK。

### 编译构建
脚本通过 `vswhere.exe` 自动定位最新可用的 x64 MSVC 编译器（`cl.exe`）与 Windows SDK（`rc.exe`），生成静态链接运行库（`/MT`）且内嵌版本与 DPI 清单资源的原生 Windows 程序：

```powershell
# 常规构建（默认输出到 build\copy-path.exe）
.\scripts\build.ps1

# 开发者发版构建（编译后自动更新 release\copy-path.exe 并重算 release\SHA256SUMS.txt）
.\scripts\build.ps1 -UpdateRelease
```

### 运行自动化测试
项目内置完善的自动化测试套件（包含剪贴板内存拦截与内存注册表替身，不会污染用户剪贴板或真实系统注册表）：

```powershell
# 一键运行全部 C++ 单元测试（19 项）、安装升级回滚测试（16 项）及发布包核验
.\scripts\test.ps1
```

更多关于真实资源管理器窗口集成测试的操作方式与验收记录，请参见 **[测试说明](docs/测试说明.md)**。

---

## 开源协议

本项目基于 [MIT License](LICENSE) 开源。

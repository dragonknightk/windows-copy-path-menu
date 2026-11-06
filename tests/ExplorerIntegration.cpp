#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <exdisp.h>
#include <shlguid.h>
#include <propkey.h>
#include <wrl/client.h>
#include <tlhelp32.h>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {
const CLSID commandClass = {0x7e4dc989, 0x2aba, 0x458a, {0x94,0xd3,0xa3,0x50,0xa3,0x52,0xc3,0x2e}};
const std::vector<std::wstring> names = {
    L"addins", L"appcompat", L"apppatch", L"AppReadiness", L"assembly", L"bcastdvr",
    L"Boot", L"Branding", L"CbsTemp", L"Containers", L"Cursors", L"debug",
    L"diagnostics", L"DiagTrack", L"DigitalLocker", L"Downloaded Program Files"
};
void Check(HRESULT hr, const char* step) {
    if (FAILED(hr)) { std::cerr << step << ": 0x" << std::hex << static_cast<unsigned long>(hr) << std::dec << '\n'; throw std::runtime_error(step); }
}
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Pump(DWORD milliseconds) {
    const ULONGLONG end = GetTickCount64() + milliseconds;
    do {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
    } while (GetTickCount64() < end);
}
struct Child {
    PITEMID_CHILD value = nullptr;
    Child() = default;
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    Child(Child&& other) noexcept : value(other.value) { other.value = nullptr; }
    ~Child() { CoTaskMemFree(value); }
};
struct View {
    ComPtr<IShellView> shell;
    ComPtr<IFolderView2> folderView;
    ComPtr<IShellFolder> folder;
    HWND window = nullptr;
};
View FindView() {
    ComPtr<IShellWindows> windows;
    Check(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&windows)), "IShellWindows");
    long count = 0; Check(windows->get_Count(&count), "window count");
    for (long index = 0; index < count; ++index) {
        VARIANT position{}; position.vt = VT_I4; position.lVal = index;
        ComPtr<IDispatch> dispatch;
        if (FAILED(windows->Item(position, &dispatch)) || !dispatch) continue;
        ComPtr<IServiceProvider> service;
        if (FAILED(dispatch.As(&service))) continue;
        ComPtr<IShellBrowser> browser;
        if (FAILED(service->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser)))) continue;
        View view;
        if (FAILED(browser->QueryActiveShellView(&view.shell))) continue;
        if (FAILED(view.shell.As(&view.folderView))) continue;
        if (FAILED(view.folderView->GetFolder(IID_PPV_ARGS(&view.folder)))) continue;
        ComPtr<IPersistFolder2> persisted;
        if (FAILED(view.folder.As(&persisted))) continue;
        PIDLIST_ABSOLUTE root = nullptr;
        if (FAILED(persisted->GetCurFolder(&root))) continue;
        wchar_t path[MAX_PATH]{};
        bool match = SHGetPathFromIDListW(root, path) && _wcsicmp(path, L"C:\\Windows") == 0;
        CoTaskMemFree(root);
        if (match) { Check(view.shell->GetWindow(&view.window), "view window"); return view; }
    }
    throw std::runtime_error("No existing C:\\Windows Explorer view");
}
// 所有视图变更均位于此恢复保护范围内；保存并恢复原选择及焦点。
struct RestoreView {
    View& view;
    std::vector<SORTCOLUMN> sort;
    PROPERTYKEY group{};
    BOOL ascending = FALSE;
    Child focus;
    std::vector<Child> originalSelection;
    bool restored = false;
    bool modified = false;
    explicit RestoreView(View& value) : view(value) {
        int selected = 0; Check(view.folderView->ItemCount(SVGIO_SELECTION, &selected), "original selection");
        ComPtr<IEnumIDList> selectedItems;
        Check(view.folderView->Items(SVGIO_SELECTION, IID_PPV_ARGS(&selectedItems)), "save original selection");
        for (;;) {
            Child child;
            HRESULT hr = selectedItems->Next(1, &child.value, nullptr);
            if (hr == S_FALSE) break;
            Check(hr, "save selected item");
            originalSelection.push_back(std::move(child));
        }
        Require(originalSelection.size() == static_cast<size_t>(selected), "Original selection changed while saving");
        int columns = 0; Check(view.folderView->GetSortColumnCount(&columns), "sort count");
        sort.resize(columns);
        Check(view.folderView->GetSortColumns(sort.data(), columns), "save sort");
        Check(view.folderView->GetGroupBy(&group, &ascending), "save grouping");
        int focused = -1;
        if (SUCCEEDED(view.folderView->GetFocusedItem(&focused)) && focused >= 0)
            Check(view.folderView->Item(focused, &focus.value), "save focus");
    }
    bool Restore() noexcept {
        if (restored || !modified) return true;
        HRESULT a = view.folderView->SetGroupBy(group, ascending);
        HRESULT b = view.folderView->SetSortColumns(sort.data(), static_cast<int>(sort.size()));
        HRESULT c = view.shell->SelectItem(nullptr, SVSI_DESELECTOTHERS);
        std::vector<PCUITEMID_CHILD> originalPidls;
        for (const auto& child : originalSelection) originalPidls.push_back(child.value);
        HRESULT selectionResult = originalPidls.empty() ? S_OK : view.folderView->SelectAndPositionItems(
            static_cast<UINT>(originalPidls.size()), originalPidls.data(), nullptr, SVSI_SELECT);
        const bool focusSelected = focus.value && std::any_of(originalSelection.begin(), originalSelection.end(),
            [&](const Child& saved) { return ILIsEqual(saved.value, focus.value) != FALSE; });
        HRESULT d = focus.value ? view.shell->SelectItem(focus.value,
            SVSI_FOCUSED | (focusSelected ? SVSI_SELECT : SVSI_DESELECT)) : S_OK;
        Pump(150);
        int selected = -1;
        HRESULT e = view.folderView->ItemCount(SVGIO_SELECTION, &selected);
        int count = -1;
        HRESULT f = view.folderView->GetSortColumnCount(&count);
        std::vector<SORTCOLUMN> actualSort(sort.size());
        HRESULT g = view.folderView->GetSortColumns(actualSort.data(), static_cast<int>(actualSort.size()));
        bool sameSort = count == static_cast<int>(sort.size());
        for (size_t i = 0; i < sort.size() && sameSort; ++i)
            sameSort = IsEqualPropertyKey(actualSort[i].propkey, sort[i].propkey) && actualSort[i].direction == sort[i].direction;
        PROPERTYKEY actualGroup{}; BOOL actualAscending = FALSE;
        HRESULT h = view.folderView->GetGroupBy(&actualGroup, &actualAscending);
        bool sameFocus = true;
        if (focus.value) {
            int index = -1; Child actualFocus;
            sameFocus = SUCCEEDED(view.folderView->GetFocusedItem(&index)) && index >= 0
                && SUCCEEDED(view.folderView->Item(index, &actualFocus.value)) && ILIsEqual(focus.value, actualFocus.value);
        }
        bool sameSelection = true;
        ComPtr<IEnumIDList> restoredItems;
        if (FAILED(view.folderView->Items(SVGIO_SELECTION, IID_PPV_ARGS(&restoredItems)))) sameSelection = false;
        while (sameSelection) {
            Child child;
            HRESULT next = restoredItems->Next(1, &child.value, nullptr);
            if (next == S_FALSE) break;
            if (FAILED(next)) { sameSelection = false; break; }
            sameSelection = std::any_of(originalSelection.begin(), originalSelection.end(),
                [&](const Child& saved) { return ILIsEqual(saved.value, child.value) != FALSE; });
        }
        restored = sameSelection && SUCCEEDED(a) && SUCCEEDED(b) && SUCCEEDED(c) && SUCCEEDED(d) && SUCCEEDED(e)
            && SUCCEEDED(f) && SUCCEEDED(g) && SUCCEEDED(h) && SUCCEEDED(selectionResult)
            && selected == static_cast<int>(originalSelection.size()) && sameSort && sameFocus
            && IsEqualPropertyKey(actualGroup, group) && actualAscending == ascending;
        if (!restored) std::cerr << "ERROR: Explorer view restoration failed: selection=" << sameSelection
            << " count=" << selected << "/" << originalSelection.size() << " sort=" << sameSort
            << " focus=" << sameFocus << " group=" << IsEqualPropertyKey(actualGroup, group)
            << " ascending=" << actualAscending << "/" << ascending << " HRESULTs=" << std::hex
            << a << "," << b << "," << c << "," << d << "," << e << "," << f << "," << g << "," << h
            << std::dec << '\n';
        if (restored) std::cout << "RESTORED: original selection, focus, sort and grouping" << std::endl;
        return restored;
    }
    ~RestoreView() { if (!restored) Restore(); }
};
std::vector<std::wstring> Order(View& view, const std::vector<std::wstring>& selected) {
    ComPtr<IEnumIDList> items;
    Check(view.folderView->Items(SVGIO_ALLVIEW | SVGIO_FLAG_VIEWORDER, IID_PPV_ARGS(&items)), "enumerate actual view order");
    std::vector<std::wstring> result;
    for (;;) {
        Child child;
        HRESULT hr = items->Next(1, &child.value, nullptr);
        if (hr == S_FALSE) break;
        Check(hr, "next view item");
        ComPtr<IShellItem> item;
        Check(SHCreateItemWithParent(nullptr, view.folder.Get(), child.value, IID_PPV_ARGS(&item)), "view shell item");
        PWSTR raw = nullptr;
        if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw))) continue;
        std::wstring path(raw); CoTaskMemFree(raw);
        if (std::find(selected.begin(), selected.end(), path) != selected.end()) result.push_back(path);
    }
    Require(result.size() == selected.size(), "Selected set is not completely represented in Explorer view");
    return result;
}
std::wstring Join(const std::vector<std::wstring>& paths) {
    std::wstring result;
    for (const auto& path : paths) { if (!result.empty()) result += L"\r\n"; result += path; }
    return result;
}
bool ReadClipboard(std::wstring& text) {
    if (!OpenClipboard(nullptr)) return false;
    HANDLE memory = GetClipboardData(CF_UNICODETEXT);
    const wchar_t* raw = memory ? static_cast<const wchar_t*>(GlobalLock(memory)) : nullptr;
    if (raw) { text = raw; GlobalUnlock(memory); }
    CloseClipboard();
    return raw != nullptr;
}
void AwaitRealMenu(const char* label, const std::wstring& expected, IObjectWithSite* keepAlive) {
    // 外部操作员通过真实 Explorer 菜单点击；此辅助程序只准备场景并核验剪贴板。
    DWORD previous = GetClipboardSequenceNumber();
    std::cout << "READY: " << label << "; click the actual Explorer copy-path menu within 600 seconds" << std::endl;
    const ULONGLONG deadline = GetTickCount64() + 600000;
    std::wstring actual;
    ULONGLONG nextActivity = 0;
    while (GetTickCount64() < deadline) {
        if (GetClipboardSequenceNumber() != previous && ReadClipboard(actual)) break;
        if (GetTickCount64() >= nextActivity) {
            ComPtr<IUnknown> unusedSite;
            HRESULT activity = keepAlive->GetSite(IID_PPV_ARGS(&unusedSite));
            // 就绪检测对象没有 site；调用本身维持服务活动，避免等待用户时暂停类工厂。
            if (activity != E_NOINTERFACE) Check(activity, "candidate activity probe");
            nextActivity = GetTickCount64() + 1000;
        }
        Pump(30);
    }
    Require(GetClipboardSequenceNumber() != previous, "Timed out waiting for actual Explorer menu clipboard update");
    if (actual != expected) {
        std::wcerr << L"EXPECTED:\n" << expected << L"\nACTUAL:\n" << actual << L"\n";
        throw std::runtime_error("Clipboard character-for-character mismatch");
    }
}
std::wstring FullPath(const std::wstring& path) {
    DWORD needed = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (needed == 0) Check(HRESULT_FROM_WIN32(GetLastError()), "resolve candidate full path");
    std::vector<wchar_t> buffer(needed);
    DWORD length = GetFullPathNameW(path.c_str(), needed, buffer.data(), nullptr);
    if (length == 0) Check(HRESULT_FROM_WIN32(GetLastError()), "resolve process full path");
    Require(length < needed, "Full path changed while resolving");
    return std::wstring(buffer.data(), length);
}
std::vector<DWORD> ServerProcesses() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) Check(HRESULT_FROM_WIN32(GetLastError()), "create process snapshot");
    std::vector<DWORD> result;
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    BOOL found = Process32FirstW(snapshot, &entry);
    DWORD error = found ? ERROR_SUCCESS : GetLastError();
    try {
        while (found) {
            if (_wcsicmp(entry.szExeFile, L"copy-path.exe") == 0) result.push_back(entry.th32ProcessID);
            found = Process32NextW(snapshot, &entry);
            if (!found) error = GetLastError();
        }
    } catch (...) { CloseHandle(snapshot); throw; }
    CloseHandle(snapshot);
    if (error != ERROR_NO_MORE_FILES) Check(HRESULT_FROM_WIN32(error), "enumerate server processes");
    return result;
}
struct Candidate {
    HANDLE process = nullptr;
    DWORD processId = 0;
    std::wstring executable;
    ComPtr<IObjectWithSite> keepAlive;
    void VerifyIdentity() const {
        DWORD state = WaitForSingleObject(process, 0);
        if (state == WAIT_FAILED) Check(HRESULT_FROM_WIN32(GetLastError()), "query candidate liveness");
        Require(state == WAIT_TIMEOUT, "Candidate exited before menu validation");
        const auto servers = ServerProcesses();
        Require(servers.size() == 1 && servers.front() == processId,
            "Server identity mismatch: expected only the launched candidate PID");
        HANDLE query = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
        if (!query) Check(HRESULT_FROM_WIN32(GetLastError()), "open candidate for image query");
        wchar_t image[32768]{};
        DWORD length = static_cast<DWORD>(sizeof(image) / sizeof(image[0]));
        BOOL queried = QueryFullProcessImageNameW(query, 0, image, &length);
        DWORD error = queried ? ERROR_SUCCESS : GetLastError();
        CloseHandle(query);
        if (!queried) Check(HRESULT_FROM_WIN32(error), "query candidate executable image");
        Require(_wcsicmp(FullPath(std::wstring(image, length)).c_str(), executable.c_str()) == 0,
            "Candidate executable path does not match the launched file");
        state = WaitForSingleObject(process, 0);
        if (state == WAIT_FAILED) Check(HRESULT_FROM_WIN32(GetLastError()), "recheck candidate liveness");
        Require(state == WAIT_TIMEOUT, "Candidate exited during identity verification");
    }
    explicit Candidate(const std::wstring& path) : executable(FullPath(path)) {
        // 固定产品文件名，保证进程枚举同时覆盖候选程序和已安装的同 CLSID 服务。
        const auto separator = executable.find_last_of(L"\\/");
        Require(separator != std::wstring::npos && _wcsicmp(executable.c_str() + separator + 1, L"copy-path.exe") == 0,
            "CandidateExe must have the basename copy-path.exe");
        Require(ServerProcesses().empty(), "Another copy-path.exe is running; do not mix servers");
        std::wstring command = L"\"" + executable + L"\" -Embedding";
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION info{};
        Require(CreateProcessW(executable.c_str(), &command[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info) != FALSE, "Start candidate failed");
        process = info.hProcess; processId = info.dwProcessId; CloseHandle(info.hThread);
        try {
            const ULONGLONG deadline = GetTickCount64() + 10000;
            // 接口支持不足以证明来源；激活后核验唯一服务器 PID、完整路径和存活句柄。
            WaitForInputIdle(process, 3000);
            while (GetTickCount64() < deadline && WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
                HRESULT hr = CoCreateInstance(commandClass, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&keepAlive));
                if (SUCCEEDED(hr)) { VerifyIdentity(); return; }
                Pump(100);
            }
            throw std::runtime_error("Candidate IObjectWithSite activation failed");
        } catch (...) {
            keepAlive.Reset();
            CloseHandle(process); process = nullptr;
            throw;
        }
    }
    bool Finish() {
        keepAlive.Reset();
        if (process) {
            const ULONGLONG deadline = GetTickCount64() + 35000;
            while (GetTickCount64() < deadline && WaitForSingleObject(process, 0) == WAIT_TIMEOUT) Pump(50);
            bool exited = WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
            CloseHandle(process);
            process = nullptr;
            if (!exited) std::cerr << "Candidate remains running after reference release\n";
            return exited;
        }
        return true;
    }
    ~Candidate() { Finish(); }
};
void Run(const std::wstring& candidatePath, int selectedCase) {
    View view = FindView();
    RestoreView restore(view);
    std::vector<Child> children(names.size());
    for (size_t i = 0; i < names.size(); ++i)
        Check(view.folder->ParseDisplayName(nullptr, nullptr, const_cast<wchar_t*>(names[i].c_str()), nullptr, &children[i].value, nullptr), "sample folder exists");
    struct Case { const char* label; PROPERTYKEY key; SORTDIRECTION direction; size_t focus; bool single; };
    const Case cases[] = {
        {"name ascending / DigitalLocker focus", PKEY_ItemNameDisplay, SORT_ASCENDING, 14, false},
        {"name ascending / Cursors focus", PKEY_ItemNameDisplay, SORT_ASCENDING, 10, false},
        {"name ascending / final item focus", PKEY_ItemNameDisplay, SORT_ASCENDING, 15, false},
        {"name descending", PKEY_ItemNameDisplay, SORT_DESCENDING, 10, false},
        {"modified time descending", PKEY_DateModified, SORT_DESCENDING, 14, false},
        {"single selection", PKEY_ItemNameDisplay, SORT_ASCENDING, 14, true}
    };
    for (int index = 0; index < static_cast<int>(sizeof(cases) / sizeof(cases[0])); ++index) {
        if (selectedCase != 0 && selectedCase != index + 1) continue;
        Candidate candidate(candidatePath);
        restore.modified = true;
        Check(view.folderView->SetGroupBy(PKEY_Null, TRUE), "disable grouping for sort cases");
        const auto& test = cases[index];
        SORTCOLUMN column{test.key, test.direction};
        Check(view.folderView->SetSortColumns(&column, 1), "set case sort");
        Check(view.shell->SelectItem(nullptr, SVSI_DESELECTOTHERS), "clear case selection");
        std::vector<PCUITEMID_CHILD> selectedPidls;
        std::vector<std::wstring> selectedPaths;
        for (size_t i = 0; i < names.size(); ++i) if (!test.single || i == test.focus) {
            selectedPidls.push_back(children[i].value); selectedPaths.push_back(L"C:\\Windows\\" + names[i]);
        }
        Check(view.folderView->SelectAndPositionItems(static_cast<UINT>(selectedPidls.size()), selectedPidls.data(), nullptr, SVSI_SELECT), "select sample set");
        Check(view.shell->SelectItem(children[test.focus].value, SVSI_SELECT | SVSI_FOCUSED), "set case focus");
        const ULONGLONG deadline = GetTickCount64() + 5000;
        std::vector<std::wstring> ordered;
        bool stable = false;
        do {
            Pump(100);
            auto current = Order(view, selectedPaths);
            if (current == ordered) { stable = true; break; }
            ordered = std::move(current);
        } while (GetTickCount64() < deadline);
        Require(stable, "View order did not stabilize");
        int count = 0; Check(view.folderView->ItemCount(SVGIO_SELECTION, &count), "actual selected count");
        Require(count == static_cast<int>(selectedPaths.size()), "Selection count mismatch");
        AwaitRealMenu(test.label, Join(ordered), candidate.keepAlive.Get());
        candidate.VerifyIdentity();
        Require(Order(view, selectedPaths) == ordered, "View changed during invocation");
        Require(candidate.Finish(), "Candidate did not exit after scenario");
        std::cout << "PASS: " << test.label << " (" << count << " paths; exact clipboard text)\n";
    }
    Require(restore.Restore(), "Could not restore original Explorer state");
    std::cout << "PASS: original selection, focus, sort and grouping restored\n";
}
}
int wmain(int argc, wchar_t** argv) {
    int selectedCase = 0;
    if ((argc != 3 && argc != 5) || wcscmp(argv[2], L"--interactive") != 0) {
        std::cerr << "Usage: ExplorerIntegration.exe <CandidateExe> --interactive [--case <1..6>]\n";
        return 2;
    }
    if (argc == 5) {
        if (wcscmp(argv[3], L"--case") != 0 || wcslen(argv[4]) != 1 || argv[4][0] < L'1' || argv[4][0] > L'6') {
            std::cerr << "--case requires a scenario index from 1 to 6\n";
            return 2;
        }
        selectedCase = argv[4][0] - L'0';
    }
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return 2;
    int result = 0;
    try { Run(argv[1], selectedCase); }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; result = 1; }
    CoUninitialize();
    return result;
}

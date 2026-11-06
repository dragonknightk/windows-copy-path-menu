#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <iostream>
#include <stdexcept>

// 截获剪贴板边界，断言失败时未清空剪贴板，并避免单元测试改变用户数据。
std::wstring capturedText;
HGLOBAL capturedMemory = NULL;
unsigned int clipboardWrites = 0;
BOOL WINAPI TestOpenClipboard(HWND) { return TRUE; }
BOOL WINAPI TestEmptyClipboard() { ++clipboardWrites; return TRUE; }
BOOL WINAPI TestCloseClipboard() { return TRUE; }
HANDLE WINAPI TestSetClipboardData(UINT format, HANDLE memory)
{
    if (format != CF_UNICODETEXT) return NULL;
    const wchar_t* text = static_cast<const wchar_t*>(GlobalLock(memory));
    if (text == NULL) return NULL;
    capturedText = text;
    GlobalUnlock(memory);
    capturedMemory = memory;
    return memory;
}

#define OpenClipboard TestOpenClipboard
#define EmptyClipboard TestEmptyClipboard
#define CloseClipboard TestCloseClipboard
#define SetClipboardData TestSetClipboardData
#include "../src/CopyPathMenu.cpp"
#undef OpenClipboard
#undef EmptyClipboard
#undef CloseClipboard
#undef SetClipboardData

void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void Check(HRESULT result)
{
    if (FAILED(result)) throw ShellOperationError{ result };
}

class ViewSite final : public IServiceProvider, public IFolderView
{
public:
    explicit ViewSite(IShellItemArray* items) : items_(items) {}
    HRESULT enumerationResult = S_OK;
    UINT requestedFlags = 0;

    STDMETHODIMP QueryInterface(REFIID id, void** object) override
    {
        if (object == NULL) return E_POINTER;
        *object = NULL;
        if (id == IID_IUnknown || id == __uuidof(IServiceProvider))
            *object = static_cast<IServiceProvider*>(this);
        else if (id == __uuidof(IFolderView))
            *object = static_cast<IFolderView*>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&references_); }
    STDMETHODIMP_(ULONG) Release() override
    {
        LONG count = InterlockedDecrement(&references_);
        if (count == 0) delete this;
        return count;
    }
    STDMETHODIMP QueryService(REFGUID service, REFIID id, void** object) override
    {
        if (object == NULL) return E_POINTER;
        *object = NULL;
        return service == SID_SFolderView ? QueryInterface(id, object) : E_NOINTERFACE;
    }
    STDMETHODIMP Items(UINT flags, REFIID id, void** object) override
    {
        requestedFlags = flags;
        if (object == NULL) return E_POINTER;
        *object = NULL;
        if (FAILED(enumerationResult)) return enumerationResult;
        return items_->QueryInterface(id, object);
    }
    STDMETHODIMP GetCurrentViewMode(UINT*) override { return E_NOTIMPL; }
    STDMETHODIMP SetCurrentViewMode(UINT) override { return E_NOTIMPL; }
    STDMETHODIMP GetFolder(REFIID, void**) override { return E_NOTIMPL; }
    STDMETHODIMP Item(int, PITEMID_CHILD*) override { return E_NOTIMPL; }
    STDMETHODIMP ItemCount(UINT, int*) override { return E_NOTIMPL; }
    STDMETHODIMP GetSelectionMarkedItem(int*) override { return E_NOTIMPL; }
    STDMETHODIMP GetFocusedItem(int*) override { return E_NOTIMPL; }
    STDMETHODIMP GetItemPosition(PCUITEMID_CHILD, POINT*) override { return E_NOTIMPL; }
    STDMETHODIMP GetSpacing(POINT*) override { return E_NOTIMPL; }
    STDMETHODIMP GetDefaultSpacing(POINT*) override { return E_NOTIMPL; }
    STDMETHODIMP GetAutoArrange() override { return E_NOTIMPL; }
    STDMETHODIMP SelectItem(int, DWORD) override { return E_NOTIMPL; }
    STDMETHODIMP SelectAndPositionItems(UINT, PCUITEMID_CHILD_ARRAY, POINT*, DWORD) override { return E_NOTIMPL; }
private:
    LONG references_ = 1;
    ComPtr<IShellItemArray> items_;
};

class Fixture
{
public:
    std::vector<std::wstring> paths;
    Fixture()
    {
        wchar_t temporary[MAX_PATH] = {};
        Require(GetTempPathW(MAX_PATH, temporary) != 0, "GetTempPath");
        GUID id;
        Check(CoCreateGuid(&id));
        wchar_t suffix[40] = {};
        StringFromGUID2(id, suffix, 40);
        root_ = std::wstring(temporary) + L"CopyPathMenuTests-" + suffix;
        Require(CreateDirectoryW(root_.c_str(), NULL) != 0, "Create test directory");
        for (const wchar_t* name : { L"file1.txt", L"file2.txt", L"file10.txt", L"中文 空格.txt", L"z.txt" })
        {
            std::wstring path = root_ + L"\\" + name;
            HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
            Require(file != INVALID_HANDLE_VALUE, "Create fixture file");
            CloseHandle(file);
            paths.push_back(path);
        }
    }
    ~Fixture()
    {
        for (const auto& path : paths) DeleteFileW(path.c_str());
        RemoveDirectoryW(root_.c_str());
    }
    ComPtr<IShellItemArray> Array(const std::vector<size_t>& order) const
    {
        std::vector<PIDLIST_ABSOLUTE> pidls;
        for (size_t index : order)
        {
            PIDLIST_ABSOLUTE pidl = NULL;
            Check(SHParseDisplayName(paths.at(index).c_str(), NULL, &pidl, 0, NULL));
            pidls.push_back(pidl);
        }
        ComPtr<IShellItemArray> array;
        HRESULT result = SHCreateShellItemArrayFromIDLists(static_cast<UINT>(pidls.size()),
            const_cast<PCIDLIST_ABSOLUTE*>(pidls.data()), &array);
        for (auto pidl : pidls) CoTaskMemFree(pidl);
        Check(result);
        return array;
    }
    std::wstring Text(const std::vector<size_t>& order) const
    {
        std::wstring text;
        for (size_t index : order)
        {
            if (!text.empty()) text += L"\r\n";
            text += paths.at(index);
        }
        return text;
    }
private:
    std::wstring root_;
};

HRESULT ExecuteSelection(IShellItemArray* selection, ViewSite* site)
{
    ComPtr<IObjectWithSelection> command;
    command.Attach(new CopyPathCommand());
    Check(command->SetSelection(selection));
    ComPtr<IObjectWithSite> withSite;
    Check(command.As(&withSite));
    Check(withSite->SetSite(site == NULL ? NULL : static_cast<IServiceProvider*>(site)));
    ComPtr<IExecuteCommand> execute;
    Check(command.As(&execute));
    capturedText = L"existing clipboard";
    clipboardWrites = 0;
    HRESULT result = execute->Execute();
    if (capturedMemory != NULL)
    {
        GlobalFree(capturedMemory);
        capturedMemory = NULL;
    }
    return result;
}

void ExpectOrder(const Fixture& fixture, const char* name, const std::vector<size_t>& selected,
    const std::vector<size_t>& view, const std::vector<size_t>& expected)
{
    auto selection = fixture.Array(selected);
    auto visibleItems = fixture.Array(view);
    ComPtr<ViewSite> site;
    site.Attach(new ViewSite(visibleItems.Get()));
    Check(ExecuteSelection(selection.Get(), site.Get()));
    Require(capturedText == fixture.Text(expected), "Unexpected path order or formatting");
    Require(clipboardWrites == 1, "Selection must write once");
    Require(site->requestedFlags == (SVGIO_ALLVIEW | SVGIO_FLAG_VIEWORDER), "Must order the original snapshot by all view items");
    std::cout << "PASS " << name << '\n';
}

void ExpectFailure(IShellItemArray* selection, ViewSite* site, const char* name)
{
    Require(FAILED(ExecuteSelection(selection, site)), "Operation unexpectedly succeeded");
    Require(clipboardWrites == 0 && capturedText == L"existing clipboard", "Failure changed the clipboard");
    std::cout << "PASS " << name << '\n';
}

void TestComContract(const Fixture& fixture)
{
    ComPtr<IObjectWithSelection> command;
    command.Attach(new CopyPathCommand());
    ComPtr<IObjectWithSite> withSite;
    ComPtr<IExecuteCommand> execute;
    Check(command.As(&withSite));
    Check(command.As(&execute));
    ComPtr<IUnknown> selectionIdentity, siteIdentity, executeIdentity;
    Check(command.As(&selectionIdentity));
    Check(withSite.As(&siteIdentity));
    Check(execute.As(&executeIdentity));
    Require(selectionIdentity.Get() == siteIdentity.Get() && siteIdentity.Get() == executeIdentity.Get(), "COM identity mismatch");
    Require(withSite->GetSite(IID_IUnknown, NULL) == E_POINTER, "Null site output");
    ComPtr<IUnknown> retrieved;
    Require(withSite->GetSite(IID_PPV_ARGS(&retrieved)) == E_NOINTERFACE, "Unset site");
    auto array = fixture.Array({0, 1});
    ComPtr<ViewSite> site;
    site.Attach(new ViewSite(array.Get()));
    Check(withSite->SetSite(static_cast<IServiceProvider*>(site.Get())));
    Check(withSite->GetSite(IID_PPV_ARGS(&retrieved)));
    Check(withSite->SetSite(NULL));
    retrieved.Reset();
    Require(withSite->GetSite(IID_PPV_ARGS(&retrieved)) == E_NOINTERFACE, "Cleared site still retained");
    std::cout << "PASS COM identity, site lifetime and clearing\n";
}

void TestDirectoryFallbackAndCommandLine()
{
    ComPtr<IExecuteCommand> execute;
    execute.Attach(new CopyPathCommand());
    Check(execute->SetDirectory(L"D:\\Example\\当前目录"));
    capturedText = L"existing clipboard";
    clipboardWrites = 0;
    Check(execute->Execute());
    if (capturedMemory != NULL)
    {
        GlobalFree(capturedMemory);
        capturedMemory = NULL;
    }
    Require(capturedText == L"D:\\Example\\当前目录" && clipboardWrites == 1, "SetDirectory fallback failed");
    std::cout << "PASS SetDirectory fallback without selection\n";

    wchar_t prog[] = L"copy-path.exe";
    wchar_t flag[] = L"--copy";
    wchar_t driveRoot[] = L"D:\"";
    wchar_t folderEscaped[] = L"D:\\文件夹\"";
    wchar_t* validArgs[] = { prog, flag, driveRoot, folderEscaped };
    capturedText = L"existing clipboard";
    clipboardWrites = 0;
    Require(RunCopyCommandLine(4, validArgs) == 0, "RunCopyCommandLine drive root normalization failed");
    if (capturedMemory != NULL)
    {
        GlobalFree(capturedMemory);
        capturedMemory = NULL;
    }
    Require(capturedText == L"D:\\\r\nD:\\文件夹" && clipboardWrites == 1, "Normalized command line text mismatch");

    wchar_t invalidQuote[] = L"D:\\bad\"path";
    wchar_t* invalidArgs[] = { prog, flag, invalidQuote };
    capturedText = L"existing clipboard";
    clipboardWrites = 0;
    Require(RunCopyCommandLine(3, invalidArgs) == 1, "Invalid embedded quote should fail");
    Require(clipboardWrites == 0 && capturedText == L"existing clipboard", "Failed CLI altered clipboard");
    std::cout << "PASS command-line drive root %V normalization\n";
}

int wmain()
{
    if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) return 2;
    int result = 0;
    try
    {
        Fixture fixture;
        ExpectOrder(fixture, "focus-rotated selection", {3, 4, 0, 1, 2}, {0, 1, 2, 3, 4}, {0, 1, 2, 3, 4});
        ExpectOrder(fixture, "descending view", {2, 1, 0, 4, 3}, {4, 3, 2, 1, 0}, {4, 3, 2, 1, 0});
        ExpectOrder(fixture, "arbitrary date/group view order", {0, 1, 2, 3, 4}, {3, 1, 4, 0, 2}, {3, 1, 4, 0, 2});
        ExpectOrder(fixture, "unselected items excluded", {3, 1}, {0, 1, 2, 3, 4}, {1, 3});
        ExpectOrder(fixture, "duplicate original items preserved", {3, 1, 3, 1}, {0, 1, 2, 3, 4}, {1, 1, 3, 3});
        ExpectOrder(fixture, "duplicate view entries emitted once", {3, 1}, {1, 1, 3, 3}, {1, 3});

        auto selection = fixture.Array({0, 1});
        ExpectFailure(selection.Get(), NULL, "multi-selection without caller view fails");
        ExpectFailure(NULL, NULL, "null selection fails");
        auto missing = fixture.Array({0, 2, 3});
        ComPtr<ViewSite> missingSite;
        missingSite.Attach(new ViewSite(missing.Get()));
        ExpectFailure(selection.Get(), missingSite.Get(), "missing original item fails atomically");
        missingSite->enumerationResult = E_ACCESSDENIED;
        ExpectFailure(selection.Get(), missingSite.Get(), "view enumeration failure is propagated");

        auto single = fixture.Array({3});
        Check(ExecuteSelection(single.Get(), NULL));
        Require(capturedText == fixture.Text({3}), "Single selection or Unicode damaged");
        std::cout << "PASS single Unicode path without site\n";

        PIDLIST_ABSOLUTE virtualPidl = NULL;
        Check(SHParseDisplayName(L"::{645FF040-5081-101B-9F08-00AA002F954E}", NULL, &virtualPidl, 0, NULL));
        ComPtr<IShellItemArray> virtualSelection;
        PCIDLIST_ABSOLUTE virtualItems[] = { virtualPidl };
        HRESULT virtualResult = SHCreateShellItemArrayFromIDLists(1, virtualItems, &virtualSelection);
        CoTaskMemFree(virtualPidl);
        Check(virtualResult);
        ExpectFailure(virtualSelection.Get(), NULL, "non-filesystem selection fails atomically");
        TestComContract(fixture);
        TestDirectoryFallbackAndCommandLine();
        std::cout << "All 15 test cases passed.\n";
    }
    catch (const ShellOperationError& error)
    {
        std::cerr << "FAIL HRESULT=0x" << std::hex << static_cast<unsigned long>(error.result) << '\n';
        result = 1;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL " << error.what() << '\n';
        result = 1;
    }
    CoUninitialize();
    return result;
}

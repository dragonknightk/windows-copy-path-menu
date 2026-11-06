#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlguid.h>
#include <objbase.h>
#include <wrl/client.h>
#include <algorithm>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using Microsoft::WRL::ComPtr;

    const CLSID CLSID_CopyPathCommand =
    { 0x7e4dc989, 0x2aba, 0x458a, { 0x94, 0xd3, 0xa3, 0x50, 0xa3, 0x52, 0xc3, 0x2e } };

    const DWORD kClipboardAttempts = 5;
    const DWORD kClipboardRetryMilliseconds = 40;
    const DWORD kIdleTimeoutMilliseconds = 30000;

    HANDLE g_activityEvent = NULL;
    volatile LONG g_objectCount = 0;
    volatile LONG g_serverLockCount = 0;
    volatile LONG g_activeCallCount = 0;
    volatile LONG g_exitRequested = 0;

    void NoteActivity()
    {
        if (g_activityEvent != NULL)
        {
            SetEvent(g_activityEvent);
        }
    }

    void RequestExitAfterCurrentCall()
    {
        InterlockedExchange(&g_exitRequested, 1);
        NoteActivity();
    }

    HRESULT HResultFromLastError()
    {
        const DWORD error = GetLastError();
        return error == ERROR_SUCCESS ? E_FAIL : HRESULT_FROM_WIN32(error);
    }

    class ActiveCallScope
    {
    public:
        ActiveCallScope()
        {
            InterlockedIncrement(&g_activeCallCount);
            NoteActivity();
        }

        ~ActiveCallScope()
        {
            InterlockedDecrement(&g_activeCallCount);
            NoteActivity();
        }
    };

    bool IsEmptyArgument(const wchar_t* value)
    {
        return value == NULL || value[0] == L'\0';
    }

    HRESULT FormatPaths(const std::vector<std::wstring>& paths, std::wstring* text)
    {
        if (text == NULL || paths.empty())
        {
            return E_INVALIDARG;
        }

        size_t totalLength = 0;
        for (size_t index = 0; index < paths.size(); ++index)
        {
            if (paths[index].empty())
            {
                return E_INVALIDARG;
            }

            totalLength += paths[index].size();
            if (index != 0)
            {
                totalLength += 2;
            }
        }

        std::wstring result;
        result.reserve(totalLength);
        for (size_t index = 0; index < paths.size(); ++index)
        {
            if (index != 0)
            {
                result.append(L"\r\n");
            }

            result.append(paths[index]);
        }

        *text = result;
        return S_OK;
    }

    HRESULT CopyUnicodeTextWithRetry(const std::wstring& text)
    {
        if (text.empty())
        {
            return E_INVALIDARG;
        }

        const SIZE_T byteCount = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, byteCount);
        if (memory == NULL)
        {
            return E_OUTOFMEMORY;
        }

        void* destination = GlobalLock(memory);
        if (destination == NULL)
        {
            const HRESULT result = HResultFromLastError();
            GlobalFree(memory);
            return result;
        }

        CopyMemory(destination, text.c_str(), byteCount);
        GlobalUnlock(memory);

        HRESULT lastError = HRESULT_FROM_WIN32(ERROR_BUSY);
        for (DWORD attempt = 0; attempt < kClipboardAttempts; ++attempt)
        {
            if (OpenClipboard(NULL))
            {
                HRESULT result = S_OK;

                if (!EmptyClipboard())
                {
                    result = HResultFromLastError();
                }
                else
                {
                    if (SetClipboardData(CF_UNICODETEXT, memory) == NULL)
                    {
                        result = HResultFromLastError();
                    }
                    else
                    {
                        memory = NULL;
                    }
                }

                CloseClipboard();
                if (SUCCEEDED(result))
                {
                    return S_OK;
                }

                lastError = result;
                break;
            }

            lastError = HResultFromLastError();
            if (attempt + 1 != kClipboardAttempts)
            {
                Sleep(kClipboardRetryMilliseconds);
            }
        }

        if (memory != NULL)
        {
            GlobalFree(memory);
        }

        return lastError;
    }

    class CoTaskMemString
    {
    public:
        CoTaskMemString() : value(NULL)
        {
        }

        ~CoTaskMemString()
        {
            if (value != NULL)
            {
                CoTaskMemFree(value);
            }
        }

        PWSTR value;
    };

    struct ShellOperationError
    {
        HRESULT result;
    };

    int CompareShellItems(IShellItem* left, IShellItem* right)
    {
        int order = 0;
        const HRESULT result = left->Compare(right, SICHINT_CANONICAL, &order);
        if (FAILED(result))
        {
            throw ShellOperationError{ result };
        }
        return order;
    }

    HRESULT GetCallerFolderView(IUnknown* site, IFolderView** view)
    {
        *view = NULL;
        if (site == NULL)
        {
            return E_NOINTERFACE;
        }

        ComPtr<IServiceProvider> services;
        HRESULT result = site->QueryInterface(IID_PPV_ARGS(&services));
        if (FAILED(result))
        {
            return result;
        }

        result = services->QueryService(SID_SFolderView, IID_PPV_ARGS(view));
        if (SUCCEEDED(result) && *view != NULL)
        {
            return S_OK;
        }

        // 不同 Shell 宿主通过视图服务或浏览器服务提供同一个调用方视图。
        ComPtr<IShellBrowser> browser;
        result = services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser));
        if (FAILED(result))
        {
            return result;
        }
        if (!browser)
        {
            return E_FAIL;
        }

        ComPtr<IShellView> shellView;
        result = browser->QueryActiveShellView(&shellView);
        if (FAILED(result))
        {
            return result;
        }
        return shellView ? shellView->QueryInterface(IID_PPV_ARGS(view)) : E_FAIL;
    }

    struct SelectedPath
    {
        ComPtr<IShellItem> item;
        std::wstring path;
        bool emitted = false;
    };

    HRESULT GetPathsInViewOrder(IShellItemArray* selection, IUnknown* site,
        std::vector<std::wstring>* paths)
    {
        if (selection == NULL)
        {
            return E_FAIL;
        }

        DWORD count = 0;
        HRESULT result = selection->GetCount(&count);
        if (FAILED(result))
        {
            return result;
        }
        if (count == 0)
        {
            return E_INVALIDARG;
        }

        std::vector<SelectedPath> selected;
        selected.reserve(count);
        for (DWORD index = 0; index < count; ++index)
        {
            SelectedPath entry;
            result = selection->GetItemAt(index, &entry.item);
            if (FAILED(result) || !entry.item)
            {
                return FAILED(result) ? result : E_FAIL;
            }

            CoTaskMemString path;
            result = entry.item->GetDisplayName(SIGDN_FILESYSPATH, &path.value);
            if (FAILED(result) || path.value == NULL || path.value[0] == L'\0')
            {
                return FAILED(result) ? result : E_FAIL;
            }
            entry.path = path.value;
            selected.push_back(std::move(entry));
        }

        paths->clear();
        paths->reserve(count);
        if (count == 1)
        {
            paths->push_back(selected[0].path);
            return S_OK;
        }

        ComPtr<IFolderView> view;
        result = GetCallerFolderView(site, &view);
        if (FAILED(result))
        {
            return result;
        }

        ComPtr<IShellItemArray> visibleItems;
        result = view->Items(SVGIO_ALLVIEW | SVGIO_FLAG_VIEWORDER, IID_PPV_ARGS(&visibleItems));
        if (FAILED(result) || !visibleItems)
        {
            return FAILED(result) ? result : E_FAIL;
        }

        // 保留菜单触发时的对象快照；视图只提供次序，后续选择变化不能改变复制对象。
        // 用 Shell 的规范身份匹配，避免按路径字符串比较误处理大小写或同名对象。
        std::stable_sort(selected.begin(), selected.end(), [](const SelectedPath& left, const SelectedPath& right) {
            return CompareShellItems(left.item.Get(), right.item.Get()) < 0;
        });

        DWORD visibleCount = 0;
        result = visibleItems->GetCount(&visibleCount);
        if (FAILED(result))
        {
            return result;
        }
        for (DWORD index = 0; index < visibleCount && paths->size() < count; ++index)
        {
            ComPtr<IShellItem> item;
            result = visibleItems->GetItemAt(index, &item);
            if (FAILED(result) || !item)
            {
                return FAILED(result) ? result : E_FAIL;
            }
            auto match = std::lower_bound(selected.begin(), selected.end(), item.Get(),
                [](const SelectedPath& entry, IShellItem* candidate) {
                    return CompareShellItems(entry.item.Get(), candidate) < 0;
                });
            while (match != selected.end() && CompareShellItems(match->item.Get(), item.Get()) == 0)
            {
                if (!match->emitted)
                {
                    paths->push_back(match->path);
                    match->emitted = true;
                }
                ++match;
            }
        }
        return paths->size() == count ? S_OK : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }

    HRESULT CopyShellSelection(IShellItemArray* selection, IUnknown* site)
    {
        try
        {
            std::vector<std::wstring> paths;
            HRESULT result = GetPathsInViewOrder(selection, site, &paths);
            if (FAILED(result))
            {
                return result;
            }
            std::wstring text;
            result = FormatPaths(paths, &text);
            if (FAILED(result))
            {
                return result;
            }

            return CopyUnicodeTextWithRetry(text);
        }
        catch (const ShellOperationError& error)
        {
            return error.result;
        }
        catch (const std::bad_alloc&)
        {
            return E_OUTOFMEMORY;
        }
        catch (...)
        {
            return E_FAIL;
        }
    }

    class CopyPathCommand : public IExecuteCommand, public IObjectWithSelection, public IObjectWithSite
    {
    public:
        CopyPathCommand() : referenceCount_(1), selection_(NULL), site_(NULL)
        {
            InitializeCriticalSection(&stateLock_);
            InterlockedIncrement(&g_objectCount);
            NoteActivity();
        }

        virtual ~CopyPathCommand()
        {
            EnterCriticalSection(&stateLock_);
            IShellItemArray* selection = selection_;
            IUnknown* site = site_;
            selection_ = NULL;
            site_ = NULL;
            LeaveCriticalSection(&stateLock_);
            if (selection != NULL)
            {
                selection->Release();
            }
            if (site != NULL)
            {
                site->Release();
            }

            DeleteCriticalSection(&stateLock_);
            InterlockedDecrement(&g_objectCount);
            NoteActivity();
        }

        STDMETHODIMP QueryInterface(REFIID interfaceId, void** object)
        {
            if (object == NULL)
            {
                return E_POINTER;
            }

            *object = NULL;
            if (interfaceId == IID_IUnknown || IsEqualIID(interfaceId, __uuidof(IExecuteCommand)))
            {
                *object = static_cast<IExecuteCommand*>(this);
            }
            else if (IsEqualIID(interfaceId, __uuidof(IObjectWithSelection)))
            {
                *object = static_cast<IObjectWithSelection*>(this);
            }
            else if (IsEqualIID(interfaceId, __uuidof(IObjectWithSite)))
            {
                *object = static_cast<IObjectWithSite*>(this);
            }
            else
            {
                return E_NOINTERFACE;
            }

            AddRef();
            return S_OK;
        }

        STDMETHODIMP_(ULONG) AddRef()
        {
            return static_cast<ULONG>(InterlockedIncrement(&referenceCount_));
        }

        STDMETHODIMP_(ULONG) Release()
        {
            const LONG remaining = InterlockedDecrement(&referenceCount_);
            if (remaining == 0)
            {
                delete this;
                return 0;
            }

            return static_cast<ULONG>(remaining);
        }

        STDMETHODIMP SetKeyState(DWORD)
        {
            ActiveCallScope call;
            NoteActivity();
            return S_OK;
        }

        STDMETHODIMP SetParameters(LPCWSTR)
        {
            ActiveCallScope call;
            NoteActivity();
            return S_OK;
        }

        STDMETHODIMP SetPosition(POINT)
        {
            ActiveCallScope call;
            NoteActivity();
            return S_OK;
        }

        STDMETHODIMP SetShowWindow(int)
        {
            ActiveCallScope call;
            NoteActivity();
            return S_OK;
        }

        STDMETHODIMP SetNoShowUI(BOOL)
        {
            ActiveCallScope call;
            NoteActivity();
            return S_OK;
        }

        STDMETHODIMP SetDirectory(LPCWSTR directory)
        {
            ActiveCallScope call;
            NoteActivity();
            try
            {
                std::wstring copied;
                if (!IsEmptyArgument(directory))
                {
                    copied = directory;
                }

                EnterCriticalSection(&stateLock_);
                directory_.swap(copied);
                LeaveCriticalSection(&stateLock_);
                return S_OK;
            }
            catch (const std::bad_alloc&)
            {
                return E_OUTOFMEMORY;
            }
            catch (...)
            {
                return E_FAIL;
            }
        }

        STDMETHODIMP Execute()
        {
            ActiveCallScope call;
            HRESULT result = E_FAIL;
            try
            {
                ComPtr<IShellItemArray> selection;
                ComPtr<IUnknown> site;
                std::wstring directory;
                EnterCriticalSection(&stateLock_);
                selection = selection_;
                site = site_;
                directory = directory_;
                LeaveCriticalSection(&stateLock_);

                if (selection == NULL && !directory.empty())
                {
                    result = CopyUnicodeTextWithRetry(directory);
                }
                else
                {
                    result = CopyShellSelection(selection.Get(), site.Get());
                }
            }
            catch (const std::bad_alloc&)
            {
                result = E_OUTOFMEMORY;
            }
            catch (...)
            {
                result = E_FAIL;
            }

            RequestExitAfterCurrentCall();
            return result;
        }

        STDMETHODIMP SetSelection(IShellItemArray* selection)
        {
            ActiveCallScope call;
            if (selection != NULL)
            {
                selection->AddRef();
            }

            EnterCriticalSection(&stateLock_);
            IShellItemArray* previous = selection_;
            selection_ = selection;
            LeaveCriticalSection(&stateLock_);

            if (previous != NULL)
            {
                previous->Release();
            }

            return S_OK;
        }

        STDMETHODIMP GetSelection(REFIID interfaceId, void** selection)
        {
            ActiveCallScope call;
            if (selection == NULL)
            {
                return E_POINTER;
            }

            *selection = NULL;
            EnterCriticalSection(&stateLock_);
            IShellItemArray* current = selection_;
            if (current != NULL)
            {
                current->AddRef();
            }

            LeaveCriticalSection(&stateLock_);
            if (current == NULL)
            {
                return E_NOINTERFACE;
            }

            const HRESULT result = current->QueryInterface(interfaceId, selection);
            current->Release();
            return result;
        }

        STDMETHODIMP SetSite(IUnknown* site)
        {
            ActiveCallScope call;
            if (site != NULL)
            {
                site->AddRef();
            }
            EnterCriticalSection(&stateLock_);
            IUnknown* previous = site_;
            site_ = site;
            LeaveCriticalSection(&stateLock_);
            if (previous != NULL)
            {
                previous->Release();
            }
            return S_OK;
        }

        STDMETHODIMP GetSite(REFIID interfaceId, void** site)
        {
            ActiveCallScope call;
            if (site == NULL)
            {
                return E_POINTER;
            }
            *site = NULL;
            ComPtr<IUnknown> current;
            EnterCriticalSection(&stateLock_);
            current = site_;
            LeaveCriticalSection(&stateLock_);
            return current ? current->QueryInterface(interfaceId, site) : E_NOINTERFACE;
        }

    private:
        volatile LONG referenceCount_;
        CRITICAL_SECTION stateLock_;
        IShellItemArray* selection_;
        IUnknown* site_;
        std::wstring directory_;
    };

    class CopyPathClassFactory : public IClassFactory
    {
    public:
        CopyPathClassFactory() : referenceCount_(1)
        {
        }

        STDMETHODIMP QueryInterface(REFIID interfaceId, void** object)
        {
            if (object == NULL)
            {
                return E_POINTER;
            }

            *object = NULL;
            if (interfaceId != IID_IUnknown && interfaceId != IID_IClassFactory)
            {
                return E_NOINTERFACE;
            }

            *object = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }

        STDMETHODIMP_(ULONG) AddRef()
        {
            return static_cast<ULONG>(InterlockedIncrement(&referenceCount_));
        }

        STDMETHODIMP_(ULONG) Release()
        {
            const LONG remaining = InterlockedDecrement(&referenceCount_);
            if (remaining == 0)
            {
                delete this;
                return 0;
            }

            return static_cast<ULONG>(remaining);
        }

        STDMETHODIMP CreateInstance(IUnknown* outer, REFIID interfaceId, void** object)
        {
            ActiveCallScope call;
            if (object == NULL)
            {
                return E_POINTER;
            }

            *object = NULL;
            if (outer != NULL)
            {
                return CLASS_E_NOAGGREGATION;
            }

            CopyPathCommand* command = new (std::nothrow) CopyPathCommand();
            if (command == NULL)
            {
                return E_OUTOFMEMORY;
            }

            const HRESULT result = command->QueryInterface(interfaceId, object);
            command->Release();
            return result;
        }

        STDMETHODIMP LockServer(BOOL lock)
        {
            ActiveCallScope call;
            if (lock)
            {
                InterlockedIncrement(&g_serverLockCount);
            }
            else
            {
                InterlockedDecrement(&g_serverLockCount);
            }

            NoteActivity();
            return S_OK;
        }

    private:
        volatile LONG referenceCount_;
    };

    bool ShutdownConditionsMet()
    {
        return InterlockedCompareExchange(&g_activeCallCount, 0, 0) == 0
            && InterlockedCompareExchange(&g_objectCount, 0, 0) == 0
            && InterlockedCompareExchange(&g_serverLockCount, 0, 0) == 0;
    }

    HRESULT SuspendAndDrainClassObjects()
    {
        HRESULT result = CoSuspendClassObjects();
        if (FAILED(result))
        {
            return result;
        }

        while (!ShutdownConditionsMet())
        {
            const DWORD waitResult = WaitForSingleObject(g_activityEvent, kIdleTimeoutMilliseconds);
            if (waitResult == WAIT_TIMEOUT)
            {
                break;
            }
            if (waitResult != WAIT_OBJECT_0)
            {
                return waitResult == WAIT_FAILED ? HResultFromLastError() : E_FAIL;
            }

            ResetEvent(g_activityEvent);
        }

        return S_OK;
    }

    HRESULT RunComServer()
    {
        HRESULT result = CoInitializeEx(NULL, COINIT_MULTITHREADED);
        if (FAILED(result))
        {
            return result;
        }

        g_activityEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (g_activityEvent == NULL)
        {
            const HRESULT eventResult = HResultFromLastError();
            CoUninitialize();
            return eventResult;
        }

        CopyPathClassFactory* factory = new (std::nothrow) CopyPathClassFactory();
        if (factory == NULL)
        {
            CloseHandle(g_activityEvent);
            g_activityEvent = NULL;
            CoUninitialize();
            return E_OUTOFMEMORY;
        }

        DWORD classCookie = 0;
        result = CoRegisterClassObject(
            CLSID_CopyPathCommand,
            static_cast<IClassFactory*>(factory),
            CLSCTX_LOCAL_SERVER,
            REGCLS_MULTIPLEUSE,
            &classCookie);
        factory->Release();
        if (FAILED(result))
        {
            CloseHandle(g_activityEvent);
            g_activityEvent = NULL;
            CoUninitialize();
            return result;
        }

        bool classObjectsSuspended = false;
        NoteActivity();
        for (;;)
        {
            const DWORD waitResult = WaitForSingleObject(g_activityEvent, kIdleTimeoutMilliseconds);
            bool shouldBeginShutdown = false;
            if (waitResult == WAIT_OBJECT_0)
            {
                ResetEvent(g_activityEvent);
                shouldBeginShutdown = InterlockedCompareExchange(&g_exitRequested, 0, 0) != 0;
            }
            else if (waitResult == WAIT_TIMEOUT)
            {
                shouldBeginShutdown = true;
            }
            else
            {
                result = HResultFromLastError();
                break;
            }

            if (!shouldBeginShutdown)
            {
                continue;
            }

            result = SuspendAndDrainClassObjects();
            if (FAILED(result))
            {
                break;
            }

            classObjectsSuspended = true;
            break;
        }

        if (classObjectsSuspended)
        {
            CoRevokeClassObject(classCookie);
        }
        CloseHandle(g_activityEvent);
        g_activityEvent = NULL;
        CoUninitialize();
        return result;
    }

    std::wstring NormalizeCommandLinePath(const wchar_t* argument)
    {
        if (IsEmptyArgument(argument))
        {
            return std::wstring();
        }

        std::wstring path(argument);
        // 资源管理器在磁盘根目录空白处展开 "%V" 时会传入 "D:\"，
        // CommandLineToArgvW 会把末尾的 \" 解析为字面量双引号（即 D:"）。
        // Windows 文件系统路径不允许包含双引号，此处将其还原为标准盘符根路径。
        if (path.size() >= 2 && path.back() == L'"')
        {
            if (path.size() == 3 && path[1] == L':')
            {
                path.back() = L'\\';
            }
            else
            {
                path.pop_back();
                if (path.size() == 2 && path[1] == L':')
                {
                    path.push_back(L'\\');
                }
            }
        }

        if (path.find(L'"') != std::wstring::npos)
        {
            return std::wstring();
        }

        return path;
    }

    int RunCopyCommandLine(int argumentCount, wchar_t** arguments)
    {
        try
        {
            if (argumentCount < 3)
            {
                return 1;
            }

            std::vector<std::wstring> paths;
            paths.reserve(static_cast<size_t>(argumentCount - 2));
            for (int index = 2; index < argumentCount; ++index)
            {
                std::wstring normalized = NormalizeCommandLinePath(arguments[index]);
                if (normalized.empty())
                {
                    return 1;
                }

                paths.push_back(std::move(normalized));
            }

            std::wstring text;
            if (FAILED(FormatPaths(paths, &text)))
            {
                return 1;
            }

            return SUCCEEDED(CopyUnicodeTextWithRetry(text)) ? 0 : 2;
        }
        catch (...)
        {
            return 1;
        }
    }

    bool IsEmbeddingArgument(const wchar_t* argument)
    {
        return argument != NULL
            && (lstrcmpiW(argument, L"-Embedding") == 0 || lstrcmpiW(argument, L"/Embedding") == 0);
    }

    bool IsHelpArgument(const wchar_t* argument)
    {
        return argument != NULL
            && (lstrcmpiW(argument, L"--help") == 0
                || lstrcmpiW(argument, L"-h") == 0
                || lstrcmpiW(argument, L"/?") == 0);
    }

    bool IsVersionArgument(const wchar_t* argument)
    {
        return argument != NULL
            && (lstrcmpiW(argument, L"--version") == 0
                || lstrcmpiW(argument, L"-v") == 0);
    }

    void WriteParentConsoleText(const wchar_t* text)
    {
        if (text == NULL)
        {
            return;
        }

        const BOOL attached = AttachConsole(ATTACH_PARENT_PROCESS);
        HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
        if (output != NULL && output != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            const DWORD length = static_cast<DWORD>(lstrlenW(text));
            if (!WriteConsoleW(output, text, length, &written, NULL))
            {
                const int utf8Bytes = WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), NULL, 0, NULL, NULL);
                if (utf8Bytes > 0)
                {
                    std::string utf8(static_cast<size_t>(utf8Bytes), '\0');
                    WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), &utf8[0], utf8Bytes, NULL, NULL);
                    WriteFile(output, utf8.data(), static_cast<DWORD>(utf8.size()), &written, NULL);
                }
            }
        }

        if (attached)
        {
            FreeConsole();
        }
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments == NULL)
    {
        return 1;
    }

    int exitCode = 1;
    try
    {
        if (argumentCount >= 2 && lstrcmpW(arguments[1], L"--copy") == 0)
        {
            exitCode = RunCopyCommandLine(argumentCount, arguments);
        }
        else if (argumentCount == 2 && IsEmbeddingArgument(arguments[1]))
        {
            exitCode = SUCCEEDED(RunComServer()) ? 0 : 1;
        }
        else if (argumentCount == 2 && IsVersionArgument(arguments[1]))
        {
            WriteParentConsoleText(L"DragonKnightK.CopyPathMenu 1.0.0\r\n");
            exitCode = 0;
        }
        else if (argumentCount == 2 && IsHelpArgument(arguments[1]))
        {
            WriteParentConsoleText(
                L"DragonKnightK.CopyPathMenu 1.0.0\r\n"
                L"用法:\r\n"
                L"  copy-path.exe --copy <路径1> [路径2 ...]\r\n"
                L"  copy-path.exe -Embedding\r\n"
                L"  copy-path.exe --version\r\n");
            exitCode = 0;
        }
    }
    catch (...)
    {
        exitCode = 1;
    }

    LocalFree(arguments);
    return exitCode;
}

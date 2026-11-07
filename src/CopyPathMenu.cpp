#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlguid.h>
#include <exdisp.h>
#include <objbase.h>
#include <commctrl.h>
#include <wrl/client.h>
#include <algorithm>
#include <cwctype>
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

    const wchar_t* const kSettingsRegistrySubkey = L"Software\\DragonKnightK.CopyPathMenu";
    const wchar_t* const kHotkeyWindowClass = L"DragonKnightK.CopyPathMenu.HotkeyHost";
    const wchar_t* const kConfigWindowClass = L"DragonKnightK.CopyPathMenu.ConfigDialog";

    const int kActiveHotkeyId = 1;
    const int kProbeHotkeyId = 2;
    const UINT kMsgReloadHotkey = WM_APP + 1;
    const UINT kMsgSuspendHotkey = WM_APP + 2;
    const UINT kMsgStopDaemon = WM_APP + 3;

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

    HRESULT OrderSelectionByFolderView(std::vector<SelectedPath>& selected, IFolderView* view,
        std::vector<std::wstring>* paths)
    {
        if (view == NULL || paths == NULL)
        {
            return E_NOINTERFACE;
        }

        const DWORD count = static_cast<DWORD>(selected.size());
        ComPtr<IShellItemArray> visibleItems;
        HRESULT result = view->Items(SVGIO_ALLVIEW | SVGIO_FLAG_VIEWORDER, IID_PPV_ARGS(&visibleItems));
        if (FAILED(result) || !visibleItems)
        {
            return FAILED(result) ? result : E_FAIL;
        }

        // 保留触发时的对象快照；视图只提供次序，后续选择变化不能改变复制对象。
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

    HRESULT CollectSelectedEntries(IShellItemArray* selection, std::vector<SelectedPath>* selected)
    {
        if (selection == NULL || selected == NULL)
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

        selected->clear();
        selected->reserve(count);
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
            selected->push_back(std::move(entry));
        }
        return S_OK;
    }

    HRESULT GetPathsInViewOrder(IShellItemArray* selection, IUnknown* site,
        std::vector<std::wstring>* paths)
    {
        std::vector<SelectedPath> selected;
        HRESULT result = CollectSelectedEntries(selection, &selected);
        if (FAILED(result))
        {
            return result;
        }

        paths->clear();
        paths->reserve(selected.size());
        if (selected.size() == 1)
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

        return OrderSelectionByFolderView(selected, view.Get(), paths);
    }

    HRESULT GetCurrentFolderPathFromView(IFolderView* view, std::wstring* folderPath)
    {
        if (view == NULL || folderPath == NULL)
        {
            return E_INVALIDARG;
        }

        ComPtr<IShellItem> folderItem;
        HRESULT result = view->GetFolder(IID_PPV_ARGS(&folderItem));
        if (FAILED(result) || !folderItem)
        {
            ComPtr<IPersistFolder2> persistFolder;
            result = view->GetFolder(IID_PPV_ARGS(&persistFolder));
            if (FAILED(result) || !persistFolder)
            {
                return FAILED(result) ? result : E_FAIL;
            }

            PIDLIST_ABSOLUTE folderPidl = NULL;
            result = persistFolder->GetCurFolder(&folderPidl);
            if (FAILED(result) || folderPidl == NULL)
            {
                return FAILED(result) ? result : E_FAIL;
            }

            result = SHCreateItemFromIDList(folderPidl, IID_PPV_ARGS(&folderItem));
            CoTaskMemFree(folderPidl);
            if (FAILED(result) || !folderItem)
            {
                return FAILED(result) ? result : E_FAIL;
            }
        }

        CoTaskMemString rawPath;
        result = folderItem->GetDisplayName(SIGDN_FILESYSPATH, &rawPath.value);
        if (FAILED(result) || rawPath.value == NULL || rawPath.value[0] == L'\0')
        {
            return FAILED(result) ? result : E_FAIL;
        }

        *folderPath = rawPath.value;
        return S_OK;
    }

    HRESULT ExtractPathsFromFolderView(IFolderView* view, std::vector<std::wstring>* paths)
    {
        if (view == NULL || paths == NULL)
        {
            return E_INVALIDARG;
        }

        paths->clear();
        int selectedCount = 0;
        HRESULT result = view->ItemCount(SVGIO_SELECTION, &selectedCount);
        if (FAILED(result))
        {
            return result;
        }

        if (selectedCount <= 0)
        {
            std::wstring currentFolder;
            result = GetCurrentFolderPathFromView(view, &currentFolder);
            if (FAILED(result))
            {
                return result;
            }
            paths->push_back(std::move(currentFolder));
            return S_OK;
        }

        ComPtr<IShellItemArray> selection;
        result = view->Items(SVGIO_SELECTION, IID_PPV_ARGS(&selection));
        if (FAILED(result) || !selection)
        {
            return FAILED(result) ? result : E_FAIL;
        }

        std::vector<SelectedPath> selected;
        result = CollectSelectedEntries(selection.Get(), &selected);
        if (FAILED(result))
        {
            return result;
        }

        paths->reserve(selected.size());
        if (selected.size() == 1)
        {
            paths->push_back(selected[0].path);
            return S_OK;
        }

        return OrderSelectionByFolderView(selected, view, paths);
    }

    HRESULT CopyFromFolderView(IFolderView* view)
    {
        try
        {
            std::vector<std::wstring> paths;
            HRESULT result = ExtractPathsFromFolderView(view, &paths);
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

    // =========================================================================
    // 快捷键配置管理、前台 Explorer 窗口感知与自定义配置界面
    // =========================================================================

    struct HotkeyConfig
    {
        bool enabled = true;
        UINT modifiers = MOD_ALT;
        UINT virtualKey = 'C';
    };

    bool IsValidHotkeyCombination(UINT modifiers, UINT virtualKey)
    {
        const UINT allowedModifiers = MOD_ALT | MOD_CONTROL | MOD_SHIFT;
        if ((modifiers & ~allowedModifiers) != 0)
        {
            return false;
        }
        if (virtualKey < 0x08 || virtualKey > 0xFE)
        {
            return false;
        }
        if (virtualKey == VK_SHIFT || virtualKey == VK_CONTROL || virtualKey == VK_MENU
            || virtualKey == VK_LWIN || virtualKey == VK_RWIN)
        {
            return false;
        }
        const bool isFunctionKey = (virtualKey >= VK_F1 && virtualKey <= VK_F24);
        if (!isFunctionKey && (modifiers & (MOD_ALT | MOD_CONTROL)) == 0)
        {
            // 普通字母或数字键必须至少包含 Alt 或 Ctrl，防止干扰资源管理器内的正常重命名输入。
            return false;
        }
        return true;
    }

    bool TryReadHotkeyConfigFromRoot(HKEY rootKey, HotkeyConfig* config)
    {
        HKEY key = NULL;
        if (RegOpenKeyExW(rootKey, kSettingsRegistrySubkey, 0, KEY_READ, &key) != ERROR_SUCCESS)
        {
            return false;
        }

        DWORD enabled = 1;
        DWORD modifiers = MOD_ALT;
        DWORD virtualKey = 'C';
        DWORD size = sizeof(DWORD);
        DWORD type = 0;

        const LONG enabledStatus = RegQueryValueExW(
            key, L"HotkeyEnabled", NULL, &type, reinterpret_cast<LPBYTE>(&enabled), &size);
        if (enabledStatus != ERROR_SUCCESS || type != REG_DWORD)
        {
            RegCloseKey(key);
            return false;
        }

        size = sizeof(DWORD);
        const LONG modStatus = RegQueryValueExW(
            key, L"HotkeyModifiers", NULL, &type, reinterpret_cast<LPBYTE>(&modifiers), &size);
        size = sizeof(DWORD);
        const LONG vkStatus = RegQueryValueExW(
            key, L"HotkeyVirtualKey", NULL, &type, reinterpret_cast<LPBYTE>(&virtualKey), &size);
        RegCloseKey(key);

        if (enabled == 0)
        {
            config->enabled = false;
            if (modStatus == ERROR_SUCCESS && vkStatus == ERROR_SUCCESS
                && IsValidHotkeyCombination(modifiers, virtualKey))
            {
                config->modifiers = modifiers;
                config->virtualKey = virtualKey;
            }
            else
            {
                config->modifiers = MOD_ALT;
                config->virtualKey = 'C';
            }
            return true;
        }

        if (modStatus != ERROR_SUCCESS || vkStatus != ERROR_SUCCESS
            || !IsValidHotkeyCombination(modifiers, virtualKey))
        {
            return false;
        }

        config->enabled = true;
        config->modifiers = modifiers;
        config->virtualKey = virtualKey;
        return true;
    }

    HotkeyConfig LoadHotkeyConfig()
    {
        HotkeyConfig config;
        if (TryReadHotkeyConfigFromRoot(HKEY_CURRENT_USER, &config))
        {
            return config;
        }
        if (TryReadHotkeyConfigFromRoot(HKEY_LOCAL_MACHINE, &config))
        {
            return config;
        }
        return config;
    }

    HRESULT SaveHotkeyConfig(const HotkeyConfig& config)
    {
        if (config.enabled && !IsValidHotkeyCombination(config.modifiers, config.virtualKey))
        {
            return E_INVALIDARG;
        }

        HKEY key = NULL;
        const LONG openResult = RegCreateKeyExW(
            HKEY_CURRENT_USER,
            kSettingsRegistrySubkey,
            0,
            NULL,
            REG_OPTION_NON_VOLATILE,
            KEY_WRITE,
            NULL,
            &key,
            NULL);
        if (openResult != ERROR_SUCCESS)
        {
            return HRESULT_FROM_WIN32(openResult);
        }

        const DWORD enabled = config.enabled ? 1u : 0u;
        const DWORD modifiers = config.modifiers;
        const DWORD virtualKey = config.virtualKey;

        LONG writeResult = RegSetValueExW(
            key, L"HotkeyEnabled", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&enabled), sizeof(DWORD));
        if (writeResult == ERROR_SUCCESS)
        {
            writeResult = RegSetValueExW(
                key, L"HotkeyModifiers", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&modifiers), sizeof(DWORD));
        }
        if (writeResult == ERROR_SUCCESS)
        {
            writeResult = RegSetValueExW(
                key, L"HotkeyVirtualKey", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&virtualKey), sizeof(DWORD));
        }

        RegCloseKey(key);
        return writeResult == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(writeResult);
    }

    std::wstring FormatVirtualKeyName(UINT virtualKey)
    {
        if ((virtualKey >= 'A' && virtualKey <= 'Z') || (virtualKey >= '0' && virtualKey <= '9'))
        {
            return std::wstring(1, static_cast<wchar_t>(virtualKey));
        }
        if (virtualKey >= VK_F1 && virtualKey <= VK_F24)
        {
            return L"F" + std::to_wstring(virtualKey - VK_F1 + 1);
        }

        const UINT scanCode = MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC);
        if (scanCode != 0)
        {
            wchar_t keyName[64] = {};
            if (GetKeyNameTextW( static_cast<LONG>(scanCode << 16), keyName, 64) > 0)
            {
                return keyName;
            }
        }
        return L"0x" + std::to_wstring(virtualKey);
    }

    std::wstring FormatHotkeyText(const HotkeyConfig& config)
    {
        if (!config.enabled)
        {
            return L"已禁用";
        }

        std::wstring text;
        if ((config.modifiers & MOD_CONTROL) != 0)
        {
            text += L"Ctrl + ";
        }
        if ((config.modifiers & MOD_ALT) != 0)
        {
            text += L"Alt + ";
        }
        if ((config.modifiers & MOD_SHIFT) != 0)
        {
            text += L"Shift + ";
        }
        text += FormatVirtualKeyName(config.virtualKey);
        return text;
    }

    std::wstring TrimAndUpper(const std::wstring& input)
    {
        size_t first = 0;
        while (first < input.size() && iswspace(input[first]))
        {
            ++first;
        }
        size_t last = input.size();
        while (last > first && iswspace(input[last - 1]))
        {
            --last;
        }
        std::wstring result = input.substr(first, last - first);
        for (size_t i = 0; i < result.size(); ++i)
        {
            result[i] = static_cast<wchar_t>(towupper(result[i]));
        }
        return result;
    }

    bool ParseHotkeyString(const wchar_t* rawInput, HotkeyConfig* outConfig)
    {
        if (IsEmptyArgument(rawInput) || outConfig == NULL)
        {
            return false;
        }

        const std::wstring upper = TrimAndUpper(rawInput);
        if (upper.empty())
        {
            return false;
        }

        if (upper == L"DEFAULT")
        {
            outConfig->enabled = true;
            outConfig->modifiers = MOD_ALT;
            outConfig->virtualKey = 'C';
            return true;
        }

        if (upper == L"DISABLE" || upper == L"DISABLED" || upper == L"NONE" || upper == L"OFF")
        {
            outConfig->enabled = false;
            outConfig->modifiers = MOD_ALT;
            outConfig->virtualKey = 'C';
            return true;
        }

        UINT modifiers = 0;
        UINT virtualKey = 0;
        size_t start = 0;
        while (start <= upper.size())
        {
            const size_t plusIndex = upper.find(L'+', start);
            const std::wstring token = TrimAndUpper(
                plusIndex == std::wstring::npos ? upper.substr(start) : upper.substr(start, plusIndex - start));
            if (token.empty())
            {
                return false;
            }

            if (token == L"ALT")
            {
                modifiers |= MOD_ALT;
            }
            else if (token == L"CTRL" || token == L"CONTROL")
            {
                modifiers |= MOD_CONTROL;
            }
            else if (token == L"SHIFT")
            {
                modifiers |= MOD_SHIFT;
            }
            else
            {
                if (virtualKey != 0)
                {
                    return false;
                }
                if (token.size() == 1 && ((token[0] >= L'A' && token[0] <= L'Z') || (token[0] >= L'0' && token[0] <= L'9')))
                {
                    virtualKey = static_cast<UINT>(token[0]);
                }
                else if (token.size() >= 2 && token[0] == L'F')
                {
                    int fnNumber = 0;
                    for (size_t i = 1; i < token.size(); ++i)
                    {
                        if (token[i] < L'0' || token[i] > L'9')
                        {
                            return false;
                        }
                        fnNumber = fnNumber * 10 + (token[i] - L'0');
                    }
                    if (fnNumber < 1 || fnNumber > 24)
                    {
                        return false;
                    }
                    virtualKey = static_cast<UINT>(VK_F1 + fnNumber - 1);
                }
                else
                {
                    return false;
                }
            }

            if (plusIndex == std::wstring::npos)
            {
                break;
            }
            start = plusIndex + 1;
        }

        if (!IsValidHotkeyCombination(modifiers, virtualKey))
        {
            return false;
        }

        outConfig->enabled = true;
        outConfig->modifiers = modifiers;
        outConfig->virtualKey = virtualKey;
        return true;
    }

    bool IsExplorerTopLevelWindow(HWND window)
    {
        if (window == NULL || !IsWindow(window))
        {
            return false;
        }

        wchar_t className[64] = {};
        if (GetClassNameW(window, className, static_cast<int>(sizeof(className) / sizeof(className[0]))) == 0)
        {
            return false;
        }

        return lstrcmpiW(className, L"CabinetWClass") == 0
            || lstrcmpiW(className, L"ExploreWClass") == 0;
    }

    HRESULT FindActiveExplorerFolderView(HWND foregroundWindow, IFolderView** outView)
    {
        if (outView == NULL)
        {
            return E_POINTER;
        }
        *outView = NULL;

        if (!IsExplorerTopLevelWindow(foregroundWindow))
        {
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        }

        HWND focusedChild = NULL;
        const DWORD threadId = GetWindowThreadProcessId(foregroundWindow, NULL);
        if (threadId != 0)
        {
            GUITHREADINFO guiInfo = {};
            guiInfo.cbSize = sizeof(guiInfo);
            if (GetGUIThreadInfo(threadId, &guiInfo))
            {
                focusedChild = guiInfo.hwndFocus;
            }
        }

        ComPtr<IShellWindows> shellWindows;
        HRESULT result = CoCreateInstance(CLSID_ShellWindows, NULL, CLSCTX_ALL, IID_PPV_ARGS(&shellWindows));
        if (FAILED(result) || !shellWindows)
        {
            return FAILED(result) ? result : E_FAIL;
        }

        long count = 0;
        result = shellWindows->get_Count(&count);
        if (FAILED(result))
        {
            return result;
        }

        ComPtr<IFolderView> visibleCandidate;
        ComPtr<IFolderView> fallbackCandidate;

        for (long index = 0; index < count; ++index)
        {
            VARIANT position = {};
            position.vt = VT_I4;
            position.lVal = index;

            ComPtr<IDispatch> dispatch;
            if (FAILED(shellWindows->Item(position, &dispatch)) || !dispatch)
            {
                continue;
            }

            ComPtr<IServiceProvider> services;
            if (FAILED(dispatch.As(&services)) || !services)
            {
                continue;
            }

            ComPtr<IShellBrowser> browser;
            if (FAILED(services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) || !browser)
            {
                continue;
            }

            ComPtr<IShellView> shellView;
            if (FAILED(browser->QueryActiveShellView(&shellView)) || !shellView)
            {
                continue;
            }

            HWND viewWindow = NULL;
            if (FAILED(shellView->GetWindow(&viewWindow)) || viewWindow == NULL)
            {
                continue;
            }

            if (GetAncestor(viewWindow, GA_ROOT) != foregroundWindow)
            {
                continue;
            }

            ComPtr<IFolderView> candidateView;
            if (FAILED(shellView.As(&candidateView)) || !candidateView)
            {
                continue;
            }

            HWND browserWindow = NULL;
            ComPtr<IOleWindow> oleWindow;
            if (SUCCEEDED(browser.As(&oleWindow)) && oleWindow)
            {
                oleWindow->GetWindow(&browserWindow);
            }

            if (focusedChild != NULL)
            {
                const bool matchesViewFocus = (viewWindow == focusedChild || IsChild(viewWindow, focusedChild));
                const bool matchesBrowserFocus = (browserWindow != NULL
                    && (browserWindow == focusedChild || IsChild(browserWindow, focusedChild)));
                if (matchesViewFocus || matchesBrowserFocus)
                {
                    *outView = candidateView.Detach();
                    return S_OK;
                }
            }

            if (!visibleCandidate && IsWindowVisible(viewWindow))
            {
                if (browserWindow == NULL || IsWindowVisible(browserWindow))
                {
                    visibleCandidate = candidateView;
                }
            }

            if (!fallbackCandidate)
            {
                fallbackCandidate = candidateView;
            }
        }

        if (visibleCandidate)
        {
            *outView = visibleCandidate.Detach();
            return S_OK;
        }
        if (fallbackCandidate)
        {
            *outView = fallbackCandidate.Detach();
            return S_OK;
        }
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }

    HRESULT CopyActiveExplorerPaths(HWND explorerWindow)
    {
        if (explorerWindow == NULL)
        {
            explorerWindow = GetForegroundWindow();
        }

        ComPtr<IFolderView> folderView;
        HRESULT result = FindActiveExplorerFolderView(explorerWindow, &folderView);
        if (FAILED(result) || !folderView)
        {
            return FAILED(result) ? result : E_FAIL;
        }

        return CopyFromFolderView(folderView.Get());
    }

    // -------------------------------------------------------------------------
    // 前台窗口事件监听守护进程（--hotkey）
    // -------------------------------------------------------------------------

    HWND g_hotkeyHostWindow = NULL;
    HotkeyConfig g_activeHotkeyConfig;
    bool g_hotkeyCurrentlyRegistered = false;
    bool g_hotkeySuspendedForProbe = false;

    void UnbindActiveHotkey()
    {
        if (g_hotkeyCurrentlyRegistered && g_hotkeyHostWindow != NULL)
        {
            UnregisterHotKey(g_hotkeyHostWindow, kActiveHotkeyId);
            g_hotkeyCurrentlyRegistered = false;
        }
    }

    void SyncHotkeyWithForegroundWindow()
    {
        if (g_hotkeyHostWindow == NULL || g_hotkeySuspendedForProbe || !g_activeHotkeyConfig.enabled)
        {
            UnbindActiveHotkey();
            return;
        }

        const HWND foreground = GetForegroundWindow();
        if (IsExplorerTopLevelWindow(foreground))
        {
            if (!g_hotkeyCurrentlyRegistered)
            {
                if (RegisterHotKey(
                    g_hotkeyHostWindow,
                    kActiveHotkeyId,
                    g_activeHotkeyConfig.modifiers | MOD_NOREPEAT,
                    g_activeHotkeyConfig.virtualKey))
                {
                    g_hotkeyCurrentlyRegistered = true;
                }
            }
        }
        else
        {
            UnbindActiveHotkey();
        }
    }

    void CALLBACK ForegroundWinEventProc(
        HWINEVENTHOOK,
        DWORD,
        HWND,
        LONG idObject,
        LONG idChild,
        DWORD,
        DWORD)
    {
        if (idObject == OBJID_WINDOW && idChild == CHILDID_SELF)
        {
            SyncHotkeyWithForegroundWindow();
        }
    }

    LRESULT CALLBACK HotkeyHostWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_HOTKEY:
            if (wParam == kActiveHotkeyId)
            {
                const HWND foreground = GetForegroundWindow();
                if (IsExplorerTopLevelWindow(foreground))
                {
                    CopyActiveExplorerPaths(foreground);
                }
                return 0;
            }
            break;

        case kMsgReloadHotkey:
            g_hotkeySuspendedForProbe = false;
            UnbindActiveHotkey();
            g_activeHotkeyConfig = LoadHotkeyConfig();
            SyncHotkeyWithForegroundWindow();
            return 1;

        case kMsgSuspendHotkey:
            g_hotkeySuspendedForProbe = true;
            UnbindActiveHotkey();
            return 1;

        case kMsgStopDaemon:
        case WM_CLOSE:
            UnbindActiveHotkey();
            DestroyWindow(window);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }

        return DefWindowProcW(window, message, wParam, lParam);
    }

    void NotifyOrStartHotkeyDaemon()
    {
        const HWND existing = FindWindowW(kHotkeyWindowClass, NULL);
        if (existing != NULL)
        {
            SendMessageTimeoutW(existing, kMsgReloadHotkey, 0, 0, SMTO_ABORTIFHUNG, 2000, NULL);
            return;
        }

        const HotkeyConfig config = LoadHotkeyConfig();
        if (!config.enabled)
        {
            return;
        }

        wchar_t exePath[MAX_PATH] = {};
        if (GetModuleFileNameW(NULL, exePath, MAX_PATH) == 0)
        {
            return;
        }

        std::wstring commandLine = L"\"" + std::wstring(exePath) + L"\" --hotkey";
        STARTUPINFOW startup = {};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION processInfo = {};
        if (CreateProcessW(
            exePath,
            &commandLine[0],
            NULL,
            NULL,
            FALSE,
            CREATE_NO_WINDOW,
            NULL,
            NULL,
            &startup,
            &processInfo))
        {
            CloseHandle(processInfo.hThread);
            CloseHandle(processInfo.hProcess);
        }
    }

    int RunHotkeyDaemon()
    {
        const HWND existing = FindWindowW(kHotkeyWindowClass, NULL);
        if (existing != NULL)
        {
            SendMessageTimeoutW(existing, kMsgReloadHotkey, 0, 0, SMTO_ABORTIFHUNG, 2000, NULL);
            return 0;
        }

        const HRESULT comResult = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        if (FAILED(comResult))
        {
            return 1;
        }

        WNDCLASSEXW windowClass = {};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = HotkeyHostWndProc;
        windowClass.hInstance = GetModuleHandleW(NULL);
        windowClass.lpszClassName = kHotkeyWindowClass;
        RegisterClassExW(&windowClass);

        g_hotkeyHostWindow = CreateWindowExW(
            0,
            kHotkeyWindowClass,
            L"DragonKnightK.CopyPathMenu.HotkeyHost",
            WS_OVERLAPPED,
            0, 0, 0, 0,
            NULL,
            NULL,
            GetModuleHandleW(NULL),
            NULL);
        if (g_hotkeyHostWindow == NULL)
        {
            CoUninitialize();
            return 1;
        }

        g_activeHotkeyConfig = LoadHotkeyConfig();
        HWINEVENTHOOK foregroundHook = SetWinEventHook(
            EVENT_SYSTEM_FOREGROUND,
            EVENT_SYSTEM_MINIMIZEEND,
            NULL,
            ForegroundWinEventProc,
            0,
            0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

        SyncHotkeyWithForegroundWindow();

        MSG message = {};
        while (GetMessageW(&message, NULL, 0, 0) > 0)
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        UnbindActiveHotkey();
        if (foregroundHook != NULL)
        {
            UnhookWinEvent(foregroundHook);
        }
        g_hotkeyHostWindow = NULL;
        CoUninitialize();
        return 0;
    }

    // -------------------------------------------------------------------------
    // 原生 Win32 图形化快捷键配置对话框（--config）
    // -------------------------------------------------------------------------

    enum ConfigControlId
    {
        kCtrlHotkeyInput = 100,
        kCtrlStatusLabel = 101,
        kCtrlSaveButton = 102,
        kCtrlDefaultButton = 103,
        kCtrlDisableButton = 104,
        kCtrlCloseButton = IDCANCEL
    };

    struct ConfigDialogState
    {
        HWND hotkeyBox = NULL;
        HWND statusLabel = NULL;
        HFONT uiFont = NULL;
    };

    WORD ModifiersToHotkeyControlFlags(UINT modifiers)
    {
        WORD flags = 0;
        if ((modifiers & MOD_ALT) != 0)
        {
            flags |= HOTKEYF_ALT;
        }
        if ((modifiers & MOD_CONTROL) != 0)
        {
            flags |= HOTKEYF_CONTROL;
        }
        if ((modifiers & MOD_SHIFT) != 0)
        {
            flags |= HOTKEYF_SHIFT;
        }
        return flags;
    }

    UINT HotkeyControlFlagsToModifiers(BYTE flags)
    {
        UINT modifiers = 0;
        if ((flags & HOTKEYF_ALT) != 0)
        {
            modifiers |= MOD_ALT;
        }
        if ((flags & HOTKEYF_CONTROL) != 0)
        {
            modifiers |= MOD_CONTROL;
        }
        if ((flags & HOTKEYF_SHIFT) != 0)
        {
            modifiers |= MOD_SHIFT;
        }
        return modifiers;
    }

    void UpdateConfigDialogDisplay(HWND dialog, const ConfigDialogState* state, const HotkeyConfig& config)
    {
        if (state == NULL)
        {
            return;
        }

        if (config.enabled)
        {
            const WORD hkFlags = ModifiersToHotkeyControlFlags(config.modifiers);
            SendMessageW(
                state->hotkeyBox,
                HKM_SETHOTKEY,
                MAKEWORD(static_cast<BYTE>(config.virtualKey), static_cast<BYTE>(hkFlags)),
                0);
        }
        else
        {
            SendMessageW(state->hotkeyBox, HKM_SETHOTKEY, 0, 0);
        }

        std::wstring statusText = L"当前生效快捷键：" + FormatHotkeyText(config);
        if (config.enabled)
        {
            statusText += L"  （选中文件时复制选中项，未选中时复制当前文件夹）";
        }
        SetWindowTextW(state->statusLabel, statusText.c_str());
        InvalidateRect(dialog, NULL, TRUE);
    }

    bool ProbeHotkeyAvailability(HWND dialog, const HotkeyConfig& config)
    {
        if (!config.enabled)
        {
            return true;
        }

        const HWND daemon = FindWindowW(kHotkeyWindowClass, NULL);
        if (daemon != NULL)
        {
            SendMessageTimeoutW(daemon, kMsgSuspendHotkey, 0, 0, SMTO_ABORTIFHUNG, 1000, NULL);
        }

        const BOOL canRegister = RegisterHotKey(
            dialog,
            kProbeHotkeyId,
            config.modifiers | MOD_NOREPEAT,
            config.virtualKey);
        if (canRegister)
        {
            UnregisterHotKey(dialog, kProbeHotkeyId);
        }

        if (daemon != NULL)
        {
            SendMessageTimeoutW(daemon, kMsgReloadHotkey, 0, 0, SMTO_ABORTIFHUNG, 1000, NULL);
        }

        return canRegister != FALSE;
    }

    void ApplyAndSaveConfigFromDialog(HWND dialog, const ConfigDialogState* state, const HotkeyConfig& config)
    {
        if (config.enabled && !ProbeHotkeyAvailability(dialog, config))
        {
            const std::wstring conflictMsg =
                L"无法设置快捷键 [ " + FormatHotkeyText(config) +
                L" ]。\r\n\r\n原因：该组合键已被系统中其他后台软件全局占用，请尝试其他组合键（例如 Alt + C）。";
            MessageBoxW(dialog, conflictMsg.c_str(), L"快捷键冲突 - CopyPathMenu", MB_OK | MB_ICONWARNING);
            return;
        }

        const HRESULT saveResult = SaveHotkeyConfig(config);
        if (FAILED(saveResult))
        {
            MessageBoxW(dialog, L"写入注册表快捷键配置失败。", L"CopyPathMenu", MB_OK | MB_ICONERROR);
            return;
        }

        NotifyOrStartHotkeyDaemon();
        UpdateConfigDialogDisplay(dialog, state, config);

        const std::wstring message = config.enabled
            ? (L"快捷键已更新为 [ " + FormatHotkeyText(config) + L" ] 并立即生效！\r\n\r\n• 在资源管理器中选中文件/文件夹后按下：复制全部选中路径\r\n• 未选中任何文件时按下：复制当前所在文件夹路径")
            : L"已禁用资源管理器快捷键（右键菜单“复制路径”仍可正常使用）。";
        MessageBoxW(dialog, message.c_str(), L"设置已生效 - CopyPathMenu", MB_OK | MB_ICONINFORMATION);
    }

    int ScaleBySystemDpi(int value, int dpi)
    {
        return MulDiv(value, dpi, 96);
    }

    LRESULT CALLBACK ConfigDialogWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        ConfigDialogState* state = reinterpret_cast<ConfigDialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));

        switch (message)
        {
        case WM_CREATE:
        {
            CREATESTRUCTW* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
            state = reinterpret_cast<ConfigDialogState*>(createStruct->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));

            HDC screenDc = GetDC(window);
            const int dpi = screenDc != NULL ? GetDeviceCaps(screenDc, LOGPIXELSX) : 96;
            if (screenDc != NULL)
            {
                ReleaseDC(window, screenDc);
            }

            NONCLIENTMETRICSW metrics = {};
            metrics.cbSize = sizeof(metrics);
            if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
            {
                metrics.lfMessageFont.lfHeight = -MulDiv(9, dpi, 72);
                state->uiFont = CreateFontIndirectW(&metrics.lfMessageFont);
            }

            const HINSTANCE instance = GetModuleHandleW(NULL);
            HWND promptLabel = CreateWindowExW(
                0,
                L"STATIC",
                L"请点击下方输入框，直接在键盘上按下想要设置的快捷键组合：\r\n（仅在文件资源管理器处于前台时激活，绝不干扰浏览器、终端或 IDE）",
                WS_CHILD | WS_VISIBLE,
                ScaleBySystemDpi(20, dpi),
                ScaleBySystemDpi(16, dpi),
                ScaleBySystemDpi(410, dpi),
                ScaleBySystemDpi(40, dpi),
                window,
                NULL,
                instance,
                NULL);

            state->hotkeyBox = CreateWindowExW(
                0,
                HOTKEY_CLASSW,
                L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                ScaleBySystemDpi(20, dpi),
                ScaleBySystemDpi(62, dpi),
                ScaleBySystemDpi(410, dpi),
                ScaleBySystemDpi(30, dpi),
                window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCtrlHotkeyInput)),
                instance,
                NULL);

            SendMessageW(
                state->hotkeyBox,
                HKM_SETRULES,
                HKCOMB_NONE | HKCOMB_S,
                MAKELPARAM(HOTKEYF_ALT, 0));

            state->statusLabel = CreateWindowExW(
                0,
                L"STATIC",
                L"",
                WS_CHILD | WS_VISIBLE,
                ScaleBySystemDpi(20, dpi),
                ScaleBySystemDpi(102, dpi),
                ScaleBySystemDpi(410, dpi),
                ScaleBySystemDpi(24, dpi),
                window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCtrlStatusLabel)),
                instance,
                NULL);

            HWND saveButton = CreateWindowExW(
                0,
                L"BUTTON",
                L"保存并生效",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                ScaleBySystemDpi(20, dpi),
                ScaleBySystemDpi(138, dpi),
                ScaleBySystemDpi(105, dpi),
                ScaleBySystemDpi(32, dpi),
                window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCtrlSaveButton)),
                instance,
                NULL);

            HWND defaultButton = CreateWindowExW(
                0,
                L"BUTTON",
                L"恢复默认 (Alt+C)",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                ScaleBySystemDpi(133, dpi),
                ScaleBySystemDpi(138, dpi),
                ScaleBySystemDpi(130, dpi),
                ScaleBySystemDpi(32, dpi),
                window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCtrlDefaultButton)),
                instance,
                NULL);

            HWND disableButton = CreateWindowExW(
                0,
                L"BUTTON",
                L"禁用快捷键",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                ScaleBySystemDpi(271, dpi),
                ScaleBySystemDpi(138, dpi),
                ScaleBySystemDpi(92, dpi),
                ScaleBySystemDpi(32, dpi),
                window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCtrlDisableButton)),
                instance,
                NULL);

            HWND closeButton = CreateWindowExW(
                0,
                L"BUTTON",
                L"关闭",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                ScaleBySystemDpi(371, dpi),
                ScaleBySystemDpi(138, dpi),
                ScaleBySystemDpi(59, dpi),
                ScaleBySystemDpi(32, dpi),
                window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCtrlCloseButton)),
                instance,
                NULL);

            if (state->uiFont != NULL)
            {
                const HWND controls[] = {
                    promptLabel, state->hotkeyBox, state->statusLabel,
                    saveButton, defaultButton, disableButton, closeButton
                };
                for (size_t i = 0; i < sizeof(controls) / sizeof(controls[0]); ++i)
                {
                    SendMessageW(controls[i], WM_SETFONT, reinterpret_cast<WPARAM>(state->uiFont), TRUE);
                }
            }

            UpdateConfigDialogDisplay(window, state, LoadHotkeyConfig());
            SetFocus(state->hotkeyBox);
            return 0;
        }

        case WM_CTLCOLORSTATIC:
        {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }

        case WM_COMMAND:
        {
            const int controlId = LOWORD(wParam);
            if (controlId == kCtrlSaveButton && state != NULL)
            {
                const LRESULT rawHotkey = SendMessageW(state->hotkeyBox, HKM_GETHOTKEY, 0, 0);
                const BYTE virtualKey = LOBYTE(LOWORD(rawHotkey));
                const BYTE controlFlags = HIBYTE(LOWORD(rawHotkey));
                const UINT modifiers = HotkeyControlFlagsToModifiers(controlFlags);

                if (!IsValidHotkeyCombination(modifiers, virtualKey))
                {
                    MessageBoxW(
                        window,
                        L"请先在输入框中按下有效的组合键（需包含 Alt 或 Ctrl，例如 Alt + C 或 Ctrl + Shift + C）。",
                        L"提示 - CopyPathMenu",
                        MB_OK | MB_ICONWARNING);
                    SetFocus(state->hotkeyBox);
                    return 0;
                }

                HotkeyConfig newConfig;
                newConfig.enabled = true;
                newConfig.modifiers = modifiers;
                newConfig.virtualKey = virtualKey;
                ApplyAndSaveConfigFromDialog(window, state, newConfig);
                return 0;
            }
            if (controlId == kCtrlDefaultButton && state != NULL)
            {
                HotkeyConfig defaultConfig;
                defaultConfig.enabled = true;
                defaultConfig.modifiers = MOD_ALT;
                defaultConfig.virtualKey = 'C';
                ApplyAndSaveConfigFromDialog(window, state, defaultConfig);
                return 0;
            }
            if (controlId == kCtrlDisableButton && state != NULL)
            {
                HotkeyConfig disabledConfig = LoadHotkeyConfig();
                disabledConfig.enabled = false;
                ApplyAndSaveConfigFromDialog(window, state, disabledConfig);
                return 0;
            }
            if (controlId == kCtrlCloseButton)
            {
                DestroyWindow(window);
                return 0;
            }
            break;
        }

        case WM_CLOSE:
            DestroyWindow(window);
            return 0;

        case WM_DESTROY:
            if (state != NULL && state->uiFont != NULL)
            {
                DeleteObject(state->uiFont);
                state->uiFont = NULL;
            }
            PostQuitMessage(0);
            return 0;
        }

        return DefWindowProcW(window, message, wParam, lParam);
    }

    int RunHotkeyConfigDialog()
    {
        INITCOMMONCONTROLSEX icc = {};
        icc.dwSize = sizeof(icc);
        icc.dwICC = ICC_HOTKEY_CLASS | ICC_STANDARD_CLASSES;
        InitCommonControlsEx(&icc);

        WNDCLASSEXW windowClass = {};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = ConfigDialogWndProc;
        windowClass.hInstance = GetModuleHandleW(NULL);
        windowClass.hCursor = LoadCursorW(NULL, IDC_ARROW);
        windowClass.hIcon = LoadIconW(NULL, IDI_APPLICATION);
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        windowClass.lpszClassName = kConfigWindowClass;
        RegisterClassExW(&windowClass);

        HDC screenDc = GetDC(NULL);
        const int dpi = screenDc != NULL ? GetDeviceCaps(screenDc, LOGPIXELSX) : 96;
        if (screenDc != NULL)
        {
            ReleaseDC(NULL, screenDc);
        }

        RECT rect = { 0, 0, ScaleBySystemDpi(450, dpi), ScaleBySystemDpi(190, dpi) };
        const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        AdjustWindowRectEx(&rect, style, FALSE, 0);
        const int windowWidth = rect.right - rect.left;
        const int windowHeight = rect.bottom - rect.top;
        const int posX = (GetSystemMetrics(SM_CXSCREEN) - windowWidth) / 2;
        const int posY = (GetSystemMetrics(SM_CYSCREEN) - windowHeight) / 2;

        ConfigDialogState state;
        HWND dialog = CreateWindowExW(
            0,
            kConfigWindowClass,
            L"CopyPathMenu 快捷键设置",
            style | WS_VISIBLE,
            posX,
            posY,
            windowWidth,
            windowHeight,
            NULL,
            NULL,
            GetModuleHandleW(NULL),
            &state);
        if (dialog == NULL)
        {
            return 1;
        }

        ShowWindow(dialog, SW_SHOW);
        UpdateWindow(dialog);

        MSG message = {};
        while (GetMessageW(&message, NULL, 0, 0) > 0)
        {
            if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE && GetFocus() != state.hotkeyBox)
            {
                DestroyWindow(dialog);
                continue;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        return 0;
    }

    int RunSetHotkeyCommandLine(const wchar_t* spec)
    {
        HotkeyConfig config;
        if (!ParseHotkeyString(spec, &config))
        {
            return 1;
        }
        if (FAILED(SaveHotkeyConfig(config)))
        {
            return 2;
        }
        NotifyOrStartHotkeyDaemon();
        return 0;
    }

    // -------------------------------------------------------------------------
    // 命令行复制与入口分发
    // -------------------------------------------------------------------------

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
        else if (argumentCount == 2 && lstrcmpiW(arguments[1], L"--hotkey") == 0)
        {
            exitCode = RunHotkeyDaemon();
        }
        else if (argumentCount == 2 && lstrcmpiW(arguments[1], L"--config") == 0)
        {
            exitCode = RunHotkeyConfigDialog();
        }
        else if (argumentCount == 3 && lstrcmpiW(arguments[1], L"--set-hotkey") == 0)
        {
            exitCode = RunSetHotkeyCommandLine(arguments[2]);
        }
        else if (argumentCount == 2 && lstrcmpiW(arguments[1], L"--get-hotkey") == 0)
        {
            const std::wstring text = FormatHotkeyText(LoadHotkeyConfig()) + L"\r\n";
            WriteParentConsoleText(text.c_str());
            exitCode = 0;
        }
        else if (argumentCount == 2 && lstrcmpiW(arguments[1], L"--copy-active") == 0)
        {
            if (SUCCEEDED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)))
            {
                exitCode = SUCCEEDED(CopyActiveExplorerPaths(NULL)) ? 0 : 2;
                CoUninitialize();
            }
        }
        else if (argumentCount == 2 && IsVersionArgument(arguments[1]))
        {
            WriteParentConsoleText(L"DragonKnightK.CopyPathMenu 1.1.0\r\n");
            exitCode = 0;
        }
        else if (argumentCount == 2 && IsHelpArgument(arguments[1]))
        {
            WriteParentConsoleText(
                L"DragonKnightK.CopyPathMenu 1.1.0\r\n"
                L"用法:\r\n"
                L"  copy-path.exe --copy <路径1> [路径2 ...]\r\n"
                L"  copy-path.exe --hotkey\r\n"
                L"  copy-path.exe --config\r\n"
                L"  copy-path.exe --set-hotkey <Alt+C|Ctrl+Shift+C|default|disable>\r\n"
                L"  copy-path.exe --get-hotkey\r\n"
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

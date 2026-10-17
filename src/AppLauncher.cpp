#include "AppLauncher.h"

#include <shobjidl.h>

#include "Utils.h"

#include <vector>

namespace ld
{

namespace
{

struct WindowSearch
{
    std::wstring processName;
    HWND foreground = nullptr;
    HWND candidate = nullptr;
    HWND minimized = nullptr;
};

struct ActivationRequest
{
    HWND target = nullptr;
};

size_t WindowsAppsPackageStart(const std::wstring& path)
{
    constexpr wchar_t kWindowsApps[] = L"\\WindowsApps\\";
    constexpr size_t markerSize = ARRAYSIZE(kWindowsApps) - 1;
    for (size_t i = 0; i + markerSize < path.size(); ++i)
    {
        if (_wcsnicmp(path.c_str() + i, kWindowsApps, markerSize) == 0)
        {
            return i + markerSize;
        }
    }
    return std::wstring::npos;
}

std::wstring PackageFamilyFromExecutable(const std::wstring& path)
{
    // The folder directly beneath WindowsApps is the MSIX package full name.
    // Let Windows convert that into a stable family name, without guessing
    // publisher IDs or a manifest Application.Id.
    const size_t packageStart = WindowsAppsPackageStart(path);
    if (packageStart == std::wstring::npos)
    {
        return {};
    }

    const size_t packageEnd = path.find(L'\\', packageStart);
    if (packageEnd == std::wstring::npos || packageEnd == packageStart)
    {
        return {};
    }

    using PackageFamilyFromFullNameFn = LONG (WINAPI*)(PCWSTR, UINT32*, PWSTR);
    static const auto familyFromFullName = []() -> PackageFamilyFromFullNameFn
    {
        HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        return kernel
            ? reinterpret_cast<PackageFamilyFromFullNameFn>(
                  GetProcAddress(kernel, "PackageFamilyNameFromFullName"))
            : nullptr;
    }();
    if (!familyFromFullName)
    {
        return {};
    }

    const std::wstring fullName =
        path.substr(packageStart, packageEnd - packageStart);
    UINT32 length = 0;
    if (familyFromFullName(fullName.c_str(), &length, nullptr)
            != ERROR_INSUFFICIENT_BUFFER || length <= 1)
    {
        return {};
    }

    std::wstring family(length, L'\0');
    if (familyFromFullName(fullName.c_str(), &length, family.data())
        != ERROR_SUCCESS)
    {
        return {};
    }
    family.resize(wcslen(family.c_str()));
    return family;
}

bool IsIdFromPackageFamily(const std::wstring& family,
                           const std::wstring& appUserModelId)
{
    return !family.empty()
        && appUserModelId.size() > family.size() + 1
        && appUserModelId[family.size()] == L'!'
        && _wcsnicmp(appUserModelId.c_str(),
                      family.c_str(), family.size()) == 0;
}

std::wstring ShellApplicationId(IShellItem* item)
{
    if (!item)
    {
        return {};
    }

    static const PROPERTYKEY appUserModelIdKey =
    {
        {0x9F4C2855, 0x9F79, 0x4B39,
         {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}},
        5
    };
    ComPtr<IShellItem2> item2;
    if (SUCCEEDED(item->QueryInterface(IID_PPV_ARGS(item2.AddressOf()))))
    {
        PWSTR identity = nullptr;
        if (SUCCEEDED(item2->GetString(appUserModelIdKey, &identity))
            && identity)
        {
            const std::wstring result(identity);
            CoTaskMemFree(identity);
            if (result.find(L'!') != std::wstring::npos)
            {
                return result;
            }
        }
        else if (identity)
        {
            CoTaskMemFree(identity);
        }
    }

    // Some AppsFolder providers expose the AUMID only as their parsing name.
    PWSTR parsingName = nullptr;
    if (FAILED(item->GetDisplayName(
            SIGDN_DESKTOPABSOLUTEPARSING, &parsingName)) || !parsingName)
    {
        return {};
    }
    const std::wstring name(parsingName);
    CoTaskMemFree(parsingName);
    const size_t slash = name.find_last_of(L"\\/");
    const std::wstring candidate = slash == std::wstring::npos
        ? name : name.substr(slash + 1);
    return candidate.find(L'!') == std::wstring::npos
        ? std::wstring() : candidate;
}

std::wstring FindRegisteredPackageAppId(const std::wstring& family)
{
    if (family.empty())
    {
        return {};
    }

    ComPtr<IShellItem> folder;
    if (FAILED(SHCreateItemFromParsingName(
            L"shell:AppsFolder", nullptr,
            IID_PPV_ARGS(folder.AddressOf()))))
    {
        return {};
    }

    ComPtr<IEnumShellItems> enumerator;
    if (FAILED(folder->BindToHandler(
            nullptr, BHID_EnumItems, IID_PPV_ARGS(enumerator.AddressOf()))))
    {
        return {};
    }

    std::wstring matched;
    for (;;)
    {
        ComPtr<IShellItem> item;
        ULONG fetched = 0;
        if (enumerator->Next(1, item.AddressOf(), &fetched) != S_OK
            || fetched != 1)
        {
            break;
        }

        const std::wstring candidate = ShellApplicationId(item.Get());
        if (!IsIdFromPackageFamily(family, candidate))
        {
            continue;
        }

        // A package may expose multiple applications. Do not silently choose
        // the wrong one when repairing a pre-existing path-only Dock pin.
        if (!matched.empty() && !EqualsIgnoreCase(matched, candidate))
        {
            return {};
        }
        matched = candidate;
    }
    return matched;
}

DWORD WINAPI ActivateWindowWorker(void* parameter)
{
    std::unique_ptr<ActivationRequest> request(
        static_cast<ActivationRequest*>(parameter));
    if (!request || !request->target || !IsWindow(request->target))
    {
        return 0;
    }

    const HWND target = request->target;

    // Queue visibility changes first. These calls never wait on the target
    // UI thread, so even a busy/hung application cannot stall LightDock.
    if (IsIconic(target))
    {
        ShowWindowAsync(target, SW_RESTORE);
    }
    else if (IsZoomed(target))
    {
        ShowWindowAsync(target, SW_SHOWMAXIMIZED);
    }
    else
    {
        ShowWindowAsync(target, SW_SHOW);
    }

    const HWND foreground = GetForegroundWindow();
    const DWORD currentThread = GetCurrentThreadId();
    const DWORD targetThread = GetWindowThreadProcessId(target, nullptr);
    const DWORD foregroundThread = foreground
        ? GetWindowThreadProcessId(foreground, nullptr) : 0;

    const bool attachForeground =
        foregroundThread != 0
        && foregroundThread != currentThread
        && AttachThreadInput(
            currentThread, foregroundThread, TRUE) != FALSE;

    const bool attachTarget =
        targetThread != 0
        && targetThread != currentThread
        && targetThread != foregroundThread
        && AttachThreadInput(
            currentThread, targetThread, TRUE) != FALSE;

    // This is the exact HWND represented by the clicked preview card.
    // Perform the stronger foreground sequence off LightDock's UI thread so
    // multi-window apps reliably switch to the requested window without
    // making the Dock depend on that application's responsiveness.
    BringWindowToTop(target);
    SetWindowPos(
        target, HWND_TOP,
        0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW
            | SWP_ASYNCWINDOWPOS);
    SetForegroundWindow(target);
    SetActiveWindow(target);

    if (attachTarget)
    {
        AttachThreadInput(currentThread, targetThread, FALSE);
    }
    if (attachForeground)
    {
        AttachThreadInput(currentThread, foregroundThread, FALSE);
    }

    if (GetForegroundWindow() != target)
    {
        using SwitchToThisWindowFn = void (WINAPI*)(HWND, BOOL);
        static SwitchToThisWindowFn switchToThisWindow = []()
            -> SwitchToThisWindowFn
        {
            HMODULE user32 = GetModuleHandleW(L"user32.dll");
            return user32
                ? reinterpret_cast<SwitchToThisWindowFn>(
                    GetProcAddress(user32, "SwitchToThisWindow"))
                : nullptr;
        }();

        if (switchToThisWindow)
        {
            switchToThisWindow(target, TRUE);
        }
        else
        {
            FLASHWINFO flash{};
            flash.cbSize = sizeof(flash);
            flash.hwnd = target;
            flash.dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG;
            flash.uCount = 2;
            FlashWindowEx(&flash);
        }
    }

    // AttachThreadInput temporarily shares input state with the target
    // application. If that app happens to be busy during activation Windows
    // can leave its wait/app-starting cursor visible under the pointer even
    // though LightDock itself is responsive. Once the queues are detached,
    // explicitly restore the normal arrow; the foreground app can still set
    // its own cursor on the next real mouse move if appropriate.
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));

    return 0;
}

BOOL CALLBACK FindApplicationWindow(HWND hwnd, LPARAM parameter)
{
    auto* search = reinterpret_cast<WindowSearch*>(parameter);
    if (!search || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)
        || hwnd == GetShellWindow())
    {
        return TRUE;
    }

    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((exStyle & WS_EX_TOOLWINDOW) != 0)
    {
        return TRUE;
    }

    wchar_t title[512]{};
    if (GetWindowTextW(hwnd, title, ARRAYSIZE(title)) <= 0)
    {
        return TRUE;
    }

    if (EqualsIgnoreCase(search->processName, L"explorer.exe"))
    {
        wchar_t className[128]{};
        GetClassNameW(hwnd, className, ARRAYSIZE(className));
        if (wcscmp(className, L"CabinetWClass") != 0
            && wcscmp(className, L"ExploreWClass") != 0)
        {
            return TRUE;
        }
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId == 0)
    {
        return TRUE;
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                 FALSE, processId);
    if (!process)
    {
        return TRUE;
    }

    wchar_t path[32768]{};
    DWORD pathLength = ARRAYSIZE(path);
    const BOOL gotPath = QueryFullProcessImageNameW(
        process, 0, path, &pathLength);
    CloseHandle(process);
    if (!gotPath || !EqualsIgnoreCase(
            GetFileName(std::wstring(path, pathLength)), search->processName))
    {
        return TRUE;
    }

    if (hwnd == search->foreground)
    {
        search->candidate = hwnd;
        return FALSE;
    }

    if (IsIconic(hwnd))
    {
        search->minimized = hwnd;
    }
    else if (!search->candidate)
    {
        search->candidate = hwnd;
    }
    return TRUE;
}

} // namespace

bool AppLauncher::ActivateWindow(HWND target)
{
    if (!target || !IsWindow(target))
    {
        return false;
    }

    auto* request = new (std::nothrow) ActivationRequest{};
    if (!request)
    {
        return false;
    }
    request->target = target;

    // Use a system worker thread for exact-window activation. The old
    // fully-async foreground sequence returned immediately but was too weak
    // for minimized/multi-window applications; doing the robust sequence on
    // the Dock UI thread fixed activation but let a busy application freeze
    // the Dock. This keeps both properties.
    if (!QueueUserWorkItem(
            ActivateWindowWorker,
            request,
            WT_EXECUTELONGFUNCTION))
    {
        delete request;

        // Last-resort non-blocking fallback.
        if (IsIconic(target))
        {
            ShowWindowAsync(target, SW_RESTORE);
        }
        else
        {
            ShowWindowAsync(target, SW_SHOW);
        }
        SetForegroundWindow(target);
        return true;
    }

    return true;
}

HWND AppLauncher::FindRunningWindow(const std::wstring& processName)
{
    if (processName.empty())
    {
        return nullptr;
    }

    WindowSearch search;
    search.processName = GetFileName(processName);
    search.foreground = GetForegroundWindow();
    EnumWindows(FindApplicationWindow, reinterpret_cast<LPARAM>(&search));

    return search.minimized ? search.minimized : search.candidate;
}

bool AppLauncher::ActivateRunningWindow(const std::wstring& processName)
{
    return ActivateWindow(FindRunningWindow(processName));
}

bool AppLauncher::IsPackagedApplicationPath(const std::wstring& path)
{
    // Avoid launching a protected WindowsApps executable directly even when
    // package metadata is temporarily unavailable (update/uninstall race).
    return WindowsAppsPackageStart(path) != std::wstring::npos;
}

bool AppLauncher::MatchesPackagedAppId(const std::wstring& path,
                                       const std::wstring& appUserModelId)
{
    return IsIdFromPackageFamily(
        PackageFamilyFromExecutable(path), appUserModelId);
}

std::wstring AppLauncher::ResolvePackagedAppId(const std::wstring& path)
{
    return FindRegisteredPackageAppId(PackageFamilyFromExecutable(path));
}

bool AppLauncher::LaunchPackagedApp(const std::wstring& appUserModelId,
                                    const std::wstring& arguments)
{
    if (appUserModelId.empty())
    {
        return false;
    }

    ComPtr<IApplicationActivationManager> manager;
    if (FAILED(CoCreateInstance(
            CLSID_ApplicationActivationManager, nullptr,
            CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(manager.AddressOf()))))
    {
        return false;
    }

    DWORD processId = 0;
    return SUCCEEDED(manager->ActivateApplication(
        appUserModelId.c_str(),
        arguments.empty() ? nullptr : arguments.c_str(),
        AO_NONE, &processId));
}

bool AppLauncher::Launch(const std::wstring& path, const std::wstring& arguments)
{
    if (path.empty())
    {
        return false;
    }

    const std::wstring workingDirectory = GetParentDirectory(path);

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    // Keep the shell UI enabled so Windows can explain missing file
    // associations or an unavailable script runtime to the user.
    // Allow the shell to complete activation/DDE work asynchronously.
    // SEE_MASK_NOASYNC can make the Dock wait behind a busy application.
    info.fMask = SEE_MASK_ASYNCOK | SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"open";
    info.lpFile = path.c_str();
    info.lpParameters = arguments.empty() ? nullptr : arguments.c_str();
    info.lpDirectory = workingDirectory.empty() ? nullptr : workingDirectory.c_str();
    info.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&info))
    {
        return false;
    }

    if (info.hProcess)
    {
        CloseHandle(info.hProcess);
    }

    return true;
}

bool AppLauncher::Open(const std::wstring& path)
{
    return Launch(path, std::wstring());
}

bool AppLauncher::RevealInExplorer(const std::wstring& path)
{
    if (path.empty())
    {
        return false;
    }

    PIDLIST_ABSOLUTE item = nullptr;
    SFGAOF attributes = 0;
    if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &item,
                                  0, &attributes))
        || !item)
    {
        return false;
    }

    PIDLIST_ABSOLUTE folder = ILClone(item);
    if (!folder)
    {
        CoTaskMemFree(item);
        return false;
    }

    PCUITEMID_CHILD child = ILFindLastID(item);
    if (!child || !ILRemoveLastID(folder))
    {
        CoTaskMemFree(folder);
        CoTaskMemFree(item);
        return false;
    }

    const HRESULT hr = SHOpenFolderAndSelectItems(folder, 1, &child, 0);

    CoTaskMemFree(folder);
    CoTaskMemFree(item);
    return SUCCEEDED(hr);
}

} // namespace ld

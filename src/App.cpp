#include "App.h"

#include "Utils.h"

#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <mmsystem.h>
#include <propsys.h>
#include <shlobj.h>

#include <cmath>

namespace ld
{

namespace
{

/// Hover magnification spring: quick, with just a hint of overshoot.
const SpringParams kScaleSpring{6.8f, 0.78f, 0.0006f, 0.02f};

/// Launch bounce: slow, springy, clearly visible.
const BounceParams kBounce;

/// Panel width in macOS mode: quick, essentially critically damped.
const SpringParams kPanelSpring{3.4f, 0.92f, 0.05f, 1.0f};
const SpringParams kFullscreenSlideSpring{5.2f, 0.92f, 0.001f, 0.03f};
constexpr float kTooltipScaleBaseline = 0.8f;

    /// Release timing for the magnification driven away from 1.0. Lower is
    /// snappier; too low and the row snaps back instead of gliding home.
    constexpr double kReleaseSeconds = 0.05;

    /// Presence under this counts as released. The exponential tail is
    /// invisible, but letting it run would keep the animation loop alive
    /// for another few hundred milliseconds after the dock looks settled.
    constexpr float kReleaseCutoff = 0.015f;

/// Process polling period.
constexpr double kPollIntervalSeconds = 1.0;

/// Gap between the bottom of the dock and the bottom of the work area.
constexpr float kBottomGap = 4.0f;

/// Id of the temporary item shown while a file is dragged over the Dock.
/// It must never be saved: it only exists until the drop/leave.
constexpr wchar_t kExternalDragPreviewId[] = L"__external_drag_preview__";

int FindItemIndex(const std::vector<std::unique_ptr<DockItem>>& items,
                  const wchar_t* id)
{
    for (size_t i = 0; i < items.size(); ++i)
    {
        if (items[i]->id == id)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool GetVisibleWindowBounds(HWND hwnd, RECT& bounds)
{
    if (!hwnd)
    {
        return false;
    }

    RECT visible{};
    if (SUCCEEDED(DwmGetWindowAttribute(
            hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
            &visible, sizeof(visible))))
    {
        bounds = visible;
        return true;
    }

    return GetWindowRect(hwnd, &bounds) != FALSE;
}

bool RectCovers(const RECT& outer, const RECT& inner, LONG tolerance)
{
    return outer.left <= inner.left + tolerance
        && outer.top <= inner.top + tolerance
        && outer.right >= inner.right - tolerance
        && outer.bottom >= inner.bottom - tolerance;
}

bool IsOrdinaryMaximizedWindow(HWND hwnd)
{
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    const bool maximized =
        IsZoomed(hwnd) != FALSE
        || (GetWindowPlacement(hwnd, &placement)
            && placement.showCmd == SW_SHOWMAXIMIZED);

    if (!maximized)
    {
        return false;
    }

    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);

    // Ordinary maximized desktop windows keep their normal resize/caption
    // frame. Borderless fullscreen windows typically drop one or both.
    return (style & WS_THICKFRAME) != 0
        && (style & WS_CAPTION) != 0;
}

std::wstring WindowProcessName(HWND hwnd)
{
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId == 0)
    {
        return {};
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                 FALSE, processId);
    if (!process)
    {
        return {};
    }

    wchar_t path[32768]{};
    DWORD pathLength = ARRAYSIZE(path);
    const BOOL gotPath = QueryFullProcessImageNameW(
        process, 0, path, &pathLength);
    CloseHandle(process);

    return gotPath && pathLength > 0
        ? GetFileName(std::wstring(path, pathLength))
        : std::wstring();
}


std::wstring PropertyStoreString(
    IPropertyStore* store,
    const PROPERTYKEY& key)
{
    if (!store)
    {
        return {};
    }

    PROPVARIANT value{};
    PropVariantInit(&value);

    std::wstring result;
    if (SUCCEEDED(store->GetValue(key, &value)))
    {
        if (value.vt == VT_LPWSTR && value.pwszVal)
        {
            result = value.pwszVal;
        }
        else if (value.vt == VT_BSTR && value.bstrVal)
        {
            result.assign(value.bstrVal, SysStringLen(value.bstrVal));
        }
    }

    PropVariantClear(&value);
    return result;
}

const PROPERTYKEY& RelaunchDisplayNameKey()
{
    static const PROPERTYKEY key =
    {
        {0x9F4C2855, 0x9F79, 0x4B39,
         {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}},
        4
    };
    return key;
}

const PROPERTYKEY& AppUserModelIdKey()
{
    static const PROPERTYKEY key =
    {
        {0x9F4C2855, 0x9F79, 0x4B39,
         {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}},
        5
    };
    return key;
}

ComPtr<IPropertyStore> WindowPropertyStore(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd))
    {
        return {};
    }

    using GetStoreFn = HRESULT (WINAPI*)(HWND, REFIID, void**);
    static GetStoreFn getStore = []() -> GetStoreFn
    {
        HMODULE shell = GetModuleHandleW(L"shell32.dll");
        if (!shell)
        {
            shell = LoadLibraryW(L"shell32.dll");
        }

        return shell
            ? reinterpret_cast<GetStoreFn>(
                GetProcAddress(shell, "SHGetPropertyStoreForWindow"))
            : nullptr;
    }();

    if (!getStore)
    {
        return {};
    }

    ComPtr<IPropertyStore> store;
    if (FAILED(getStore(
            hwnd, IID_IPropertyStore,
            reinterpret_cast<void**>(store.AddressOf()))))
    {
        return {};
    }

    return store;
}

std::wstring ResolveIndirectShellString(const std::wstring& value)
{
    if (value.empty())
    {
        return {};
    }

    wchar_t resolved[1024]{};
    if (SUCCEEDED(SHLoadIndirectString(
            value.c_str(), resolved, ARRAYSIZE(resolved), nullptr))
        && resolved[0] != L'\0')
    {
        return resolved;
    }

    // Some Win32 property stores return a direct display name instead of an
    // indirect @... resource reference.
    return value.front() == L'@' ? std::wstring() : value;
}

std::wstring ApplicationUserModelIdForProcess(DWORD processId)
{
    if (processId == 0)
    {
        return {};
    }

    using GetApplicationUserModelIdFn =
        LONG (WINAPI*)(HANDLE, UINT32*, PWSTR);

    static GetApplicationUserModelIdFn getApplicationUserModelId = []()
        -> GetApplicationUserModelIdFn
    {
        HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        return kernel
            ? reinterpret_cast<GetApplicationUserModelIdFn>(
                GetProcAddress(kernel, "GetApplicationUserModelId"))
            : nullptr;
    }();

    if (!getApplicationUserModelId)
    {
        return {};
    }

    HANDLE process = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process)
    {
        return {};
    }

    UINT32 length = 0;
    const LONG first =
        getApplicationUserModelId(process, &length, nullptr);
    if (first != ERROR_INSUFFICIENT_BUFFER || length <= 1)
    {
        CloseHandle(process);
        return {};
    }

    std::wstring id(length, L'\0');
    const LONG second =
        getApplicationUserModelId(process, &length, id.data());
    CloseHandle(process);

    if (second != ERROR_SUCCESS || length <= 1)
    {
        return {};
    }

    if (!id.empty() && id.back() == L'\0')
    {
        id.pop_back();
    }
    return id;
}

std::wstring DirectWindowAppUserModelId(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd))
    {
        return {};
    }

    if (ComPtr<IPropertyStore> store = WindowPropertyStore(hwnd))
    {
        const std::wstring id =
            PropertyStoreString(store.Get(), AppUserModelIdKey());
        if (!id.empty())
        {
            return id;
        }
    }

    // Do not accept the host process identity for a UWP frame. Its child
    // window belongs to the real packaged application and carries the AUMID
    // that Windows uses for the taskbar label/icon.
    if (EqualsIgnoreCase(
            WindowProcessName(hwnd), L"ApplicationFrameHost.exe"))
    {
        return {};
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    return ApplicationUserModelIdForProcess(processId);
}

struct ChildApplicationIdentitySearch
{
    DWORD hostProcessId = 0;
    std::wstring appUserModelId;
};

BOOL CALLBACK FindChildApplicationIdentity(HWND hwnd, LPARAM parameter)
{
    auto* search =
        reinterpret_cast<ChildApplicationIdentitySearch*>(parameter);
    if (!search || !search->appUserModelId.empty())
    {
        return FALSE;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);

    // ApplicationFrameHost owns the outer frame; the real packaged app
    // normally owns a descendant CoreWindow/XAML window with a different PID.
    if (processId != 0 && processId != search->hostProcessId)
    {
        search->appUserModelId = DirectWindowAppUserModelId(hwnd);
    }

    return search->appUserModelId.empty() ? TRUE : FALSE;
}

std::wstring ApplicationUserModelIdFromWindow(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd))
    {
        return {};
    }

    if (const std::wstring direct = DirectWindowAppUserModelId(hwnd);
        !direct.empty())
    {
        return direct;
    }

    ChildApplicationIdentitySearch search;
    GetWindowThreadProcessId(hwnd, &search.hostProcessId);
    EnumChildWindows(
        hwnd, FindChildApplicationIdentity,
        reinterpret_cast<LPARAM>(&search));

    return search.appUserModelId;
}

std::wstring LocalizedApplicationNameFromAumid(
    const std::wstring& appUserModelId)
{
    if (appUserModelId.empty())
    {
        return {};
    }

    const std::wstring parsingName =
        L"shell:AppsFolder\\" + appUserModelId;

    ComPtr<IShellItem> shellItem;
    if (FAILED(SHCreateItemFromParsingName(
            parsingName.c_str(), nullptr,
            IID_PPV_ARGS(shellItem.AddressOf())))
        || !shellItem)
    {
        return {};
    }

    wchar_t* name = nullptr;
    if (FAILED(shellItem->GetDisplayName(SIGDN_NORMALDISPLAY, &name))
        || !name)
    {
        return {};
    }

    std::wstring localized(name);
    CoTaskMemFree(name);
    return localized;
}

std::wstring LocalizedApplicationNameFromWindow(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd))
    {
        return {};
    }

    // Win32 apps that explicitly set a taskbar display resource can be
    // resolved without an AUMID.
    if (ComPtr<IPropertyStore> store = WindowPropertyStore(hwnd))
    {
        const std::wstring displayResource =
            PropertyStoreString(store.Get(), RelaunchDisplayNameKey());
        if (const std::wstring localized =
                ResolveIndirectShellString(displayResource);
            !localized.empty())
        {
            return localized;
        }
    }

    // Hosted/packaged apps need their real application identity, not the
    // ApplicationFrameHost.exe file description.
    return LocalizedApplicationNameFromAumid(
        ApplicationUserModelIdFromWindow(hwnd));
}

bool IsWindowsShellUiWindow(HWND hwnd)
{
    const std::wstring name = WindowProcessName(hwnd);
    return EqualsIgnoreCase(name, L"StartMenuExperienceHost.exe")
        || EqualsIgnoreCase(name, L"SearchHost.exe")
        || EqualsIgnoreCase(name, L"SearchApp.exe")
        || EqualsIgnoreCase(name, L"SearchUI.exe")
        || EqualsIgnoreCase(name, L"ShellExperienceHost.exe")
        || EqualsIgnoreCase(name, L"TextInputHost.exe")
        || EqualsIgnoreCase(name, L"LockApp.exe");
}

bool IsWindowsShellUiForeground()
{
    HWND foreground = GetForegroundWindow();
    HWND root = foreground ? GetAncestor(foreground, GA_ROOT) : nullptr;
    return root && IsWindowsShellUiWindow(root);
}

bool IsWindows11OrLater()
{
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);

    static const bool windows11 = []()
    {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll)
        {
            return false;
        }

        auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(
            GetProcAddress(ntdll, "RtlGetVersion"));
        if (!rtlGetVersion)
        {
            return false;
        }

        OSVERSIONINFOW version{};
        version.dwOSVersionInfoSize = sizeof(version);
        if (rtlGetVersion(&version) != 0)
        {
            return false;
        }

        return version.dwMajorVersion > 10
            || (version.dwMajorVersion == 10
                && version.dwBuildNumber >= 22000);
    }();

    return windows11;
}

std::wstring WindowTitleWithoutApplicationName(
    const std::wstring& title,
    const std::wstring& applicationName,
    const std::wstring& processName)
{
    if (title.empty())
    {
        return title;
    }

    std::vector<std::wstring> candidates;
    if (!applicationName.empty())
    {
        candidates.push_back(applicationName);
    }

    // Explorer's executable metadata and its localized window-title suffix
    // are not always the same string. Cover the common shell labels so
    // "TerryDock - 文件资源管理器" becomes simply "TerryDock".
    if (EqualsIgnoreCase(processName, L"explorer.exe"))
    {
        candidates.emplace_back(L"文件资源管理器");
        candidates.emplace_back(L"资源管理器");
        candidates.emplace_back(L"File Explorer");
        candidates.emplace_back(L"Windows Explorer");
    }
    else if (EqualsIgnoreCase(processName, L"chrome.exe"))
    {
        candidates.emplace_back(L"Google Chrome");
    }
    else if (EqualsIgnoreCase(processName, L"msedge.exe"))
    {
        candidates.emplace_back(L"Microsoft Edge");
    }
    else if (EqualsIgnoreCase(processName, L"firefox.exe"))
    {
        candidates.emplace_back(L"Mozilla Firefox");
        candidates.emplace_back(L"Firefox");
    }
    else if (EqualsIgnoreCase(processName, L"Code.exe"))
    {
        candidates.emplace_back(L"Visual Studio Code");
    }

    static constexpr const wchar_t* separators[] =
    {
        L" - ", L" – ", L" — ", L" | "
    };

    for (const std::wstring& candidate : candidates)
    {
        if (candidate.empty() || EqualsIgnoreCase(title, candidate))
        {
            continue;
        }

        for (const wchar_t* separator : separators)
        {
            const std::wstring suffix =
                std::wstring(separator) + candidate;
            if (title.size() > suffix.size()
                && _wcsicmp(title.c_str() + title.size() - suffix.size(),
                            suffix.c_str()) == 0)
            {
                return title.substr(0, title.size() - suffix.size());
            }

            const std::wstring prefix =
                candidate + separator;
            if (title.size() > prefix.size()
                && _wcsnicmp(title.c_str(), prefix.c_str(), prefix.size()) == 0)
            {
                return title.substr(prefix.size());
            }
        }
    }

    return title;
}

struct VisibleTaskbarContext
{
    RECT monitor{};
    DockEdge edge = DockEdge::Bottom;
    int inset = 0;
};

BOOL CALLBACK FindVisibleTaskbarInset(HWND hwnd, LPARAM parameter)
{
    auto* context = reinterpret_cast<VisibleTaskbarContext*>(parameter);
    if (!context || !IsWindowVisible(hwnd))
    {
        return TRUE;
    }

    wchar_t className[128]{};
    GetClassNameW(hwnd, className, ARRAYSIZE(className));
    if (wcscmp(className, L"Shell_TrayWnd") != 0
        && wcscmp(className, L"Shell_SecondaryTrayWnd") != 0)
    {
        return TRUE;
    }

    RECT rect{};
    if (!GetWindowRect(hwnd, &rect))
    {
        return TRUE;
    }

    const RECT& monitor = context->monitor;
    const LONG overlapLeft = (std::max)(rect.left, monitor.left);
    const LONG overlapTop = (std::max)(rect.top, monitor.top);
    const LONG overlapRight = (std::min)(rect.right, monitor.right);
    const LONG overlapBottom = (std::min)(rect.bottom, monitor.bottom);

    if (overlapRight <= overlapLeft || overlapBottom <= overlapTop)
    {
        return TRUE;
    }

    int inset = 0;
    switch (context->edge)
    {
    case DockEdge::Top:
        if (rect.top <= monitor.top + 2
            && rect.bottom > monitor.top)
        {
            inset = static_cast<int>(
                overlapBottom - monitor.top);
        }
        break;

    case DockEdge::Left:
        if (rect.left <= monitor.left + 2
            && rect.right > monitor.left)
        {
            inset = static_cast<int>(
                overlapRight - monitor.left);
        }
        break;

    case DockEdge::Right:
        if (rect.right >= monitor.right - 2
            && rect.left < monitor.right)
        {
            inset = static_cast<int>(
                monitor.right - overlapLeft);
        }
        break;

    case DockEdge::Bottom:
    default:
        if (rect.bottom >= monitor.bottom - 2
            && rect.top < monitor.bottom)
        {
            inset = static_cast<int>(
                monitor.bottom - overlapTop);
        }
        break;
    }

    // Auto-hidden taskbars leave only a one- or two-pixel activation strip.
    // Ignore that strip; only react when Explorer has actually revealed the
    // taskbar (for example because Start/Search was opened).
    if (inset >= 6)
    {
        context->inset = (std::max)(context->inset, inset);
    }

    return TRUE;
}

int VisibleTaskbarInset(const RECT& monitor, DockEdge edge)
{
    VisibleTaskbarContext context;
    context.monitor = monitor;
    context.edge = edge;
    EnumWindows(FindVisibleTaskbarInset,
                reinterpret_cast<LPARAM>(&context));
    return context.inset;
}

void ArmTimer(HANDLE timer, int milliseconds)
{
    LARGE_INTEGER due{};
    due.QuadPart = -static_cast<LONGLONG>(milliseconds) * 10000LL;

    SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
}

// --- dialog appearance helpers --------------------------------------------

/// Default plate colour for a new icon.
const D2D1_COLOR_F kDefaultPlateTop{1.0f, 1.0f, 1.0f, 1.0f};

/// The automatically derived gradient partner stays close to the selected
/// colour; white therefore fades to #EBEBEB instead of a dark gray.
D2D1_COLOR_F DerivedPlateBottom(const D2D1_COLOR_F& top)
{
    return D2D1::ColorF(
        top.r * 0.92f, top.g * 0.92f, top.b * 0.92f, 1.0f);
}

enum class SystemDockGlyph
{
    Start,
    Search,
};

ComPtr<IWICBitmap> CreateSystemDockGlyph(IWICImagingFactory* wic,
                                         SystemDockGlyph glyph)
{
    if (!wic)
    {
        return {};
    }

    constexpr UINT kSize = 256;
    ComPtr<IWICBitmap> bitmap;
    if (FAILED(wic->CreateBitmap(
            kSize, kSize, GUID_WICPixelFormat32bppPBGRA,
            WICBitmapCacheOnLoad, bitmap.AddressOf())))
    {
        return {};
    }

    WICRect area{0, 0, static_cast<INT>(kSize), static_cast<INT>(kSize)};
    ComPtr<IWICBitmapLock> lock;
    if (FAILED(bitmap->Lock(&area, WICBitmapLockWrite, lock.AddressOf())))
    {
        return {};
    }

    UINT stride = 0;
    UINT byteCount = 0;
    BYTE* pixels = nullptr;
    if (FAILED(lock->GetStride(&stride))
        || FAILED(lock->GetDataPointer(&byteCount, &pixels))
        || !pixels)
    {
        return {};
    }

    std::memset(pixels, 0, byteCount);

    auto writePixel = [&](int x, int y, BYTE r, BYTE g, BYTE b, float coverage)
    {
        if (x < 0 || x >= static_cast<int>(kSize)
            || y < 0 || y >= static_cast<int>(kSize))
        {
            return;
        }

        coverage = ClampF(coverage, 0.0f, 1.0f);
        BYTE* pixel = pixels + static_cast<size_t>(y) * stride
            + static_cast<size_t>(x) * 4;
        const BYTE alpha = static_cast<BYTE>(
            std::lround(coverage * 255.0f));

        if (alpha <= pixel[3])
        {
            return;
        }

        pixel[3] = alpha;
        pixel[2] = static_cast<BYTE>(
            (static_cast<unsigned int>(r) * alpha + 127) / 255);
        pixel[1] = static_cast<BYTE>(
            (static_cast<unsigned int>(g) * alpha + 127) / 255);
        pixel[0] = static_cast<BYTE>(
            (static_cast<unsigned int>(b) * alpha + 127) / 255);
    };

    if (glyph == SystemDockGlyph::Start)
    {
        // Four clean Windows panes, deliberately drawn rather than taken from
        // a system executable so Windows 10/11 icon resource changes cannot
        // break the Dock button.
        constexpr BYTE r = 0;
        constexpr BYTE g = 164;
        constexpr BYTE b = 239;
        const int left = 48;
        const int top = 48;
        const int pane = 74;
        const int gap = 10;

        for (int py = 0; py < 2; ++py)
        {
            for (int px = 0; px < 2; ++px)
            {
                const int x0 = left + px * (pane + gap);
                const int y0 = top + py * (pane + gap);

                for (int y = y0; y < y0 + pane; ++y)
                {
                    for (int x = x0; x < x0 + pane; ++x)
                    {
                        writePixel(x, y, r, g, b, 1.0f);
                    }
                }
            }
        }
    }
    else
    {
        // Anti-aliased magnifier built from a circular ring and a rounded
        // diagonal handle.
        constexpr BYTE r = 224;
        constexpr BYTE g = 226;
        constexpr BYTE b = 230;
        constexpr float cx = 108.0f;
        constexpr float cy = 108.0f;
        constexpr float radius = 58.0f;
        constexpr float thickness = 18.0f;
        constexpr float hx0 = 149.0f;
        constexpr float hy0 = 149.0f;
        constexpr float hx1 = 207.0f;
        constexpr float hy1 = 207.0f;
        constexpr float handleRadius = 9.0f;

        const float vx = hx1 - hx0;
        const float vy = hy1 - hy0;
        const float vv = vx * vx + vy * vy;

        for (int y = 28; y < 228; ++y)
        {
            for (int x = 28; x < 228; ++x)
            {
                const float fx = static_cast<float>(x) + 0.5f;
                const float fy = static_cast<float>(y) + 0.5f;

                const float dx = fx - cx;
                const float dy = fy - cy;
                const float radial = std::sqrt(dx * dx + dy * dy);
                const float ringDistance =
                    std::fabs(radial - radius) - thickness * 0.5f;
                const float ringCoverage =
                    ClampF(1.0f - ringDistance, 0.0f, 1.0f);

                const float wx = fx - hx0;
                const float wy = fy - hy0;
                const float t = ClampF((wx * vx + wy * vy) / vv,
                                       0.0f, 1.0f);
                const float qx = hx0 + vx * t;
                const float qy = hy0 + vy * t;
                const float lx = fx - qx;
                const float ly = fy - qy;
                const float lineDistance =
                    std::sqrt(lx * lx + ly * ly) - handleRadius;
                const float lineCoverage =
                    ClampF(1.0f - lineDistance, 0.0f, 1.0f);

                writePixel(x, y, r, g, b,
                           (std::max)(ringCoverage, lineCoverage));
            }
        }
    }

    return bitmap;
}

void SendShellShortcut(bool search)
{
    INPUT inputs[4]{};
    UINT count = 0;

    inputs[count].type = INPUT_KEYBOARD;
    inputs[count].ki.wVk = VK_LWIN;
    ++count;

    if (search)
    {
        inputs[count].type = INPUT_KEYBOARD;
        inputs[count].ki.wVk = L'S';
        ++count;

        inputs[count].type = INPUT_KEYBOARD;
        inputs[count].ki.wVk = L'S';
        inputs[count].ki.dwFlags = KEYEVENTF_KEYUP;
        ++count;
    }

    inputs[count].type = INPUT_KEYBOARD;
    inputs[count].ki.wVk = VK_LWIN;
    inputs[count].ki.dwFlags = KEYEVENTF_KEYUP;
    ++count;

    SendInput(count, inputs, sizeof(INPUT));
}

bool IsMatchingShellPopupProcess(HWND hwnd, bool search)
{
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId == 0)
    {
        return false;
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                 FALSE, processId);
    if (!process)
    {
        return false;
    }

    wchar_t path[32768]{};
    DWORD pathLength = ARRAYSIZE(path);
    const BOOL gotPath = QueryFullProcessImageNameW(
        process, 0, path, &pathLength);
    CloseHandle(process);

    if (!gotPath || pathLength == 0)
    {
        return false;
    }

    const std::wstring name =
        GetFileName(std::wstring(path, pathLength));

    if (search)
    {
        return EqualsIgnoreCase(name, L"SearchHost.exe")
            || EqualsIgnoreCase(name, L"SearchApp.exe")
            || EqualsIgnoreCase(name, L"SearchUI.exe");
    }

    return EqualsIgnoreCase(name, L"StartMenuExperienceHost.exe")
        || EqualsIgnoreCase(name, L"ShellExperienceHost.exe");
}

struct ShellPopupWindowSearch
{
    bool search = false;
    HMONITOR monitor = nullptr;
    HWND foreground = nullptr;
    HWND best = nullptr;
    std::uint64_t bestScore = 0;
};

BOOL CALLBACK FindShellPopupWindow(HWND hwnd, LPARAM parameter)
{
    auto* search =
        reinterpret_cast<ShellPopupWindowSearch*>(parameter);
    if (!search || !IsWindowVisible(hwnd) || IsIconic(hwnd))
    {
        return TRUE;
    }

    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(
            hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))
        && cloaked != 0)
    {
        return TRUE;
    }

    if (!IsMatchingShellPopupProcess(hwnd, search->search))
    {
        return TRUE;
    }

    if (search->monitor
        && MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)
            != search->monitor)
    {
        return TRUE;
    }

    RECT rect{};
    if (!GetVisibleWindowBounds(hwnd, rect))
    {
        return TRUE;
    }

    const LONG width = rect.right - rect.left;
    const LONG height = rect.bottom - rect.top;
    if (width < 160 || height < 120)
    {
        return TRUE;
    }

    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (search->monitor
        && GetMonitorInfoW(search->monitor, &monitorInfo))
    {
        const LONG monitorWidth =
            monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left;
        const LONG monitorHeight =
            monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top;

        // Ignore transparent/full-monitor shell host surfaces. We only want
        // the compact Start/Search popup that the user actually sees.
        if (width >= monitorWidth * 95 / 100
            && height >= monitorHeight * 95 / 100)
        {
            return TRUE;
        }
    }

    std::uint64_t score =
        static_cast<std::uint64_t>(width)
        * static_cast<std::uint64_t>(height);

    if (hwnd == search->foreground)
    {
        score += (1ull << 62);
    }

    if (!search->best || score > search->bestScore)
    {
        search->best = hwnd;
        search->bestScore = score;
    }

    return TRUE;
}

} // namespace

int App::Run(HINSTANCE instance)
{
    DockWindow::EnablePerMonitorDpiAwareness();

    // TEMP-DIAG-BEGIN
    for (int i = 0; i < __argc; ++i)
    {
        const std::wstring arg = __wargv[i];
        const std::wstring key = L"--diag-hover=";

        if (arg.rfind(key, 0) == 0)
        {
            diagMouseX_ = static_cast<float>(_wtof(arg.c_str() + key.size()));
        }
    }
    // TEMP-DIAG-END

    // RegisterDragDrop requires OLE initialization, not just COM STA.
    const HRESULT com = OleInitialize(nullptr);
    if (FAILED(com))
    {
        MessageBoxW(nullptr, L"无法初始化 Windows 拖放服务。", L"LightDock", MB_OK | MB_ICONERROR);
        return 1;
    }

    if (!Initialize(instance))
    {
        if (SUCCEEDED(com))
        {
            OleUninitialize();
        }

        return 1;
    }

    running_ = true;
    timeBeginPeriod(1);

    // High-resolution waitable timers avoid the coarse timer quantum that
    // Remote Desktop sessions can apply to ordinary waitable timers.
    constexpr ULONG kCreateWaitableTimerHighResolution = 0x00000002u;
    HANDLE timer = CreateWaitableTimerExW(
        nullptr, nullptr, kCreateWaitableTimerHighResolution,
        TIMER_ALL_ACCESS);
    if (!timer)
    {
        timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }

    lastFrame_ = std::chrono::steady_clock::now();
    lastPoll_ = lastFrame_;
    lastDisplayCheck_ = lastFrame_;
    lastFullscreenCheck_ = lastFrame_;

    MSG message{};

    while (running_)
    {
        ArmTimer(timer,
                 (animating_ || shellPopupPlacementPending_)
                    ? frameIntervalMs_ : kIdleIntervalMs);

        MsgWaitForMultipleObjectsEx(
            1, &timer, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);

        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT)
            {
                running_ = false;
                break;
            }

            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        if (!running_)
        {
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        const double dt =
            std::chrono::duration<double>(now - lastFrame_).count();
        lastFrame_ = now;

        CheckPointer();
        UpdateShellPopupPlacement();

        if (now - lastPoll_ >= std::chrono::duration<double>(kPollIntervalSeconds))
        {
            lastPoll_ = now;
            PollProcesses();
        }

        if (displayResyncPending_
            || now - lastDisplayCheck_ >= std::chrono::seconds(1))
        {
            lastDisplayCheck_ = now;
            CheckDisplayChange();
        }

        if (now - lastFullscreenCheck_ >= std::chrono::milliseconds(250))
        {
            lastFullscreenCheck_ = now;
            CheckFullscreen();
        }

        if (animating_ || needsRender_)
        {
            needsRender_ = false;
            Tick(dt);
        }
    }

    timeEndPeriod(1);

    if (timer)
    {
        CloseHandle(timer);
    }

    Shutdown();

    if (SUCCEEDED(com))
    {
        OleUninitialize();
    }

    return 0;
}

bool App::Initialize(HINSTANCE instance)
{
    instance_ = instance;

    config_ = Config::Load();
    fullscreenVisibility_.Reset(config_.settings.autoHide ? 0.0f : 1.0f);
    hideAnimationStart_ = fullscreenVisibility_.value;
    hideAnimationTarget_ = fullscreenVisibility_.value;
    hideIndicatorVisibility_.Reset(0.0f);

    EnsureDirectoryExists(Config::Directory());
    EnsureDirectoryExists(GetIconCacheDir());

    if (!icons_.Initialize())
    {
        return false;
    }

    if (!renderer_.Initialize())
    {
        return false;
    }

    if (!window_.Create(instance, this))
    {
        return false;
    }

    // Thumbnail previews are an enhancement, not a startup dependency.
    // If the helper window cannot be created on a particular Windows/DWM
    // configuration, keep LightDock running and simply omit thumbnails.
    windowPreview_.Initialize(instance, window_.Handle(), this);
    launcher_.Initialize(window_.Handle(), this);

    DetectRefreshRate();
    RefreshMonitors();
    if (monitorIndex_ >= 0
        && monitorIndex_ < static_cast<int>(monitors_.size()))
    {
        window_.SetDpi(DockWindow::QueryMonitorDpi(
            monitors_[static_cast<size_t>(monitorIndex_)].handle,
            window_.Handle()));
    }
    LoadItems();
    RebuildMetrics();
    PollProcesses();

    window_.AddTrayIcon(MakeTrayIcon(GetSystemMetrics(SM_CXSMICON)), L"LightDock");

    // The previous release's updater terminated LightDock with taskkill /F.
    // Give Explorer a brief moment to clean up old AppBars, then perform a
    // single fresh ABM_REMOVE/ABM_NEW cycle, just like the working manual
    // auto-hide toggle. This is a one-shot recovery, not a periodic redraw.
    initialAppBarRecoveryPending_ = !config_.settings.autoHide;
    initialAppBarRecoveryAt_ = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(1200);

    return true;
}

void App::Shutdown()
{
    window_.RemoveTrayIcon();

    items_.clear();
    pointers_.clear();

    launcher_.Shutdown();
    windowPreview_.Shutdown();
    renderer_.Shutdown();
    window_.Destroy();
}

// ---------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------

void App::LoadItems()
{
    items_.clear();

    auto addSystemButton = [&](DockItemKind kind,
                               const wchar_t* id,
                               const wchar_t* name,
                               SystemDockGlyph glyph)
    {
        auto item = std::make_unique<DockItem>();
        item->kind = kind;
        item->id = id;
        item->name = name;
        item->plate.enabled = false;
        item->iconSource = CreateSystemDockGlyph(icons_.Factory(), glyph);
        item->scale = 1.0f;
        item->scaleSpring.Reset(1.0f);
        item->bounceSpring.Reset(0.0f);
        items_.push_back(std::move(item));
    };

    addSystemButton(DockItemKind::StartButton,
                    L"__lightdock_start", UiText(L"开始"),
                    SystemDockGlyph::Start);
    addSystemButton(DockItemKind::SearchButton,
                    L"__lightdock_search", UiText(L"搜索"),
                    SystemDockGlyph::Search);

    const std::wstring cacheDirectory = GetIconCacheDir();
    bool migratedPackagedPins = false;

    for (const AppEntry& entry : config_.apps)
    {
        auto item = std::make_unique<DockItem>();
        item->kind = DockItemKind::Pinned;

        item->id = entry.id;
        item->name = entry.name;
        item->targetPath = entry.targetPath;
        item->resolvedPath = entry.resolvedPath;
        item->arguments = entry.arguments;
        item->processName = entry.processName;
        item->appUserModelId = entry.appUserModelId;
        item->iconFile = entry.iconFile;
        item->plate = entry.plate;

        if (item->id.empty())
        {
            item->id = MakeStableId(item->targetPath);
        }

        if (item->resolvedPath.empty())
        {
            item->resolvedPath = item->targetPath;
        }

        // Repair path-only pins created by earlier versions. The original
        // icon/name/plate is kept; the saved AUMID survives MSIX updates.
        if (item->appUserModelId.empty()
            && AppLauncher::IsPackagedApplicationPath(item->resolvedPath))
        {
            item->appUserModelId =
                AppLauncher::ResolvePackagedAppId(item->resolvedPath);
            migratedPackagedPins |= !item->appUserModelId.empty();
        }

        // Icon cache first: never re-parse every executable on startup.
        const std::wstring cacheFile = cacheDirectory + L"\\" + item->iconFile;
        item->iconSource = icons_.LoadFromCache(cacheFile);

        if (!item->iconSource && FileExists(item->resolvedPath))
        {
            AppInfo info = icons_.Inspect(item->resolvedPath, 256);
            if (info.icon)
            {
                item->iconSource = info.icon;
                icons_.SaveToCache(info.icon.Get(), cacheFile);
            }
        }

        item->scale = 1.0f;
        item->scaleSpring.Reset(1.0f);
        item->bounceSpring.Reset(0.0f);

        items_.push_back(std::move(item));
    }

    RebuildItemPointers();
    if (migratedPackagedPins)
    {
        SaveConfiguration();
    }
}

void App::RebuildItemPointers()
{
    pointers_.clear();
    pointers_.reserve(items_.size());

    for (auto& item : items_)
    {
        pointers_.push_back(item.get());
    }
}

void App::SaveConfiguration() const
{
    DockConfig snapshot;
    snapshot.settings = config_.settings;
    snapshot.launcherPins = config_.launcherPins;
    snapshot.launcherHidden = config_.launcherHidden;
    snapshot.apps.reserve(items_.size());

    for (const auto& item : items_)
    {
        if (item->kind != DockItemKind::Pinned
            || item->id == kExternalDragPreviewId)
        {
            continue;
        }

        AppEntry entry;
        entry.id = item->id;
        entry.name = item->name;
        entry.targetPath = item->targetPath;
        entry.resolvedPath = item->resolvedPath;
        entry.arguments = item->arguments;
        entry.processName = item->processName;
        entry.appUserModelId = item->appUserModelId;
        entry.iconFile = item->iconFile;
        entry.plate = item->plate;

        snapshot.apps.push_back(std::move(entry));
    }

    Config::Save(snapshot);
}

void App::AddApplication(const std::wstring& path,
                         const std::wstring& appUserModelId)
{
    if (path.empty())
    {
        return;
    }

    const std::wstring id = MakeStableId(path);
    AppInfo info = icons_.Inspect(path, 256);
    if (info.resolvedPath.empty())
    {
        info.resolvedPath = path;
    }

    std::wstring packagedAppId;
    if (AppLauncher::IsPackagedApplicationPath(info.resolvedPath))
    {
        // Pinning from the transient running section gives us the exact
        // Windows identity, even if the package exposes multiple apps.
        if (AppLauncher::MatchesPackagedAppId(
                info.resolvedPath, appUserModelId))
        {
            packagedAppId = appUserModelId;
        }

        if (packagedAppId.empty())
        {
            for (const auto& existing : items_)
            {
                if (existing->kind == DockItemKind::RunningTransient
                    && EqualsIgnoreCase(existing->resolvedPath,
                                        info.resolvedPath)
                    && AppLauncher::MatchesPackagedAppId(
                        info.resolvedPath, existing->runtimeAppUserModelId))
                {
                    packagedAppId = existing->runtimeAppUserModelId;
                    break;
                }
            }
        }

        if (packagedAppId.empty())
        {
            packagedAppId = AppLauncher::ResolvePackagedAppId(
                info.resolvedPath);
        }
    }

    for (const auto& existing : items_)
    {
        if (existing->kind != DockItemKind::Pinned)
        {
            continue;
        }

        const bool samePackageApp = !packagedAppId.empty()
            && (EqualsIgnoreCase(
                    existing->appUserModelId, packagedAppId)
                || (existing->appUserModelId.empty()
                    && AppLauncher::MatchesPackagedAppId(
                        existing->resolvedPath, packagedAppId)));
        if (samePackageApp && existing->appUserModelId.empty())
        {
            // Old versioned EXE path still identifies the same app.
            existing->appUserModelId = packagedAppId;
            SaveConfiguration();
        }
        if (existing->id == id || samePackageApp)
        {
            return; // already pinned; do not duplicate after MSIX updates
        }
    }

    if (info.processName.empty())
    {
        info.processName = GetFileName(info.resolvedPath);
    }

    if (info.name.empty())
    {
        info.name = GetFileStem(info.resolvedPath);
    }

    // A shortcut already carries its own arguments; passing them again would
    // duplicate them.
    if (GetFileExtension(path) == L".lnk")
    {
        info.arguments.clear();
    }

    auto item = std::make_unique<DockItem>();
    item->kind = DockItemKind::Pinned;
    item->id = id;
    item->name = info.name;
    item->targetPath = path;
    item->resolvedPath = info.resolvedPath;
    item->arguments = info.arguments;
    item->processName = info.processName;
    item->appUserModelId = packagedAppId;

    // Readable cache name so the icons directory can be browsed: the
    // executable's stem plus a short stable suffix, e.g.
    // "explorer_3f2a1b0c.png". Characters outside the safe ASCII set
    // (Chinese names and the like) collapse to underscores; a stem made of
    // nothing else falls back to "app" so the name never turns into hash
    // soup.
    std::wstring readable;
    for (wchar_t ch : GetFileStem(info.resolvedPath))
    {
        const bool safe = (ch >= L'0' && ch <= L'9')
            || (ch >= L'A' && ch <= L'Z')
            || (ch >= L'a' && ch <= L'z')
            || ch == L'-' || ch == L'_';

        readable.push_back(safe ? ch : L'_');
    }

    if (readable.find_first_not_of(L'_') == std::wstring::npos)
    {
        readable = L"app";
    }

    item->iconFile = readable.substr(0, 24) + L"_" + id.substr(0, 8) + L".png";
    item->iconSource = info.icon;

    if (item->iconSource)
    {
        // Choose a useful first-run appearance from the alpha silhouette.
        // Pure circles can occupy the full slot cleanly without a plate;
        // everything else gets a rounded plate and a restrained 85% fill.
        const bool circular = icons_.IsCircularIcon(item->iconSource.Get());
        item->plate.enabled = !circular;
        item->plate.iconScale = circular ? 0.0f : 0.85f;

        icons_.SaveToCache(item->iconSource.Get(),
                           GetIconCacheDir() + L"\\" + item->iconFile);
    }

    item->scale = 1.0f;
    item->scaleSpring.Reset(1.0f);
    item->bounceSpring.Reset(0.0f);

    // If this app is currently represented by a temporary running icon,
    // pinning it moves it into the persistent launcher section instead of
    // leaving a duplicate on the right.
    items_.erase(
        std::remove_if(items_.begin(), items_.end(),
            [&](const std::unique_ptr<DockItem>& existing)
            {
                return existing->kind == DockItemKind::RunningTransient
                    && (EqualsIgnoreCase(existing->resolvedPath,
                                         info.resolvedPath)
                        || EqualsIgnoreCase(existing->targetPath,
                                            info.resolvedPath)
                        || (!packagedAppId.empty()
                            && EqualsIgnoreCase(
                                existing->runtimeAppUserModelId,
                                packagedAppId)));
            }),
        items_.end());

    auto transientBegin = std::find_if(
        items_.begin(), items_.end(),
        [](const std::unique_ptr<DockItem>& existing)
        {
            return existing->kind == DockItemKind::RunningTransient;
        });
    items_.insert(transientBegin, std::move(item));
    RebuildItemPointers();

    SaveConfiguration();
    UpdateSurfaceAndGeometry();
    RepositionWindow();
    WakeAnimation();
}

void App::RemoveApplication(size_t index)
{
    if (index >= items_.size()
        || items_[index]->kind != DockItemKind::Pinned)
    {
        return;
    }

    const std::wstring iconFile = items_[index]->iconFile;

    items_.erase(items_.begin() + static_cast<ptrdiff_t>(index));
    RebuildItemPointers();

    if (!iconFile.empty())
    {
        DeleteFileIfExists(GetIconCacheDir() + L"\\" + iconFile);
    }

    SaveConfiguration();
    UpdateSurfaceAndGeometry();
    RepositionWindow();
    WakeAnimation();
}

void App::LaunchApplication(size_t index)
{
    if (index >= items_.size())
    {
        return;
    }

    DockItem& item = *items_[index];

    // Windows 11 never lets its Start menu / search flyout be moved next to
    // the Dock, so both buttons open LightDock's own launcher instead. It
    // lists the same apps (shell:AppsFolder) and points at the icon.
    if (item.kind == DockItemKind::StartButton)
    {
        ToggleLauncher(Launcher::Mode::Start, index);
        return;
    }

    if (item.kind == DockItemKind::SearchButton)
    {
        ToggleLauncher(Launcher::Mode::Search, index);
        return;
    }

    if (item.kind == DockItemKind::RunningTransient)
    {
        if (!item.windows.empty()
            && AppLauncher::ActivateWindow(item.windows.front().hwnd))
        {
            return;
        }

        const std::wstring& path = item.resolvedPath.empty()
            ? item.targetPath : item.resolvedPath;
        if (AppLauncher::IsPackagedApplicationPath(path))
        {
            const std::wstring appId = AppLauncher::MatchesPackagedAppId(
                path, item.runtimeAppUserModelId)
                ? item.runtimeAppUserModelId
                : AppLauncher::ResolvePackagedAppId(path);
            if (!appId.empty())
            {
                AppLauncher::LaunchPackagedApp(appId, {});
            }
        }
        else
        {
            AppLauncher::Open(path);
        }
        return;
    }

    if (!item.windows.empty()
        && AppLauncher::ActivateWindow(item.windows.front().hwnd))
    {
        return;
    }

    if (item.running
        && AppLauncher::ActivateRunningWindow(item.processName))
    {
        return;
    }

    item.launching = true;
    item.launchElapsed = 0.0;
    item.bounceTimer = 0.0f;
    item.bounceSpring.Reset(0.0f);

    const std::wstring& launchPath = item.resolvedPath.empty()
        ? item.targetPath : item.resolvedPath;
    bool launched = false;
    if (AppLauncher::IsPackagedApplicationPath(launchPath))
    {
        if (!AppLauncher::MatchesPackagedAppId(
                launchPath, item.appUserModelId))
        {
            item.appUserModelId =
                AppLauncher::ResolvePackagedAppId(launchPath);
            if (!item.appUserModelId.empty())
            {
                SaveConfiguration();
            }
        }

        if (!item.appUserModelId.empty())
        {
            launched = AppLauncher::LaunchPackagedApp(
                item.appUserModelId, item.arguments);
        }

        if (!launched)
        {
            // Never attempt ShellExecute on an MSIX WindowsApps EXE: Windows
            // denies access because app activation belongs to the package.
            ShowAlert(UiText(L"无法打开应用"),
                UiText(L"无法启动该 Windows 打包应用。请确认应用仍已安装；"
                       L"若此程序来自旧版本，可先运行应用，再从 Dock 重新固定。"),
                L"好", nullptr);
        }
    }
    else
    {
        launched = AppLauncher::Launch(item.targetPath, item.arguments);
    }

    if (!launched)
    {
        item.launching = false;
        item.bounceSpring.Reset(0.0f);
    }

    WakeAnimation();
}

RECT App::ItemScreenRect(const DockItem& item) const
{
    const float bottom = item.baselineBottom + item.bounceOffset;
    const D2D1_RECT_F logical = D2D1::RectF(
        item.centerX - item.size * 0.5f, bottom - item.size,
        item.centerX + item.size * 0.5f, bottom);
    const D2D1_RECT_F physical = dockTransform_.ToPhysical(logical);
    const RECT bounds = window_.GetBounds();
    return RECT{
        bounds.left + static_cast<LONG>(std::lround(physical.left)),
        bounds.top + static_cast<LONG>(std::lround(physical.top)),
        bounds.left + static_cast<LONG>(std::lround(physical.right)),
        bounds.top + static_cast<LONG>(std::lround(physical.bottom))};
}

void App::ToggleLauncher(Launcher::Mode mode, size_t index)
{
    if (index >= items_.size())
    {
        return;
    }

    // Clicking the icon of an open launcher closes it. The click itself may
    // already have deactivated (and hidden) the popup a moment ago.
    const bool justClosed = !launcher_.Visible()
        && GetTickCount64() - launcher_.HiddenAt() < 300
        && launcher_.CurrentMode() == mode;
    if (launcher_.Visible() && launcher_.CurrentMode() == mode)
    {
        launcher_.Hide();
        return;
    }
    if (justClosed)
    {
        return;
    }

    launcher_.Show(mode, ItemScreenRect(*items_[index]),
                   config_.settings.dockEdge);
}

const wchar_t* App::LauncherText(const wchar_t* chinese) const
{
    return UiText(chinese);
}

void App::OnLauncherPin(const std::wstring& path)
{
    AddApplication(path);
}

std::vector<std::wstring> App::LauncherPins() const
{
    return config_.launcherPins;
}

void App::SetLauncherPins(const std::vector<std::wstring>& pins)
{
    config_.launcherPins = pins;
    SaveConfiguration();
}

std::vector<std::wstring> App::LauncherHidden() const
{
    return config_.launcherHidden;
}

void App::SetLauncherHidden(const std::vector<std::wstring>& hidden)
{
    config_.launcherHidden = hidden;
    SaveConfiguration();
}

void App::OnLauncherSystemStart()
{
    BeginShellPopupPlacement(false, mousePhysicalX_, mousePhysicalY_);
}

void App::OnLauncherVisibilityChanged(bool visible)
{
    // Keep an auto-hiding Dock up while its popup is open.
    if (visible)
    {
        autoHideHidePending_ = false;
        if (!fullscreenActive_)
        {
            SetFullscreenVisibilityTarget(1.0f);
        }
    }
    needsRender_ = true;
    WakeAnimation();
}

void App::BeginShellPopupPlacement(bool search,
                                   float physicalX,
                                   float physicalY)
{
    if (!window_.Handle())
    {
        return;
    }

    const RECT dockBounds = window_.GetBounds();
    shellPopupAnchorScreen_.x = dockBounds.left
        + static_cast<LONG>(std::lround(physicalX));
    shellPopupAnchorScreen_.y = dockBounds.top
        + static_cast<LONG>(std::lround(physicalY));

    shellPopupSearch_ = search;
    shellPopupPlacementPending_ = true;
    shellPopupPlacementStarted_ = std::chrono::steady_clock::now();

    SendShellShortcut(search);

    // Start/Search also raises Explorer's taskbar. Keep LightDock above it
    // immediately; the normal pointer loop continues to maintain topmost
    // while overlay mode is visible.
    window_.EnsureTopmost();
}

void App::UpdateShellPopupPlacement()
{
    if (!shellPopupPlacementPending_)
    {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - shellPopupPlacementStarted_);

    // Explorer raises its taskbar while Start/Search animate in. Reassert
    // LightDock's z-order throughout that transition.
    window_.EnsureTopmost();

    if (elapsed > std::chrono::milliseconds(1800))
    {
        shellPopupPlacementPending_ = false;
        // Stop reserving the native taskbar band once the shell transition is
        // over. This also snaps an auto-hidden Dock back to its true monitor
        // edge if Explorer happened to leave its taskbar visible briefly.
        UpdateDockWindowPosition();
        return;
    }

    // On Windows 11 the visible Start/Search surfaces are composition-owned;
    // moving the hosting HWND is ignored or immediately undone by the shell.
    // Keep the relocation experiment only on Windows 10.
    if (IsWindows11OrLater())
    {
        return;
    }

    if (elapsed > std::chrono::milliseconds(1000))
    {
        return;
    }

    const HMONITOR monitor = MonitorFromPoint(
        shellPopupAnchorScreen_, MONITOR_DEFAULTTONEAREST);

    ShellPopupWindowSearch search;
    search.search = shellPopupSearch_;
    search.monitor = monitor;
    search.foreground = GetForegroundWindow();
    EnumWindows(FindShellPopupWindow,
                reinterpret_cast<LPARAM>(&search));

    if (!search.best)
    {
        return;
    }

    RECT popupRect{};
    if (!GetWindowRect(search.best, &popupRect))
    {
        return;
    }

    const LONG popupWidth = popupRect.right - popupRect.left;
    const LONG popupHeight = popupRect.bottom - popupRect.top;
    if (popupWidth <= 0 || popupHeight <= 0)
    {
        return;
    }

    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(monitor, &monitorInfo))
    {
        return;
    }

    const RECT& screen = monitorInfo.rcMonitor;
    const LONG gap = (std::max)(
        4L, static_cast<LONG>(std::lround(8.0f * dpiScale_)));

    LONG x = popupRect.left;
    LONG y = popupRect.top;

    switch (config_.settings.dockEdge)
    {
    case DockEdge::Top:
        x = shellPopupAnchorScreen_.x - popupWidth / 2;
        y = shellPopupAnchorScreen_.y + gap;
        break;

    case DockEdge::Left:
        x = shellPopupAnchorScreen_.x + gap;
        y = shellPopupAnchorScreen_.y - popupHeight / 2;
        break;

    case DockEdge::Right:
        x = shellPopupAnchorScreen_.x - popupWidth - gap;
        y = shellPopupAnchorScreen_.y - popupHeight / 2;
        break;

    case DockEdge::Bottom:
    default:
        x = shellPopupAnchorScreen_.x - popupWidth / 2;
        y = shellPopupAnchorScreen_.y - popupHeight - gap;
        break;
    }

    const LONG maxX = (std::max)(
        screen.left, screen.right - popupWidth);
    const LONG maxY = (std::max)(
        screen.top, screen.bottom - popupHeight);
    x = (std::clamp)(x, screen.left, maxX);
    y = (std::clamp)(y, screen.top, maxY);

    SetWindowPos(
        search.best, nullptr,
        x, y, 0, 0,
        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE
            | SWP_NOOWNERZORDER);
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

void App::RebuildMetrics()
{
    dpiScale_ = static_cast<float>(window_.GetDpi()) / 96.0f;
    if (dpiScale_ < 1.0f)
    {
        dpiScale_ = 1.0f;
    }

    dockScale_ = dpiScale_ * ClampF(config_.settings.overallScale, 0.5f, 1.5f);
    metrics_ = MakeMetrics(config_.settings, dockScale_);

    UpdateSurfaceAndGeometry();
    RepositionWindow();
}

void App::UpdateSurfaceAndGeometry()
{
    // The separator owns real horizontal space, so the surface, shadow,
    // panel and hit bounds all include it when transient apps exist.
    const auto firstTransient = std::find_if(
        items_.begin(), items_.end(),
        [](const std::unique_ptr<DockItem>& item)
        {
            return item->kind == DockItemKind::RunningTransient;
        });
    const bool hasGroupDivider =
        firstTransient != items_.end() && firstTransient != items_.begin();
    geometry_ = ComputeGeometry(static_cast<int>(items_.size()), metrics_,
                                config_.settings.dockEdge, hasGroupDivider);
    dockTransform_ = DockTransform(config_.settings.dockEdge,
        static_cast<float>(geometry_.surfaceWidth),
        static_cast<float>(geometry_.surfaceHeight));
    renderer_.SetOrientation(config_.settings.dockEdge,
        static_cast<float>(geometry_.surfaceWidth),
        static_cast<float>(geometry_.surfaceHeight));

    const int physicalWidth = config_.settings.dockEdge == DockEdge::Left
            || config_.settings.dockEdge == DockEdge::Right
        ? geometry_.surfaceHeight : geometry_.surfaceWidth;
    const int physicalHeight = config_.settings.dockEdge == DockEdge::Left
            || config_.settings.dockEdge == DockEdge::Right
        ? geometry_.surfaceWidth : geometry_.surfaceHeight;
    if (!renderer_.Resize(physicalWidth, physicalHeight))
    {
        return;
    }

    // The render target was rebuilt: every cached D2D bitmap is stale.
    for (auto& item : items_)
    {
        item->icon.Reset();
    }

    renderer_.SetPanelStyle(
        geometry_.maxPanelWidth,
        geometry_.panelHeight,
        metrics_.cornerRadius,
        config_.settings.shadowOpacity,
        14.0f * dockScale_,
        5.0f * dockScale_);

    // Seed base positions so the first magnification pass has sane centres.
    if (config_.settings.panelMode == PanelMode::Fixed)
    {
        panelWidth_.Reset(geometry_.basePanelWidth);
        frame_ = ApplyLayout(pointers_, geometry_,
                             panelWidth_.value, mouseX_, hoverPresence_,
                             metrics_);
    }
    else
    {
        frame_ = ApplyLayout(pointers_, geometry_,
                             0.0f, mouseX_, hoverPresence_, metrics_);
    }

    RepositionWindow();
}

void App::RepositionWindow(bool forceAppBarPositionUpdate)
{
    // The window is always sized from the geometry, never measured from the
    // current HWND rect: on first run the window is still 1x1 and measuring
    // it would place the dock's top left corner at the screen centre.
    const int width = geometry_.surfaceWidth;
    const int height = geometry_.surfaceHeight;

    if (width <= 0 || height <= 0)
    {
        return;
    }

    const int gap = static_cast<int>(std::round(kBottomGap * dockScale_));
    if (config_.settings.autoHide || exitRequested_)
    {
        window_.RemoveAppBarReservation();
    }
    else if (monitorIndex_ >= 0
             && monitorIndex_ < static_cast<int>(monitors_.size()))
    {
        const int reservation = static_cast<int>(
            std::round(geometry_.panelHeight)) + gap;
        window_.SetAppBarReservation(
            monitors_[static_cast<size_t>(monitorIndex_)].rect,
            config_.settings.dockEdge, reservation,
            forceAppBarPositionUpdate);
    }
    UpdateDockWindowPosition();
}

void App::UpdateDockWindowPosition()
{
    const bool vertical = config_.settings.dockEdge == DockEdge::Left
        || config_.settings.dockEdge == DockEdge::Right;
    const int width = vertical ? geometry_.surfaceHeight
                               : geometry_.surfaceWidth;
    const int height = vertical ? geometry_.surfaceWidth
                                : geometry_.surfaceHeight;
    if (width <= 0 || height <= 0)
    {
        return;
    }

    const RECT work = CurrentWorkArea();
    lastWorkArea_ = work;

    RECT monitorRect = work;
    if (monitorIndex_ >= 0 && monitorIndex_ < static_cast<int>(monitors_.size()))
    {
        monitorRect = monitors_[static_cast<size_t>(monitorIndex_)].rect;
    }

    const LONG monitorWidth = monitorRect.right - monitorRect.left;
    const LONG monitorHeight = monitorRect.bottom - monitorRect.top;
    if (monitorWidth <= 0 || monitorHeight <= 0)
    {
        return;
    }

    // A work-area rectangle is not necessarily trustworthy immediately
    // after RDP connect/disconnect. It can temporarily reflect the old
    // display or an invalid AppBar reservation (sometimes near the TOP
    // even though the bottom edge was selected).
    const bool validWork =
        work.left >= monitorRect.left && work.top >= monitorRect.top
        && work.right <= monitorRect.right
        && work.bottom <= monitorRect.bottom
        && work.right - work.left > monitorWidth / 2
        && work.bottom - work.top > monitorHeight / 2;
    RECT anchorRect = (!config_.settings.autoHide && validWork)
        ? work : monitorRect;

    if (!config_.settings.autoHide)
    {
        // The physical Dock panel occupies our AppBar reservation.
        // Use the RECT that Explorer *approved* in ABM_SETPOS, not rcWork:
        // rcWork may be stale during RDP transitions and incorrectly place
        // a bottom Dock against the TOP of the remote desktop.
        RECT approved{};
        bool validAppBar = window_.GetAppBarReservationBounds(approved);
        if (validAppBar)
        {
            const LONG midX = monitorRect.left + monitorWidth / 2;
            const LONG midY = monitorRect.top + monitorHeight / 2;
            validAppBar =
                approved.left >= monitorRect.left
                && approved.top >= monitorRect.top
                && approved.right <= monitorRect.right
                && approved.bottom <= monitorRect.bottom;
            if (validAppBar)
            {
                switch (config_.settings.dockEdge)
                {
                case DockEdge::Bottom:
                    validAppBar = approved.top >= midY;
                    break;
                case DockEdge::Top:
                    validAppBar = approved.bottom <= midY;
                    break;
                case DockEdge::Left:
                    validAppBar = approved.right <= midX;
                    break;
                case DockEdge::Right:
                    validAppBar = approved.left >= midX;
                    break;
                }
            }
        }

        const LONG reservation = static_cast<LONG>(
            std::lround(geometry_.panelHeight + kBottomGap * dockScale_));
        // If Explorer did not provide a usable reservation, fall back to a
        // monitor-relative edge position. This is transient and does not
        // alter the user's configured monitor or dock edge.
        switch (config_.settings.dockEdge)
        {
        case DockEdge::Bottom:
            anchorRect.bottom = validAppBar ? approved.top
                : monitorRect.bottom - reservation;
            break;
        case DockEdge::Top:
            anchorRect.top = validAppBar ? approved.bottom
                : monitorRect.top + reservation;
            break;
        case DockEdge::Left:
            anchorRect.left = validAppBar ? approved.right
                : monitorRect.left + reservation;
            break;
        case DockEdge::Right:
            anchorRect.right = validAppBar ? approved.left
                : monitorRect.right - reservation;
            break;
        }
    }

    // Overlay/auto-hide mode normally hugs the physical monitor edge. If
    // Explorer temporarily reveals its own taskbar (notably when Start/Search
    // opens), move LightDock just inside that visible taskbar instead of
    // letting two shell surfaces fight for the same topmost screen strip.

    // Only yield to Explorer's revealed taskbar while LightDock itself
    // intentionally opened Start/Search. A normal edge reveal often wakes
    // the auto-hidden Windows taskbar too; treating that as permanent inset
    // made LightDock pop up floating above the taskbar instead of from the
    // physical screen edge.
    const bool shellUiOpen = shellPopupPlacementPending_;
    const int visibleTaskbarInset =
        config_.settings.autoHide && shellUiOpen
        ? VisibleTaskbarInset(monitorRect, config_.settings.dockEdge)
        : 0;
    lastVisibleTaskbarInset_ = visibleTaskbarInset;

    if (visibleTaskbarInset > 0)
    {
        switch (config_.settings.dockEdge)
        {
        case DockEdge::Top:
            anchorRect.top += visibleTaskbarInset;
            break;
        case DockEdge::Left:
            anchorRect.left += visibleTaskbarInset;
            break;
        case DockEdge::Right:
            anchorRect.right -= visibleTaskbarInset;
            break;
        case DockEdge::Bottom:
        default:
            anchorRect.bottom -= visibleTaskbarInset;
            break;
        }
    }

    const int gap = static_cast<int>(std::round(kBottomGap * dockScale_));
    int x = anchorRect.left
        + ((anchorRect.right - anchorRect.left) - width) / 2;
    int y = anchorRect.bottom
        - static_cast<int>(std::round(geometry_.panelY));

    switch (config_.settings.dockEdge)
    {
    case DockEdge::Top:
        y = config_.settings.autoHide
            ? anchorRect.top + gap
                - static_cast<int>(std::round(geometry_.panelY))
            : anchorRect.top - static_cast<int>(std::round(
                geometry_.panelY + geometry_.panelHeight));
        break;
    case DockEdge::Left:
        x = config_.settings.autoHide
            ? anchorRect.left + gap
                - static_cast<int>(std::round(geometry_.panelY))
            : anchorRect.left - static_cast<int>(std::round(
                geometry_.panelY + geometry_.panelHeight));
        y = anchorRect.top
            + ((anchorRect.bottom - anchorRect.top) - height) / 2;
        break;
    case DockEdge::Right:
        x = config_.settings.autoHide
            ? anchorRect.right - gap - width
                + static_cast<int>(std::round(geometry_.panelY))
            : anchorRect.right - width
                + static_cast<int>(std::round(geometry_.panelY
                                               + geometry_.panelHeight));
        y = anchorRect.top
            + ((anchorRect.bottom - anchorRect.top) - height) / 2;
        break;
    case DockEdge::Bottom:
    default:
        if (config_.settings.autoHide)
        {
            const int panelBottom = static_cast<int>(
                std::round(geometry_.panelY + geometry_.panelHeight));
            y = anchorRect.bottom - gap - panelBottom;
        }
        break;
    }
    const float hiddenProgress =
        1.0f - ClampF(fullscreenVisibility_.value, 0.0f, 1.0f);
    int hiddenX = x;
    int hiddenY = y;
    switch (config_.settings.dockEdge)
    {
    case DockEdge::Top: hiddenY = monitorRect.top - height; break;
    case DockEdge::Left: hiddenX = monitorRect.left - width; break;
    case DockEdge::Right: hiddenX = monitorRect.right; break;
    case DockEdge::Bottom:
    default: hiddenY = monitorRect.bottom; break;
    }
    x += static_cast<int>(std::lround((hiddenX - x) * hiddenProgress));
    y += static_cast<int>(std::lround((hiddenY - y) * hiddenProgress));

    window_.SetBounds(x, y, width, height);
}

void App::OnAppBarChanged()
{
    if (!exitRequested_)
    {
        // ABN_POSCHANGED explicitly requires ABM_QUERYPOS / ABM_SETPOS.
        // An unchanged monitor/edge/thickness is NOT evidence that Explorer
        // kept its prior AppBar allocation (especially across RDP).
        RepositionWindow(true);
    }
}

void App::SetFullscreenVisibilityTarget(float target)
{
    target = ClampF(target, 0.0f, 1.0f);
    if (std::fabs(fullscreenVisibility_.target - target) < 0.0001f)
    {
        return;
    }

    hideAnimationStart_ = fullscreenVisibility_.value;
    hideAnimationTarget_ = target;
    hideAnimationElapsed_ = 0.0;
    hideAnimationActive_ = true;
    fullscreenVisibility_.target = target;
}

void App::CheckFullscreen()
{
    if (exitRequested_)
    {
        return;
    }

    bool fullscreenCandidate = false;

    if (monitorIndex_ >= 0
        && monitorIndex_ < static_cast<int>(monitors_.size()))
    {
        const MonitorTarget& target =
            monitors_[static_cast<size_t>(monitorIndex_)];
        HWND foreground = GetForegroundWindow();
        HWND root = foreground ? GetAncestor(foreground, GA_ROOT) : nullptr;

        if (root && root != window_.Handle() && root != GetShellWindow()
            && IsWindowVisible(root) && !IsIconic(root)
            && !IsWindowsShellUiWindow(root)
            && MonitorFromWindow(root, MONITOR_DEFAULTTONEAREST)
                == target.handle)
        {
            wchar_t className[128]{};
            GetClassNameW(root, className, ARRAYSIZE(className));
            const bool shellSurface = wcscmp(className, L"Progman") == 0
                || wcscmp(className, L"WorkerW") == 0
                || wcscmp(className, L"Shell_TrayWnd") == 0
                || wcscmp(className, L"Shell_SecondaryTrayWnd") == 0;

            if (!shellSurface)
            {
                MONITORINFO monitorInfo{};
                monitorInfo.cbSize = sizeof(monitorInfo);

                RECT visibleBounds{};
                if (GetMonitorInfoW(target.handle, &monitorInfo)
                    && GetVisibleWindowBounds(root, visibleBounds))
                {
                    // GetWindowRect includes invisible resize borders on
                    // maximized Chromium/Win32 windows. DWM's extended frame
                    // bounds describe what is actually visible on screen.
                    constexpr LONG kFullscreenTolerance = 1;
                    const bool coversMonitor = RectCovers(
                        visibleBounds, monitorInfo.rcMonitor,
                        kFullscreenTolerance);

                    // A regular maximized desktop window is not fullscreen,
                    // even on an auto-hidden taskbar setup where rcWork can
                    // nearly equal rcMonitor.
                    fullscreenCandidate =
                        coversMonitor && !IsOrdinaryMaximizedWindow(root);
                }
            }
        }
    }

    // Require two consecutive positive samples (the checker runs every
    // 250 ms) before entering fullscreen. Leaving fullscreen is immediate.
    // This filters transient maximize/DWM state changes without making the
    // dock feel sticky after the foreground app exits fullscreen.
    if (fullscreenCandidate)
    {
        fullscreenCandidateChecks_ =
            (std::min)(fullscreenCandidateChecks_ + 1, 2);
    }
    else
    {
        fullscreenCandidateChecks_ = 0;
    }

    const bool fullscreen = fullscreenActive_
        ? fullscreenCandidate
        : fullscreenCandidateChecks_ >= 2;

    const auto now = std::chrono::steady_clock::now();
    if (fullscreen != fullscreenActive_)
    {
        fullscreenActive_ = fullscreen;

        if (fullscreen)
        {
            const int delay = (std::clamp)(
                config_.settings.autoHideDelayMs, 0, 5000);
            hideDeadline_ = now + std::chrono::milliseconds(delay);
            mouseActive_ = false;
        }
        else
        {
            // Leaving fullscreen does not reveal an auto-hidden dock by
            // itself. The edge-reveal policy below decides when to show it.
            autoHideHidePending_ = false;
            if (!config_.settings.autoHide)
            {
                SetFullscreenVisibilityTarget(1.0f);
            }
        }

        WakeAnimation();
    }

    // Fullscreen hiding is mandatory. autoHide only controls whether the
    // dock reserves work-area space; it never disables this transition.
    if (fullscreen && fullscreenVisibility_.target > 0.0f
        && now >= hideDeadline_)
    {
        SetFullscreenVisibilityTarget(0.0f);
        WakeAnimation();
    }
}

bool App::RefreshMonitors()
{
    std::vector<MonitorTarget> refreshed;
    const BOOL enumerated = EnumDisplayMonitors(
        nullptr,
        nullptr,
        [](HMONITOR monitor, HDC, LPRECT rect, LPARAM data) -> BOOL
        {
            auto* targets = reinterpret_cast<std::vector<MonitorTarget>*>(data);

            MONITORINFOEXW info{};
            info.cbSize = sizeof(info);

            MonitorTarget target;
            target.handle = monitor;
            if (GetMonitorInfoW(monitor, &info))
            {
                target.device = info.szDevice;
                target.rect = info.rcMonitor;
                target.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
            }
            else if (rect)
            {
                target.rect = *rect;
            }
            else
            {
                return TRUE;
            }

            targets->push_back(std::move(target));
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&refreshed));

    // Ignore an incomplete RDP/driver transition instead of discarding our
    // last valid monitor information and jumping to a bogus (0, 0) position.
    if (!enumerated || refreshed.empty())
    {
        return false;
    }

    // A temporary RDP monitor must never overwrite the user's saved display
    // choice. Prefer it if present; otherwise fall back to the current primary
    // monitor, and automatically return when that device reappears.
    int selected = -1;
    if (!config_.settings.monitor.empty())
    {
        for (size_t i = 0; i < refreshed.size(); ++i)
        {
            if (refreshed[i].device == config_.settings.monitor)
            {
                selected = static_cast<int>(i);
                break;
            }
        }
    }
    if (selected < 0)
    {
        for (size_t i = 0; i < refreshed.size(); ++i)
        {
            if (refreshed[i].primary)
            {
                selected = static_cast<int>(i);
                break;
            }
        }
    }
    if (selected < 0)
    {
        selected = 0;
    }

    // Even if the monitor count and work area match, RDP may have changed
    // its device/handle, actual boundaries or which monitor is primary.
    bool changed = monitorIndex_ != selected
        || monitors_.size() != refreshed.size();
    if (!changed)
    {
        for (size_t i = 0; i < refreshed.size(); ++i)
        {
            if (monitors_[i].handle != refreshed[i].handle
                || monitors_[i].device != refreshed[i].device
                || monitors_[i].primary != refreshed[i].primary
                || !EqualRect(&monitors_[i].rect, &refreshed[i].rect))
            {
                changed = true;
                break;
            }
        }
    }

    monitors_ = std::move(refreshed);
    monitorIndex_ = selected;
    return changed;
}

RECT App::CurrentWorkArea() const
{
    if (monitorIndex_ >= 0 && monitorIndex_ < static_cast<int>(monitors_.size()))
    {
        MONITORINFO info{};
        info.cbSize = sizeof(info);

        if (GetMonitorInfoW(monitors_[static_cast<size_t>(monitorIndex_)].handle,
                            &info))
        {
            return info.rcWork;
        }
    }

    MONITORINFO fallback{};
    fallback.cbSize = sizeof(fallback);

    if (GetMonitorInfoW(MonitorFromWindow(window_.Handle(),
                                          MONITOR_DEFAULTTOPRIMARY),
                        &fallback))
    {
        return fallback.rcWork;
    }

    return RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
}

void App::SelectMonitor(int index)
{
    if (index < 0 || index >= static_cast<int>(monitors_.size()))
    {
        return;
    }

    monitorIndex_ = index;
    config_.settings.monitor = monitors_[static_cast<size_t>(index)].device;

    // Query the destination display before moving the window there.
    window_.SetDpi(DockWindow::QueryMonitorDpi(
        monitors_[static_cast<size_t>(index)].handle, window_.Handle()));
    RebuildMetrics();
    WakeAnimation();

    SaveConfiguration();
}

void App::AppendMonitorMenu(std::vector<ui::MenuItem>& menu)
{
    if (monitors_.size() < 2)
    {
        return;
    }

    std::vector<ui::MenuItem> displays;
    for (size_t i = 0; i < monitors_.size(); ++i)
    {
        const MonitorTarget& target = monitors_[i];

        const long width = target.rect.right - target.rect.left;
        const long height = target.rect.bottom - target.rect.top;

        wchar_t label[128];
        swprintf(label, 128, L"%ls %zu  (%ld × %ld)%ls",
                 UiText(L"显示器"), i + 1, width, height,
                 target.primary ? UiText(L"  · 主显示器") : L"");

        displays.push_back(ui::MenuItem::Command(
            kMenuMonitorBase + static_cast<UINT>(i), label, true,
            static_cast<int>(i) == monitorIndex_));
    }

    menu.push_back(ui::MenuItem::Submenu(UiText(L"显示器"),
                                         std::move(displays)));
}

void App::CheckDisplayChange()
{
    // Enumerate every time: the monitor count can remain unchanged across
    // RDP connect/disconnect, while resolution, bounds or identity changes.
    const bool topologyChanged = RefreshMonitors();
    if (monitorIndex_ < 0
        || monitorIndex_ >= static_cast<int>(monitors_.size()))
    {
        return;
    }

    const RECT work = CurrentWorkArea();
    const bool workChanged = !EqualRect(&work, &lastWorkArea_);
    const int monitorDpi = DockWindow::QueryMonitorDpi(
        monitors_[static_cast<size_t>(monitorIndex_)].handle,
        window_.Handle());
    const bool dpiChanged = monitorDpi != window_.GetDpi();

    // WM_DISPLAYCHANGE and session notifications also mean Explorer may have
    // forgotten the AppBar, even if screen geometry is unchanged.
    const bool forceResync = displayResyncPending_;
    displayResyncPending_ = false;

    // A one-time startup repair is needed for users upgrading from builds
    // whose update script force-killed the process before ABM_REMOVE.
    // Fold it into the regular topology/DPI refresh: RDP might also switch
    // scaling while the delayed repair is pending.
    bool startupAppBarRecovery = false;
    if (initialAppBarRecoveryPending_
        && std::chrono::steady_clock::now() >= initialAppBarRecoveryAt_)
    {
        initialAppBarRecoveryPending_ = false;
        startupAppBarRecovery = !config_.settings.autoHide
            && !exitRequested_;
    }

    if (topologyChanged || dpiChanged || forceResync
        || startupAppBarRecovery)
    {
        window_.RemoveAppBarReservation();
        if (dpiChanged)
        {
            window_.SetDpi(monitorDpi);
            RebuildMetrics();
        }
        else
        {
            RepositionWindow();
        }
        WakeAnimation();
    }
    else if (workChanged)
    {
        // Another AppBar or the Windows taskbar may have changed the work
        // area while our cached reservation dimensions stayed the same.
        RepositionWindow(true);
    }
    else if (!config_.settings.autoHide && !exitRequested_)
    {
        RECT approved{};
        if (!window_.GetAppBarReservationBounds(approved))
        {
            // ABM_NEW/SETPOS can temporarily fail while Explorer restarts.
            // Retry on the existing 1Hz monitor check, not each frame.
            RepositionWindow();
        }
    }
}

void App::DetectRefreshRate()
{
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);

    int hertz = 60;
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode)
        && mode.dmDisplayFrequency > 0)
    {
        hertz = mode.dmDisplayFrequency;
    }

    int interval = 1000 / hertz;
    if (interval < 5)
    {
        interval = 5;
    }

    if (interval > 17)
    {
        interval = 17;
    }

    frameIntervalMs_ = interval;
}

float App::HoverStrength() const
{
    if (!mouseActive_)
    {
        return 0.0f;
    }

    // Fixed mode deliberately treats the whole expanded panel as the hover
    // surface. Using the base icon row here would make the panel collapse
    // as soon as the cursor crossed into either side of its reserved space,
    // even though HitTest still considered that space part of the dock.
    if (config_.settings.panelMode == PanelMode::Fixed
        && frame_.panelWidth > 0.0f)
    {
        const float left = frame_.panelX - metrics_.hitMargin;
        const float right = frame_.panelX + frame_.panelWidth
            + metrics_.hitMargin;

        if (mouseX_ >= left && mouseX_ <= right)
        {
            return 1.0f;
        }

        const float fadeDistance = metrics_.paddingX;
        return RowInfluenceFade(mouseX_, left, right, fadeDistance);
    }

    float coreLeft = 0.0f;
    float coreRight = 0.0f;
    float fadeDistance = 0.0f;
    RowInfluenceBounds(geometry_, items_.size(), metrics_,
                       coreLeft, coreRight, fadeDistance);

    return RowInfluenceFade(mouseX_, coreLeft, coreRight, fadeDistance);
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

void App::Tick(double dt)
{
    // How much the pointer drives the dock this frame. Engaging is instant;
    // releasing glides down so the icons never snap back, whatever way the
    // cursor leaves (past the end of the row, upwards, or off the window).
    const float wanted = HoverStrength();

    // Lift the dragged icon smoothly above the dock while it is held.
    const float wantedLift = draggingIndex_ >= 0 ? 1.0f : 0.0f;
    const float liftStep = ClampF(static_cast<float>(dt) * 14.0f,
                                  0.0f, 1.0f);
    dragLift_ += (wantedLift - dragLift_) * liftStep;

    if (wanted >= hoverPresence_)
    {
        hoverPresence_ = wanted;
    }
    else
    {
        hoverPresence_ = wanted
            + (hoverPresence_ - wanted)
              * static_cast<float>(std::exp(-dt / kReleaseSeconds));

        if (hoverPresence_ - wanted < kReleaseCutoff)
        {
            hoverPresence_ = wanted;
        }
    }

    // Follow the cursor while it still drives the dock; hold the last
    // influential position through the release so the geometry has a stable
    // shape to fade out from.
    if (wanted > 0.0f)
    {
        anchorX_ = mouseX_;
    }

    UpdateScaleTargets(
        pointers_, anchorX_, hoverPresence_, metrics_,
        config_.settings.panelMode == PanelMode::Elastic);

    // Only the release tail keeps the loop alive; a settled hover stays
    // idle so a resting dock costs nothing.
    bool moving = hoverPresence_ != wanted
        || std::fabs(dragLift_ - wantedLift) > 0.001f;

    for (auto& item : items_)
    {
        if (config_.settings.panelMode == PanelMode::Static)
        {
            item->launching = false;
            item->scaleSpring.Reset(1.0f);
            item->bounceSpring.Reset(0.0f);
            item->scale = 1.0f;
            item->bounceOffset = 0.0f;
            continue;
        }

        item->scaleSpring.Update(dt, kScaleSpring);
        item->scale = ClampF(item->scaleSpring.value,
                             0.5f, metrics_.magnification + 0.1f);

        if (item->launching)
        {
            item->launchElapsed += dt;
            item->bounceTimer -= static_cast<float>(dt);

            if (item->bounceTimer <= 0.0f)
            {
                item->bounceSpring.AddImpulse(-kBounce.impulse * dockScale_);
                item->bounceTimer = kBounce.interval;
            }

            if (item->launchElapsed > static_cast<double>(kBounce.timeout))
            {
                item->launching = false;
            }
        }

        item->bounceSpring.Update(dt, kBounce.spring);
        item->bounceOffset = item->bounceSpring.value;

        if (item->launching
            || !item->scaleSpring.Settled(kScaleSpring)
            || !item->bounceSpring.Settled(kBounce.spring))
        {
            moving = true;
        }
    }

    if (config_.settings.panelMode == PanelMode::Fixed)
    {
        // Expand once on entry, collapse on exit: the width is not a function
        // of the current scales, so the icons always stay inside the panel.
        // Driven by the same eased presence value as the icons, so leaving
        // the dock never pops.
        // Fixed-panel expansion stays independent of the magnification
        // setting. The extra width is based only on unscaled icon dimensions.
        constexpr float kFixedPanelStretchPerIcon = 0.42f;
        const float fixedStretch = static_cast<float>(items_.size())
            * metrics_.iconSize * kFixedPanelStretchPerIcon;
        panelWidth_.target = geometry_.basePanelWidth
            + fixedStretch * hoverPresence_;

        panelWidth_.Update(dt, kPanelSpring);

        if (!panelWidth_.Settled(kPanelSpring))
        {
            moving = true;
        }

        frame_ = ApplyLayout(pointers_, geometry_, panelWidth_.value,
                             anchorX_, hoverPresence_, metrics_);
    }
    else
    {
        frame_ = ApplyLayout(pointers_, geometry_, 0.0f,
                             anchorX_, hoverPresence_, metrics_);
    }

    // Keep a live insertion slot under the dragged icon. The row is laid out
    // normally first, then the neighbours are shifted by one icon cell so a
    // visible gap opens where the dragged item will be dropped.
    if (draggingIndex_ >= 0
        && draggingIndex_ < static_cast<int>(items_.size()))
    {
        const int from = draggingIndex_;
        int firstPinned = -1;
        int pinnedCount = 0;
        int beforePointer = 0;

        for (int i = 0; i < static_cast<int>(items_.size()); ++i)
        {
            if (items_[static_cast<size_t>(i)]->kind != DockItemKind::Pinned)
            {
                continue;
            }

            if (firstPinned < 0)
            {
                firstPinned = i;
            }

            if (i != from
                && mouseX_ > items_[static_cast<size_t>(i)]->centerX)
            {
                ++beforePointer;
            }
            ++pinnedCount;
        }

        int target = from;
        if (firstPinned >= 0 && pinnedCount > 0)
        {
            target = firstPinned + beforePointer;
            target = std::clamp(
                target, firstPinned, firstPinned + pinnedCount - 1);
        }

        dragTargetIndex_ = target;
        if (target != from)
        {
            // Keep the last slot alive while the pointer crosses the
            // original position, so displaced neighbours ease back home.
            dragVisualTargetIndex_ = target;
        }

        const float desired = target == from ? 0.0f : 1.0f;
        const float placeholderStep = ClampF(
            static_cast<float>(dt) * 16.0f, 0.0f, 1.0f);
        dragPlaceholderAmount_ +=
            (desired - dragPlaceholderAmount_) * placeholderStep;

        if (std::fabs(dragPlaceholderAmount_ - desired) > 0.001f)
        {
            moving = true;
        }

        const int visualTarget = dragVisualTargetIndex_;
        if (visualTarget >= 0
            && visualTarget != from
            && dragPlaceholderAmount_ > 0.001f)
        {
            const float slot = items_[static_cast<size_t>(from)]->size
                + metrics_.spacing;

            if (visualTarget > from)
            {
                for (int i = from + 1; i <= visualTarget; ++i)
                {
                    DockItem& item = *items_[static_cast<size_t>(i)];
                    item.x -= slot * dragPlaceholderAmount_;
                    item.centerX -= slot * dragPlaceholderAmount_;
                }
            }
            else
            {
                for (int i = visualTarget; i < from; ++i)
                {
                    DockItem& item = *items_[static_cast<size_t>(i)];
                    item.x += slot * dragPlaceholderAmount_;
                    item.centerX += slot * dragPlaceholderAmount_;
                }
            }
        }
    }

    int tooltipIndex = -1;
    // During an auto-hide reveal, the dock moves underneath a stationary
    // pointer. A transient hit on an icon must not start a tooltip/menu that
    // will immediately fade when the moving dock clears the pointer.
    const bool dockReadyForTooltip = !config_.settings.autoHide
        || (fullscreenVisibility_.target >= 0.999f
            && fullscreenVisibility_.value >= 0.98f);
    const bool pointerOnWindowMenu =
        PointInWindowMenu(mousePhysicalX_, mousePhysicalY_);

    if (draggingIndex_ < 0 && mouseActive_ && dockReadyForTooltip
        && !pointerOnWindowMenu)
    {
        tooltipIndex = IndexAtPoint(mouseX_, mouseY_);
    }

    DockItem* hoveredItem = tooltipIndex >= 0
        && tooltipIndex < static_cast<int>(items_.size())
        ? items_[static_cast<size_t>(tooltipIndex)].get()
        : nullptr;

    const bool canExpandWindows =
        hoveredItem && hoveredItem->running;

    if (draggingIndex_ >= 0 || !dockReadyForTooltip)
    {
        windowMenuVisible_ = false;
        windowMenuLeaveElapsed_ = 0.0f;
        windowMenuHoveredRow_ = -1;
        windowMenuPressedRow_ = -1;
        windowMenuBounds_ = D2D1::RectF(0, 0, 0, 0);
    }
    else if (canExpandWindows)
    {
        if (windowMenuItemId_ != hoveredItem->id)
        {
            windowMenuItemId_ = hoveredItem->id;
            windowMenuHoveredRow_ = -1;
            windowMenuPressedRow_ = -1;
            windowMenuBounds_ = D2D1::RectF(0, 0, 0, 0);
        }

        // Preview menu replaces the ordinary tooltip bubble, so it follows
        // the same immediate hover semantics instead of maintaining its own
        // Windows-taskbar-style delay setting.
        if (!windowMenuVisible_)
        {
            windowMenuVisible_ = true;
            needsRender_ = true;
        }

        windowMenuLeaveElapsed_ = 0.0f;
    }
    else if (windowMenuVisible_)
    {
        if (pointerOnWindowMenu)
        {
            windowMenuLeaveElapsed_ = 0.0f;
        }
        else
        {
            // Keep only the small icon-to-bubble crossing grace period.
            constexpr float kWindowMenuLeaveDelay = 0.18f;
            windowMenuLeaveElapsed_ += static_cast<float>(dt);
            moving = true;

            if (windowMenuLeaveElapsed_ >= kWindowMenuLeaveDelay)
            {
                windowMenuVisible_ = false;
                windowMenuLeaveElapsed_ = 0.0f;
                windowMenuHoveredRow_ = -1;
                windowMenuPressedRow_ = -1;
                windowMenuBounds_ = D2D1::RectF(0, 0, 0, 0);
                needsRender_ = true;
            }
        }
    }
    else
    {
        windowMenuLeaveElapsed_ = 0.0f;
    }

    // The represented app may have closed its last previewable window.
    // Never leave a stale preview bubble alive.
    if (windowMenuVisible_)
    {
        const auto menuItem = std::find_if(
            items_.begin(), items_.end(), [this](const auto& item)
            {
                return item->id == windowMenuItemId_;
            });

        if (menuItem == items_.end() || !(*menuItem)->running)
        {
            windowMenuVisible_ = false;
            windowMenuHoveredRow_ = -1;
            windowMenuBounds_ = D2D1::RectF(0, 0, 0, 0);
            needsRender_ = true;
        }
    }

    if (tooltipIndex >= 0
        && tooltipIndex < static_cast<int>(items_.size())
        && !canExpandWindows)
    {
        tooltipItemId_ = items_[static_cast<size_t>(tooltipIndex)]->id;
    }

    // A previewable running app *is* the tooltip bubble. Kill the ordinary
    // name bubble immediately so the two never overlap or cross-fade.
    const bool suppressTooltip =
        canExpandWindows
        || (windowMenuVisible_ && pointerOnWindowMenu);

    if (suppressTooltip)
    {
        tooltipPresence_ = 0.0f;
        tooltipOpacity_ = 0.0f;
    }
    else
    {
        const float tooltipTarget = tooltipIndex >= 0 ? 1.0f : 0.0f;
        const float fadeSeconds = ClampF(
            config_.settings.tooltipFadeSeconds, 0.05f, 1.0f);
        const float tooltipStep = 1.0f - static_cast<float>(
            std::exp(-dt / (static_cast<double>(fadeSeconds) * 0.35)));
        tooltipPresence_ += (tooltipTarget - tooltipPresence_) * tooltipStep;
        if (std::fabs(tooltipPresence_ - tooltipTarget) < 0.003f)
        {
            tooltipPresence_ = tooltipTarget;
        }
        tooltipOpacity_ =
            ClampF(config_.settings.tooltipOpacity, 0.0f, 1.0f)
            * tooltipPresence_;
        if (tooltipPresence_ != tooltipTarget)
        {
            moving = true;
        }
    }

    SpringParams fullscreenSpring = kFullscreenSlideSpring;
    const float previousFullscreenVisibility = fullscreenVisibility_.value;
    if (hideAnimationActive_)
    {
        hideAnimationElapsed_ += dt;
        const int durationMs = std::clamp(
            config_.settings.autoHideAnimationMs, 0, 1000);
        if (durationMs == 0)
        {
            fullscreenVisibility_.value = hideAnimationTarget_;
            hideAnimationActive_ = false;
        }
        else
        {
            const double duration = durationMs / 1000.0;
            const float t = static_cast<float>(std::clamp(
                hideAnimationElapsed_ / duration, 0.0, 1.0));
            // Ease in/out so both reveal and retract remain visibly smooth.
            const float eased = t * t * (3.0f - 2.0f * t);
            fullscreenVisibility_.value = hideAnimationStart_
                + (hideAnimationTarget_ - hideAnimationStart_) * eased;
            if (t >= 1.0f)
            {
                fullscreenVisibility_.value = hideAnimationTarget_;
                hideAnimationActive_ = false;
            }
        }
        fullscreenVisibility_.target = hideAnimationTarget_;
    }
    else
    {
        fullscreenVisibility_.Update(dt, fullscreenSpring);
    }
    if (std::fabs(fullscreenVisibility_.value - previousFullscreenVisibility)
        > 0.0001f)
    {
        UpdateDockWindowPosition();
    }
    if (hideAnimationActive_ || !fullscreenVisibility_.Settled(fullscreenSpring))
    {
        moving = true;
    }

    const bool indicatorAllowed = config_.settings.autoHide
        && !fullscreenActive_ && !exitRequested_ && !settingsWindowOpen_
        && !launcher_.Visible();
    // Drive the status indicator from the dock's own progress. Independent
    // springs made the bar appear/disappear a beat before the dock moved.
    hideIndicatorVisibility_.target = indicatorAllowed
        ? 1.0f - ClampF(fullscreenVisibility_.value, 0.0f, 1.0f)
        : 0.0f;
    hideIndicatorVisibility_.value = hideIndicatorVisibility_.target;

    if (hideIndicatorVisibility_.value > 0.001f)
    {
        const RECT dockBounds = window_.GetBounds();
        RECT indicatorRect = CurrentWorkArea();
        if (monitorIndex_ >= 0
            && monitorIndex_ < static_cast<int>(monitors_.size()))
        {
            indicatorRect =
                monitors_[static_cast<size_t>(monitorIndex_)].rect;
        }

        const int indicatorHeight = (std::max)(
            1, static_cast<int>(std::lround(5.0f * dockScale_)));
        const int fullIndicatorWidth = (std::max)(
            1, static_cast<int>(std::lround(frame_.panelWidth)));
        const float indicatorStretch = 0.88f
            + 0.12f * ClampF(hideIndicatorVisibility_.value, 0.0f, 1.0f);
        const int indicatorLength = (std::max)(
            1, static_cast<int>(std::lround(
                fullIndicatorWidth * indicatorStretch)));
        int indicatorX = dockBounds.left
            + static_cast<int>(std::lround(frame_.panelX
                + (fullIndicatorWidth - indicatorLength) * 0.5f));
        int indicatorY = indicatorRect.bottom - indicatorHeight;
        int indicatorWidth = indicatorLength;
        int actualIndicatorHeight = indicatorHeight;
        if (config_.settings.dockEdge == DockEdge::Top)
        {
            indicatorY = indicatorRect.top;
        }
        else if (config_.settings.dockEdge == DockEdge::Left
                 || config_.settings.dockEdge == DockEdge::Right)
        {
            indicatorWidth = indicatorHeight;
            actualIndicatorHeight = indicatorLength;
            indicatorX = config_.settings.dockEdge == DockEdge::Left
                ? indicatorRect.left : indicatorRect.right - indicatorWidth;
            indicatorY = dockBounds.top + static_cast<int>(std::lround(
                frame_.panelX + (fullIndicatorWidth - indicatorLength) * 0.5f));
        }

        float red = 1.0f, green = 1.0f, blue = 1.0f, colorAlpha = 1.0f;
        const std::wstring& edgeColor =
            config_.settings.backgroundBottom.empty()
                ? config_.settings.backgroundTop
                : config_.settings.backgroundBottom;
        ParseHexColor(edgeColor, red, green, blue, colorAlpha);
        const COLORREF indicatorColor = RGB(
            static_cast<BYTE>(std::lround(ClampF(red, 0.0f, 1.0f) * 255.0f)),
            static_cast<BYTE>(std::lround(ClampF(green, 0.0f, 1.0f) * 255.0f)),
            static_cast<BYTE>(std::lround(ClampF(blue, 0.0f, 1.0f) * 255.0f)));
        const BYTE indicatorOpacity = static_cast<BYTE>(std::lround(
            255.0f * ClampF(config_.settings.backgroundOpacity, 0.0f, 1.0f)
            * ClampF(colorAlpha, 0.0f, 1.0f)
            * ClampF(hideIndicatorVisibility_.value, 0.0f, 1.0f)));
        window_.UpdateHideIndicator(
            true, indicatorX, indicatorY, indicatorWidth,
            actualIndicatorHeight,
            indicatorOpacity, indicatorColor);
    }
    else
    {
        window_.UpdateHideIndicator(false, 0, 0, 0, 0, 0, RGB(255, 255, 255));
    }

    Render();

    if (exitRequested_ && !hideAnimationActive_
        && fullscreenVisibility_.Settled(fullscreenSpring))
    {
        // Finish the slide before destroying the layered window and
        // releasing its work-area reservation.
        window_.Destroy();
        return;
    }

    animating_ = moving;
}

void App::Render()
{
    // Update the panel brushes before BeginDraw() builds the brush cache.
    // Settings dialogs call Render() synchronously while the main loop is
    // modal; doing this after BeginDraw() makes a newly picked colour wait
    // for an unrelated later frame (often the next mouse move).
    {
        float tr = 0.0f, tg = 0.0f, tb = 0.0f, ta = 0.0f;
        float br = 0.0f, bg = 0.0f, bb = 0.0f, ba = 0.0f;

        const bool hasTop = ParseHexColor(config_.settings.backgroundTop,
                                          tr, tg, tb, ta);
        const bool hasBottom = ParseHexColor(config_.settings.backgroundBottom,
                                             br, bg, bb, ba);

        // A single top colour is a deliberate solid-colour panel. The
        // bottom stop is only needed when the user enables the gradient.
        const bool usePanelColour = hasTop;
        const D2D1_COLOR_F solidOrTop = hasTop
            ? D2D1::ColorF(tr, tg, tb)
            : D2D1::ColorF(1.0f, 1.0f, 1.0f);

        renderer_.SetPanelBackground(
            solidOrTop,
            hasBottom ? D2D1::ColorF(br, bg, bb) : solidOrTop,
            usePanelColour);
    }

    if (!renderer_.BeginDraw())
    {
        return;
    }

    renderer_.DrawShadow(frame_.panelX, geometry_.panelY, frame_.panelWidth);

    renderer_.DrawBackground(frame_.panelX,
                             geometry_.panelY,
                             frame_.panelWidth,
                             config_.settings.backgroundOpacity,
                             config_.settings.borderOpacity);

    // macOS-style boundary between persistent launchers and temporary
    // running applications. It follows the animated item centres, so the
    // divider remains visually attached to the two groups while magnifying.
    const auto firstTransient = std::find_if(
        items_.begin(), items_.end(),
        [](const std::unique_ptr<DockItem>& item)
        {
            return item->kind == DockItemKind::RunningTransient;
        });
    if (firstTransient != items_.end() && firstTransient != items_.begin())
    {
        const DockItem& right = **firstTransient;
        const DockItem& left = **(firstTransient - 1);
        // Centre the line in the visible empty gap, even when the two
        // neighbouring icons have different animated magnification.
        const float dividerX = (left.x + left.size + right.x) * 0.5f;
        const float inset = metrics_.paddingY * 0.7f;
        renderer_.DrawDivider(
            dividerX,
            geometry_.panelY + inset,
            geometry_.panelY + geometry_.panelHeight - inset);
    }

    // Keep the uploaded mip close to the largest on-screen icon size.
    // D2D1's DC render target only offers bilinear bitmap filtering; feeding
    // it a bitmap that is ~2x larger than the final icon forces another
    // heavy minification pass and produces visible stair-stepping on 1080p
    // displays. WIC performs the expensive high-quality downscale once, then
    // D2D only has to resize within a narrow range while hover animation runs.
    // 1.25 leaves headroom for spring overshoot and the 8% drag lift without
    // making the normal-size icon a tiny sample of an oversized bitmap.
    constexpr float kIconMipHeadroom = 1.25f;
    const unsigned int displaySize = static_cast<unsigned int>(
        std::ceil(metrics_.iconSize * metrics_.magnification
                  * kIconMipHeadroom));

    // Resolves the plate look for one icon. Radius and opacity are global
    // (the tile shape is a dock wide constant, like macOS); the on/off
    // switch, the icon-to-plate ratio and the colours belong to the icon.
    auto plateColor = [&](const std::wstring& text,
                          const D2D1_COLOR_F& fallback) -> D2D1_COLOR_F
    {
        float r = fallback.r, g = fallback.g, b = fallback.b, a = 1.0f;

        if (ParseHexColor(text, r, g, b, a))
        {
            return D2D1::ColorF(r, g, b, 1.0f);
        }

        return fallback;
    };

    const IconBackdrop& backdrop = config_.settings.backdrop;

    // Temporarily move the dragged item for this frame only. The layout
    // remains unchanged, so the normal row can still be used to calculate
    // the eventual insertion point on release.
    DockItem* draggedVisual = nullptr;
    float savedDragCenterX = 0.0f;
    float savedDragSize = 0.0f;
    float savedDragBaselineBottom = 0.0f;

    if (draggingIndex_ >= 0
        && draggingIndex_ < static_cast<int>(items_.size()))
    {
        draggedVisual = items_[static_cast<size_t>(draggingIndex_)].get();
        savedDragCenterX = draggedVisual->centerX;
        savedDragSize = draggedVisual->size;
        savedDragBaselineBottom = draggedVisual->baselineBottom;

        const float visualSize = savedDragSize
            * (1.0f + 0.08f * dragLift_);
        const float visualCenterX = mouseX_ - dragGrabX_;
        const float visualCenterY = mouseY_ - dragGrabY_
            - 8.0f * dockScale_ * dragLift_;

        draggedVisual->centerX = visualCenterX;
        draggedVisual->size = visualSize;
        draggedVisual->baselineBottom = visualCenterY + visualSize * 0.5f;
    }

    for (auto& item : items_)
    {
        const PlateStyle& own = item->plate;

        const bool plateOn = own.enabled;

        // How much of the plate the icon itself occupies. A zero stored
        // ratio means "follow the global default".
        const float iconFill = plateOn
            ? ClampF(own.iconScale > 0.0f ? own.iconScale : backdrop.iconScale,
                     0.4f, 1.5f)
            : 1.0f;

        // Clip against the plate, independent of the artwork's fill ratio.
        // The artwork itself stays unmasked so shrinking it does not shrink
        // the clipping boundary along with it.
        const float baseRadius = backdrop.cornerRadius * dockScale_;

        if (!item->EnsureIconBitmap(renderer_.Target(), renderer_.Wic(),
                                    displaySize, 0.0f, iconFill))
        {
            continue;
        }

        const float size = item->size;
        const float bottom = item->baselineBottom + item->bounceOffset;

        const D2D1_RECT_F plate = D2D1::RectF(
            item->centerX - size * 0.5f,
            bottom - size,
            item->centerX + size * 0.5f,
            bottom);

        const float plateOpacity = ClampF(
            own.opacity >= 0.0f ? own.opacity : backdrop.opacity,
            0.0f, 1.0f);

        // The plate is always a vertical blend: from the icon's colour, or
        // from an automatically darkened offset unless the user pinned a
        // second colour.
        const D2D1_COLOR_F top = plateColor(own.top, kDefaultPlateTop);
        const D2D1_COLOR_F bottomColor = own.customBottom
            ? plateColor(own.bottom, DerivedPlateBottom(top))
            : DerivedPlateBottom(top);

        if (plateOn)
        {
            renderer_.DrawIconBackdrop(
                plate,
                baseRadius * item->scale,
                plateOpacity,
                true,
                top,
                bottomColor);
        }

        const float iconSize = size * iconFill;
        const float iconTop = plate.top + (size - iconSize) * 0.5f;
        if (!renderer_.PushRoundedClip(D2D1::RoundedRect(
            plate, baseRadius * item->scale,
            baseRadius * item->scale)))
        {
            continue;
        }
        renderer_.DrawIcon(item->icon.Get(),
            D2D1::RectF(item->centerX - iconSize * 0.5f,
                        iconTop,
                        item->centerX + iconSize * 0.5f,
                        iconTop + iconSize));
        renderer_.PopClip();

        // The rim belongs to the rounded plate. Turning the plate off
        // hides the rim too; users should not have to zero a separate opacity
        // control just to get a plain icon.
        if (plateOn)
        {
            renderer_.DrawPlateRim(plate,
                                   baseRadius * item->scale,
                                   item->plate.strokeOpacity >= 0.0f
                                       ? item->plate.strokeOpacity
                                       : backdrop.strokeOpacity,
                                   config_.settings.backdrop.strokeWidth
                                       * dockScale_ * item->scale,
                                   [&]()
                                   {
                                       float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
                                       if (ParseHexColor(item->plate.strokeColor,
                                                         r, g, b, a))
                                       {
                                           return D2D1::ColorF(r, g, b, a);
                                       }
                                       return D2D1::ColorF(1, 1, 1, 1);
                                   }());
        }
    }

    windowMenuBounds_ = D2D1::RectF(0, 0, 0, 0);
    windowMenuVisibleRows_ = 0;
    windowMenuItemWidth_ = 0.0f;

    if (windowMenuVisible_ && !windowMenuItemId_.empty())
    {
        const auto menuItem = std::find_if(
            items_.begin(), items_.end(), [this](const auto& item)
            {
                return item->id == windowMenuItemId_;
            });

        if (menuItem != items_.end() && (*menuItem)->running)
        {
            const DockItem& item = **menuItem;

            // The preview menu is the running-app version of the name
            // bubble. It inherits the bubble's scale, corner radius, opacity
            // and triangular tail instead of maintaining a separate style.
            const float bubbleScale = ClampF(
                config_.settings.tooltipScale * kTooltipScaleBaseline,
                0.5f, 2.0f);
            const float uiScale = dpiScale_ * bubbleScale;

            // Compact horizontal cards. Each card frames the window title
            // above the live thumbnail; the application name is drawn once
            // beneath the whole strip inside the bubble body.
            // Preview cards are 10% larger than the previous compact
            // version so live content stays readable without changing the
            // bubble visual language.
            // Previous +10% was visually muted by the preview bubble's
            // 0.8 baseline scale. Increase the physical card/menu dimensions
            // another 10% so the change is clearly visible.
            const float itemWidth = 151.25f * uiScale;
            const float menuHeight = 152.0f * uiScale;
            const float tailLength = 8.0f * uiScale;
            const float tipGap = 12.0f * uiScale;

            // The rounded bubble is painted on the Dock surface; the
            // rectangular DWM preview window sits inside it, inset far
            // enough that its square corners stay within the rounded body.
            const float bubbleRadius = config_.settings.tooltipCornerRadius
                * uiScale;
            const float bodyInset = std::ceil(bubbleRadius * 0.3f
                                              + 2.0f * dpiScale_);
            const float tailReach = tailLength + bodyInset;
            const float margin = 8.0f * dpiScale_ + bodyInset;

            const float availableWidth = (std::max)(
                itemWidth,
                static_cast<float>(renderer_.Width()) - margin * 2.0f);
            const size_t maxVisibleByWidth = (std::max)(
                static_cast<size_t>(1),
                static_cast<size_t>(
                    std::floor(availableWidth / itemWidth)));
            constexpr size_t kMaxVisibleWindowItems = 8;
            const size_t previewCount = (std::max)(
                static_cast<size_t>(1), item.windows.size());
            const size_t rowCount = (std::min)(
                previewCount,
                (std::min)(kMaxVisibleWindowItems, maxVisibleByWidth));
            const float menuWidth =
                itemWidth * static_cast<float>(rowCount);

            const float bottom = item.baselineBottom + item.bounceOffset;
            const D2D1_RECT_F logicalIcon = D2D1::RectF(
                item.centerX - item.size * 0.5f,
                bottom - item.size,
                item.centerX + item.size * 0.5f,
                bottom);
            const D2D1_RECT_F iconRect =
                dockTransform_.ToPhysical(logicalIcon);

            D2D1_RECT_F menu{};
            D2D1_POINT_2F tailTip{};
            const float iconCenterX =
                (iconRect.left + iconRect.right) * 0.5f;
            const float iconCenterY =
                (iconRect.top + iconRect.bottom) * 0.5f;

            switch (config_.settings.dockEdge)
            {
            case DockEdge::Top:
                tailTip = D2D1::Point2F(
                    iconCenterX, iconRect.bottom + tipGap);
                menu.left = iconCenterX - menuWidth * 0.5f;
                menu.top = tailTip.y + tailReach;
                menu.right = menu.left + menuWidth;
                menu.bottom = menu.top + menuHeight;
                break;

            case DockEdge::Left:
                tailTip = D2D1::Point2F(
                    iconRect.right + tipGap, iconCenterY);
                menu.left = tailTip.x + tailReach;
                menu.top = iconCenterY - menuHeight * 0.5f;
                menu.right = menu.left + menuWidth;
                menu.bottom = menu.top + menuHeight;
                break;

            case DockEdge::Right:
                tailTip = D2D1::Point2F(
                    iconRect.left - tipGap, iconCenterY);
                menu.right = tailTip.x - tailReach;
                menu.left = menu.right - menuWidth;
                menu.top = iconCenterY - menuHeight * 0.5f;
                menu.bottom = menu.top + menuHeight;
                break;

            case DockEdge::Bottom:
            default:
                tailTip = D2D1::Point2F(
                    iconCenterX, iconRect.top - tipGap);
                menu.left = iconCenterX - menuWidth * 0.5f;
                menu.bottom = tailTip.y - tailReach;
                menu.right = menu.left + menuWidth;
                menu.top = menu.bottom - menuHeight;
                break;
            }

            const float surfaceWidth =
                static_cast<float>(renderer_.Width());
            const float surfaceHeight =
                static_cast<float>(renderer_.Height());

            if (menu.left < margin)
            {
                const float shift = margin - menu.left;
                menu.left += shift;
                menu.right += shift;
            }
            if (menu.right > surfaceWidth - margin)
            {
                const float shift = menu.right - (surfaceWidth - margin);
                menu.left -= shift;
                menu.right -= shift;
            }
            if (menu.top < margin)
            {
                const float shift = margin - menu.top;
                menu.top += shift;
                menu.bottom += shift;
            }
            if (menu.bottom > surfaceHeight - margin)
            {
                const float shift = menu.bottom - (surfaceHeight - margin);
                menu.top -= shift;
                menu.bottom -= shift;
            }

            windowMenuBounds_ = menu;
            windowMenuVisibleRows_ = static_cast<int>(rowCount);
            windowMenuItemWidth_ = itemWidth;

            std::vector<WindowPreview::Entry> previewEntries;
            previewEntries.reserve(rowCount);

            for (size_t i = 0; i < rowCount; ++i)
            {
                WindowPreview::Entry entry;
                if (i < item.windows.size())
                {
                    const DockWindowEntry& window = item.windows[i];
                    entry.hwnd = window.hwnd;
                    entry.title = window.title.empty()
                        ? item.name
                        : WindowTitleWithoutApplicationName(
                            window.title, item.name, item.processName);
                    entry.active = window.active;
                    entry.minimized = window.minimized;
                }
                else
                {
                    // A running-dot item must still own the preview bubble.
                    // Some launchers/background-hosted apps expose their HWND
                    // late (or through a shell host); keep a stable card until
                    // the next process poll resolves the real window.
                    entry.hwnd = nullptr;
                    entry.title = item.name;
                    entry.active = false;
                    entry.minimized = false;
                }
                previewEntries.push_back(std::move(entry));
            }

            // Body, outline and tail are one antialiased shape on the layered
            // Dock surface, so the corners are smooth and the tail has no seam.
            renderer_.DrawPreviewBubble(
                D2D1::RectF(menu.left - bodyInset, menu.top - bodyInset,
                            menu.right + bodyInset, menu.bottom + bodyInset),
                tailTip, config_.settings.dockEdge, uiScale,
                bubbleRadius);

            const RECT dockBounds = window_.GetBounds();
            const RECT screenMenu{
                dockBounds.left
                    + static_cast<LONG>(std::lround(menu.left)),
                dockBounds.top
                    + static_cast<LONG>(std::lround(menu.top)),
                dockBounds.left
                    + static_cast<LONG>(std::lround(menu.right)),
                dockBounds.top
                    + static_cast<LONG>(std::lround(menu.bottom))};

            const std::wstring& previewApplicationName =
                item.runtimeApplicationName.empty()
                ? item.name : item.runtimeApplicationName;

            windowPreview_.Show(
                screenMenu, previewApplicationName, previewEntries,
                windowMenuHoveredRow_, dpiScale_,
                bubbleScale,
                config_.settings.tooltipCornerRadius,
                config_.settings.tooltipOpacity);
        }
    }

    if (!windowMenuVisible_
        || windowMenuBounds_.right <= windowMenuBounds_.left
        || windowMenuBounds_.bottom <= windowMenuBounds_.top)
    {
        windowPreview_.Hide();
    }

    // The label is rendered in the same layered surface as the dock, so it
    // follows the icon's animated centre and size without a separate window.
    if (!windowMenuVisible_ && !launcher_.Visible()
        && tooltipOpacity_ > 0.003f && !tooltipItemId_.empty())
    {
        const auto tooltipItem = std::find_if(
            items_.begin(), items_.end(), [this](const auto& item)
            {
                return item->id == tooltipItemId_;
            });
        if (tooltipItem != items_.end()
            && (draggingIndex_ < 0
                || tooltipItem->get() != draggedVisual))
        {
            const DockItem& item = **tooltipItem;
            const bool verticalDock = config_.settings.dockEdge == DockEdge::Left
                || config_.settings.dockEdge == DockEdge::Right;
            const bool topDock = config_.settings.dockEdge == DockEdge::Top;
            // Tooltip size must not inherit the icon's animated scale.
            // Previously this used item.scale, so Static mode rendered at
            // 1.0x while a magnified hover could jump to ~1.6x. That made the
            // same tooltip look tiny in one mode and oversized in another.
            // Give name bubbles one stable visual baseline and let only the
            // user's Tooltip Scale setting change it.
            constexpr float kNameBubbleVisualBaseline = 1.25f;
            const float nameBubbleScale =
                kNameBubbleVisualBaseline
                * config_.settings.tooltipScale;

            renderer_.DrawTooltip(
                item.name,
                item.centerX,
                verticalDock ? item.baselineBottom
                    : topDock ? item.baselineBottom + 12.0f * dockScale_
                    : item.baselineBottom - item.size - 12.0f * dockScale_,
                nameBubbleScale,
                dockScale_,
                config_.settings.tooltipCornerRadius * dockScale_
                    * nameBubbleScale,
                tooltipOpacity_, topDock);
        }
    }

    // Running indicator: keep it in the panel padding, tracking the growing
    // edge for top-anchored icons and the fixed baseline on other edges.
    const float dot = metrics_.indicatorHeight;

    for (const auto& item : items_)
    {
        if (!item->running || item.get() == draggedVisual)
        {
            continue;
        }

        const bool verticalDock = config_.settings.dockEdge == DockEdge::Left
            || config_.settings.dockEdge == DockEdge::Right;
        const float indicatorY = config_.settings.dockEdge == DockEdge::Top
            ? item->baselineBottom - item->size
                - metrics_.paddingY * 0.5f
            : verticalDock
                ? item->baselineBottom - item->size
                    - metrics_.paddingY * 0.5f
                : geometry_.indicatorCenterY;
        renderer_.DrawIndicator(item->centerX - dot * 0.5f,
                                indicatorY - dot * 0.5f, dot);
    }

    if (draggedVisual)
    {
        draggedVisual->centerX = savedDragCenterX;
        draggedVisual->size = savedDragSize;
        draggedVisual->baselineBottom = savedDragBaselineBottom;
    }

    if (!renderer_.EndDraw())
    {
        // The render target was lost; it has been recreated and will be drawn
        // again on the next frame.
        return;
    }

    window_.Present(renderer_.SurfaceDC(), renderer_.Width(), renderer_.Height());
}

void App::PollProcesses()
{
    RefreshRunningApplications();

    std::vector<DockItem*> pinned;
    std::vector<std::wstring> names;
    pinned.reserve(items_.size());
    names.reserve(items_.size());

    for (auto& item : items_)
    {
        if (item->kind == DockItemKind::RunningTransient)
        {
            item->running = true;
            continue;
        }

        if (item->kind != DockItemKind::Pinned)
        {
            item->running = false;
            continue;
        }

        pinned.push_back(item.get());
        names.push_back(item->processName);
    }

    std::vector<bool> running;
    ProcessMonitor::Query(names, running);

    for (size_t i = 0; i < pinned.size() && i < running.size(); ++i)
    {
        DockItem& item = *pinned[i];

        // Keep the black-dot contract useful: if the process scanner says an
        // app is running but the strict taskbar-window pass found nothing,
        // make one more pass for a normal visible top-level HWND. This gives
        // single-window/running pinned apps a DWM preview whenever Windows
        // exposes an actual window for them.
        if (running[i] && item.windows.empty())
        {
            if (HWND hwnd = AppLauncher::FindRunningWindow(item.processName))
            {
                wchar_t title[512]{};
                GetWindowTextW(hwnd, title, ARRAYSIZE(title));

                DockWindowEntry entry;
                entry.hwnd = hwnd;
                entry.title = title;
                entry.minimized = IsIconic(hwnd) != FALSE;
                entry.active = hwnd == GetForegroundWindow();
                item.windows.push_back(std::move(entry));
                item.runtimeApplicationName =
                    LocalizedApplicationNameFromWindow(hwnd);
            }
        }

        const bool isRunning = running[i] || !item.windows.empty();

        if (isRunning && item.launching)
        {
            item.launching = false;
        }

        if (isRunning != item.running)
        {
            item.running = isRunning;
            needsRender_ = true;
        }
    }
}

void App::RefreshRunningApplications()
{
    const std::vector<RunningWindowInfo> windows =
        FindRunningTaskbarWindows();

    const HWND foreground = GetForegroundWindow();

    // First clear window lists on persistent launchers. They will be rebuilt
    // from the current taskbar-window snapshot below.
    for (auto& item : items_)
    {
        if (item->kind == DockItemKind::Pinned)
        {
            item->windows.clear();
            item->runtimeApplicationName.clear();
            item->runtimeAppUserModelId.clear();
        }
    }

    bool restoredPinnedIdentity = false;
    auto pinnedForWindow = [&](const RunningWindowInfo& window) -> DockItem*
    {
        // AUMID is the primary key for packaged apps: their EXE locations
        // change after MSIX upgrades, whereas the package identity does not.
        if (!window.appUserModelId.empty())
        {
            for (auto& item : items_)
            {
                if (item->kind == DockItemKind::Pinned
                    && !item->appUserModelId.empty()
                    && EqualsIgnoreCase(
                        item->appUserModelId, window.appUserModelId))
                {
                    return item.get();
                }
            }
        }

        const std::wstring id = MakeStableId(window.path);
        for (auto& item : items_)
        {
            if (item->kind != DockItemKind::Pinned)
            {
                continue;
            }

            const bool samePath = item->id == id
                || EqualsIgnoreCase(item->targetPath, window.path)
                || EqualsIgnoreCase(item->resolvedPath, window.path);
            const bool samePackageIdentity =
                !window.appUserModelId.empty()
                && AppLauncher::MatchesPackagedAppId(
                    item->resolvedPath, window.appUserModelId);

            if (!samePath && !samePackageIdentity)
            {
                continue;
            }

            // Distinct launchable apps can live inside the same package.
            if (samePackageIdentity && !item->appUserModelId.empty()
                && !EqualsIgnoreCase(
                    item->appUserModelId, window.appUserModelId))
            {
                continue;
            }

            if (samePackageIdentity && item->appUserModelId.empty())
            {
                item->appUserModelId = window.appUserModelId;
                restoredPinnedIdentity = true;
            }
            return item.get();
        }
        return nullptr;
    };

    struct Group
    {
        std::wstring path;
        std::wstring appUserModelId;
        std::wstring applicationName;
        std::vector<DockWindowEntry> windows;
    };

    std::vector<Group> groups;
    for (const RunningWindowInfo& window : windows)
    {
        DockWindowEntry entry;
        entry.hwnd = window.hwnd;
        entry.title = window.title;
        entry.minimized = IsIconic(window.hwnd) != FALSE;
        entry.active = window.hwnd == foreground;

        if (DockItem* pinned = pinnedForWindow(window))
        {
            if (!window.applicationName.empty())
            {
                pinned->runtimeApplicationName = window.applicationName;
            }
            if (!window.appUserModelId.empty())
            {
                pinned->runtimeAppUserModelId = window.appUserModelId;
            }
            pinned->windows.push_back(std::move(entry));
            continue;
        }

        auto group = std::find_if(
            groups.begin(), groups.end(),
            [&](const Group& candidate)
            {
                if (!window.appUserModelId.empty()
                    && !candidate.appUserModelId.empty())
                {
                    return EqualsIgnoreCase(
                        candidate.appUserModelId,
                        window.appUserModelId);
                }

                return EqualsIgnoreCase(candidate.path, window.path);
            });

        if (group == groups.end())
        {
            Group created;
            created.path = window.path;
            created.appUserModelId = window.appUserModelId;
            created.applicationName = window.applicationName;
            created.windows.push_back(std::move(entry));
            groups.push_back(std::move(created));
        }
        else
        {
            if (group->applicationName.empty()
                && !window.applicationName.empty())
            {
                group->applicationName = window.applicationName;
            }
            if (group->appUserModelId.empty()
                && !window.appUserModelId.empty())
            {
                group->appUserModelId = window.appUserModelId;
            }
            group->windows.push_back(std::move(entry));
        }
    }

    std::vector<std::unique_ptr<DockItem>> persistent;
    std::vector<std::unique_ptr<DockItem>> previousTransient;
    persistent.reserve(items_.size());
    previousTransient.reserve(items_.size());

    for (auto& item : items_)
    {
        if (item->kind == DockItemKind::RunningTransient)
        {
            previousTransient.push_back(std::move(item));
        }
        else
        {
            persistent.push_back(std::move(item));
        }
    }

    std::vector<bool> matched(groups.size(), false);
    std::vector<std::unique_ptr<DockItem>> nextTransient;
    nextTransient.reserve(groups.size());

    bool geometryChanged = false;
    bool contentChanged = false;

    // Preserve application order in the transient section. Packaged/UWP apps
    // are matched by AUMID first because several of them can share
    // ApplicationFrameHost.exe as the outer taskbar-window process.
    for (auto& item : previousTransient)
    {
        if (!item)
        {
            continue;
        }

        const std::wstring itemPath = item->resolvedPath.empty()
            ? item->targetPath : item->resolvedPath;

        size_t match = groups.size();
        for (size_t i = 0; i < groups.size(); ++i)
        {
            if (matched[i])
            {
                continue;
            }

            const bool aumidMatch =
                !item->runtimeAppUserModelId.empty()
                && !groups[i].appUserModelId.empty()
                && EqualsIgnoreCase(
                    item->runtimeAppUserModelId,
                    groups[i].appUserModelId);

            const bool pathMatch =
                EqualsIgnoreCase(itemPath, groups[i].path);

            if (aumidMatch || pathMatch)
            {
                match = i;
                break;
            }
        }

        if (match >= groups.size())
        {
            geometryChanged = true;
            continue;
        }

        matched[match] = true;
        Group& group = groups[match];

        const bool identityChanged =
            !EqualsIgnoreCase(
                item->runtimeAppUserModelId,
                group.appUserModelId);

        item->running = true;
        item->windows = group.windows;
        item->windowHandle = item->windows.empty()
            ? nullptr : item->windows.front().hwnd;
        item->windowTitle = item->windows.empty()
            ? std::wstring() : item->windows.front().title;
        item->runtimeAppUserModelId = group.appUserModelId;
        item->runtimeApplicationName = group.applicationName;

        if (!group.applicationName.empty())
        {
            item->name = group.applicationName;
        }

        const bool hostedShellApp =
            EqualsIgnoreCase(
                GetFileName(group.path),
                L"ApplicationFrameHost.exe");

        if (hostedShellApp
            && !item->windows.empty()
            && (identityChanged || !item->runtimeWindowIconLoaded))
        {
            // Only hosted Windows Shell/UWP frames need the taskbar HICON
            // workaround. Ordinary Win32 apps (DingTalk, Chrome, Adobe, etc.)
            // keep their high-resolution executable/resource icon instead of
            // being replaced by a 32/48 px window icon.
            if (ComPtr<IWICBitmap> windowIcon =
                    icons_.LoadWindowIcon(
                        item->windows.front().hwnd, 256))
            {
                item->iconSource = std::move(windowIcon);
                item->runtimeWindowIconLoaded = true;
                item->icon.Reset();
                item->iconBitmapScale = 0.0f;
                item->iconCornerFraction = -1.0f;
            }
            else if (identityChanged
                     && !group.appUserModelId.empty())
            {
                if (ComPtr<IWICBitmap> shellIcon =
                        icons_.LoadShellApplicationIcon(
                            group.appUserModelId, 256))
                {
                    item->iconSource = std::move(shellIcon);
                    item->runtimeWindowIconLoaded = false;
                    item->icon.Reset();
                    item->iconBitmapScale = 0.0f;
                    item->iconCornerFraction = -1.0f;
                }
            }
        }
        else if (!hostedShellApp)
        {
            item->runtimeWindowIconLoaded = false;

            // If this transient icon was created by an older build using a
            // low-resolution window HICON, restore the normal executable
            // resource icon on the next process refresh.
            if (identityChanged || !item->iconSource)
            {
                AppInfo refreshed = icons_.Inspect(group.path, 256);
                if (refreshed.icon)
                {
                    item->iconSource = refreshed.icon;
                    item->icon.Reset();
                    item->iconBitmapScale = 0.0f;
                    item->iconCornerFraction = -1.0f;
                }
            }
        }

        contentChanged = true;
        nextTransient.push_back(std::move(item));
    }

    for (size_t i = 0; i < groups.size(); ++i)
    {
        if (matched[i])
        {
            continue;
        }

        Group& group = groups[i];
        AppInfo info = icons_.Inspect(group.path, 256);

        auto item = std::make_unique<DockItem>();
        item->kind = DockItemKind::RunningTransient;

        const std::wstring identity = group.appUserModelId.empty()
            ? group.path : group.appUserModelId;
        item->id = L"__running_" + MakeStableId(identity);

        item->name = !group.applicationName.empty()
            ? group.applicationName
            : (info.name.empty()
                ? GetFileStem(group.path) : info.name);
        item->targetPath = group.path;
        item->resolvedPath =
            info.resolvedPath.empty() ? group.path : info.resolvedPath;
        item->processName = info.processName.empty()
            ? GetFileName(item->resolvedPath) : info.processName;

        const bool hostedShellApp =
            EqualsIgnoreCase(
                GetFileName(group.path),
                L"ApplicationFrameHost.exe");

        if (hostedShellApp && !group.windows.empty())
        {
            item->iconSource =
                icons_.LoadWindowIcon(
                    group.windows.front().hwnd, 256);
            item->runtimeWindowIconLoaded =
                static_cast<bool>(item->iconSource);
        }

        if (hostedShellApp
            && !item->iconSource
            && !group.appUserModelId.empty())
        {
            item->iconSource =
                icons_.LoadShellApplicationIcon(
                    group.appUserModelId, 256);
            item->runtimeWindowIconLoaded = false;
        }

        if (!item->iconSource)
        {
            // Normal EXE apps keep the original high-resolution extraction
            // path. Do not replace it with the low-resolution WM_GETICON copy.
            item->iconSource = info.icon;
            item->runtimeWindowIconLoaded = false;
        }

        item->running = true;
        item->windows = group.windows;
        item->windowHandle = item->windows.empty()
            ? nullptr : item->windows.front().hwnd;
        item->windowTitle = item->windows.empty()
            ? std::wstring() : item->windows.front().title;
        item->runtimeAppUserModelId = group.appUserModelId;
        item->runtimeApplicationName = group.applicationName;

        if (item->iconSource)
        {
            const bool circular =
                icons_.IsCircularIcon(item->iconSource.Get());
            item->plate.enabled = !circular;
            item->plate.iconScale = circular ? 0.0f : 0.85f;
        }

        item->scale = 1.0f;
        item->scaleSpring.Reset(1.0f);
        item->bounceSpring.Reset(0.0f);
        nextTransient.push_back(std::move(item));
        geometryChanged = true;
    }

    items_.clear();
    items_.reserve(persistent.size() + nextTransient.size());

    for (auto& item : persistent)
    {
        items_.push_back(std::move(item));
    }
    for (auto& item : nextTransient)
    {
        items_.push_back(std::move(item));
    }

    RebuildItemPointers();
    if (restoredPinnedIdentity)
    {
        SaveConfiguration();
    }

    if (geometryChanged)
    {
        pressedIndex_ = -1;
        // An external file drag keeps following its preview item.
        draggingIndex_ = externalDragActive_
            ? FindItemIndex(items_, kExternalDragPreviewId) : -1;
        if (externalDragActive_)
        {
            dragTargetIndex_ = std::clamp(
                dragTargetIndex_, 0,
                (std::max)(0, static_cast<int>(items_.size()) - 1));
            dragVisualTargetIndex_ = dragTargetIndex_;
        }
        UpdateSurfaceAndGeometry();
        RepositionWindow();
        WakeAnimation();
    }
    else
    {
        // Window titles, minimized state and the active window can all change
        // without changing the number of dock icons. Refresh once after each
        // 1 Hz window snapshot so an open hover stack never goes stale.
        (void)contentChanged;
        needsRender_ = true;
    }
}

void App::CheckPointer()
{
    if (exitRequested_)
    {
        mouseActive_ = false;
        return;
    }

    // TEMP-DIAG-BEGIN
    if (diagMouseX_ >= 0.0f)
    {
        mouseActive_ = true;
        mouseX_ = diagMouseX_;
        mouseY_ = geometry_.panelY + geometry_.panelHeight * 0.5f;
        return;
    }
    // TEMP-DIAG-END

    POINT cursor{};
    if (!GetCursorPos(&cursor))
    {
        return;
    }

    const RECT bounds = window_.GetBounds();
    const float physicalX = static_cast<float>(cursor.x - bounds.left);
    const float physicalY = static_cast<float>(cursor.y - bounds.top);
    mousePhysicalX_ = physicalX;
    mousePhysicalY_ = physicalY;

    const bool overWindowMenu = PointInWindowMenu(physicalX, physicalY);
    const int polledWindowRow =
        WindowMenuRowAt(physicalX, physicalY);
    if (windowMenuVisible_)
    {
        const int nextHoveredRow =
            overWindowMenu ? polledWindowRow : -1;
        if (nextHoveredRow != windowMenuHoveredRow_)
        {
            windowMenuHoveredRow_ = nextHoveredRow;
            windowPreview_.SetHoveredRow(nextHoveredRow);
            needsRender_ = true;
        }
    }

    const D2D1_POINT_2F logical = dockTransform_.ToLogical(physicalX, physicalY);
    const float x = logical.x;
    const float y = logical.y;

    const bool inside = HitTest(physicalX, physicalY);

    if (config_.settings.autoHide
        && shellPopupPlacementPending_
        && monitorIndex_ >= 0
        && monitorIndex_ < static_cast<int>(monitors_.size()))
    {
        const int taskbarInset = VisibleTaskbarInset(
            monitors_[static_cast<size_t>(monitorIndex_)].rect,
            config_.settings.dockEdge);

        if (taskbarInset != lastVisibleTaskbarInset_)
        {
            UpdateDockWindowPosition();
        }
    }

    if (!fullscreenActive_)
    {
        const float previousTarget = fullscreenVisibility_.target;
        const auto now = std::chrono::steady_clock::now();

        if (settingsWindowOpen_ || launcher_.Visible())
        {
            autoHideHidePending_ = false;
            SetFullscreenVisibilityTarget(1.0f);
        }
        else if (config_.settings.autoHide)
        {
            const RECT monitorRect = monitorIndex_ >= 0
                    && monitorIndex_ < static_cast<int>(monitors_.size())
                ? monitors_[static_cast<size_t>(monitorIndex_)].rect
                : RECT{0, 0, GetSystemMetrics(SM_CXSCREEN),
                       GetSystemMetrics(SM_CYSCREEN)};
            const LONG revealBand = (std::max)(2L, static_cast<LONG>(
                std::lround(4.0f * dockScale_)));
            const RECT dockBounds = window_.GetBounds();
            const bool verticalDock = config_.settings.dockEdge == DockEdge::Left
                || config_.settings.dockEdge == DockEdge::Right;
            const LONG dockSpanStart = static_cast<LONG>(std::lround(
                (verticalDock ? dockBounds.top : dockBounds.left)
                + frame_.panelX));
            const LONG dockSpanEnd = static_cast<LONG>(std::lround(
                (verticalDock ? dockBounds.top : dockBounds.left)
                + frame_.panelX + frame_.panelWidth));
            const bool alongDock = verticalDock
                ? cursor.y >= dockSpanStart && cursor.y < dockSpanEnd
                : cursor.x >= dockSpanStart && cursor.x < dockSpanEnd;
            bool atRevealEdge = false;
            switch (config_.settings.dockEdge)
            {
            case DockEdge::Top:
                atRevealEdge = alongDock
                    && cursor.y >= monitorRect.top
                    && cursor.y < monitorRect.top + revealBand;
                break;
            case DockEdge::Left:
                atRevealEdge = alongDock
                    && cursor.x >= monitorRect.left
                    && cursor.x < monitorRect.left + revealBand;
                break;
            case DockEdge::Right:
                atRevealEdge = alongDock
                    && cursor.x < monitorRect.right
                    && cursor.x >= monitorRect.right - revealBand;
                break;
            case DockEdge::Bottom:
            default:
                atRevealEdge = alongDock
                    && cursor.y >= monitorRect.bottom - revealBand
                    && cursor.y < monitorRect.bottom;
                break;
            }

            if (inside || atRevealEdge)
            {
                autoHideHidePending_ = false;
                SetFullscreenVisibilityTarget(1.0f);
            }
            else
            {
                if (!autoHideHidePending_)
                {
                    const int delay = (std::clamp)(
                        config_.settings.autoHideDelayMs, 0, 5000);
                    hideDeadline_ = now + std::chrono::milliseconds(delay);
                    autoHideHidePending_ = true;
                }

                if (now >= hideDeadline_)
                {
                    SetFullscreenVisibilityTarget(0.0f);
                    autoHideHidePending_ = false;
                }
            }
        }
        else
        {
            autoHideHidePending_ = false;
            SetFullscreenVisibilityTarget(1.0f);
        }

        if (fullscreenVisibility_.target != previousTarget)
        {
            WakeAnimation();
        }

        // Explorer's own auto-hidden taskbar can raise itself after our reveal
        // animation has already settled. While LightDock is intentionally
        // visible in overlay mode, periodically reassert its topmost band so
        // the system taskbar cannot cover it at the shared screen edge.
        if (config_.settings.autoHide
            && fullscreenVisibility_.target > 0.0f)
        {
            window_.EnsureTopmost();
        }
    }

    if (inside)
    {
        if (!mouseActive_)
        {
            mouseActive_ = true;
            if (!overWindowMenu)
            {
                mouseX_ = x;
                mouseY_ = y;
            }
            WakeAnimation();
        }
    }
    else if (mouseActive_)
    {
        mouseActive_ = false;
        WakeAnimation();
    }

    if (inside && !overWindowMenu)
    {
        mouseY_ = y;
    }
}

void App::WakeAnimation()
{
    animating_ = true;
    needsRender_ = true;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

bool App::HitTest(float x, float y)
{
    if (modal_ || fullscreenActive_ || exitRequested_)
    {
        return false;
    }

    // The hover window stack lives outside the panel itself but must remain
    // interactive so the pointer can travel from the icon into the menu.
    if (PointInWindowMenu(x, y))
    {
        return true;
    }

    const D2D1_POINT_2F logical = dockTransform_.ToLogical(x, y);
    x = logical.x;
    y = logical.y;

    // The dock only reacts over the icon row. The padding at either end and
    // the shadow margins stay transparent, so the magnified state releases
    // the moment the cursor leaves the icons instead of clinging to the
    // panel edges. The core bounds are widened by the fade band so that
    // release can be animated rather than cut.
    float coreLeft = 0.0f;
    float coreRight = 0.0f;
    float fadeDistance = 0.0f;
    RowInfluenceBounds(geometry_, items_.size(), metrics_,
                       coreLeft, coreRight, fadeDistance);

    // The gate must follow the row, not the resting slots: in Fixed mode the
    // panel widens and the icons spread into the extra space, and in Elastic
    // mode the row drifts sideways. Take the widest span of the two so the
    // detection region grows and shrinks with the panel.
    // In fixed mode the panel itself is the interaction surface. Do this
    // before the icon-row gate: the expanded side padding must remain live
    // while the pointer travels across it, otherwise WM_NCHITTEST switches
    // to HTTRANSPARENT and the panel collapses before the cursor reaches the
    // other icons.
    if (config_.settings.panelMode == PanelMode::Fixed
        && frame_.panelWidth > 0.0f)
    {
        const float left = frame_.panelX - metrics_.hitMargin;
        const float right = frame_.panelX + frame_.panelWidth
            + metrics_.hitMargin;
        const float top = geometry_.panelY - metrics_.hitMargin;
        const float bottom = geometry_.panelY + geometry_.panelHeight
            + metrics_.hitMargin;

        if (x >= left && x <= right && y >= top && y <= bottom)
        {
            return true;
        }
    }

    if (!items_.empty())
    {
        float left = items_.front()->x;
        float right = items_.front()->x + items_.front()->size;

        for (const auto& item : items_)
        {
            left = (std::min)(left, item->x);
            right = (std::max)(right, item->x + item->size);
        }

        coreLeft = (std::min)(coreLeft, left - metrics_.hitMargin);
        coreRight = (std::max)(coreRight, right + metrics_.hitMargin);
    }

    if (x < coreLeft - fadeDistance || x > coreRight + fadeDistance)
    {
        return false;
    }

    if (HitTestDock(x, y, frame_, geometry_, metrics_))
    {
        return true;
    }

    for (const auto& item : items_)
    {
        if (item->size <= 0.0f)
        {
            continue;
        }

        const float bottom = item->baselineBottom + item->bounceOffset;
        const float half = item->size * 0.5f;

        if (x >= item->centerX - half && x <= item->centerX + half
            && y >= bottom - item->size && y <= bottom)
        {
            return true;
        }
    }

    return false;
}

int App::IndexAtPoint(float x, float y) const
{
    for (size_t i = 0; i < items_.size(); ++i)
    {
        const DockItem& item = *items_[i];

        if (item.size <= 0.0f)
        {
            continue;
        }

        const float bottom = item.baselineBottom + item.bounceOffset;
        const float half = item.size * 0.5f;

        if (x >= item.centerX - half && x <= item.centerX + half
            && y >= bottom - item.size && y <= bottom)
        {
            return static_cast<int>(i);
        }
    }

    return -1;
}

bool App::PointInWindowMenu(float physicalX, float physicalY) const
{
    if (!windowMenuVisible_
        || windowMenuBounds_.right <= windowMenuBounds_.left
        || windowMenuBounds_.bottom <= windowMenuBounds_.top)
    {
        return false;
    }

    // Slightly inflate the interaction region so the 10 px visual gap
    // between icon and menu is a safe hover corridor rather than a dead zone.
    const float safe = 12.0f * dockScale_;
    return physicalX >= windowMenuBounds_.left - safe
        && physicalX <= windowMenuBounds_.right + safe
        && physicalY >= windowMenuBounds_.top - safe
        && physicalY <= windowMenuBounds_.bottom + safe;
}

int App::WindowMenuRowAt(float physicalX, float physicalY) const
{
    if (!windowMenuVisible_
        || physicalX < windowMenuBounds_.left
        || physicalX > windowMenuBounds_.right
        || physicalY < windowMenuBounds_.top
        || physicalY > windowMenuBounds_.bottom
        || windowMenuItemWidth_ <= 0.0f
        || windowMenuVisibleRows_ <= 0)
    {
        return -1;
    }

    const int row = static_cast<int>(
        (physicalX - windowMenuBounds_.left) / windowMenuItemWidth_);
    return row >= 0 && row < windowMenuVisibleRows_ ? row : -1;
}

void App::OnMouseMove(float x, float y)
{
    if (modal_)
    {
        return;
    }

    mousePhysicalX_ = x;
    mousePhysicalY_ = y;

    const bool overWindowMenu = PointInWindowMenu(x, y);
    const int windowRow = WindowMenuRowAt(x, y);
    if (overWindowMenu)
    {
        if (windowMenuHoveredRow_ != windowRow)
        {
            windowMenuHoveredRow_ = windowRow;
            windowPreview_.SetHoveredRow(windowRow);
            needsRender_ = true;
        }

        mouseActive_ = true;
        WakeAnimation();
        return;
    }

    if (windowMenuHoveredRow_ != -1)
    {
        windowMenuHoveredRow_ = -1;
        windowPreview_.SetHoveredRow(-1);
        needsRender_ = true;
    }

    const D2D1_POINT_2F logical = dockTransform_.ToLogical(x, y);
    x = logical.x;
    y = logical.y;
    mouseX_ = x;
    mouseY_ = y;

    // A short movement threshold keeps an ordinary click from becoming a
    // reorder operation. Measure *real screen-pointer travel*, not the
    // pointer's logical position inside the animated dock. Hover growth,
    // launch bounce and dock repositioning can all move the icon/window under
    // a stationary mouse; those visual motions must never start a drag.
    if (pressedIndex_ >= 0 && draggingIndex_ < 0)
    {
        POINT cursor{};
        if (!GetCursorPos(&cursor))
        {
            const RECT bounds = window_.GetBounds();
            cursor.x = bounds.left
                + static_cast<LONG>(std::lround(mousePhysicalX_));
            cursor.y = bounds.top
                + static_cast<LONG>(std::lround(mousePhysicalY_));
        }

        const float dx = static_cast<float>(
            cursor.x - dragStartScreen_.x);
        const float dy = static_cast<float>(
            cursor.y - dragStartScreen_.y);
        const float threshold = 8.0f * dockScale_;

        if (dx * dx + dy * dy >= threshold * threshold
            && pressedIndex_ < static_cast<int>(items_.size())
            && items_[static_cast<size_t>(pressedIndex_)]->kind
                == DockItemKind::Pinned)
        {
            draggingIndex_ = pressedIndex_;
            pressedIndex_ = -1;
            dragLift_ = 0.0f;
            dragPlaceholderAmount_ = 0.0f;
            dragTargetIndex_ = draggingIndex_;
            dragVisualTargetIndex_ = draggingIndex_;

            if (window_.Handle())
            {
                SetCapture(window_.Handle());
            }
        }
    }

    if (!mouseActive_)
    {
        mouseActive_ = true;
    }

    WakeAnimation();
}

void App::OnPreviewWindowActivated(int row)
{
    if (row < 0 || windowMenuItemId_.empty())
    {
        return;
    }

    const auto item = std::find_if(
        items_.begin(), items_.end(), [this](const auto& candidate)
        {
            return candidate->id == windowMenuItemId_;
        });

    if (item == items_.end()
        || row >= static_cast<int>((*item)->windows.size()))
    {
        return;
    }

    const HWND target =
        (*item)->windows[static_cast<size_t>(row)].hwnd;

    // Close the menu state first so the preview popup cannot immediately
    // reappear on the next render while the activation request is in flight.
    windowMenuVisible_ = false;
    windowMenuHoveredRow_ = -1;
    windowMenuPressedRow_ = -1;
    windowMenuLeaveElapsed_ = 0.0f;
    windowMenuBounds_ = D2D1::RectF(0, 0, 0, 0);
    windowPreview_.Hide();

    needsRender_ = true;
    WakeAnimation();

    AppLauncher::ActivateWindow(target);
}

void App::OnMouseButton(int button, bool down, float x, float y)
{
    if (modal_)
    {
        return;
    }

    mousePhysicalX_ = x;
    mousePhysicalY_ = y;

    const int windowRow = WindowMenuRowAt(x, y);
    if (button == 0 && down && windowRow >= 0)
    {
        windowMenuPressedRow_ = windowRow;
        if (window_.Handle())
        {
            SetCapture(window_.Handle());
        }
        return;
    }

    if (button == 0 && !down && windowMenuPressedRow_ >= 0)
    {
        const int pressedRow = windowMenuPressedRow_;
        windowMenuPressedRow_ = -1;

        if (GetCapture() == window_.Handle())
        {
            ReleaseCapture();
        }

        if (windowRow == pressedRow)
        {
            auto item = std::find_if(
                items_.begin(), items_.end(), [this](const auto& candidate)
                {
                    return candidate->id == windowMenuItemId_;
                });

            if (item != items_.end()
                && pressedRow < static_cast<int>((*item)->windows.size()))
            {
                AppLauncher::ActivateWindow(
                    (*item)->windows[static_cast<size_t>(pressedRow)].hwnd);
            }
        }

        windowMenuVisible_ = false;
        windowPreview_.Hide();
        windowMenuHoveredRow_ = -1;
        windowMenuLeaveElapsed_ = 0.0f;
        windowMenuBounds_ = D2D1::RectF(0, 0, 0, 0);
        needsRender_ = true;
        WakeAnimation();
        return;
    }

    const D2D1_POINT_2F logical = dockTransform_.ToLogical(x, y);
    x = logical.x;
    y = logical.y;

    if (button == 0 && down)
    {
        pressedIndex_ = IndexAtPoint(x, y);
        dragStartX_ = x;
        dragStartY_ = y;

        if (!GetCursorPos(&dragStartScreen_))
        {
            const RECT bounds = window_.GetBounds();
            dragStartScreen_.x = bounds.left
                + static_cast<LONG>(std::lround(mousePhysicalX_));
            dragStartScreen_.y = bounds.top
                + static_cast<LONG>(std::lround(mousePhysicalY_));
        }

        if (pressedIndex_ >= 0)
        {
            const DockItem& item = *items_[static_cast<size_t>(pressedIndex_)];
            dragGrabX_ = x - item.centerX;
            dragGrabY_ = y - (item.baselineBottom - item.size * 0.5f);
        }

        if (pressedIndex_ >= 0 && window_.Handle())
        {
            // Capture from the press, not only after the threshold, so a
            // quick drag can leave the icon or panel without losing the
            // eventual mouse-up event.
            SetCapture(window_.Handle());
        }

        return;
    }

    if (button == 0 && !down)
    {
        if (draggingIndex_ >= 0)
        {
            if (GetCapture() == window_.Handle())
            {
                ReleaseCapture();
            }

            FinishIconDrag(x, y);

            draggingIndex_ = -1;
            pressedIndex_ = -1;
            dragLift_ = 0.0f;
            dragPlaceholderAmount_ = 0.0f;
            dragTargetIndex_ = -1;
            dragVisualTargetIndex_ = -1;
            WakeAnimation();
            return;
        }

        const int index = IndexAtPoint(x, y);
        const int pressed = pressedIndex_;

        // End the press/capture state *before* launching or activating the
        // target. ShellExecute/foreground activation may re-enter the message
        // queue on some Windows paths; leaving pressedIndex_ alive during that
        // window let a synthetic/moved WM_MOUSEMOVE promote the click into a
        // drag while the launch bounce had already started.
        if (GetCapture() == window_.Handle())
        {
            ReleaseCapture();
        }

        pressedIndex_ = -1;
        dragLift_ = 0.0f;
        dragPlaceholderAmount_ = 0.0f;
        dragTargetIndex_ = -1;
        dragVisualTargetIndex_ = -1;

        if (index >= 0 && index == pressed)
        {
            LaunchApplication(static_cast<size_t>(index));
        }

        return;
    }

    if (button == 1 && !down)
    {
        ShowContextMenu(IndexAtPoint(x, y), x, y);
    }
}

void App::FinishIconDrag(float x, float y)
{
    const int from = draggingIndex_;
    if (from < 0 || from >= static_cast<int>(items_.size())
        || items_[static_cast<size_t>(from)]->kind
            != DockItemKind::Pinned)
    {
        return;
    }

    // A dragged icon released outside the dock is a request to remove it.
    // Ask every time, including an ordinary drag that simply leaves the
    // detection area, so an accidental flick cannot delete an entry.
    const D2D1_POINT_2F physical = dockTransform_.ToPhysical(x, y);
    if (!HitTest(physical.x, physical.y))
    {
        const std::wstring& name = items_[static_cast<size_t>(from)]->name;
        const std::wstring message = config_.settings.englishLanguage
            ? L"Remove \u201C" + name + L"\u201D from the Dock?"
            : L"确定要将“" + name + L"”移出 Dock 吗？";

        if (ShowAlert(UiText(L"移出 Dock"), message, L"移除", L"取消"))
        {
            RemoveApplication(static_cast<size_t>(from));
        }

        return;
    }

    // Count the centres to the left of the drop point, excluding the item
    // being dragged. This gives the insertion index directly for both
    // leftward and rightward moves.
    int firstPinned = -1;
    int pinnedCount = 0;
    int beforeDrop = 0;

    for (int i = 0; i < static_cast<int>(items_.size()); ++i)
    {
        if (items_[static_cast<size_t>(i)]->kind != DockItemKind::Pinned)
        {
            continue;
        }

        if (firstPinned < 0)
        {
            firstPinned = i;
        }

        if (i != from && x > items_[static_cast<size_t>(i)]->centerX)
        {
            ++beforeDrop;
        }
        ++pinnedCount;
    }

    if (firstPinned < 0 || pinnedCount <= 1)
    {
        return;
    }

    int target = firstPinned + beforeDrop;
    target = std::clamp(target, firstPinned,
                        firstPinned + pinnedCount - 1);

    if (target == from)
    {
        return;
    }

    auto moved = std::move(items_[static_cast<size_t>(from)]);
    items_.erase(items_.begin() + from);
    items_.insert(items_.begin() + target, std::move(moved));

    RebuildItemPointers();
    SaveConfiguration();
    UpdateSurfaceAndGeometry();
}

void App::OnDisplayConfigurationChanged()
{
    // Defer monitor enumeration / AppBar changes until after the window
    // message is dispatched. Polling will retry if Windows is still
    // rebuilding its display topology during an RDP transition.
    displayResyncPending_ = true;
    WakeAnimation();
}

void App::OnDpiChanged(int dpi)
{
    if (dpi <= 0)
    {
        return;
    }

    window_.SetDpi(dpi);
    displayResyncPending_ = true;
    RebuildMetrics();
    WakeAnimation();
}

void App::OnCommand(UINT id)
{
    HandleMenuCommand(id);
}

void App::OnTrayNotify(LPARAM lParam)
{
    switch (LOWORD(lParam))
    {
    case WM_RBUTTONUP:
    case WM_CONTEXTMENU:
        ShowTrayMenu();
        break;

    case WM_LBUTTONUP:
    default:
        break;
    }
}

void App::OnDropFiles(HDROP drop)
{
    if (!drop)
    {
        return;
    }

    const UINT count = DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0);

    if (!modal_)
    {
        for (UINT i = 0; i < count; ++i)
        {
            const UINT length = DragQueryFileW(drop, i, nullptr, 0);
            if (length == 0)
            {
                continue;
            }

            std::wstring path(length + 1, L'\0');
            DragQueryFileW(drop, i, path.data(), length + 1);
            path.resize(length);
            AddApplication(path);
        }
    }

    DragFinish(drop);
}

void App::OnExternalDragEnter(const std::vector<std::wstring>& paths,
                              float x, float y)
{
    if (modal_ || paths.empty())
    {
        return;
    }

    const D2D1_POINT_2F logical = dockTransform_.ToLogical(x, y);
    x = logical.x;
    y = logical.y;

    if (externalDragActive_)
    {
        FinishExternalDrag(false, x, y);
    }

    externalDragPaths_ = paths;

    const std::wstring& path = externalDragPaths_.front();
    AppInfo info = icons_.Inspect(path, 256);
    if (info.resolvedPath.empty())
    {
        info.resolvedPath = path;
    }

    if (info.name.empty())
    {
        info.name = GetFileStem(path);
    }

    auto preview = std::make_unique<DockItem>();
    preview->id = kExternalDragPreviewId;
    preview->name = info.name;
    preview->targetPath = path;
    preview->resolvedPath = info.resolvedPath;
    preview->arguments = info.arguments;
    preview->processName = info.processName;
    preview->iconSource = info.icon;
    if (preview->iconSource)
    {
        const bool circular = icons_.IsCircularIcon(preview->iconSource.Get());
        preview->plate.enabled = !circular;
        preview->plate.iconScale = circular ? 0.0f : 0.85f;
    }
    preview->scale = 1.0f;
    preview->scaleSpring.Reset(1.0f);
    preview->bounceSpring.Reset(0.0f);

    items_.push_back(std::move(preview));
    RebuildItemPointers();

    externalDragActive_ = true;
    lastExternalDragFrame_ = std::chrono::steady_clock::now();
    draggingIndex_ = static_cast<int>(items_.size()) - 1;
    pressedIndex_ = -1;
    dragLift_ = 0.0f;
    dragPlaceholderAmount_ = 0.0f;
    dragTargetIndex_ = draggingIndex_;
    dragVisualTargetIndex_ = draggingIndex_;
    dragGrabX_ = metrics_.iconSize * 0.5f;
    dragGrabY_ = metrics_.iconSize * 0.5f;
    mouseX_ = x;
    mouseY_ = y;
    mouseActive_ = true;
    UpdateSurfaceAndGeometry();
    SetTimer(window_.Handle(), 0x4C444B31u,
             static_cast<UINT>((std::max)(8, frameIntervalMs_)), nullptr);
    WakeAnimation();
    OnAnimationTimer();
}

void App::OnExternalDragMove(float x, float y)
{
    if (!externalDragActive_ || modal_)
    {
        return;
    }

    const D2D1_POINT_2F logical = dockTransform_.ToLogical(x, y);
    mouseX_ = logical.x;
    mouseY_ = logical.y;
    mouseActive_ = true;
    WakeAnimation();
    // Some OLE drag loops do not dispatch the dock window's timer messages
    // reliably. Advance immediately on every DragOver as a guaranteed
    // repaint path; the timer still fills the gaps when the pointer pauses.
    OnAnimationTimer();
}

void App::OnExternalDragLeave()
{
    if (externalDragActive_)
    {
        FinishExternalDrag(false, mouseX_, mouseY_);
    }
}

void App::OnExternalDrop(float x, float y)
{
    if (!externalDragActive_)
    {
        return;
    }

    OnExternalDragMove(x, y);
    FinishExternalDrag(true, mouseX_, mouseY_);
}

void App::FinishExternalDrag(bool commit, float x, float y)
{
    if (!externalDragActive_)
    {
        return;
    }

    mouseX_ = x;
    mouseY_ = y;

    // Find the preview by id, not by draggingIndex_: the 1 Hz running-app
    // refresh rebuilds items_ while a drag hovers the Dock, which used to
    // reset the index, leave the preview stuck in the Dock and eventually
    // save it to config.json as a real pin.
    int insertion = dragTargetIndex_;
    const int previewIndex = FindItemIndex(items_, kExternalDragPreviewId);
    if (previewIndex >= 0)
    {
        items_.erase(items_.begin() + previewIndex);
        RebuildItemPointers();
    }
    const int originalCount = static_cast<int>(items_.size());

    draggingIndex_ = -1;
    pressedIndex_ = -1;
    dragLift_ = 0.0f;
    dragPlaceholderAmount_ = 0.0f;
    dragTargetIndex_ = -1;
    dragVisualTargetIndex_ = -1;
    externalDragActive_ = false;
    KillTimer(window_.Handle(), 0x4C444B31u);

    if (commit && !externalDragPaths_.empty())
    {
        insertion = std::clamp(insertion, 0, originalCount);

        for (size_t pathIndex = 0;
             pathIndex < externalDragPaths_.size(); ++pathIndex)
        {
            const std::wstring path = externalDragPaths_[pathIndex];
            const std::wstring id = MakeStableId(path);
            AddApplication(path);

            auto found = std::find_if(
                items_.begin(), items_.end(),
                [&id](const std::unique_ptr<DockItem>& item)
                {
                    return item->id == id;
                });

            if (found != items_.end())
            {
                const int target = std::clamp(
                    insertion + static_cast<int>(pathIndex),
                    0, static_cast<int>(items_.size()) - 1);
                auto moved = std::move(*found);
                items_.erase(found);
                items_.insert(items_.begin() + target, std::move(moved));
                RebuildItemPointers();
            }
        }

        SaveConfiguration();
        UpdateSurfaceAndGeometry();
    }

    externalDragPaths_.clear();
    RebuildItemPointers();
    WakeAnimation();
}

void App::OnAnimationTimer()
{
    if (!externalDragActive_ || modal_ || !running_)
    {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(
        now - lastExternalDragFrame_).count();
    lastExternalDragFrame_ = now;
    dt = std::clamp(dt, 0.0, 0.05);

    if (animating_ || needsRender_)
    {
        needsRender_ = false;
        Tick(dt);
    }
}

HICON App::MakeTrayIcon(int size)
{
    if (size <= 0)
    {
        size = 16;
    }

    HDC screen = GetDC(nullptr);
    if (!screen)
    {
        return nullptr;
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(
        screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);

    HICON result = nullptr;

    if (color)
    {
        HDC dc = CreateCompatibleDC(screen);

        if (dc)
        {
            HGDIOBJ previous = SelectObject(dc, color);

            if (bits)
            {
                std::memset(bits, 0, static_cast<size_t>(size) * size * 4);
            }

            // Rounded dark tray plate.
            HBRUSH plate = CreateSolidBrush(RGB(26, 27, 33));
            HPEN outline = CreatePen(PS_SOLID, 1, RGB(96, 100, 112));

            HGDIOBJ previousBrush = SelectObject(dc, plate);
            HGDIOBJ previousPen = SelectObject(dc, outline);

            const int inset = (std::max)(1, size / 16);
            const int radius = (std::max)(2, size / 4);

            RoundRect(dc, inset, inset, size - inset, size - inset,
                      radius, radius);

            // Three "docks" pinned to the bottom of the plate.
            HBRUSH dotBrush = CreateSolidBrush(RGB(140, 190, 255));
            HGDIOBJ previousDot = SelectObject(dc, dotBrush);

            const int dotRadius = (std::max)(1, size / 9);
            const int centerY = size * 5 / 8;
            const int spacing = dotRadius * 3;

            for (int i = -1; i <= 1; ++i)
            {
                const int cx = size / 2 + i * spacing;
                Ellipse(dc, cx - dotRadius, centerY - dotRadius,
                        cx + dotRadius, centerY + dotRadius);
            }

            SelectObject(dc, previousDot);
            DeleteObject(dotBrush);

            SelectObject(dc, previousBrush);
            SelectObject(dc, previousPen);
            SelectObject(dc, previous);

            DeleteObject(plate);
            DeleteObject(outline);

            HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);

            if (mask)
            {
                ICONINFO iconInfo{};
                iconInfo.fIcon = TRUE;
                iconInfo.hbmMask = mask;
                iconInfo.hbmColor = color;

                result = CreateIconIndirect(&iconInfo);
                DeleteObject(mask);
            }

            DeleteDC(dc);
        }

        DeleteObject(color);
    }

    ReleaseDC(nullptr, screen);

    return result;
}

void App::OnDestroy()
{
    running_ = false;
    PostQuitMessage(0);
}

void App::OnCloseRequested()
{
    RequestExit();
}

void App::RequestExit()
{
    if (exitRequested_)
    {
        return;
    }

    exitRequested_ = true;
    initialAppBarRecoveryPending_ = false;
    // Return the reserved desktop strip before the exit animation; when
    // rebuilding, Explorer must not keep an allocation owned by the old Dock.
    window_.RemoveAppBarReservation();
    mouseActive_ = false;
    tooltipItemId_.clear();
    tooltipPresence_ = 0.0f;
    autoHideHidePending_ = false;
    SetFullscreenVisibilityTarget(0.0f);
    if (settings_.form && settings_.form->Handle())
    {
        PostMessageW(settings_.form->Handle(), WM_CLOSE, 0, 0);
    }
    if (editor_.form && editor_.form->Handle())
    {
        PostMessageW(editor_.form->Handle(), WM_CLOSE, 0, 0);
    }
    WakeAnimation();
}

const wchar_t* App::UiText(const wchar_t* chinese) const
{
    if (!chinese || !config_.settings.englishLanguage)
    {
        return chinese;
    }

    struct Translation { const wchar_t* zh; const wchar_t* en; };
    static constexpr Translation translations[] =
    {
        {L"打开", L"Open"},
        {L"开始", L"Start"},
        {L"搜索", L"Search"},
        {L"固定到 Dock", L"Pin to Dock"},
        {L"图标配置…", L"Icon settings..."},
        {L"图标配置 — ", L"Icon settings — "},
        {L"从 Dock 移除", L"Remove from Dock"},
        {L"添加应用", L"Add application"},
        {L"没有可添加的运行应用", L"No running apps to add"},
        {L"添加正在运行的应用", L"Add running applications"},
        {L"Dock 设置…", L"Dock settings..."},
        {L"退出", L"Quit"},
        {L"显示器", L"Display"},
        {L"Chinese", L"Chinese"},
        {L"English", L"English"},
        {L"行为", L"Behavior"},
        {L"背景栏", L"Dock background"},
        {L"图标", L"Icons"},
        {L"气泡", L"Tooltips"},
        {L"关于", L"About"},
        {L"背景栏模式", L"Panel mode"},
        {L"固定宽度（进入时展开）", L"Fixed width (expand on hover)"},
        {L"弹性跟随图标", L"Elastic (follows icons)"},
        {L"无动画（名称气泡正常）", L"No animation (tooltips enabled)"},
        {L"停靠位置", L"Dock position"},
        {L"底部", L"Bottom"},
        {L"顶部", L"Top"},
        {L"左侧（竖排）", L"Left (vertical)"},
        {L"右侧（竖排）", L"Right (vertical)"},
        {L"图标大小", L"Icon size"},
        {L"图标间距", L"Icon spacing"},
        {L"放大倍率", L"Magnification"},
        {L"自动隐藏/覆盖模式（不占用桌面下方空间）", L"Auto-hide/overlay (does not reserve screen space)"},
        {L"自动隐藏/覆盖模式", L"Auto-hide/overlay mode"},
        {L"收回延迟", L"Hide delay"},
        {L"自动隐藏动画时长", L"Auto-hide animation duration"},
        {L"整体大小", L"Overall size"},
        {L"背景栏外观", L"Dock background appearance"},
        {L"自定义背景渐变", L"Custom background gradient"},
        {L"背景栏不透明度", L"Background opacity"},
        {L"背景栏圆角", L"Background corner radius"},
        {L"顶部颜色", L"Top color"},
        {L"底部颜色", L"Bottom color"},
        {L"全局图标设置", L"Global icon settings"},
        {L"圆角半径", L"Corner radius"},
        {L"图标在底板内的比例", L"Icon scale inside plate"},
        {L"不透明度", L"Opacity"},
        {L"内描边粗细", L"Inner stroke width"},
        {L"描边不透明度", L"Stroke opacity"},
        {L"名称气泡", L"Name tooltip"},
        {L"气泡不透明度", L"Tooltip opacity"},
        {L"淡入淡出时长", L"Fade duration"},
        {L"气泡比例", L"Tooltip scale"},
        {L"气泡圆角", L"Tooltip corner radius"},
        {L"运行应用预览沿用这些设置", L"Running-app previews use these settings"},
        {L"保存", L"Save"},
        {L"取消", L"Cancel"},
        {L"固定宽度（进入时展开）", L"Fixed width (expand on hover)"},
        {L"弹性跟随图标", L"Elastic (follows icons)"},
        {L"无动画（名称气泡正常）", L"No animation (tooltips enabled)"},
        {L"底部", L"Bottom"},
        {L"顶部", L"Top"},
        {L"左侧（竖排）", L"Left (vertical)"},
        {L"右侧（竖排）", L"Right (vertical)"},
        {L"名称", L"Name"},
        {L"程序", L"Application"},
        {L"附加命令", L"Arguments"},
        {L"图标文件", L"Icon file"},
        {L"打开所在文件夹", L"Open file location"},
        {L"浏览…", L"Browse..."},
        {L"来自所选程序", L"From selected application"},
        {L"选择程序", L"Select application"},
        {L"选择图标", L"Select icon"},
        {L"作者：TerryXu 和 ChatGPT", L"Authors: TerryXu and ChatGPT"},
        {L"联系邮箱：851858419@.com", L"Contact: 851858419@.com"},
        {L"简介：LightDock 是一款轻量、可自定义的 Windows 桌面停靠栏，让常用应用触手可及。\n支持将停靠栏放置在屏幕的上、下、左、右侧，并可设置自动隐藏、展开动画和图标悬停效果。\n你还可以调整图标大小、间距、背景渐变、圆角和透明度，打造适合自己的桌面风格。", L"About: LightDock is a lightweight, customizable Windows desktop dock that keeps your favorite apps within easy reach.\nPlace it along any screen edge and tailor its auto-hide behavior, reveal animation, and icon hover effects.\nAdjust icon size and spacing, background gradients, corner radius, and opacity to make your desktop your own."},
        {L"语言", L"Language"},
        {L"Dock 设置", L"Dock settings"},
        {L"行为", L"Behavior"},
        {L"背景栏", L"Dock background"},
        {L"图标", L"Icons"},
        {L"气泡", L"Tooltips"},
        {L"无法启动该 Windows 打包应用。请确认应用仍已安装；若此程序来自旧版本，可先运行应用，再从 Dock 重新固定。",
         L"Could not activate this packaged Windows app. Check that it is still installed. Open it from Start, then pin it to the Dock again if needed."},
        {L"快捷方式", L"Shortcut"},
        {L"图标外观", L"Icon appearance"},
        {L"自动隐藏/覆盖", L"Auto-hide/overlay"},
        {L"自定义背景渐变", L"Custom background gradient"},
        {L"启用圆角底板", L"Enable rounded icon plate"},
        {L"自定义底板透明度", L"Custom plate opacity"},
        {L"底板透明度", L"Plate opacity"},
        {L"自定义第二颜色（渐变）", L"Custom second color (gradient)"},
        {L"自定义描边颜色", L"Custom stroke color"},
        {L"自定义描边透明度", L"Custom stroke opacity"},
        {L"浏览...", L"Browse..."},
        {L"恢复默认值", L"Restore defaults"},
        {L"自动隐藏/覆盖模式（不占用停靠边空间）", L"Auto-hide/overlay (does not reserve dock-edge space)"},
        {L"添加应用…", L"Add Application..."},
        {L"  · 主显示器", L"  · Primary"},
        {L"通用", L"General"},
        {L"自动隐藏", L"Auto-hide"},
        {L"不占用停靠边的屏幕空间", L"Doesn't reserve space along the docked edge"},
        {L"动画时长", L"Animation duration"},
        {L"外观", L"Appearance"},
        {L"圆角", L"Corner radius"},
        {L"颜色", L"Color"},
        {L"关闭渐变时，背景栏使用单一颜色。", L"With the gradient off, the dock background uses a single color."},
        {L"背景颜色", L"Background color"},
        {L"渐变", L"Gradient"},
        {L"渐变底部颜色", L"Gradient bottom color"},
        {L"渐变关闭", L"Gradient off"},
        {L"底板", L"Icon plate"},
        {L"这些是全部图标的默认值；可在图标右键菜单的“图标配置…”中单独调整。", L"Defaults for every icon. Fine-tune a single icon with \u201CIcon settings...\u201D in its context menu."},
        {L"图标比例", L"Icon scale"},
        {L"内描边", L"Inner stroke"},
        {L"粗细", L"Width"},
        {L"比例", L"Scale"},
        {L"作者", L"Authors"},
        {L"联系邮箱", L"Contact"},
        {L"轻量、可自定义的 Windows 桌面停靠栏", L"A lightweight, customizable dock for Windows"},
        {L"LightDock 让常用应用触手可及。停靠栏可以放在屏幕的上、下、左、右侧，"
         L"支持自动隐藏、展开动画和图标悬停放大；图标大小、间距、背景渐变、"
         L"圆角和透明度都可以按你的喜好调整。",
         L"LightDock keeps your favorite apps within easy reach. Place it along any screen edge, with auto-hide, reveal animation and hover magnification. Icon size, spacing, background gradient, corner radius and opacity can all be tuned to taste."},
        {L"应用", L"Application"},
        {L"自定义透明度", L"Custom opacity"},
        {L"透明度", L"Opacity"},
        {L"描边颜色", L"Stroke color"},
        {L"（自动）", L" (auto)"},
        {L"（默认）", L" (default)"},
        {L"无法打开应用", L"Couldn't open the app"},
        {L"移出 Dock", L"Remove from Dock"},
        {L"移除", L"Remove"},
        {L"好", L"OK"},
        {L"搜索应用", L"Search apps"},
        {L"所有应用", L"All apps"},
        {L"没有找到应用", L"No matching apps"},
        {L"打开系统开始菜单", L"Windows Start menu"},
        {L"以管理员身份运行", L"Run as administrator"},
        {L"锁定", L"Lock"},
        {L"注销", L"Sign out"},
        {L"重启", L"Restart"},
        {L"关机", L"Shut down"},
        {L"置顶", L"Pinned"},
        {L"取消置顶", L"Unpin"},
        {L"把应用拖到这里，或点击图标右上角的图钉来置顶", L"Drag apps here, or click the pin on an app's icon"},
        {L"设计创作", L"Design & Creative"},
        {L"开发工具", L"Developer Tools"},
        {L"办公效率", L"Productivity"},
        {L"AI 助手", L"AI Assistants"},
        {L"通讯社交", L"Communication"},
        {L"影音娱乐", L"Media & Entertainment"},
        {L"游戏", L"Games"},
        {L"网络与远程", L"Internet & Remote"},
        {L"系统工具", L"System Tools"},
        {L"其他", L"Other"},
        {L"已隐藏", L"Hidden"},
        {L"隐藏", L"Hide"},
        {L"取消隐藏", L"Unhide"},
        {L"设置", L"Settings"},
        {L"电源", L"Power"},
    };

    for (const Translation& entry : translations)
    {
        if (wcscmp(chinese, entry.zh) == 0)
        {
            return entry.en;
        }
    }
    return chinese;
}

namespace
{

ComPtr<IWICBitmapSource> AsBitmapSource(IWICBitmap* bitmap)
{
    if (bitmap)
    {
        bitmap->AddRef();
    }
    return ComPtr<IWICBitmapSource>(bitmap);
}

} // namespace

UINT App::RunMenu(const std::vector<ui::MenuItem>& items, POINT anchor,
                  ui::MenuOptions options)
{
    // Keep the dock's frame clock running while the menu owns the message
    // loop, so hover/launch animations finish smoothly underneath it.
    auto lastFrame = std::chrono::steady_clock::now();
    options.idle = [this, &lastFrame]()
    {
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::min(
            std::chrono::duration<double>(now - lastFrame).count(), 0.05);
        lastFrame = now;
        if (animating_ || needsRender_)
        {
            needsRender_ = false;
            Tick(dt);
        }
    };

    SetForegroundWindow(window_.Handle());
    return ui::TrackMenu(window_.Handle(), items, anchor, options);
}

void App::ShowContextMenu(int index, float x, float y)
{
    using ui::MenuItem;
    std::vector<MenuItem> menu;
    menuRunningCandidates_.clear();

    if (index >= 0)
    {
        const DockItem& item = *items_[static_cast<size_t>(index)];

        if (item.kind == DockItemKind::Pinned)
        {
            menu.push_back(MenuItem::Command(kMenuOpen, UiText(L"打开")));
            menu.push_back(MenuItem::Separator());
            menu.push_back(MenuItem::Command(kMenuOpenFolder,
                                             UiText(L"打开所在文件夹")));
            menu.push_back(MenuItem::Command(kMenuEdit, UiText(L"图标配置…")));
            menu.push_back(MenuItem::Separator());
            menu.push_back(MenuItem::Command(kMenuRemove,
                                             UiText(L"从 Dock 移除")));
        }
        else if (item.kind == DockItemKind::RunningTransient)
        {
            menu.push_back(MenuItem::Command(kMenuOpen, UiText(L"打开")));
            menu.push_back(MenuItem::Separator());
            menu.push_back(MenuItem::Command(kMenuOpenFolder,
                                             UiText(L"打开所在文件夹")));
            menu.push_back(MenuItem::Command(kMenuPinRunning,
                                             UiText(L"固定到 Dock")));
        }
        else
        {
            menu.push_back(MenuItem::Command(kMenuOpen, UiText(L"打开")));
        }
    }
    else
    {
        menu.push_back(MenuItem::Command(kMenuAdd, UiText(L"添加应用…")));

        std::vector<MenuItem> running;
        menuRunningCandidates_ = FindRunningTaskbarApplications();
        for (size_t i = 0; i < menuRunningCandidates_.size(); ++i)
        {
            const std::wstring& path = menuRunningCandidates_[i];
            AppInfo info = icons_.Inspect(path, 32);
            std::wstring label = info.name.empty()
                ? GetShellDisplayName(path) : info.name;
            if (label.empty())
            {
                label = path;
            }
            MenuItem entry = MenuItem::Command(
                kMenuAddRunningBase + static_cast<UINT>(i), label);
            entry.icon = AsBitmapSource(info.icon.Get());
            running.push_back(std::move(entry));
        }
        if (running.empty())
        {
            running.push_back(MenuItem::Command(
                kMenuAddRunningBase, UiText(L"没有可添加的运行应用"), false));
        }
        menu.push_back(MenuItem::Submenu(UiText(L"添加正在运行的应用"),
                                         std::move(running)));
        menu.push_back(MenuItem::Separator());
        menu.push_back(MenuItem::Command(kMenuSettings, UiText(L"Dock 设置…")));
    }

    menuIndex_ = index;

    const RECT bounds = window_.GetBounds();
    const D2D1_POINT_2F physical = dockTransform_.ToPhysical(x, y);
    POINT screen{};
    screen.x = bounds.left + static_cast<LONG>(std::lround(physical.x));
    screen.y = bounds.top + static_cast<LONG>(std::lround(physical.y));

    const UINT command = RunMenu(menu, screen);
    if (command != 0)
    {
        HandleMenuCommand(command);
    }
    menuRunningCandidates_.clear();
}

void App::ShowTrayMenu()
{
    using ui::MenuItem;

    // Tray menu is the "home" for global actions: dock settings, picking a
    // display, language and quitting. Adding apps lives in the dock
    // blank-area menu and drag & drop.
    std::vector<MenuItem> menu;
    menu.push_back(MenuItem::Command(kMenuSettings, UiText(L"Dock 设置…")));
    AppendMonitorMenu(menu);
    menu.push_back(MenuItem::Submenu(UiText(L"语言"),
        {
            MenuItem::Command(kMenuLanguageChinese, L"简体中文", true,
                              !config_.settings.englishLanguage),
            MenuItem::Command(kMenuLanguageEnglish, L"English", true,
                              config_.settings.englishLanguage),
        }));
    menu.push_back(MenuItem::Separator());
    menu.push_back(MenuItem::Command(kMenuAbout, UiText(L"关于")));
    menu.push_back(MenuItem::Separator());
    menu.push_back(MenuItem::Command(kMenuExit, UiText(L"退出")));

    POINT cursor{};
    GetCursorPos(&cursor);

    ui::MenuOptions options;
    options.alignRight = true;
    options.alignBottom = true;
    const UINT command = RunMenu(menu, cursor, options);
    if (command != 0)
    {
        HandleMenuCommand(command);
    }
}

void App::HandleMenuCommand(UINT id)
{
    if (id >= kMenuAddRunningBase
        && id < kMenuMonitorBase)
    {
        AddRunningApplication(static_cast<size_t>(id - kMenuAddRunningBase));
        menuRunningCandidates_.clear();
        menuIndex_ = -1;
        return;
    }

    switch (id)
    {
    case kMenuOpen:
        if (menuIndex_ >= 0)
        {
            LaunchApplication(static_cast<size_t>(menuIndex_));
        }

        break;

    case kMenuOpenFolder:
        if (menuIndex_ >= 0)
        {
            const DockItem& item =
                *items_[static_cast<size_t>(menuIndex_)];
            const std::wstring& path = item.resolvedPath.empty()
                ? item.targetPath : item.resolvedPath;
            AppLauncher::RevealInExplorer(path);
        }

        break;

    case kMenuPinRunning:
        if (menuIndex_ >= 0
            && menuIndex_ < static_cast<int>(items_.size()))
        {
            const DockItem& item =
                *items_[static_cast<size_t>(menuIndex_)];
            if (item.kind == DockItemKind::RunningTransient)
            {
                const std::wstring path = item.resolvedPath.empty()
                    ? item.targetPath : item.resolvedPath;
                AddApplication(path, item.runtimeAppUserModelId);
            }
        }

        break;

    case kMenuRemove:
        if (menuIndex_ >= 0)
        {
            RemoveApplication(static_cast<size_t>(menuIndex_));
        }

        break;

    case kMenuEdit:
        if (menuIndex_ >= 0
            && EditItem(static_cast<size_t>(menuIndex_)))
        {
            SaveConfiguration();
            UpdateSurfaceAndGeometry();
            WakeAnimation();
        }

        break;

    case kMenuSettings:
        if (ShowSettings())
        {
            ApplySettings();
        }

        break;

    case kMenuLanguageChinese:
    case kMenuLanguageEnglish:
        config_.settings.englishLanguage = id == kMenuLanguageEnglish;
        for (auto& item : items_)
        {
            if (item->kind == DockItemKind::StartButton)
            {
                item->name = UiText(L"开始");
            }
            else if (item->kind == DockItemKind::SearchButton)
            {
                item->name = UiText(L"搜索");
            }
        }
        SaveConfiguration();
        needsRender_ = true;
        break;

    case kMenuAbout:
        // "About" is the last page of the settings window.
        if (ShowSettings(kSettingsAboutPage))
        {
            ApplySettings();
        }

        break;

    case kMenuAdd:
    {
        const std::vector<std::wstring> paths = PickApplicationFiles();
        for (const std::wstring& path : paths)
        {
            AddApplication(path);
        }

        break;
    }

    case kMenuExit:
        if (window_.Handle())
        {
            PostMessageW(window_.Handle(), WM_CLOSE, 0, 0);
        }

        break;

    default:
        if (id >= kMenuMonitorBase)
        {
            SelectMonitor(static_cast<int>(id - kMenuMonitorBase));
        }

        break;
    }

    menuIndex_ = -1;
}

namespace
{

struct TaskbarWindowRecord
{
    HWND hwnd = nullptr;
    std::wstring path;
    std::wstring title;
};

struct TaskbarWindowContext
{
    HWND dock = nullptr;
    std::vector<TaskbarWindowRecord> windows;
};

bool IsShellUiProcess(const std::wstring& path)
{
    const std::wstring name = GetFileName(path);
    return EqualsIgnoreCase(name, L"SearchHost.exe")
        || EqualsIgnoreCase(name, L"SearchApp.exe")
        || EqualsIgnoreCase(name, L"StartMenuExperienceHost.exe")
        || EqualsIgnoreCase(name, L"ShellExperienceHost.exe")
        || EqualsIgnoreCase(name, L"TextInputHost.exe")
        || EqualsIgnoreCase(name, L"LockApp.exe");
}

BOOL CALLBACK CollectTaskbarWindow(HWND hwnd, LPARAM parameter)
{
    auto* context = reinterpret_cast<TaskbarWindowContext*>(parameter);
    if (!context || !IsWindowVisible(hwnd) || hwnd == context->dock)
    {
        return TRUE;
    }

    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    const HWND owner = GetWindow(hwnd, GW_OWNER);

    // Match normal taskbar eligibility more closely: owned windows are
    // normally secondary/tool surfaces unless WS_EX_APPWINDOW explicitly
    // asks for taskbar presence.
    if (owner != nullptr && (exStyle & WS_EX_APPWINDOW) == 0)
    {
        return TRUE;
    }
    if ((exStyle & WS_EX_TOOLWINDOW) != 0
        && (exStyle & WS_EX_APPWINDOW) == 0)
    {
        return TRUE;
    }

    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(
            hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))
        && cloaked != 0)
    {
        return TRUE;
    }

    wchar_t title[512]{};
    const int titleLength = GetWindowTextW(hwnd, title, ARRAYSIZE(title));
    if (titleLength <= 0)
    {
        return TRUE;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId == 0 || processId == GetCurrentProcessId())
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

    if (!gotPath || pathLength == 0)
    {
        return TRUE;
    }

    std::wstring candidate(path, pathLength);
    if (IsShellUiProcess(candidate))
    {
        return TRUE;
    }

    TaskbarWindowRecord record;
    record.hwnd = hwnd;
    record.path = std::move(candidate);
    record.title.assign(title, static_cast<size_t>(titleLength));
    context->windows.push_back(std::move(record));
    return TRUE;
}

} // namespace

std::vector<App::RunningWindowInfo> App::FindRunningTaskbarWindows() const
{
    TaskbarWindowContext context;
    context.dock = window_.Handle();
    EnumWindows(CollectTaskbarWindow, reinterpret_cast<LPARAM>(&context));

    std::vector<RunningWindowInfo> result;
    result.reserve(context.windows.size());

    for (auto& record : context.windows)
    {
        RunningWindowInfo info;
        info.hwnd = record.hwnd;
        info.path = std::move(record.path);
        info.title = std::move(record.title);

        info.appUserModelId =
            ApplicationUserModelIdFromWindow(info.hwnd);
        if (!info.appUserModelId.empty())
        {
            info.applicationName =
                LocalizedApplicationNameFromAumid(
                    info.appUserModelId);
        }
        if (info.applicationName.empty())
        {
            info.applicationName =
                LocalizedApplicationNameFromWindow(info.hwnd);
        }

        result.push_back(std::move(info));
    }

    return result;
}


std::vector<std::wstring> App::FindRunningTaskbarApplications() const
{
    const std::vector<RunningWindowInfo> windows =
        FindRunningTaskbarWindows();

    std::vector<std::wstring> filtered;
    for (const RunningWindowInfo& window : windows)
    {
        const std::wstring id = MakeStableId(window.path);
        const bool alreadyPinned = std::any_of(
            items_.begin(), items_.end(), [&](const auto& item)
            {
                return item->kind == DockItemKind::Pinned
                    && ((item->appUserModelId.empty()
                            && (item->id == id
                                || EqualsIgnoreCase(
                                    item->targetPath, window.path)
                                || EqualsIgnoreCase(
                                    item->resolvedPath, window.path)))
                        || (!window.appUserModelId.empty()
                            && EqualsIgnoreCase(
                                item->appUserModelId, window.appUserModelId))
                        || (item->appUserModelId.empty()
                            && !window.appUserModelId.empty()
                            && AppLauncher::MatchesPackagedAppId(
                                item->resolvedPath, window.appUserModelId)));
            });

        if (alreadyPinned)
        {
            continue;
        }

        const bool alreadyListed = std::any_of(
            filtered.begin(), filtered.end(),
            [&](const std::wstring& path)
            {
                return EqualsIgnoreCase(path, window.path);
            });

        if (!alreadyListed)
        {
            filtered.push_back(window.path);
        }
    }

    return filtered;
}

void App::AddRunningApplication(size_t index)
{
    if (index < menuRunningCandidates_.size())
    {
        AddApplication(menuRunningCandidates_[index]);
    }
}

std::wstring App::PickFile(const wchar_t* title,
                            const COMDLG_FILTERSPEC* filters,
                            UINT filterCount)
{
    const std::vector<std::wstring> paths =
        PickFiles(title, filters, filterCount, false);
    return paths.empty() ? std::wstring() : paths.front();
}

std::vector<std::wstring> App::PickFiles(
    const wchar_t* title, const COMDLG_FILTERSPEC* filters,
    UINT filterCount, bool allowMultiSelect)
{
    std::vector<std::wstring> paths;
    ComPtr<IFileOpenDialog> dialog;

    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(dialog.AddressOf()))))
    {
        return paths;
    }

    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options)))
    {
        dialog->SetOptions(options
                           | FOS_FORCEFILESYSTEM
                           | FOS_FILEMUSTEXIST
                           | FOS_PATHMUSTEXIST
                           | FOS_NOCHANGEDIR
                           | FOS_DONTADDTORECENT
                           | (allowMultiSelect
                                  ? static_cast<DWORD>(FOS_ALLOWMULTISELECT)
                                  : 0u));
    }

    dialog->SetFileTypes(filterCount, filters);
    dialog->SetFileTypeIndex(1);
    dialog->SetTitle(title);

    if (FAILED(dialog->Show(nullptr)))
    {
        return paths;
    }

    ComPtr<IShellItemArray> results;
    if (FAILED(dialog->GetResults(results.AddressOf())))
    {
        return paths;
    }

    DWORD count = 0;
    if (FAILED(results->GetCount(&count)))
    {
        return paths;
    }

    paths.reserve(count);
    for (DWORD i = 0; i < count; ++i)
    {
        ComPtr<IShellItem> result;
        if (FAILED(results->GetItemAt(i, result.AddressOf())))
        {
            continue;
        }

        wchar_t* path = nullptr;
        if (FAILED(result->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path)
        {
            continue;
        }

        paths.emplace_back(path);
        CoTaskMemFree(path);
    }

    return paths;
}

std::vector<std::wstring> App::PickApplicationFiles()
{
    const COMDLG_FILTERSPEC filters[] =
    {
        {L"应用程序 (*.exe; *.lnk)", L"*.exe;*.lnk"},
        {L"所有文件 (*.*)", L"*.*"},
    };

    return PickFiles(L"添加应用到 LightDock", filters,
                     ARRAYSIZE(filters), true);
}

// ---------------------------------------------------------------------------
// Dialogs (built on ui::Form, see MacUi.h)
// ---------------------------------------------------------------------------

namespace
{

std::wstring HexOf(COLORREF color)
{
    wchar_t text[16];
    swprintf(text, 16, L"#%02X%02X%02X",
             GetRValue(color), GetGValue(color), GetBValue(color));
    return std::wstring(text);
}

COLORREF ColorRefOf(const std::wstring& hex, COLORREF fallback)
{
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
    if (ParseHexColor(hex, r, g, b, a))
    {
        return RGB(static_cast<int>(r * 255.0f + 0.5f),
                   static_cast<int>(g * 255.0f + 0.5f),
                   static_cast<int>(b * 255.0f + 0.5f));
    }
    return fallback;
}

/// The automatically derived gradient partner of a picked plate colour.
COLORREF DerivedColor(COLORREF top)
{
    return RGB(GetRValue(top) * 92 / 100,
               GetGValue(top) * 92 / 100,
               GetBValue(top) * 92 / 100);
}

int Percent(float value)
{
    return static_cast<int>(std::lround(value * 100.0f));
}

std::wstring FormatPercent(int value)
{
    return std::to_wstring(value) + L"%";
}

std::wstring FormatPixels(int value)
{
    return std::to_wstring(value) + L" px";
}

std::wstring FormatMillis(int value)
{
    return std::to_wstring(value) + L" ms";
}

enum : int
{
    kButtonSave = 1,
    kButtonCancel = 2,
    kButtonRestore = 3,
};

} // namespace

HICON App::AppIconHandle()
{
    if (!appIcon_)
    {
        appIcon_ = static_cast<HICON>(LoadImageW(
            instance_, L"IDI_LIGHTDOCK", IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    }
    return appIcon_;
}

IWICBitmapSource* App::AppIconBitmap()
{
    if (!appIconBitmap_ && icons_.Factory())
    {
        HICON icon = static_cast<HICON>(LoadImageW(
            instance_, L"IDI_LIGHTDOCK", IMAGE_ICON, 256, 256, 0));
        if (icon)
        {
            icons_.Factory()->CreateBitmapFromHICON(icon, &appIconBitmap_);
            DestroyIcon(icon);
        }
    }
    return appIconBitmap_.Get();
}

void App::RunModal(HWND window, const bool& closed)
{
    // Keep the dock non-interactive, but continue its frame clock while the
    // dialog owns the nested message loop. This lets the dock glide closed
    // instead of freezing in its hovered state.
    modal_ = true;
    mouseActive_ = false;
    WakeAnimation();

    MSG message{};
    auto lastModalFrame = std::chrono::steady_clock::now();
    while (!closed && running_ && IsWindow(window))
    {
        MsgWaitForMultipleObjectsEx(
            0, nullptr, static_cast<DWORD>(frameIntervalMs_),
            QS_ALLINPUT, MWMO_INPUTAVAILABLE);

        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT)
            {
                running_ = false;
                break;
            }

            // ui::Form handles its own keyboard navigation.
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        if (!running_)
        {
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        const double dt = std::min(
            std::chrono::duration<double>(now - lastModalFrame).count(),
            0.05);
        lastModalFrame = now;

        // Continue recovering from RDP/display transitions even while a
        // settings or item-editor modal loop is open.
        if (displayResyncPending_
            || now - lastDisplayCheck_ >= std::chrono::seconds(1))
        {
            lastDisplayCheck_ = now;
            CheckDisplayChange();
        }

        if (animating_ || needsRender_)
        {
            needsRender_ = false;
            Tick(dt);
        }
    }

    modal_ = false;
    needsRender_ = true;
}

void App::ApplySettings()
{
    SaveConfiguration();
    RebuildMetrics();
    WakeAnimation();
}

bool App::ShowAlert(const std::wstring& title, const std::wstring& message,
                    const wchar_t* confirm, const wchar_t* cancel)
{
    ui::Form form;
    bool closed = false;
    bool confirmed = false;

    form.AddSection();
    form.AddHeader([this]() { return AppIconBitmap(); },
                   [title]() { return title; }, {}, 44.0f);
    form.AddParagraph(message);
    if (cancel)
    {
        form.AddButton(kButtonCancel, UiText(cancel));
    }
    form.AddButton(kButtonSave, UiText(confirm), ui::ButtonRole::Primary);

    form.onButton = [&](int id)
    {
        confirmed = id == kButtonSave;
        closed = true;
    };
    form.onClose = [&]() { closed = true; };

    ui::Form::Options options;
    options.owner = window_.Handle();
    options.instance = instance_;
    options.title = L"LightDock";
    options.width = 420.0f;
    options.height = 0.0f;
    options.center = CurrentWorkArea();

    if (!form.Create(options))
    {
        const int answer = MessageBoxW(
            window_.Handle(), message.c_str(), title.c_str(),
            cancel ? (MB_OKCANCEL | MB_ICONQUESTION) : MB_OK);
        return answer == IDOK;
    }

    RunModal(form.Handle(), closed);
    form.Destroy();
    return confirmed;
}

// --- Dock settings ------------------------------------------------------------

void App::LoadSettingsModel(const DockSettings& s)
{
    SettingsEditor& m = settings_;
    m.mode = s.panelMode == PanelMode::Fixed ? 0
        : s.panelMode == PanelMode::Elastic ? 1 : 2;
    m.edge = s.dockEdge == DockEdge::Top ? 1
        : s.dockEdge == DockEdge::Left ? 2
        : s.dockEdge == DockEdge::Right ? 3 : 0;
    m.overallScale = Percent(s.overallScale);
    m.spacing = s.iconSpacing;
    m.magnify = static_cast<int>(std::lround(s.magnification * 10.0f));
    m.autoHide = s.autoHide;
    m.autoHideDelay = s.autoHideDelayMs;
    m.autoHideSpeed = s.autoHideAnimationMs;

    m.backgroundOpacity = Percent(s.backgroundOpacity);
    m.dockCorner = static_cast<int>(std::lround(s.cornerRadius));
    m.gradient = !s.backgroundBottom.empty();
    m.topColor = ColorRefOf(s.backgroundTop, RGB(255, 255, 255));
    m.bottomColor = ColorRefOf(s.backgroundBottom, RGB(255, 255, 255));

    m.plateCorner = static_cast<int>(std::lround(s.backdrop.cornerRadius));
    m.plateScale = Percent(s.backdrop.iconScale);
    m.plateOpacity = Percent(s.backdrop.opacity);
    m.strokeWidth = static_cast<int>(std::lround(s.backdrop.strokeWidth * 10.0f));
    m.strokeOpacity = Percent(s.backdrop.strokeOpacity);

    m.tooltipOpacity = Percent(s.tooltipOpacity);
    m.tooltipFade = static_cast<int>(std::lround(s.tooltipFadeSeconds * 1000.0f));
    m.tooltipScale = Percent(s.tooltipScale);
    m.tooltipCorner = static_cast<int>(std::lround(s.tooltipCornerRadius));
}

void App::PreviewSettings()
{
    const SettingsEditor& m = settings_;
    DockSettings& s = config_.settings;

    s.panelMode = m.mode == 1 ? PanelMode::Elastic
        : m.mode == 2 ? PanelMode::Static : PanelMode::Fixed;
    s.dockEdge = m.edge == 1 ? DockEdge::Top
        : m.edge == 2 ? DockEdge::Left
        : m.edge == 3 ? DockEdge::Right : DockEdge::Bottom;

    // Keep the legacy base icon size fixed. Overall size is the only
    // dock-wide sizing control exposed to users.
    s.iconSize = 50;
    s.iconSpacing = m.spacing;
    s.overallScale = static_cast<float>(m.overallScale) / 100.0f;
    s.autoHide = m.autoHide;
    s.autoHideDelayMs = m.autoHideDelay;
    s.autoHideAnimationMs = m.autoHideSpeed;
    s.magnification = static_cast<float>(m.magnify) / 10.0f;
    s.cornerRadius = static_cast<float>(m.dockCorner);
    s.tooltipScale = static_cast<float>(m.tooltipScale) / 100.0f;
    s.tooltipCornerRadius = static_cast<float>(m.tooltipCorner);

    IconBackdrop& backdrop = s.backdrop;
    backdrop.cornerRadius = static_cast<float>(m.plateCorner);
    backdrop.iconScale = static_cast<float>(m.plateScale) / 100.0f;
    backdrop.opacity = static_cast<float>(m.plateOpacity) / 100.0f;
    backdrop.strokeWidth = static_cast<float>(m.strokeWidth) / 10.0f;
    backdrop.strokeOpacity = static_cast<float>(m.strokeOpacity) / 100.0f;

    // The first colour is the solid panel colour even when the gradient
    // switch is off. Only the second stop is cleared in solid mode.
    s.backgroundTop = HexOf(m.topColor);
    s.backgroundBottom = m.gradient ? HexOf(m.bottomColor) : std::wstring();
    s.backgroundOpacity = static_cast<float>(m.backgroundOpacity) / 100.0f;
    s.tooltipOpacity = static_cast<float>(m.tooltipOpacity) / 100.0f;
    s.tooltipFadeSeconds = static_cast<float>(m.tooltipFade) / 1000.0f;

    RebuildMetrics();
    CheckFullscreen();

    // The settings window runs its own modal loop; paint synchronously so
    // every change shows up on the dock immediately.
    Render();
    WakeAnimation();
}

bool App::ShowSettings(int initialPage)
{
    settings_ = SettingsEditor{};
    settings_.original = config_.settings;
    LoadSettingsModel(config_.settings);

    settings_.form = std::make_unique<ui::Form>();
    ui::Form& form = *settings_.form;
    SettingsEditor& m = settings_;

    // --- General ---------------------------------------------------------
    form.AddPage(UiText(L"通用"), 0xE713, D2D1::ColorF(0x8E8E93));
    form.AddSection(L"Dock");
    form.AddChoice(UiText(L"停靠位置"), &m.edge,
                   {UiText(L"底部"), UiText(L"顶部"),
                    UiText(L"左侧（竖排）"), UiText(L"右侧（竖排）")});
    form.AddChoice(UiText(L"背景栏模式"), &m.mode,
                   {UiText(L"固定宽度（进入时展开）"),
                    UiText(L"弹性跟随图标"),
                    UiText(L"无动画（名称气泡正常）")});
    form.AddSlider(UiText(L"整体大小"), &m.overallScale, 50, 150, FormatPercent);
    form.AddSlider(UiText(L"图标间距"), &m.spacing, 0, 32, FormatPixels);
    form.AddSlider(UiText(L"放大倍率"), &m.magnify, 10, 25, [](int value)
    {
        wchar_t text[16];
        swprintf(text, 16, L"%.1f×", static_cast<double>(value) / 10.0);
        return std::wstring(text);
    });

    form.AddSection(UiText(L"自动隐藏"));
    form.AddSwitch(UiText(L"自动隐藏/覆盖模式"), &m.autoHide,
                   UiText(L"不占用停靠边的屏幕空间"));
    form.AddSlider(UiText(L"收回延迟"), &m.autoHideDelay, 0, 5000,
                   FormatMillis, 50);
    form.AddSlider(UiText(L"动画时长"), &m.autoHideSpeed, 0, 1000,
                   FormatMillis, 10);

    // --- Dock background ---------------------------------------------------
    form.AddPage(UiText(L"背景栏"), 0xE790, D2D1::ColorF(0x0A84FF));
    form.AddSection(UiText(L"外观"));
    form.AddSlider(UiText(L"不透明度"), &m.backgroundOpacity, 10, 100,
                   FormatPercent);
    form.AddSlider(UiText(L"圆角"), &m.dockCorner, 0, 48, FormatPixels);

    form.AddSection(UiText(L"颜色"),
                    UiText(L"关闭渐变时，背景栏使用单一颜色。"));
    form.AddColor(UiText(L"背景颜色"), &m.topColor,
                  [this]() { return HexOf(settings_.topColor); });
    form.AddSwitch(UiText(L"渐变"), &m.gradient);
    form.AddColor(UiText(L"渐变底部颜色"), &m.bottomColor, [this]()
    {
        return settings_.gradient ? HexOf(settings_.bottomColor)
                                  : std::wstring(UiText(L"渐变关闭"));
    }).enabled = [this]() { return settings_.gradient; };

    // --- Icons -------------------------------------------------------------
    form.AddPage(UiText(L"图标"), 0xE71D, D2D1::ColorF(0xAF52DE));
    form.AddSection(UiText(L"底板"),
        UiText(L"这些是全部图标的默认值；可在图标右键菜单的“图标配置…”中单独调整。"));
    form.AddSlider(UiText(L"圆角半径"), &m.plateCorner, 0, 28, FormatPixels);
    form.AddSlider(UiText(L"图标比例"), &m.plateScale, 50, 150, FormatPercent);
    form.AddSlider(UiText(L"不透明度"), &m.plateOpacity, 0, 100, FormatPercent);

    form.AddSection(UiText(L"内描边"));
    form.AddSlider(UiText(L"粗细"), &m.strokeWidth, 0, 40, [](int value)
    {
        wchar_t text[16];
        swprintf(text, 16, L"%.1f px", static_cast<double>(value) / 10.0);
        return std::wstring(text);
    });
    form.AddSlider(UiText(L"不透明度"), &m.strokeOpacity, 0, 100, FormatPercent);

    // --- Tooltips ----------------------------------------------------------
    form.AddPage(UiText(L"名称气泡"), 0xE8BD, D2D1::ColorF(0x34C759));
    form.AddSection(UiText(L"外观"), UiText(L"运行应用预览沿用这些设置"));
    form.AddSlider(UiText(L"不透明度"), &m.tooltipOpacity, 10, 100,
                   FormatPercent);
    form.AddSlider(UiText(L"比例"), &m.tooltipScale, 50, 188, FormatPercent);
    form.AddSlider(UiText(L"圆角"), &m.tooltipCorner, 0, 40, FormatPixels);
    form.AddSlider(UiText(L"淡入淡出时长"), &m.tooltipFade, 50, 1000,
                   FormatMillis, 10);

    // --- About -------------------------------------------------------------
    form.AddPage(UiText(L"关于"), 0xE946, D2D1::ColorF(0x8E8E93));
    form.AddSection();
    form.AddHeader([this]() { return AppIconBitmap(); },
                   []() { return std::wstring(L"LightDock"); },
                   [this]() { return std::wstring(
                       UiText(L"轻量、可自定义的 Windows 桌面停靠栏")); },
                   64.0f);
    form.AddSection();
    form.AddInfo(UiText(L"作者"),
                 []() { return std::wstring(L"TerryXu & ChatGPT"); });
    form.AddInfo(UiText(L"联系邮箱"),
                 []() { return std::wstring(L"851858419@.com"); });
    form.AddSection();
    form.AddParagraph(UiText(
        L"LightDock 让常用应用触手可及。停靠栏可以放在屏幕的上、下、左、右侧，"
        L"支持自动隐藏、展开动画和图标悬停放大；图标大小、间距、背景渐变、"
        L"圆角和透明度都可以按你的喜好调整。"));

    // --- Footer --------------------------------------------------------------
    form.AddButton(kButtonRestore, UiText(L"恢复默认值"),
                   ui::ButtonRole::Normal, true);
    form.AddButton(kButtonCancel, UiText(L"取消"));
    form.AddButton(kButtonSave, UiText(L"保存"), ui::ButtonRole::Primary);

    auto restore = [this]()
    {
        config_.settings = settings_.original;
        RebuildMetrics();
        Render();
        WakeAnimation();
    };

    form.onChange = [this](ui::Form::Control&) { PreviewSettings(); };
    form.onClose = [this, restore]()
    {
        if (!settings_.saved)
        {
            restore();
        }
        settings_.closed = true;
    };
    form.onButton = [this, restore](int id)
    {
        switch (id)
        {
        case kButtonSave:
            PreviewSettings();
            settings_.saved = true;
            settings_.closed = true;
            break;

        case kButtonCancel:
            restore();
            settings_.closed = true;
            break;

        case kButtonRestore:
        {
            // Defaults for everything except language and display.
            DockSettings defaults;
            LoadSettingsModel(defaults);
            settings_.topColor = RGB(255, 255, 255);
            settings_.bottomColor = RGB(235, 235, 235);
            settings_.form->Refresh();
            PreviewSettings();
            break;
        }

        default:
            break;
        }
    };

    form.SelectPage(initialPage);

    ui::Form::Options options;
    options.owner = window_.Handle();
    options.instance = instance_;
    options.title = UiText(L"Dock 设置");
    options.width = 720.0f;
    options.height = 500.0f;
    options.navigation = ui::Navigation::Sidebar;
    options.icon = AppIconHandle();
    options.center = CurrentWorkArea();

    if (!form.Create(options))
    {
        settings_.form.reset();
        return false;
    }

    settingsWindowOpen_ = true;
    autoHideHidePending_ = false;
    if (!fullscreenActive_)
    {
        SetFullscreenVisibilityTarget(1.0f);
    }
    WakeAnimation();

    RunModal(form.Handle(), settings_.closed);

    if (!settings_.saved)
    {
        restore();
    }

    settingsWindowOpen_ = false;
    autoHideHidePending_ = false;
    WakeAnimation();

    settings_.form->Destroy();
    settings_.form.reset();
    return settings_.saved;
}

// --- Icon editor ----------------------------------------------------------------

void App::PreviewPlate()
{
    if (!editor_.item)
    {
        return;
    }

    ItemEditor& m = editor_;
    if (!m.customBottom)
    {
        m.bottomColor = DerivedColor(m.topColor);
    }

    PlateStyle& plate = m.item->plate;
    plate.enabled = m.plateEnabled;
    plate.iconScale = static_cast<float>(m.iconScale) / 100.0f;
    plate.opacity = m.customOpacity
        ? static_cast<float>(m.opacity) / 100.0f : -1.0f;
    plate.customBottom = m.customBottom;
    plate.top = HexOf(m.topColor);
    plate.bottom = HexOf(m.bottomColor);
    plate.strokeColor = m.customStroke ? HexOf(m.strokeColor) : std::wstring();
    plate.strokeOpacity = m.customStrokeOpacity
        ? static_cast<float>(m.strokeOpacity) / 100.0f : -1.0f;

    Render();
}

bool App::EditItem(size_t index)
{
    if (index >= items_.size() || !window_.Handle())
    {
        return false;
    }

    editor_ = ItemEditor{};
    ItemEditor& m = editor_;
    m.item = items_[index].get();
    DockItem& item = *m.item;

    // Snapshot for the live preview rollback on cancel.
    m.originalPlate = item.plate;
    m.originalIconSource = item.iconSource;

    m.name = item.name;
    m.path = item.targetPath;
    m.arguments = item.arguments;
    m.iconLabel = item.iconFile;

    const PlateStyle& own = item.plate;
    const IconBackdrop& global = config_.settings.backdrop;
    m.plateEnabled = own.enabled;
    m.iconScale = Percent(own.iconScale > 0.0f ? own.iconScale : global.iconScale);
    m.customOpacity = own.opacity >= 0.0f;
    m.opacity = Percent(own.opacity >= 0.0f ? own.opacity : global.opacity);
    m.topColor = ColorRefOf(own.top, RGB(59, 66, 82));
    m.customBottom = own.customBottom;
    m.bottomColor = own.customBottom
        ? ColorRefOf(own.bottom, DerivedColor(m.topColor))
        : DerivedColor(m.topColor);
    m.customStroke = !own.strokeColor.empty();
    m.strokeColor = ColorRefOf(own.strokeColor, RGB(255, 255, 255));
    m.customStrokeOpacity = own.strokeOpacity >= 0.0f;
    m.strokeOpacity = Percent(own.strokeOpacity >= 0.0f ? own.strokeOpacity
                                                        : global.strokeOpacity);

    m.form = std::make_unique<ui::Form>();
    ui::Form& form = *m.form;

    // --- Shortcut page -------------------------------------------------------
    form.AddPage(UiText(L"快捷方式"));
    form.AddSection();
    form.AddHeader(
        [this]() -> IWICBitmapSource*
        {
            return editor_.item ? editor_.item->iconSource.Get() : nullptr;
        },
        [this]() { return editor_.name; },
        [this]() { return editor_.path; },
        52.0f);

    form.AddSection(UiText(L"应用"));
    ui::Form::Control& nameField = form.AddEdit(UiText(L"名称"), &m.name);
    ui::Form::Control* nameControl = &nameField;
    ui::Form::Control* pathControl = nullptr;
    ui::Form::Control* iconControl = nullptr;

    pathControl = &form.AddEdit(UiText(L"程序"), &m.path, UiText(L"浏览…"),
        [this, &pathControl, nameControl, &iconControl]()
        {
            const COMDLG_FILTERSPEC filters[] =
            {
                {L"应用程序 (*.exe; *.lnk)", L"*.exe;*.lnk"},
                {L"所有文件 (*.*)", L"*.*"},
            };
            const std::wstring path = PickFile(UiText(L"选择程序"), filters,
                                               ARRAYSIZE(filters));
            if (path.empty())
            {
                return;
            }

            editor_.form->SetText(*pathControl, path);

            // Refresh name and icon from the new target, like a shortcut.
            AppInfo info = icons_.Inspect(path, 256);
            if (!info.name.empty())
            {
                editor_.form->SetText(*nameControl, info.name);
            }
            if (info.icon)
            {
                editor_.pendingIcon = info.icon;
                editor_.pendingIconPath.clear();
                editor_.form->SetText(*iconControl, UiText(L"来自所选程序"));
                editor_.item->iconSource = editor_.pendingIcon;
                editor_.item->icon.Reset();
                Render();
            }
        });

    form.AddEdit(UiText(L"附加命令"), &m.arguments);

    iconControl = &form.AddEdit(UiText(L"图标文件"), &m.iconLabel,
        UiText(L"浏览…"),
        [this, &iconControl]()
        {
            const COMDLG_FILTERSPEC filters[] =
            {
                {L"图片或程序 (*.png; *.exe; *.lnk)", L"*.png;*.exe;*.lnk"},
                {L"图片 (*.png)", L"*.png"},
                {L"所有文件 (*.*)", L"*.*"},
            };
            const std::wstring path = PickFile(UiText(L"选择图标"), filters,
                                               ARRAYSIZE(filters));
            if (path.empty())
            {
                return;
            }

            ComPtr<IWICBitmap> icon;
            if (GetFileExtension(path) == L".png")
            {
                icon = icons_.LoadFromCache(path);
            }
            else
            {
                icon = icons_.Inspect(path, 256).icon;
            }

            if (icon)
            {
                editor_.pendingIcon = icon;
                editor_.pendingIconPath = path;
                editor_.form->SetText(*iconControl, GetFileName(path));
                editor_.item->iconSource = editor_.pendingIcon;
                editor_.item->icon.Reset();
                Render();
            }
        });
    iconControl->readOnly = true;

    // --- Appearance page -----------------------------------------------------
    auto plateOn = [this]() { return editor_.plateEnabled; };

    form.AddPage(UiText(L"图标外观"));
    form.AddSection(UiText(L"底板"));
    form.AddSwitch(UiText(L"启用圆角底板"), &m.plateEnabled);
    form.AddSlider(UiText(L"图标比例"), &m.iconScale, 50, 150, FormatPercent)
        .enabled = plateOn;
    form.AddSwitch(UiText(L"自定义透明度"), &m.customOpacity).enabled = plateOn;
    form.AddSlider(UiText(L"透明度"), &m.opacity, 0, 100, FormatPercent)
        .enabled = [this]()
        {
            return editor_.plateEnabled && editor_.customOpacity;
        };

    form.AddSection(UiText(L"颜色"));
    form.AddColor(UiText(L"顶部颜色"), &m.topColor,
                  [this]() { return HexOf(editor_.topColor); })
        .enabled = plateOn;
    form.AddSwitch(UiText(L"自定义第二颜色（渐变）"), &m.customBottom)
        .enabled = plateOn;
    form.AddColor(UiText(L"底部颜色"), &m.bottomColor, [this]()
    {
        return editor_.customBottom
            ? HexOf(editor_.bottomColor)
            : HexOf(DerivedColor(editor_.topColor)) + UiText(L"（自动）");
    }).enabled = [this]()
    {
        return editor_.plateEnabled && editor_.customBottom;
    };

    form.AddSection(UiText(L"内描边"));
    form.AddSwitch(UiText(L"自定义描边颜色"), &m.customStroke).enabled = plateOn;
    form.AddColor(UiText(L"描边颜色"), &m.strokeColor, [this]()
    {
        return editor_.customStroke
            ? HexOf(editor_.strokeColor)
            : std::wstring(L"#FFFFFF") + UiText(L"（默认）");
    }).enabled = [this]()
    {
        return editor_.plateEnabled && editor_.customStroke;
    };
    form.AddSwitch(UiText(L"自定义描边透明度"), &m.customStrokeOpacity)
        .enabled = plateOn;
    form.AddSlider(UiText(L"描边不透明度"), &m.strokeOpacity, 0, 100,
                   FormatPercent)
        .enabled = [this]()
        {
            return editor_.plateEnabled && editor_.customStrokeOpacity;
        };

    form.AddButton(kButtonRestore, UiText(L"恢复默认值"),
                   ui::ButtonRole::Normal, true);
    form.AddButton(kButtonCancel, UiText(L"取消"));
    form.AddButton(kButtonSave, UiText(L"保存"), ui::ButtonRole::Primary);

    /// Puts back the plate and icon the entry had before the dialog opened.
    auto restorePlate = [this]()
    {
        if (editor_.item)
        {
            editor_.item->plate = editor_.originalPlate;
            editor_.item->iconSource = editor_.originalIconSource;
            editor_.item->icon.Reset();
            Render();
        }
    };

    form.onChange = [this](ui::Form::Control& control)
    {
        if (control.kind != ui::Form::Kind::Edit)
        {
            PreviewPlate();
        }
    };
    form.onClose = [this, restorePlate]()
    {
        if (!editor_.saved)
        {
            restorePlate();
        }
        editor_.closed = true;
    };
    form.onButton = [this, restorePlate](int id)
    {
        switch (id)
        {
        case kButtonCancel:
            restorePlate();
            editor_.closed = true;
            break;

        case kButtonRestore:
        {
            const IconBackdrop& backdrop = config_.settings.backdrop;
            editor_.plateEnabled = true;
            editor_.iconScale = Percent(backdrop.iconScale);
            editor_.customOpacity = false;
            editor_.opacity = Percent(backdrop.opacity);
            editor_.customBottom = false;
            editor_.customStroke = false;
            editor_.customStrokeOpacity = false;
            editor_.strokeOpacity = Percent(backdrop.strokeOpacity);
            editor_.topColor = RGB(255, 255, 255);
            editor_.bottomColor = RGB(235, 235, 235);
            editor_.strokeColor = RGB(255, 255, 255);
            editor_.form->Refresh();
            PreviewPlate();
            break;
        }

        case kButtonSave:
        {
            if (!editor_.item)
            {
                editor_.closed = true;
                break;
            }

            DockItem& target = *editor_.item;
            std::wstring name = editor_.name;
            const std::wstring path = editor_.path;
            std::wstring arguments = editor_.arguments;

            if (!path.empty() && path != target.targetPath)
            {
                AppInfo info = icons_.Inspect(path, 256);

                target.targetPath = path;
                target.resolvedPath =
                    info.resolvedPath.empty() ? path : info.resolvedPath;
                target.processName = GetFileName(target.resolvedPath);
                // Editing the target must not keep the previous app's identity.
                target.appUserModelId =
                    AppLauncher::ResolvePackagedAppId(target.resolvedPath);

                if (name.empty())
                {
                    name = info.name;
                }

                // A shortcut already carries its own arguments.
                if (GetFileExtension(path) == L".lnk")
                {
                    arguments.clear();
                }

                if (!editor_.pendingIcon && info.icon)
                {
                    editor_.pendingIcon = info.icon;
                }
            }

            if (name.empty())
            {
                name = GetFileStem(target.resolvedPath);
            }

            target.name = name;
            target.arguments = arguments;

            if (editor_.pendingIcon)
            {
                target.iconSource = editor_.pendingIcon;
                target.icon.Reset();

                // Persist every chosen icon into LightDock's own cache,
                // including custom PNGs. Otherwise a PNG previews correctly
                // for this session but the old cached icon returns after the
                // next launch.
                icons_.SaveToCache(
                    editor_.pendingIcon.Get(),
                    GetIconCacheDir() + L"\\" + target.iconFile);
            }

            // --- plate look --------------------------------------------------
            PreviewPlate();
            PlateStyle& plate = target.plate;

            // Pin the ratio only when it deviates from the global default;
            // matching values stay unpinned so global changes keep working.
            const float chosen = static_cast<float>(editor_.iconScale) / 100.0f;
            plate.iconScale =
                std::abs(chosen - config_.settings.backdrop.iconScale) < 0.005f
                ? 0.0f : chosen;

            editor_.saved = true;
            editor_.closed = true;
            break;
        }

        default:
            break;
        }
    };

    ui::Form::Options options;
    options.owner = window_.Handle();
    options.instance = instance_;
    // The title names the app being configured so the user always knows
    // which entry they are editing.
    options.title = std::wstring(UiText(L"图标配置 — ")) + item.name;
    options.width = 560.0f;
    options.height = 600.0f;
    options.navigation = ui::Navigation::Segmented;
    options.icon = AppIconHandle();
    options.center = CurrentWorkArea();

    if (!form.Create(options))
    {
        editor_ = ItemEditor{};
        return false;
    }

    // Modal: the dock keeps painting but stops reacting to the mouse.
    RunModal(form.Handle(), editor_.closed);

    if (!editor_.saved)
    {
        restorePlate();
    }

    const bool saved = editor_.saved;
    editor_.form->Destroy();
    editor_.form.reset();
    editor_.item = nullptr;
    return saved;
}

} // namespace ld

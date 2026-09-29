#include "App.h"

#include "Utils.h"

#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <mmsystem.h>
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
constexpr wchar_t kDialogTabPageProperty[] = L"LightDock.DialogTabPage";

HBITMAP CreateMenuIconBitmap(IWICImagingFactory* factory, IWICBitmap* source)
{
    if (!factory || !source)
    {
        return nullptr;
    }

    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateBitmapScaler(scaler.AddressOf()))
        || FAILED(scaler->Initialize(source, 16, 16,
                                     WICBitmapInterpolationModeFant))
        || FAILED(factory->CreateFormatConverter(converter.AddressOf()))
        || FAILED(converter->Initialize(
            scaler.Get(), GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0,
            WICBitmapPaletteTypeCustom)))
    {
        return nullptr;
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = 16;
    info.bmiHeader.biHeight = -16;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS,
                                     &pixels, nullptr, 0);
    if (!bitmap || !pixels)
    {
        if (bitmap)
        {
            DeleteObject(bitmap);
        }
        return nullptr;
    }

    if (FAILED(converter->CopyPixels(nullptr, 16 * 4, 16 * 16 * 4,
                                     static_cast<BYTE*>(pixels))))
    {
        DeleteObject(bitmap);
        return nullptr;
    }

    return bitmap;
}

void MarkDialogTabPage(HWND control, int page)
{
    if (control)
    {
        SetPropW(control, kDialogTabPageProperty,
                 reinterpret_cast<HANDLE>(static_cast<INT_PTR>(page + 1)));
    }
}

struct TabVisibilityContext
{
    int page = 0;
};

BOOL CALLBACK ApplyTabVisibility(HWND child, LPARAM parameter)
{
    const auto* context = reinterpret_cast<TabVisibilityContext*>(parameter);
    HANDLE tag = GetPropW(child, kDialogTabPageProperty);
    if (tag)
    {
        const int page = static_cast<int>(reinterpret_cast<INT_PTR>(tag)) - 1;
        ShowWindow(child, page == context->page ? SW_SHOW : SW_HIDE);
    }

    return TRUE;
}

void ShowDialogTabPage(HWND dialog, int page)
{
    TabVisibilityContext context{page};
    EnumChildWindows(dialog, ApplyTabVisibility,
                     reinterpret_cast<LPARAM>(&context));
}

struct TagDialogChildrenContext
{
    HWND dialog = nullptr;
    int tabId = 0;
    int saveId = 0;
    int cancelId = 0;
    int splitY = 0;
    int endFirstPageY = 0;
    int finalPage = 1;
    int shiftFirstPage = 0;
    int shiftSecondPage = 0;
    int shiftFinalPage = 0;
};

BOOL CALLBACK TagDialogChildren(HWND child, LPARAM parameter)
{
    auto* context = reinterpret_cast<TagDialogChildrenContext*>(parameter);
    const int id = GetDlgCtrlID(child);
    if (id == context->tabId || id == context->saveId || id == context->cancelId
        || GetPropW(child, kDialogTabPageProperty))
    {
        return TRUE;
    }

    RECT rect{};
    GetWindowRect(child, &rect);
    MapWindowPoints(nullptr, context->dialog,
                    reinterpret_cast<POINT*>(&rect), 2);

    int page = 0;
    if (rect.top >= context->splitY)
    {
        page = rect.top < context->endFirstPageY ? 1 : context->finalPage;
    }

    MarkDialogTabPage(child, page);
    const int shift = page == 0 ? context->shiftFirstPage
        : page == 1 ? context->shiftSecondPage
                    : context->shiftFinalPage;
    if (shift != 0)
    {
        SetWindowPos(child, nullptr, rect.left,
                     rect.top + shift, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    return TRUE;
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

/// DPI of the monitor a dialog lives on (falls back to the system DPI).
int DialogDpi(HWND hwnd)
{
    using FnGetDpiForWindow = UINT(WINAPI*)(HWND);

    static FnGetDpiForWindow dpiForWindow = []() -> FnGetDpiForWindow
    {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        return user32
            ? reinterpret_cast<FnGetDpiForWindow>(
                  GetProcAddress(user32, "GetDpiForWindow"))
            : nullptr;
    }();

    if (dpiForWindow)
    {
        const UINT dpi = dpiForWindow(hwnd);
        if (dpi > 0)
        {
            return static_cast<int>(dpi);
        }
    }

    HDC dc = GetDC(hwnd);
    const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    ReleaseDC(hwnd, dc);
    return dpi > 0 ? dpi : 96;
}

/// Segoe UI 9pt (the modern Windows UI font), scaled for the dialog's DPI.
/// The stock DEFAULT_GUI_FONT renders like Windows 95, which is what made
/// the dialogs look dated. The fonts live for the process lifetime.
HFONT DialogFont(bool bold)
{
    static HFONT regular = nullptr;
    static HFONT semibold = nullptr;

    HFONT& slot = bold ? semibold : regular;

    if (!slot)
    {
        HDC dc = GetDC(nullptr);
        const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
        ReleaseDC(nullptr, dc);

        const int height = -MulDiv(9, dpi, 72);

        slot = CreateFontW(
            height, 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    }

    return slot;
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

        if (now - lastDisplayCheck_ >= std::chrono::seconds(1))
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
    windowPreview_.Initialize(instance, window_.Handle());

    DetectRefreshRate();
    RefreshMonitors();
    LoadItems();
    RebuildMetrics();
    PollProcesses();

    window_.AddTrayIcon(MakeTrayIcon(GetSystemMetrics(SM_CXSMICON)), L"LightDock");

    return true;
}

void App::Shutdown()
{
    window_.RemoveTrayIcon();

    items_.clear();
    pointers_.clear();

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
    snapshot.apps.reserve(items_.size());

    for (const auto& item : items_)
    {
        if (item->kind != DockItemKind::Pinned)
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
        entry.iconFile = item->iconFile;
        entry.plate = item->plate;

        snapshot.apps.push_back(std::move(entry));
    }

    Config::Save(snapshot);
}

void App::AddApplication(const std::wstring& path)
{
    if (path.empty())
    {
        return;
    }

    const std::wstring id = MakeStableId(path);

    for (const auto& item : items_)
    {
        if (item->kind == DockItemKind::Pinned && item->id == id)
        {
            return; // already pinned on the dock
        }
    }

    AppInfo info = icons_.Inspect(path, 256);
    if (info.resolvedPath.empty())
    {
        info.resolvedPath = path;
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
                                            info.resolvedPath));
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

    if (item.kind == DockItemKind::StartButton)
    {
        BeginShellPopupPlacement(
            false, mousePhysicalX_, mousePhysicalY_);
        return;
    }

    if (item.kind == DockItemKind::SearchButton)
    {
        BeginShellPopupPlacement(
            true, mousePhysicalX_, mousePhysicalY_);
        return;
    }

    if (item.kind == DockItemKind::RunningTransient)
    {
        if (!item.windows.empty()
            && AppLauncher::ActivateWindow(item.windows.front().hwnd))
        {
            return;
        }

        AppLauncher::Open(item.resolvedPath.empty()
            ? item.targetPath : item.resolvedPath);
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

    AppLauncher::Launch(item.targetPath, item.arguments);

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
    geometry_ = ComputeGeometry(static_cast<int>(items_.size()), metrics_,
                                config_.settings.dockEdge);
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

void App::RepositionWindow()
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
    if (config_.settings.autoHide)
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
            config_.settings.dockEdge, reservation);
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

    // Overlay/auto-hide mode normally hugs the physical monitor edge. If
    // Explorer temporarily reveals its own taskbar (notably when Start/Search
    // opens), move LightDock just inside that visible taskbar instead of
    // letting two shell surfaces fight for the same topmost screen strip.
    RECT anchorRect =
        config_.settings.autoHide ? monitorRect : work;

    // Only yield to Explorer's revealed taskbar while LightDock itself
    // intentionally opened Start/Search. A normal edge reveal often wakes
    // the auto-hidden Windows taskbar too; treating that as permanent inset
    // made LightDock pop up floating above the taskbar instead of from the
    // physical screen edge.
    const int visibleTaskbarInset =
        config_.settings.autoHide && shellPopupPlacementPending_
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
    RepositionWindow();
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

void App::RefreshMonitors()
{
    monitors_.clear();

    EnumDisplayMonitors(
        nullptr,
        nullptr,
        [](HMONITOR monitor, HDC, LPRECT rect, LPARAM data) -> BOOL
        {
            auto* self = reinterpret_cast<App*>(data);

            MONITORINFOEXW info{};
            info.cbSize = sizeof(info);

            if (GetMonitorInfoW(monitor, &info))
            {
                MonitorTarget target;
                target.handle = monitor;
                target.device = info.szDevice;
                target.rect = info.rcMonitor;
                target.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;

                self->monitors_.push_back(std::move(target));
            }
            else if (rect)
            {
                MonitorTarget target;
                target.handle = monitor;
                target.rect = *rect;
                self->monitors_.push_back(std::move(target));
            }

            return TRUE;
        },
        reinterpret_cast<LPARAM>(this));

    // Keep pointing at the same display when possible.
    monitorIndex_ = 0;

    if (!config_.settings.monitor.empty())
    {
        for (size_t i = 0; i < monitors_.size(); ++i)
        {
            if (monitors_[i].device == config_.settings.monitor)
            {
                monitorIndex_ = static_cast<int>(i);
                break;
            }
        }
    }
    else
    {
        for (size_t i = 0; i < monitors_.size(); ++i)
        {
            if (monitors_[i].primary)
            {
                monitorIndex_ = static_cast<int>(i);
                break;
            }
        }
    }
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

    // The new display may run at a different DPI.
    window_.SetDpi(DockWindow::QueryDpi(window_.Handle()));
    RebuildMetrics();
    WakeAnimation();

    SaveConfiguration();
}

void App::AppendMonitorMenu(HMENU menu)
{
    if (monitors_.size() < 2)
    {
        return;
    }

    HMENU submenu = CreatePopupMenu();
    if (!submenu)
    {
        return;
    }

    for (size_t i = 0; i < monitors_.size(); ++i)
    {
        const MonitorTarget& target = monitors_[i];

        const long width = target.rect.right - target.rect.left;
        const long height = target.rect.bottom - target.rect.top;

        wchar_t label[128];
        swprintf(label, 128, L"显示器 %zu  (%ld × %ld)%ls",
                 i + 1, width, height,
                 target.primary ? L"  · 主显示器" : L"");

        const UINT flags = MF_STRING
            | ((static_cast<int>(i) == monitorIndex_) ? MF_CHECKED : 0);

        AppendMenuW(submenu, flags,
                    kMenuMonitorBase + static_cast<UINT>(i), label);
    }

    AppendMenuW(menu, MF_POPUP,
                reinterpret_cast<UINT_PTR>(submenu), L"显示器");
}

void App::CheckDisplayChange()
{
    const int count = GetSystemMetrics(SM_CMONITORS);
    if (count != static_cast<int>(monitors_.size()))
    {
        RefreshMonitors();
        RepositionWindow();
        return;
    }

    const RECT work = CurrentWorkArea();

    if (work.left != lastWorkArea_.left
        || work.top != lastWorkArea_.top
        || work.right != lastWorkArea_.right
        || work.bottom != lastWorkArea_.bottom)
    {
        RepositionWindow();
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
        hoveredItem && hoveredItem->running
        && !hoveredItem->windows.empty();

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

        if (menuItem == items_.end() || (*menuItem)->windows.empty())
        {
            windowMenuVisible_ = false;
            windowMenuHoveredRow_ = -1;
            windowMenuBounds_ = D2D1::RectF(0, 0, 0, 0);
            needsRender_ = true;
        }
    }

    if (tooltipIndex >= 0
        && tooltipIndex < static_cast<int>(items_.size()))
    {
        tooltipItemId_ = items_[static_cast<size_t>(tooltipIndex)]->id;
    }

    // A running app with at least one real top-level window uses the preview
    // bubble as its tooltip. Do not show the ordinary name bubble first and
    // then replace it a moment later.
    const bool suppressTooltip =
        canExpandWindows
        || (windowMenuVisible_ && pointerOnWindowMenu);
    const float tooltipTarget =
        tooltipIndex >= 0 && !suppressTooltip ? 1.0f : 0.0f;
    const float fadeSeconds = ClampF(
        config_.settings.tooltipFadeSeconds, 0.05f, 1.0f);
    const float tooltipStep = 1.0f - static_cast<float>(
        std::exp(-dt / (static_cast<double>(fadeSeconds) * 0.35)));
    tooltipPresence_ += (tooltipTarget - tooltipPresence_) * tooltipStep;
    if (std::fabs(tooltipPresence_ - tooltipTarget) < 0.003f)
    {
        tooltipPresence_ = tooltipTarget;
    }
    tooltipOpacity_ = ClampF(config_.settings.tooltipOpacity, 0.0f, 1.0f)
        * tooltipPresence_;
    if (tooltipPresence_ != tooltipTarget)
    {
        moving = true;
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
        && !fullscreenActive_ && !exitRequested_ && !settingsWindowOpen_;
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
        const float dividerX = (left.centerX + right.centerX) * 0.5f;
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

        if (menuItem != items_.end() && !(*menuItem)->windows.empty())
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
            const float itemWidth = 125.0f * uiScale;
            const float menuHeight = 132.0f * uiScale;
            const float tailLength = 8.0f * uiScale;
            const float tipGap = 7.0f * uiScale;
            const float margin = 8.0f * dpiScale_;

            const float availableWidth = (std::max)(
                itemWidth,
                static_cast<float>(renderer_.Width()) - margin * 2.0f);
            const size_t maxVisibleByWidth = (std::max)(
                static_cast<size_t>(1),
                static_cast<size_t>(
                    std::floor(availableWidth / itemWidth)));
            constexpr size_t kMaxVisibleWindowItems = 8;
            const size_t rowCount = (std::min)(
                item.windows.size(),
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
                menu.top = tailTip.y + tailLength;
                menu.right = menu.left + menuWidth;
                menu.bottom = menu.top + menuHeight;
                break;

            case DockEdge::Left:
                tailTip = D2D1::Point2F(
                    iconRect.right + tipGap, iconCenterY);
                menu.left = tailTip.x + tailLength;
                menu.top = iconCenterY - menuHeight * 0.5f;
                menu.right = menu.left + menuWidth;
                menu.bottom = menu.top + menuHeight;
                break;

            case DockEdge::Right:
                tailTip = D2D1::Point2F(
                    iconRect.left - tipGap, iconCenterY);
                menu.right = tailTip.x - tailLength;
                menu.left = menu.right - menuWidth;
                menu.top = iconCenterY - menuHeight * 0.5f;
                menu.bottom = menu.top + menuHeight;
                break;

            case DockEdge::Bottom:
            default:
                tailTip = D2D1::Point2F(
                    iconCenterX, iconRect.top - tipGap);
                menu.left = iconCenterX - menuWidth * 0.5f;
                menu.bottom = tailTip.y - tailLength;
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
                const DockWindowEntry& window = item.windows[i];
                WindowPreview::Entry entry;
                entry.hwnd = window.hwnd;
                entry.title = window.title.empty()
                    ? item.name
                    : WindowTitleWithoutApplicationName(
                        window.title, item.name, item.processName);
                entry.active = window.active;
                entry.minimized = window.minimized;
                previewEntries.push_back(std::move(entry));
            }

            // Tail stays in the layered Dock surface so it has exactly the
            // same antialiasing and colour as an ordinary name bubble.
            renderer_.DrawBubbleTail(
                menu, tailTip, config_.settings.dockEdge,
                uiScale, config_.settings.tooltipOpacity);

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

            windowPreview_.Show(
                screenMenu, item.name, previewEntries,
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
    if (tooltipOpacity_ > 0.003f && !tooltipItemId_.empty())
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
            renderer_.DrawTooltip(
                item.name,
                item.centerX,
                verticalDock ? item.baselineBottom
                    : topDock ? item.baselineBottom + 7.0f * dockScale_
                    : item.baselineBottom - item.size - 7.0f * dockScale_,
                item.scale * config_.settings.tooltipScale
                    * kTooltipScaleBaseline,
                dockScale_,
                config_.settings.tooltipCornerRadius * dockScale_ * item.scale
                    * config_.settings.tooltipScale
                    * kTooltipScaleBaseline,
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
        }
    }

    auto pinnedForPath = [&](const std::wstring& path) -> DockItem*
    {
        const std::wstring id = MakeStableId(path);
        for (auto& item : items_)
        {
            if (item->kind == DockItemKind::Pinned
                && (item->id == id
                    || EqualsIgnoreCase(item->targetPath, path)
                    || EqualsIgnoreCase(item->resolvedPath, path)))
            {
                return item.get();
            }
        }
        return nullptr;
    };

    struct Group
    {
        std::wstring path;
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

        if (DockItem* pinned = pinnedForPath(window.path))
        {
            pinned->windows.push_back(std::move(entry));
            continue;
        }

        auto group = std::find_if(
            groups.begin(), groups.end(),
            [&](const Group& candidate)
            {
                return EqualsIgnoreCase(candidate.path, window.path);
            });

        if (group == groups.end())
        {
            Group created;
            created.path = window.path;
            created.windows.push_back(std::move(entry));
            groups.push_back(std::move(created));
        }
        else
        {
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

    // Preserve application order in the transient section. The windows
    // inside each application remain in EnumWindows Z-order, so index 0 is
    // the most recently front-most taskbar window and is the natural target
    // for an ordinary single click.
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
            if (!matched[i] && EqualsIgnoreCase(itemPath, groups[i].path))
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
        item->running = true;
        item->windows = groups[match].windows;
        item->windowHandle = item->windows.empty()
            ? nullptr : item->windows.front().hwnd;
        item->windowTitle = item->windows.empty()
            ? std::wstring() : item->windows.front().title;
        contentChanged = true;
        nextTransient.push_back(std::move(item));
    }

    for (size_t i = 0; i < groups.size(); ++i)
    {
        if (matched[i])
        {
            continue;
        }

        AppInfo info = icons_.Inspect(groups[i].path, 256);

        auto item = std::make_unique<DockItem>();
        item->kind = DockItemKind::RunningTransient;
        item->id = L"__running_" + MakeStableId(groups[i].path);
        item->name = info.name.empty()
            ? GetFileStem(groups[i].path) : info.name;
        item->targetPath = groups[i].path;
        item->resolvedPath =
            info.resolvedPath.empty() ? groups[i].path : info.resolvedPath;
        item->processName = info.processName.empty()
            ? GetFileName(item->resolvedPath) : info.processName;
        item->iconSource = info.icon;
        item->running = true;
        item->windows = groups[i].windows;
        item->windowHandle = item->windows.empty()
            ? nullptr : item->windows.front().hwnd;
        item->windowTitle = item->windows.empty()
            ? std::wstring() : item->windows.front().title;

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

    if (geometryChanged)
    {
        pressedIndex_ = -1;
        draggingIndex_ = -1;
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

        if (settingsWindowOpen_)
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
    // reorder operation. Once crossed, capture the mouse so the drag can be
    // completed even if the pointer leaves the panel's rounded hit region.
    if (pressedIndex_ >= 0 && draggingIndex_ < 0)
    {
        const float dx = x - dragStartX_;
        const float dy = y - dragStartY_;
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

        if (index >= 0 && index == pressedIndex_)
        {
            LaunchApplication(static_cast<size_t>(index));
        }

        if (GetCapture() == window_.Handle())
        {
            ReleaseCapture();
        }

        pressedIndex_ = -1;
        dragLift_ = 0.0f;
        dragPlaceholderAmount_ = 0.0f;
        dragTargetIndex_ = -1;
        dragVisualTargetIndex_ = -1;
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
        const std::wstring message =
            L"确定要将“" + items_[static_cast<size_t>(from)]->name
            + L"”移出 Dock 吗？";

        const int answer = MessageBoxW(
            window_.Handle(), message.c_str(), L"移出 Dock",
            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);

        if (answer == IDYES)
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

void App::OnDpiChanged(int dpi)
{
    if (dpi <= 0)
    {
        return;
    }

    window_.SetDpi(dpi);
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
    preview->id = L"__external_drag_preview__";
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
    if (!externalDragActive_
        || draggingIndex_ < 0
        || draggingIndex_ >= static_cast<int>(items_.size()))
    {
        return;
    }

    mouseX_ = x;
    mouseY_ = y;

    int insertion = dragTargetIndex_;
    const int previewIndex = draggingIndex_;
    const int originalCount = static_cast<int>(items_.size()) - 1;

    items_.erase(items_.begin() + previewIndex);
    RebuildItemPointers();

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
    mouseActive_ = false;
    tooltipItemId_.clear();
    tooltipPresence_ = 0.0f;
    autoHideHidePending_ = false;
    SetFullscreenVisibilityTarget(0.0f);
    if (settings_.dialog)
    {
        PostMessageW(settings_.dialog, WM_CLOSE, 0, 0);
    }
    if (editor_.dialog)
    {
        PostMessageW(editor_.dialog, WM_CLOSE, 0, 0);
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
        {L"窗口菜单", L"Window menu"},
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

void App::ShowContextMenu(int index, float x, float y)
{
    HMENU menu = CreatePopupMenu();
    if (!menu)
    {
        return;
    }

    if (index >= 0)
    {
        const DockItem& item = *items_[static_cast<size_t>(index)];

        if (item.kind == DockItemKind::Pinned)
        {
            AppendMenuW(menu, MF_STRING, kMenuOpen, UiText(L"打开"));
            AppendMenuW(menu, MF_STRING, kMenuOpenFolder,
                        UiText(L"打开所在文件夹"));
            AppendMenuW(menu, MF_STRING, kMenuEdit, UiText(L"图标配置…"));
            AppendMenuW(menu, MF_STRING, kMenuRemove, UiText(L"从 Dock 移除"));
        }
        else if (item.kind == DockItemKind::RunningTransient)
        {
            AppendMenuW(menu, MF_STRING, kMenuOpen, UiText(L"打开"));
            AppendMenuW(menu, MF_STRING, kMenuOpenFolder,
                        UiText(L"打开所在文件夹"));
            AppendMenuW(menu, MF_STRING, kMenuPinRunning,
                        UiText(L"固定到 Dock"));
        }
        else
        {
            AppendMenuW(menu, MF_STRING, kMenuOpen, UiText(L"打开"));
        }
    }
    else
    {
        AppendMenuW(menu, MF_STRING, kMenuAdd, UiText(L"添加应用"));

        HMENU runningMenu = CreatePopupMenu();
        const std::vector<std::wstring> runningApps =
            FindRunningTaskbarApplications();
        menuRunningCandidates_ = runningApps;
        menuRunningBitmaps_.clear();
        if (runningMenu)
        {
            if (runningApps.empty())
            {
                AppendMenuW(runningMenu, MF_STRING | MF_GRAYED,
                            kMenuAddRunningBase, UiText(L"没有可添加的运行应用"));
            }
            else
            {
                for (size_t i = 0; i < runningApps.size(); ++i)
                {
                    AppInfo info = icons_.Inspect(runningApps[i], 32);
                    const std::wstring label = info.name.empty()
                        ? GetShellDisplayName(runningApps[i]) : info.name;
                    const UINT itemId = kMenuAddRunningBase
                        + static_cast<UINT>(i);
                    AppendMenuW(runningMenu, MF_STRING,
                                itemId, label.empty()
                                    ? runningApps[i].c_str() : label.c_str());

                    HBITMAP bitmap = CreateMenuIconBitmap(
                        icons_.Factory(), info.icon.Get());
                    if (bitmap)
                    {
                        MENUITEMINFOW itemInfo{};
                        itemInfo.cbSize = sizeof(itemInfo);
                        itemInfo.fMask = MIIM_BITMAP;
                        itemInfo.hbmpItem = bitmap;
                        if (SetMenuItemInfoW(runningMenu, itemId, FALSE,
                                             &itemInfo))
                        {
                            menuRunningBitmaps_.push_back(bitmap);
                        }
                        else
                        {
                            DeleteObject(bitmap);
                        }
                    }
                }
            }
            AppendMenuW(menu, MF_POPUP,
                        reinterpret_cast<UINT_PTR>(runningMenu),
                        UiText(L"添加正在运行的应用"));
        }
        else
        {
            AppendMenuW(menu, MF_STRING | MF_GRAYED,
                        kMenuAddRunningBase, UiText(L"添加正在运行的应用"));
        }

        AppendMenuW(menu, MF_STRING, kMenuSettings, UiText(L"Dock 设置…"));

        // TrackPopupMenuEx owns the submenu once attached to the parent menu.
    }

    menuIndex_ = index;

    const RECT bounds = window_.GetBounds();
    const D2D1_POINT_2F physical = dockTransform_.ToPhysical(x, y);
    POINT screen{};
    screen.x = bounds.left + static_cast<LONG>(std::lround(physical.x));
    screen.y = bounds.top + static_cast<LONG>(std::lround(physical.y));

    SetForegroundWindow(window_.Handle());

    const UINT command = TrackPopupMenuEx(
        menu,
        TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD,
        screen.x,
        screen.y,
        window_.Handle(),
        nullptr);

    DestroyMenu(menu);

    for (HBITMAP bitmap : menuRunningBitmaps_)
    {
        DeleteObject(bitmap);
    }
    menuRunningBitmaps_.clear();

    // Makes sure the menu is fully dismissed even without an active window.
    PostMessageW(window_.Handle(), WM_NULL, 0, 0);

    if (command != 0)
    {
        HandleMenuCommand(command);
    }
    menuRunningCandidates_.clear();
}

void App::ShowTrayMenu()
{
    HMENU menu = CreatePopupMenu();
    if (!menu)
    {
        return;
    }

    // Tray menu is the "home" for global actions: dock settings (which now
    // includes the global icon backdrop), picking a display and quitting.
    // Adding apps lives in the dock blank-area menu and drag & drop.
    AppendMenuW(menu, MF_STRING, kMenuSettings, UiText(L"Dock 设置…"));
    AppendMonitorMenu(menu);
    HMENU languageMenu = CreatePopupMenu();
    if (languageMenu)
    {
        AppendMenuW(languageMenu, MF_STRING | (!config_.settings.englishLanguage ? MF_CHECKED : 0),
                    kMenuLanguageChinese, UiText(L"Chinese"));
        AppendMenuW(languageMenu, MF_STRING | (config_.settings.englishLanguage ? MF_CHECKED : 0),
                    kMenuLanguageEnglish, UiText(L"English"));
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(languageMenu),
                    L"Language");
    }
    AppendMenuW(menu, MF_STRING, kMenuAbout, UiText(L"关于"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, UiText(L"退出"));

    POINT cursor{};
    GetCursorPos(&cursor);

    SetForegroundWindow(window_.Handle());

    const UINT command = TrackPopupMenuEx(
        menu,
        TPM_RIGHTALIGN | TPM_BOTTOMALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD,
        cursor.x,
        cursor.y,
        window_.Handle(),
        nullptr);

    DestroyMenu(menu);
    PostMessageW(window_.Handle(), WM_NULL, 0, 0);

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
                AddApplication(path);
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
    {
        const wchar_t* title = UiText(L"关于");
        const wchar_t* text = UiText(L"作者：TerryXu 和 ChatGPT");
        std::wstring about = text;
        about += L"\n";
        about += UiText(L"联系邮箱：851858419@.com");
        about += L"\n\n";
        about += UiText(L"简介：LightDock 是一款轻量、可自定义的 Windows 桌面 Dock，支持应用快捷启动、自动隐藏，以及图标和背景栏外观设置。");
        MessageBoxW(window_.Handle(), about.c_str(), title, MB_OK | MB_ICONINFORMATION);
        break;
    }

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
                    && (item->id == id
                        || EqualsIgnoreCase(item->targetPath, window.path)
                        || EqualsIgnoreCase(item->resolvedPath, window.path));
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

bool App::EditItem(size_t index)
{
    if (index >= items_.size() || !window_.Handle())
    {
        return false;
    }

    editor_ = ItemEditor{};
    editor_.item = items_[index].get();

    constexpr wchar_t kEditorClass[] = L"LightDock_ItemEditor";

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &App::EditorProc;
    windowClass.hInstance = instance_;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = kEditorClass;

    RegisterClassExW(&windowClass);

    constexpr int width = 500;
    constexpr int height = 570;

    // The title names the app being configured so the user always knows
    // which entry they are editing.
    const std::wstring title =
        std::wstring(UiText(L"图标配置 — ")) + items_[index]->name;

    const int scaledWidth = static_cast<int>(std::lround(width * dpiScale_));
    const int scaledHeight = static_cast<int>(std::lround(height * dpiScale_));

    const RECT work = CurrentWorkArea();
    const int x = work.left + ((work.right - work.left) - scaledWidth) / 2;
    const int y = work.top + ((work.bottom - work.top) - scaledHeight) / 2;

    HWND dialog = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        kEditorClass,
        title.c_str(),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        x, y, scaledWidth, scaledHeight,
        window_.Handle(),
        nullptr,
        instance_,
        this);

    if (!dialog)
    {
        editor_ = ItemEditor{};
        return false;
    }

    editor_.dialog = dialog;
    ShowWindow(dialog, SW_SHOW);
    SetForegroundWindow(dialog);

    // Modal: the dock keeps painting but stops reacting to the mouse.
    RunModal(dialog, editor_.closed);

    if (editor_.dialog)
    {
        DestroyWindow(editor_.dialog);
        editor_.dialog = nullptr;
    }

    const bool saved = editor_.saved;
    editor_.item = nullptr;

    return saved;
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
    while (!closed && running_)
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

            if (!IsDialogMessageW(window, &message))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
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

bool App::ShowSettings()
{
    settings_ = SettingsEditor{};

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_BAR_CLASSES | ICC_TAB_CLASSES;
    InitCommonControlsEx(&controls);

    constexpr wchar_t kSettingsClass[] = L"LightDock_Settings";
    constexpr wchar_t kSettingsPageClass[] = L"LightDock_SettingsPage";

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &App::SettingsProc;
    windowClass.hInstance = instance_;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = kSettingsClass;

    RegisterClassExW(&windowClass);

    WNDCLASSEXW pageClass{};
    pageClass.cbSize = sizeof(pageClass);
    pageClass.lpfnWndProc = &App::SettingsViewportProc;
    pageClass.hInstance = instance_;
    pageClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    pageClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    pageClass.lpszClassName = kSettingsPageClass;
    RegisterClassExW(&pageClass);

    // Give the tab pages enough breathing room for real group spacing.
    // The old compact 424x390 layout forced separators directly against
    // sliders/labels, so native controls painted over the line and made it
    // look broken.
    constexpr int width = 500;
    constexpr int height = 460;

    // Scale the dialog for the monitor it will appear on.
    const int scaledWidth = static_cast<int>(std::lround(width * dpiScale_));
    const int scaledHeight = static_cast<int>(std::lround(height * dpiScale_));

    const RECT work = CurrentWorkArea();
    const int x = work.left + ((work.right - work.left) - scaledWidth) / 2;
    const int y = work.top + ((work.bottom - work.top) - scaledHeight) / 2;

    HWND dialog = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        kSettingsClass,
        UiText(L"Dock 设置"),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        x, y, scaledWidth, scaledHeight,
        window_.Handle(),
        nullptr,
        instance_,
        this);

    if (!dialog)
    {
        return false;
    }

    settings_.dialog = dialog;
    settingsWindowOpen_ = true;
    autoHideHidePending_ = false;
    if (!fullscreenActive_)
    {
        SetFullscreenVisibilityTarget(1.0f);
    }
    WakeAnimation();
    ShowWindow(dialog, SW_SHOW);
    SetForegroundWindow(dialog);

    RunModal(dialog, settings_.closed);

    settingsWindowOpen_ = false;
    autoHideHidePending_ = false;
    WakeAnimation();

    if (settings_.dialog)
    {
        DestroyWindow(settings_.dialog);
        settings_.dialog = nullptr;
    }

    return settings_.saved;
}

LRESULT CALLBACK App::SettingsProc(HWND hwnd, UINT message,
                                   WPARAM wParam, LPARAM lParam)
{
    App* self = nullptr;

    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<App*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    else
    {
        self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (!self)
    {
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    return self->HandleSettingsMessage(hwnd, message, wParam, lParam);
}

LRESULT App::HandleSettingsMessage(HWND hwnd, UINT message,
                                   WPARAM wParam, LPARAM lParam)
{
    enum : int
    {
        kIdMode = 201,
        kIdDockEdge = 2200,
        kIdSize,
        kIdSpacing,
        kIdMagnify,
        kIdAutoHide = 205,
        kIdFullscreenHideDelay,
        kIdFullscreenHideSpeed,

        kIdCorner = 221,
        kIdScale,
        kIdOpacity,

        kIdBgCustom = 231,
        kIdBgTop,
        kIdBgBottom,
        kIdBgOpacity,
        kIdTooltipOpacity = 252,
        kIdTooltipFade,
        kIdSettingsTabs = 254,
        kIdDockCorner = 255,
        kIdTooltipScale,
        kIdTooltipCorner,
        kIdStrokeWidth,
        kIdStrokeOpacity,
        kIdOverallScale,

        kIdSave = 241,
        kIdCancel,
        kIdRestoreDefaults,
    };

    auto sliderValue = [](HWND slider) -> int
    {
        return slider ? static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0))
                      : 0;
    };

    auto hexOf = [](COLORREF color) -> std::wstring
    {
        wchar_t text[16];
        swprintf(text, 16, L"#%02X%02X%02X",
                 GetRValue(color), GetGValue(color), GetBValue(color));
        return std::wstring(text);
    };

    auto backgroundChecked = [&]() -> bool
    {
        return settings_.customBox
            && SendMessageW(settings_.customBox, BM_GETCHECK, 0, 0)
               == BST_CHECKED;
    };

    /// The top swatch is always active because it is also the solid panel
    /// colour. The bottom swatch is only active for the optional gradient.
    auto syncBackgroundControls = [&]()
    {
        const bool on = backgroundChecked();

        if (settings_.topSwatch)
        {
            EnableWindow(settings_.topSwatch, TRUE);
        }

        if (settings_.bottomSwatch)
        {
            EnableWindow(settings_.bottomSwatch, on ? TRUE : FALSE);
        }

        const std::wstring top = hexOf(settings_.topColor);
        SetWindowTextW(settings_.topHex, top.c_str());

        const std::wstring bottom = on
            ? hexOf(settings_.bottomColor) : L"（渐变关闭）";
        SetWindowTextW(settings_.bottomHex, bottom.c_str());
    };

    /// Live preview: pushes every control into the running config, rebuilds
    /// the layout and repaints the dock. Cancelling rolls back to the
    /// snapshot taken in WM_CREATE.
    auto previewSettings = [&]()
    {
        DockSettings& s = config_.settings;

        if (settings_.modeCombo)
        {
            const LRESULT selection =
                SendMessageW(settings_.modeCombo, CB_GETCURSEL, 0, 0);

            s.panelMode = selection == 1 ? PanelMode::Elastic
                : selection == 2 ? PanelMode::Static : PanelMode::Fixed;
        }
        if (settings_.edgeCombo)
        {
            const LRESULT selection = SendMessageW(
                settings_.edgeCombo, CB_GETCURSEL, 0, 0);
            s.dockEdge = selection == 1 ? DockEdge::Top
                : selection == 2 ? DockEdge::Left
                : selection == 3 ? DockEdge::Right : DockEdge::Bottom;
        }

        // Keep the legacy base icon size fixed. Overall size is now the only
        // dock-wide sizing control exposed to users.
        s.iconSize = 50;
        s.iconSpacing = sliderValue(settings_.spacingSlider);
        s.overallScale = static_cast<float>(
            sliderValue(settings_.overallScaleSlider)) / 100.0f;
        s.autoHide = settings_.autoHideBox
            && SendMessageW(settings_.autoHideBox, BM_GETCHECK, 0, 0)
               == BST_CHECKED;
        s.autoHideDelayMs = sliderValue(settings_.autoHideDelaySlider);
        s.autoHideAnimationMs = sliderValue(settings_.autoHideSpeedSlider);
        s.magnification =
            static_cast<float>(sliderValue(settings_.magnifySlider)) / 10.0f;
        s.cornerRadius = static_cast<float>(sliderValue(settings_.dockCornerSlider));
        s.tooltipScale = static_cast<float>(sliderValue(settings_.tooltipScaleSlider))
            / 100.0f;
        s.tooltipCornerRadius = static_cast<float>(sliderValue(
            settings_.tooltipCornerSlider));

        IconBackdrop& backdrop = s.backdrop;

        backdrop.cornerRadius =
            static_cast<float>(sliderValue(settings_.cornerSlider));
        backdrop.iconScale =
            static_cast<float>(sliderValue(settings_.scaleSlider)) / 100.0f;
        backdrop.opacity =
            static_cast<float>(sliderValue(settings_.opacitySlider)) / 100.0f;
        backdrop.strokeWidth = static_cast<float>(sliderValue(
            settings_.strokeWidthSlider)) / 10.0f;
        backdrop.strokeOpacity = static_cast<float>(sliderValue(
            settings_.strokeOpacitySlider)) / 100.0f;

        // The first colour is the solid panel colour even when the gradient
        // switch is off. Only the second stop is cleared in solid mode.
        s.backgroundTop = hexOf(settings_.topColor);
        s.backgroundBottom = backgroundChecked()
            ? hexOf(settings_.bottomColor)
            : std::wstring();

        s.backgroundOpacity =
            static_cast<float>(sliderValue(settings_.bgOpacitySlider))
            / 100.0f;
        s.tooltipOpacity =
            static_cast<float>(sliderValue(settings_.tooltipOpacitySlider))
            / 100.0f;
        s.tooltipFadeSeconds =
            static_cast<float>(sliderValue(settings_.tooltipFadeSlider))
            / 1000.0f;

        RebuildMetrics();
        CheckFullscreen();

        // The settings dialog runs its own modal loop, so the dock's frame
        // loop is parked: `needsRender_` alone would not repaint until the
        // dialog closes. Paint synchronously, exactly like the per-icon
        // editor does.
        Render();
        WakeAnimation();
    };

    /// Puts back the settings the dock had before the dialog opened.
    auto restoreSettings = [&]()
    {
        config_.settings = settings_.original;
        RebuildMetrics();
        Render();
        WakeAnimation();
    };

    auto refreshLabels = [&]()
    {
        wchar_t text[48];

        swprintf(text, 48, L"%d px", sliderValue(settings_.spacingSlider));
        SetWindowTextW(settings_.spacingLabel, text);

        swprintf(text, 48, L"%.1fx",
                 static_cast<double>(sliderValue(settings_.magnifySlider))
                     / 10.0);
        SetWindowTextW(settings_.magnifyLabel, text);

        swprintf(text, 48, L"%d px", sliderValue(settings_.cornerSlider));
        SetWindowTextW(settings_.cornerLabel, text);

        swprintf(text, 48, L"%d px", sliderValue(settings_.dockCornerSlider));
        SetWindowTextW(settings_.dockCornerLabel, text);

        swprintf(text, 48, L"%d%%", sliderValue(settings_.scaleSlider));
        SetWindowTextW(settings_.scaleLabel, text);

        swprintf(text, 48, L"%d%%", sliderValue(settings_.opacitySlider));
        SetWindowTextW(settings_.opacityLabel, text);

        swprintf(text, 48, L"%d%%", sliderValue(settings_.bgOpacitySlider));
        SetWindowTextW(settings_.bgOpacityLabel, text);

        swprintf(text, 48, L"%d%%", sliderValue(settings_.tooltipOpacitySlider));
        SetWindowTextW(settings_.tooltipOpacityLabel, text);

        swprintf(text, 48, L"%d ms", sliderValue(settings_.tooltipFadeSlider));
        SetWindowTextW(settings_.tooltipFadeLabel, text);

        swprintf(text, 48, L"%d%%", sliderValue(settings_.tooltipScaleSlider));
        SetWindowTextW(settings_.tooltipScaleLabel, text);

        swprintf(text, 48, L"%d px",
                 sliderValue(settings_.tooltipCornerSlider));
        SetWindowTextW(settings_.tooltipCornerLabel, text);

        swprintf(text, 48, L"%.1f px",
                 static_cast<double>(sliderValue(settings_.strokeWidthSlider)) / 10.0);
        SetWindowTextW(settings_.strokeWidthLabel, text);

        swprintf(text, 48, L"%d%%",
                 sliderValue(settings_.strokeOpacitySlider));
        SetWindowTextW(settings_.strokeOpacityLabel, text);

        swprintf(text, 48, L"%d%%", sliderValue(settings_.overallScaleSlider));
        SetWindowTextW(settings_.overallScaleLabel, text);

        swprintf(text, 48, L"%d ms",
                 sliderValue(settings_.autoHideDelaySlider));
        SetWindowTextW(settings_.autoHideDelayLabel, text);

        swprintf(text, 48, L"%d ms",
                 sliderValue(settings_.autoHideSpeedSlider));
        SetWindowTextW(settings_.autoHideSpeedLabel, text);

    };

    switch (message)
    {
    case WM_NCCREATE:
        // Must fall through to DefWindowProc, otherwise the window title
        // passed to CreateWindowExW is never stored and the caption is blank.
        return DefWindowProcW(hwnd, message, wParam, lParam);

    case WM_CREATE:
    {
        if (!hwnd)
        {
            return -1;
        }

        // Scale every coordinate for the dialog's monitor DPI.
        const float scale = DialogDpi(hwnd) / 96.0f;

        auto S = [scale](int value) -> int
        {
            return static_cast<int>(std::lround(value * scale));
        };

        auto setFont = [](HWND control, bool bold = false)
        {
            HFONT font = DialogFont(bold);
            if (control && font)
            {
                SendMessageW(control, WM_SETFONT,
                             reinterpret_cast<WPARAM>(font), TRUE);
            }
        };

        auto makeLabel = [&](const wchar_t* text, int x, int y, int w,
                             bool bold = false)
        {
            text = UiText(text);
            HWND control = CreateWindowExW(
                0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                S(x), S(y), S(w), S(20), hwnd, nullptr, instance_, nullptr);
            setFont(control, bold);
            return control;
        };

        auto makeSlider = [&](int id, int x, int y, int w,
                              int lo, int hi, int value)
        {
            HWND control = CreateWindowExW(
                0, TRACKBAR_CLASSW, nullptr,
                WS_CHILD | WS_VISIBLE | TBS_HORZ | WS_TABSTOP,
                S(x), S(y), S(w), S(28), hwnd,
                reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
                instance_, nullptr);

            if (control)
            {
                SendMessageW(control, TBM_SETRANGE, TRUE, MAKELPARAM(lo, hi));
                SendMessageW(control, TBM_SETPOS, TRUE, value);
                setFont(control);
            }

            return control;
        };

        auto makeButton = [&](const wchar_t* text, int id, int x, int y)
        {
            text = UiText(text);
            HWND control = CreateWindowExW(
                0, L"BUTTON", text,
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                S(x), S(y), S(84), S(28), hwnd,
                reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
                instance_, nullptr);
            setFont(control);
            return control;
        };

        auto makeCheck = [&](const wchar_t* text, bool checked, int id,
                             int x, int y, int w)
        {
            text = UiText(text);
            HWND control = CreateWindowExW(
                0, L"BUTTON", text,
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
                S(x), S(y), S(w), S(22), hwnd,
                reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
                instance_, nullptr);

            if (control)
            {
                SendMessageW(control, BM_SETCHECK,
                             checked ? BST_CHECKED : BST_UNCHECKED, 0);
                setFont(control);
            }

            return control;
        };

        // Snapshot for the live preview rollback on cancel.
        settings_.original = config_.settings;

        // --- dock behaviour -------------------------------------------------
        HWND modeCaption = makeLabel(L"背景栏模式", 22, 22, 90);

        settings_.modeCombo = CreateWindowExW(
            0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
            S(118), S(18), S(260), S(140), hwnd,
            reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdMode)),
            instance_, nullptr);

        if (settings_.modeCombo)
        {
            SendMessageW(settings_.modeCombo, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(UiText(L"固定宽度（进入时展开）")));
            SendMessageW(settings_.modeCombo, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(UiText(L"弹性跟随图标")));

            SendMessageW(settings_.modeCombo, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(UiText(L"无动画（名称气泡正常）")));

            SendMessageW(settings_.modeCombo, CB_SETCURSEL,
                         config_.settings.panelMode == PanelMode::Fixed ? 0
                         : config_.settings.panelMode == PanelMode::Elastic ? 1 : 2,
                         0);

            setFont(settings_.modeCombo);
        }

        HWND dockEdgeCaption = makeLabel(L"停靠位置", 22, 56, 90);
        settings_.edgeCombo = CreateWindowExW(
            0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
            S(118), S(52), S(260), S(120), hwnd,
            reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdDockEdge)),
            instance_, nullptr);
        if (settings_.edgeCombo)
        {
            SendMessageW(settings_.edgeCombo, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(UiText(L"底部")));
            SendMessageW(settings_.edgeCombo, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(UiText(L"顶部")));
            SendMessageW(settings_.edgeCombo, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(UiText(L"左侧（竖排）")));
            SendMessageW(settings_.edgeCombo, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(UiText(L"右侧（竖排）")));
            SendMessageW(settings_.edgeCombo, CB_SETCURSEL,
                config_.settings.dockEdge == DockEdge::Top ? 1
                : config_.settings.dockEdge == DockEdge::Left ? 2
                : config_.settings.dockEdge == DockEdge::Right ? 3 : 0, 0);
            setFont(settings_.edgeCombo);
        }

        HWND overallScaleCaption = makeLabel(L"整体大小", 22, 88, 100);
        settings_.overallScaleSlider = makeSlider(
            kIdOverallScale, 118, 80, 180, 50, 150,
            static_cast<int>(std::lround(
                config_.settings.overallScale * 100.0f)));
        settings_.overallScaleLabel = makeLabel(L"", 306, 88, 60);
        settings_.spacingSlider = makeSlider(
            kIdSpacing, 118, 108, 180, 0, 32, config_.settings.iconSpacing);
        settings_.magnifySlider = makeSlider(
            kIdMagnify, 118, 136, 180, 10, 25,
            static_cast<int>(std::lround(config_.settings.magnification
                                         * 10.0f)));

        HWND spacingCaption = makeLabel(L"图标间距", 22, 116, 90);
        HWND magnifyCaption = makeLabel(L"放大倍率", 22, 144, 90);
        settings_.spacingLabel = makeLabel(L"", 306, 116, 60);
        settings_.magnifyLabel = makeLabel(L"", 306, 144, 60);

        settings_.autoHideBox = makeCheck(
            L"自动隐藏/覆盖模式（不占用停靠边空间）",
            config_.settings.autoHide, kIdAutoHide, 22, 178, 340);
        HWND autoHideDelayCaption =
            makeLabel(L"收回延迟", 22, 218, 120);
        settings_.autoHideDelaySlider = makeSlider(
            kIdFullscreenHideDelay, 158, 214, 140, 0, 5000,
            config_.settings.autoHideDelayMs);
        settings_.autoHideDelayLabel = makeLabel(L"", 306, 218, 65);
        HWND autoHideSpeedCaption =
            makeLabel(L"自动隐藏动画时长", 22, 250, 130);
        settings_.autoHideSpeedSlider = makeSlider(
            kIdFullscreenHideSpeed, 158, 246, 140, 0, 1000,
            config_.settings.autoHideAnimationMs);
        settings_.autoHideSpeedLabel = makeLabel(L"", 306, 250, 65);
        // Divider inside the Behavior tab: hover/window interaction above,
        // auto-hide timing below.
        HWND behaviorSeparator = CreateWindowExW(
            0, L"STATIC", nullptr, WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
            S(22), S(172), S(430), S(2), hwnd, nullptr, instance_, nullptr);
        MarkDialogTabPage(behaviorSeparator, 0);

        // --- panel background ------------------------------------------------
        makeLabel(L"背景栏外观", 22, 182, 300, true);

        settings_.customBox = makeCheck(L"自定义背景渐变",
                                        !config_.settings.backgroundBottom.empty(),
                                        kIdBgCustom, 22, 212, 240);

        makeLabel(L"背景栏不透明度", 22, 246, 130);
        settings_.bgOpacitySlider = makeSlider(
            kIdBgOpacity, 158, 242, 140, 10, 100,
            static_cast<int>(std::lround(config_.settings.backgroundOpacity
                                         * 100.0f)));
        settings_.bgOpacityLabel = makeLabel(L"", 306, 246, 60);

        auto colorRefOf = [](const std::wstring& hex,
                             COLORREF fallback) -> COLORREF
        {
            float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
            if (ParseHexColor(hex, r, g, b, a))
            {
                return RGB(static_cast<int>(r * 255.0f + 0.5f),
                           static_cast<int>(g * 255.0f + 0.5f),
                           static_cast<int>(b * 255.0f + 0.5f));
            }

            return fallback;
        };

        // Built-in white, only shown while the gradient is off. If the user
        // enables the gradient, start from a clean white pair of stops.
        settings_.topColor = colorRefOf(config_.settings.backgroundTop,
                                        RGB(255, 255, 255));
        settings_.bottomColor = colorRefOf(config_.settings.backgroundBottom,
                                           RGB(255, 255, 255));

        auto makeSwatch = [&](int id, int x, int y)
        {
            return CreateWindowExW(
                0, L"BUTTON", nullptr,
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
                S(x), S(y), S(64), S(24),
                hwnd, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
                instance_, nullptr);
        };

        makeLabel(L"背景栏圆角", 22, 282, 110);
        settings_.dockCornerSlider = makeSlider(
            kIdDockCorner, 158, 278, 140, 0, 48,
            static_cast<int>(std::lround(config_.settings.cornerRadius)));
        settings_.dockCornerLabel = makeLabel(L"", 306, 282, 60);

        // Divider inside the Background tab: geometry/opacity above,
        // colour controls below. This is intentionally in page-1 authored
        // coordinates and will be shifted with the rest of that tab.
        CreateWindowExW(
            0, L"STATIC", nullptr, WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
            S(22), S(318), S(430), S(2), hwnd, nullptr, instance_, nullptr);

        makeLabel(L"顶部颜色", 22, 330, 90);
        settings_.topSwatch = makeSwatch(kIdBgTop, 118, 326);
        settings_.topHex = makeLabel(L"", 194, 330, 150);

        makeLabel(L"底部颜色", 22, 364, 90);
        settings_.bottomSwatch = makeSwatch(kIdBgBottom, 118, 360);
        settings_.bottomHex = makeLabel(L"", 194, 364, 150);

        // --- global plate defaults ------------------------------------------
        makeLabel(L"全局图标设置", 22, 392, 300, true);

        const IconBackdrop& backdrop = config_.settings.backdrop;

        makeLabel(L"圆角半径", 22, 424, 90);
        makeLabel(L"图标在底板内的比例", 22, 462, 130);
        makeLabel(L"不透明度", 22, 500, 90);
        makeLabel(L"内描边粗细", 22, 554, 100);
        makeLabel(L"描边不透明度", 22, 592, 110);

        settings_.cornerSlider = makeSlider(
            kIdCorner, 118, 416, 180, 0, 28,
            static_cast<int>(std::lround(backdrop.cornerRadius)));
        settings_.scaleSlider = makeSlider(
            kIdScale, 158, 454, 140, 50, 150,
            static_cast<int>(std::lround(backdrop.iconScale * 100.0f)));
        settings_.opacitySlider = makeSlider(
            kIdOpacity, 118, 492, 180, 0, 100,
            static_cast<int>(std::lround(backdrop.opacity * 100.0f)));

        settings_.strokeWidthSlider = makeSlider(
            kIdStrokeWidth, 158, 546, 140, 0, 40,
            static_cast<int>(std::lround(backdrop.strokeWidth * 10.0f)));
        settings_.cornerLabel = makeLabel(L"", 306, 424, 60);
        settings_.scaleLabel = makeLabel(L"", 306, 462, 60);
        settings_.opacityLabel = makeLabel(L"", 306, 500, 60);

        // Divider inside the Icons tab: tile shape/fill above, rim controls
        // below. In page-2 authored coordinates this lands between the two
        // groups after tab layout.
        CreateWindowExW(
            0, L"STATIC", nullptr, WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
            S(22), S(528), S(430), S(2), hwnd, nullptr, instance_, nullptr);

        settings_.strokeWidthLabel = makeLabel(L"", 306, 554, 60);

        settings_.strokeOpacitySlider = makeSlider(
            kIdStrokeOpacity, 158, 584, 140, 0, 100,
            static_cast<int>(std::lround(backdrop.strokeOpacity * 100.0f)));
        settings_.strokeOpacityLabel = makeLabel(L"", 306, 592, 60);

        // --- tooltip bubble -------------------------------------------------
        HWND tooltipHeader = makeLabel(L"名称气泡", 22, 54, 300, true);
        HWND tooltipOpacityCaption = makeLabel(L"气泡不透明度", 22, 80, 120);
        settings_.tooltipOpacitySlider = makeSlider(
            kIdTooltipOpacity, 158, 76, 140, 10, 100,
            static_cast<int>(std::lround(config_.settings.tooltipOpacity
                                         * 100.0f)));
        settings_.tooltipOpacityLabel = makeLabel(L"", 306, 80, 60);
        HWND tooltipFadeCaption = makeLabel(L"淡入淡出时长", 22, 108, 120);
        settings_.tooltipFadeSlider = makeSlider(
            kIdTooltipFade, 158, 104, 140, 50, 1000,
            static_cast<int>(std::lround(
                config_.settings.tooltipFadeSeconds * 1000.0f)));
        settings_.tooltipFadeLabel = makeLabel(L"", 306, 108, 70);
        HWND tooltipScaleCaption = makeLabel(L"气泡比例", 22, 136, 120);
        settings_.tooltipScaleSlider = makeSlider(
            kIdTooltipScale, 158, 132, 140, 50, 188,
            static_cast<int>(std::lround(config_.settings.tooltipScale * 100.0f)));
        settings_.tooltipScaleLabel = makeLabel(L"", 306, 136, 60);
        HWND tooltipCornerCaption = makeLabel(L"气泡圆角", 22, 202, 120);
        settings_.tooltipCornerSlider = makeSlider(
            kIdTooltipCorner, 158, 198, 140, 0, 40,
            static_cast<int>(std::lround(
                config_.settings.tooltipCornerRadius)));
        settings_.tooltipCornerLabel = makeLabel(L"", 306, 202, 60);

        refreshLabels();
        syncBackgroundControls();

        HWND save = makeButton(L"保存", kIdSave, 300, 400);
        makeButton(L"取消", kIdCancel, 392, 400);

        settings_.tabControl = CreateWindowExW(
            0, WC_TABCONTROLW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | TCS_TABS,
            S(12), S(8), S(476), S(370), hwnd,
            reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdSettingsTabs)),
            instance_, nullptr);
        setFont(settings_.tabControl);
        if (settings_.tabControl)
        {
            TCITEMW tab{};
            tab.mask = TCIF_TEXT;
            tab.pszText = const_cast<wchar_t*>(UiText(L"行为"));
            TabCtrl_InsertItem(settings_.tabControl, 0, &tab);
            tab.pszText = const_cast<wchar_t*>(UiText(L"背景栏"));
            TabCtrl_InsertItem(settings_.tabControl, 1, &tab);
            tab.pszText = const_cast<wchar_t*>(UiText(L"图标"));
            TabCtrl_InsertItem(settings_.tabControl, 2, &tab);
            tab.pszText = const_cast<wchar_t*>(UiText(L"气泡"));
            TabCtrl_InsertItem(settings_.tabControl, 3, &tab);
            TabCtrl_SetCurSel(settings_.tabControl, 0);

            // These controls are laid out in the behavior page's final
            // coordinates, below the original compact interaction rows.
            MarkDialogTabPage(settings_.autoHideDelaySlider, 0);
            MarkDialogTabPage(settings_.autoHideBox, 0);
            MarkDialogTabPage(dockEdgeCaption, 0);
            MarkDialogTabPage(settings_.edgeCombo, 0);
            MarkDialogTabPage(settings_.autoHideDelayLabel, 0);
            MarkDialogTabPage(settings_.autoHideSpeedSlider, 0);
            MarkDialogTabPage(settings_.autoHideSpeedLabel, 0);
            // These captions share their controls' final behavior-page
            // coordinates; keep them on page 0 instead of letting the
            // generic coordinate pass classify them as background-page
            // labels and shift them out of view.
            MarkDialogTabPage(autoHideDelayCaption, 0);
            MarkDialogTabPage(autoHideSpeedCaption, 0);
            // Keep every behavior-page control on its authored row. The
            // generic page classifier shifts untagged controls down to make
            // room for another page; that would collide with the newly added
            // dock-edge selector and the auto-hide rows.
            MarkDialogTabPage(modeCaption, 0);
            MarkDialogTabPage(settings_.modeCombo, 0);
            MarkDialogTabPage(overallScaleCaption, 0);
            MarkDialogTabPage(spacingCaption, 0);
            MarkDialogTabPage(magnifyCaption, 0);
            MarkDialogTabPage(settings_.overallScaleSlider, 0);
            MarkDialogTabPage(settings_.spacingSlider, 0);
            MarkDialogTabPage(settings_.magnifySlider, 0);
            MarkDialogTabPage(settings_.overallScaleLabel, 0);
            MarkDialogTabPage(settings_.spacingLabel, 0);
            MarkDialogTabPage(settings_.magnifyLabel, 0);
            // These controls are already positioned in the bubble page's
            // final coordinates; pre-tag them so the generic page classifier
            // does not shift them through another tab's coordinate range.
            MarkDialogTabPage(tooltipCornerCaption, 3);
            MarkDialogTabPage(tooltipHeader, 3);
            MarkDialogTabPage(tooltipOpacityCaption, 3);
            MarkDialogTabPage(tooltipFadeCaption, 3);
            MarkDialogTabPage(tooltipScaleCaption, 3);
            MarkDialogTabPage(settings_.tooltipOpacitySlider, 3);
            MarkDialogTabPage(settings_.tooltipOpacityLabel, 3);
            MarkDialogTabPage(settings_.tooltipFadeSlider, 3);
            MarkDialogTabPage(settings_.tooltipFadeLabel, 3);
            MarkDialogTabPage(settings_.tooltipScaleSlider, 3);
            MarkDialogTabPage(settings_.tooltipScaleLabel, 3);
            MarkDialogTabPage(settings_.tooltipCornerSlider, 3);
            MarkDialogTabPage(settings_.tooltipCornerLabel, 3);

            TagDialogChildrenContext tagContext{
                hwnd, kIdSettingsTabs, kIdSave, kIdCancel,
                S(160), S(382), 2, S(38), -S(120), -S(338)};
            EnumChildWindows(hwnd, TagDialogChildren,
                             reinterpret_cast<LPARAM>(&tagContext));
            EnumChildWindows(hwnd, [](HWND child, LPARAM amount) -> BOOL
            {
                HANDLE tag = GetPropW(child, kDialogTabPageProperty);
                if (tag
                    && static_cast<int>(reinterpret_cast<INT_PTR>(tag)) - 1 == 3)
                {
                    RECT rect{};
                    GetWindowRect(child, &rect);
                    MapWindowPoints(nullptr, GetParent(child),
                                    reinterpret_cast<POINT*>(&rect), 2);
                    SetWindowPos(child, nullptr, rect.left,
                        rect.top - static_cast<int>(amount), 0, 0,
                        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                }
                return TRUE;
            }, S(38));
            ShowDialogTabPage(hwnd, 0);
            // The controls remain direct dialog children so existing page
            // visibility and layout logic stays intact. Keep the enlarged
            // tab control behind them as the framed page surface.
            SetWindowPos(settings_.tabControl, HWND_BOTTOM, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

            HWND restore = CreateWindowExW(
                0, L"BUTTON", UiText(L"恢复默认值"),
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                S(20), S(400), S(120), S(28), hwnd,
                reinterpret_cast<HMENU>(static_cast<UINT_PTR>(
                    kIdRestoreDefaults)), instance_, nullptr);
            setFont(restore);

            // Give each tab a real clipped page surface. Previously the
            // controls were siblings of the tab control, so rows outside its
            // page could paint over the tab strip and footer instead of being
            // clipped or scrollable.
            RECT pageRect{};
            GetClientRect(settings_.tabControl, &pageRect);
            TabCtrl_AdjustRect(settings_.tabControl, FALSE, &pageRect);
            POINT pageOrigin{pageRect.left, pageRect.top};
            MapWindowPoints(settings_.tabControl, hwnd, &pageOrigin, 1);
            const int pageHeight = std::max(
                S(1), static_cast<int>(pageRect.bottom - pageRect.top));
            settings_.pageViewport = CreateWindowExW(
                0, L"LightDock_SettingsPage", L"",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPCHILDREN,
                pageOrigin.x, pageOrigin.y,
                pageRect.right - pageRect.left,
                pageHeight,
                hwnd, nullptr, instance_, this);

            if (settings_.pageViewport)
            {
                std::vector<HWND> children;
                EnumChildWindows(hwnd,
                    [](HWND child, LPARAM parameter) -> BOOL
                    {
                        auto* controls = reinterpret_cast<std::vector<HWND>*>(
                            parameter);
                        controls->push_back(child);
                        return TRUE;
                    }, reinterpret_cast<LPARAM>(&children));

                // Snapshot first, then reparent only direct dialog children.
                // Mutating the hierarchy during EnumChildWindows can skip
                // controls, while moving nested native control internals
                // (such as combo-box list windows) breaks those controls.
                for (HWND child : children)
                {
                    if (GetParent(child) != hwnd)
                    {
                        continue;
                    }

                    const int id = GetDlgCtrlID(child);
                    if (child == settings_.tabControl
                        || child == settings_.pageViewport
                        || id == kIdSave || id == kIdCancel
                        || id == kIdRestoreDefaults)
                    {
                        continue;
                    }

                    RECT rect{};
                    GetWindowRect(child, &rect);
                    MapWindowPoints(nullptr, hwnd,
                        reinterpret_cast<POINT*>(&rect), 2);

                    HANDLE tag = GetPropW(child, kDialogTabPageProperty);
                    const int page = tag
                        ? static_cast<int>(reinterpret_cast<INT_PTR>(tag)) - 1
                        : 0;
                    const int x = rect.left - pageOrigin.x;
                    const int y = rect.top - pageOrigin.y;

                    SetParent(child, settings_.pageViewport);
                    SetWindowPos(child, nullptr, x, y, 0, 0,
                        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

                    settings_.scrollChildren.push_back({child, x, y, page});
                    RECT childRect{};
                    GetWindowRect(child, &childRect);
                    MapWindowPoints(nullptr, settings_.pageViewport,
                        reinterpret_cast<POINT*>(&childRect), 2);
                    settings_.pageContentBottom[page] = std::max(
                        settings_.pageContentBottom[page],
                        static_cast<int>(childRect.bottom));
                }

                int pageTop[4] = {
                    std::numeric_limits<int>::max(),
                    std::numeric_limits<int>::max(),
                    std::numeric_limits<int>::max(),
                    std::numeric_limits<int>::max()};
                for (const SettingsEditor::ScrollChild& child
                     : settings_.scrollChildren)
                {
                    pageTop[child.page] = std::min(pageTop[child.page],
                                                   child.y);
                }
                for (int page = 0; page < 4; ++page)
                {
                    if (pageTop[page] == std::numeric_limits<int>::max())
                    {
                        continue;
                    }

                    // The authored coordinates start above the tab page for
                    // the behavior and bubble tabs. Preserve a clear top
                    // inset so their first controls are fully visible.
                    const int offset = std::max(0, S(8) - pageTop[page]);
                    if (offset == 0)
                    {
                        continue;
                    }

                    settings_.pageContentBottom[page] += offset;
                    for (SettingsEditor::ScrollChild& child
                         : settings_.scrollChildren)
                    {
                        if (child.page != page)
                        {
                            continue;
                        }

                        child.y += offset;
                        SetWindowPos(child.hwnd, nullptr, child.x, child.y,
                            0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                    }
                }

                ShowDialogTabPage(hwnd, 0);
                SetSettingsPageScroll(0);
            }
        }

        SetFocus(save);

        return 0;
    }

    case WM_NOTIFY:
    {
        const auto* header = reinterpret_cast<const NMHDR*>(lParam);
        if (header && header->idFrom == kIdSettingsTabs
            && header->code == TCN_SELCHANGE)
        {
            ShowDialogTabPage(hwnd,
                TabCtrl_GetCurSel(settings_.tabControl));
            SetSettingsPageScroll(0);
            // A tab switch changes which native controls are visible inside
            // the clipped scroll viewport. Repaint the viewport and dialog
            // synchronously so the new page is not left blank until input.
            RedrawWindow(settings_.pageViewport, nullptr, nullptr,
                RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME
                    | RDW_UPDATENOW);
            RedrawWindow(hwnd, nullptr, nullptr,
                RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
            return 0;
        }
        break;
    }

    case WM_HSCROLL:
        refreshLabels();
        previewSettings();
        return 0;

    case WM_DRAWITEM:
    {
        const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);

        if (draw->CtlID != kIdBgTop && draw->CtlID != kIdBgBottom)
        {
            break;
        }

        const COLORREF fill = draw->CtlID == kIdBgTop
            ? settings_.topColor
            : settings_.bottomColor;

        HBRUSH brush = CreateSolidBrush(fill);
        FillRect(draw->hDC, &draw->rcItem, brush);
        DeleteObject(brush);

        FrameRect(draw->hDC, &draw->rcItem, GetSysColorBrush(COLOR_3DDKSHADOW));

        if ((draw->itemState & ODS_FOCUS) != 0)
        {
            DrawFocusRect(draw->hDC, &draw->rcItem);
        }

        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case kIdRestoreDefaults:
        {
            const DockSettings defaults;
            auto setSlider = [](HWND control, int value)
            {
                if (control)
                {
                    SendMessageW(control, TBM_SETPOS, TRUE, value);
                }
            };

            SendMessageW(settings_.modeCombo, CB_SETCURSEL,
                         defaults.panelMode == PanelMode::Fixed ? 0
                         : defaults.panelMode == PanelMode::Elastic ? 1 : 2,
                         0);
            SendMessageW(settings_.edgeCombo, CB_SETCURSEL,
                         defaults.dockEdge == DockEdge::Top ? 1
                         : defaults.dockEdge == DockEdge::Left ? 2
                         : defaults.dockEdge == DockEdge::Right ? 3 : 0, 0);
            setSlider(settings_.overallScaleSlider,
                      static_cast<int>(std::lround(defaults.overallScale * 100.0f)));
            setSlider(settings_.spacingSlider, defaults.iconSpacing);
            setSlider(settings_.magnifySlider,
                      static_cast<int>(std::lround(defaults.magnification * 10.0f)));
            SendMessageW(settings_.autoHideBox, BM_SETCHECK,
                         defaults.autoHide ? BST_CHECKED : BST_UNCHECKED, 0);
            setSlider(settings_.autoHideDelaySlider, defaults.autoHideDelayMs);
            setSlider(settings_.autoHideSpeedSlider,
                      defaults.autoHideAnimationMs);
            setSlider(settings_.dockCornerSlider,
                      static_cast<int>(std::lround(defaults.cornerRadius)));
            setSlider(settings_.tooltipOpacitySlider,
                      static_cast<int>(std::lround(defaults.tooltipOpacity * 100.0f)));
            setSlider(settings_.tooltipFadeSlider,
                      static_cast<int>(std::lround(defaults.tooltipFadeSeconds * 1000.0f)));
            setSlider(settings_.tooltipScaleSlider,
                      static_cast<int>(std::lround(defaults.tooltipScale * 100.0f)));
            setSlider(settings_.tooltipCornerSlider,
                      static_cast<int>(std::lround(defaults.tooltipCornerRadius)));
            setSlider(settings_.cornerSlider,
                      static_cast<int>(std::lround(defaults.backdrop.cornerRadius)));
            setSlider(settings_.scaleSlider,
                      static_cast<int>(std::lround(defaults.backdrop.iconScale * 100.0f)));
            setSlider(settings_.opacitySlider,
                      static_cast<int>(std::lround(defaults.backdrop.opacity * 100.0f)));
            setSlider(settings_.strokeWidthSlider,
                      static_cast<int>(std::lround(defaults.backdrop.strokeWidth * 10.0f)));
            setSlider(settings_.strokeOpacitySlider,
                      static_cast<int>(std::lround(defaults.backdrop.strokeOpacity * 100.0f)));
            setSlider(settings_.bgOpacitySlider,
                      static_cast<int>(std::lround(defaults.backgroundOpacity * 100.0f)));

            settings_.topColor = RGB(255, 255, 255);
            settings_.bottomColor = RGB(235, 235, 235);
            SendMessageW(settings_.customBox, BM_SETCHECK,
                         defaults.backgroundBottom.empty()
                             ? BST_UNCHECKED : BST_CHECKED, 0);
            refreshLabels();
            syncBackgroundControls();
            previewSettings();
            return 0;
        }

        case kIdMode:
        case kIdDockEdge:
            if (HIWORD(wParam) == CBN_SELCHANGE)
            {
                previewSettings();
            }
            return 0;

        case kIdAutoHide:
            previewSettings();
            return 0;

        case kIdBgCustom:
            syncBackgroundControls();
            previewSettings();
            return 0;

        case kIdBgTop:
        case kIdBgBottom:
        {
            static COLORREF customColors[16] = {};

            CHOOSECOLORW chooser{};
            chooser.lStructSize = sizeof(chooser);
            chooser.hwndOwner = hwnd;
            chooser.rgbResult = (LOWORD(wParam) == kIdBgTop)
                ? settings_.topColor
                : settings_.bottomColor;
            chooser.lpCustColors = customColors;
            chooser.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR;

            if (ChooseColorW(&chooser))
            {
                if (LOWORD(wParam) == kIdBgTop)
                {
                    settings_.topColor = chooser.rgbResult;
                }
                else
                {
                    settings_.bottomColor = chooser.rgbResult;
                }

                syncBackgroundControls();
                previewSettings();
            }

            return 0;
        }

        case kIdSave:
        {
            // The live preview already wrote every control into the config;
            // persist it and close.
            previewSettings();
            settings_.saved = true;
            settings_.closed = true;
            return 0;
        }

        case kIdCancel:
            // Roll back whatever the live preview already applied.
            if (!settings_.saved)
            {
                restoreSettings();
            }

            settings_.closed = true;
            return 0;
        }

        return 0;

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    {
        // Blend labels and edits into the white dialog background instead of
        // the themed control's gray filler.
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkMode(dc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }

    case WM_CLOSE:
        if (!settings_.saved)
        {
            restoreSettings();
        }

        settings_.closed = true;
        return 0;

    case WM_DESTROY:
        if (!settings_.saved)
        {
            restoreSettings();
        }

        settings_.closed = true;
        return 0;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK App::SettingsViewportProc(HWND hwnd, UINT message,
                                            WPARAM wParam, LPARAM lParam)
{
    App* self = nullptr;
    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<App*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(self));
    }
    else
    {
        self = reinterpret_cast<App*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self)
    {
        const LRESULT result = self->HandleSettingsViewportMessage(
            hwnd, message, wParam, lParam);
        if (result != std::numeric_limits<LRESULT>::min())
        {
            return result;
        }
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT App::HandleSettingsViewportMessage(HWND hwnd, UINT message,
                                            WPARAM wParam, LPARAM lParam)
{
    if (hwnd != settings_.pageViewport)
    {
        return std::numeric_limits<LRESULT>::min();
    }

    switch (message)
    {
    case WM_COMMAND:
    case WM_HSCROLL:
    case WM_NOTIFY:
    case WM_DRAWITEM:
    case WM_MEASUREITEM:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
        return SendMessageW(settings_.dialog, message, wParam, lParam);
    default:
        break;
    }

    if (message == WM_MOUSEWHEEL)
    {
        const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        SetSettingsPageScroll(settings_.pageScrollPosition
            - (delta / WHEEL_DELTA) * 48);
        return 0;
    }

    if (message != WM_VSCROLL)
    {
        return std::numeric_limits<LRESULT>::min();
    }

    int position = settings_.pageScrollPosition;
    const int page = settings_.tabControl
        ? std::max(0, TabCtrl_GetCurSel(settings_.tabControl)) : 0;
    RECT client{};
    GetClientRect(hwnd, &client);
    const int maxPosition = std::max(
        0, settings_.pageContentBottom[page]
            - static_cast<int>(client.bottom - client.top));

    switch (LOWORD(wParam))
    {
    case SB_LINEUP:       position -= 24; break;
    case SB_LINEDOWN:     position += 24; break;
    case SB_PAGEUP:       position -= client.bottom - client.top; break;
    case SB_PAGEDOWN:     position += client.bottom - client.top; break;
    case SB_TOP:          position = 0; break;
    case SB_BOTTOM:       position = maxPosition; break;
    case SB_THUMBPOSITION:
    case SB_THUMBTRACK:
    {
        SCROLLINFO info{};
        info.cbSize = sizeof(info);
        info.fMask = SIF_TRACKPOS;
        GetScrollInfo(hwnd, SB_VERT, &info);
        position = info.nTrackPos;
        break;
    }
    default:
        return 0;
    }

    SetSettingsPageScroll(std::clamp(position, 0, maxPosition));
    return 0;
}

void App::SetSettingsPageScroll(int position)
{
    if (!settings_.pageViewport || !settings_.tabControl)
    {
        return;
    }

    const int page = std::clamp(TabCtrl_GetCurSel(settings_.tabControl), 0, 3);
    RECT client{};
    GetClientRect(settings_.pageViewport, &client);
    const int pageHeight = static_cast<int>(client.bottom - client.top);
    const int maximum = std::max(
        0, settings_.pageContentBottom[page] - pageHeight);
    settings_.pageScrollPosition = std::clamp(position, 0, maximum);

    SCROLLINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    info.nMin = 0;
    info.nMax = std::max(0, settings_.pageContentBottom[page] - 1);
    info.nPage = static_cast<UINT>(std::max(0, pageHeight));
    info.nPos = settings_.pageScrollPosition;
    SetScrollInfo(settings_.pageViewport, SB_VERT, &info, TRUE);
    ShowScrollBar(settings_.pageViewport, SB_VERT, maximum > 0);

    for (const SettingsEditor::ScrollChild& child : settings_.scrollChildren)
    {
        if (child.page == page)
        {
            SetWindowPos(child.hwnd, nullptr, child.x,
                child.y - settings_.pageScrollPosition, 0, 0,
                SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
        }
    }

    // Scrolling moves many native controls at once. Suppress their individual
    // intermediate paints above, then invalidate both the exposed background
    // and every child so no stale glyphs or slider fragments remain behind.
    RedrawWindow(settings_.pageViewport, nullptr, nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME
            | RDW_UPDATENOW);
}

LRESULT CALLBACK App::EditorProc(HWND hwnd, UINT message,
                                 WPARAM wParam, LPARAM lParam)
{
    App* self = nullptr;

    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<App*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    else
    {
        self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (!self)
    {
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    return self->HandleEditorMessage(hwnd, message, wParam, lParam);
}

LRESULT App::HandleEditorMessage(HWND hwnd, UINT message,
                                 WPARAM wParam, LPARAM lParam)
{
    enum : int
    {
        kIdName = 101,
        kIdPath,
        kIdArguments,
        kIdIcon,
        kIdBrowsePath,
        kIdBrowseIcon,

        kIdEnable = 111,
        kIdScale,
        kIdSecond,
        kIdTop,
        kIdBottom,
        kIdStrokeOverride = 116,
        kIdStrokeColor,
        kIdStrokeOpacityOverride,
        kIdStrokeOpacitySlider,
        kIdPlateOpacityOverride = 140,
        kIdPlateOpacitySlider,

        kIdSave = 121,
        kIdCancel,
        kIdRestoreDefaults,
        kIdEditorTabs = 131,
    };

    auto sliderValue = [](HWND slider) -> int
    {
        return slider ? static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0))
                      : 0;
    };

    auto windowText = [](HWND control) -> std::wstring
    {
        if (!control)
        {
            return {};
        }

        const int length = GetWindowTextLengthW(control);
        std::wstring value(static_cast<size_t>(length) + 1, L'\0');
        const int written = GetWindowTextW(control, value.data(), length + 1);
        value.resize(written > 0 ? static_cast<size_t>(written) : 0);
        return value;
    };

    auto hexOf = [](COLORREF color) -> std::wstring
    {
        wchar_t text[16];
        swprintf(text, 16, L"#%02X%02X%02X",
                 GetRValue(color), GetGValue(color), GetBValue(color));
        return std::wstring(text);
    };

    /// The automatically derived gradient partner of a picked colour.
    auto derivedColor = [](COLORREF top) -> COLORREF
    {
        return RGB(GetRValue(top) * 92 / 100,
                   GetGValue(top) * 92 / 100,
                   GetBValue(top) * 92 / 100);
    };

    /// Repaints the two swatches and their hex captions.
    auto refreshSwatches = [&]()
    {
        SetWindowTextW(editor_.topHex, hexOf(editor_.topColor).c_str());

        const std::wstring bottomHex = editor_.secondBox
            && SendMessageW(editor_.secondBox, BM_GETCHECK, 0, 0) == BST_CHECKED
            ? hexOf(editor_.bottomColor)
            : hexOf(derivedColor(editor_.topColor)) + L"（自动）";
        SetWindowTextW(editor_.bottomHex, bottomHex.c_str());
        SetWindowTextW(editor_.strokeHex,
            editor_.strokeOverrideBox
                && SendMessageW(editor_.strokeOverrideBox, BM_GETCHECK, 0, 0)
                   == BST_CHECKED
                ? hexOf(editor_.strokeColor).c_str() : L"#FFFFFF（默认）");

        if (editor_.topSwatch)
        {
            InvalidateRect(editor_.topSwatch, nullptr, TRUE);
        }

        if (editor_.bottomSwatch)
        {
            InvalidateRect(editor_.bottomSwatch, nullptr, TRUE);
        }
        if (editor_.strokeSwatch)
        {
            InvalidateRect(editor_.strokeSwatch, nullptr, TRUE);
        }
    };

    /// Plate controls only make sense while the plate is enabled, and the
    /// second colour picker only while the user opted out of the automatic
    /// gradient partner.
    auto syncAppearance = [&]()
    {
        const bool on = editor_.enableBox
            && SendMessageW(editor_.enableBox, BM_GETCHECK, 0, 0) == BST_CHECKED;
        const bool second = editor_.secondBox
            && SendMessageW(editor_.secondBox, BM_GETCHECK, 0, 0) == BST_CHECKED;
        const bool stroke = editor_.strokeOverrideBox
            && SendMessageW(editor_.strokeOverrideBox, BM_GETCHECK, 0, 0)
               == BST_CHECKED;
        const bool strokeOpacity = editor_.strokeOpacityBox
            && SendMessageW(editor_.strokeOpacityBox, BM_GETCHECK, 0, 0)
               == BST_CHECKED;
        const bool plateOpacity = editor_.opacityBox
            && SendMessageW(editor_.opacityBox, BM_GETCHECK, 0, 0)
               == BST_CHECKED;

        HWND plateControls[] =
        {
            editor_.scaleSlider,
            editor_.topSwatch,
            editor_.secondBox,
            editor_.opacityBox,
        };

        for (HWND control : plateControls)
        {
            if (control)
            {
                EnableWindow(control, on ? TRUE : FALSE);
            }
        }

        if (editor_.bottomSwatch)
        {
            EnableWindow(editor_.bottomSwatch, on && second ? TRUE : FALSE);
        }
        if (editor_.strokeOverrideBox)
        {
            EnableWindow(editor_.strokeOverrideBox, on ? TRUE : FALSE);
        }
        if (editor_.strokeSwatch)
        {
            EnableWindow(editor_.strokeSwatch,
                         on && stroke ? TRUE : FALSE);
        }
        if (editor_.strokeOpacityBox)
        {
            EnableWindow(editor_.strokeOpacityBox, on ? TRUE : FALSE);
        }
        if (editor_.strokeOpacitySlider)
        {
            EnableWindow(editor_.strokeOpacitySlider,
                         on && strokeOpacity ? TRUE : FALSE);
        }
        if (editor_.opacitySlider)
        {
            EnableWindow(editor_.opacitySlider,
                         on && plateOpacity ? TRUE : FALSE);
        }
        if (editor_.opacityLabel)
        {
            EnableWindow(editor_.opacityLabel,
                         on && plateOpacity ? TRUE : FALSE);
        }
    };

    auto refreshLabels = [&]()
    {
        wchar_t text[48];

        swprintf(text, 48, L"%d%%", sliderValue(editor_.scaleSlider));
        SetWindowTextW(editor_.scaleLabel, text);
        swprintf(text, 48, L"%d%%", sliderValue(editor_.opacitySlider));
        SetWindowTextW(editor_.opacityLabel, text);
        swprintf(text, 48, L"%d%%",
                 sliderValue(editor_.strokeOpacitySlider));
        SetWindowTextW(editor_.strokeOpacityLabel, text);
    };

    /// Live preview: pushes the current control values straight into the item
    /// being edited and repaints the dock, so colour and ratio changes show
    /// up immediately instead of only after pressing 保存. Cancelling rolls
    /// back to the snapshot taken in WM_CREATE.
    auto previewPlate = [&]()
    {
        if (!editor_.item)
        {
            return;
        }

        PlateStyle& plate = editor_.item->plate;

        plate.enabled = editor_.enableBox
            && SendMessageW(editor_.enableBox, BM_GETCHECK, 0, 0)
               == BST_CHECKED;

        plate.iconScale =
            static_cast<float>(sliderValue(editor_.scaleSlider)) / 100.0f;

        const bool opacityOverride = editor_.opacityBox
            && SendMessageW(editor_.opacityBox, BM_GETCHECK, 0, 0)
               == BST_CHECKED;
        plate.opacity = opacityOverride
            ? static_cast<float>(sliderValue(editor_.opacitySlider)) / 100.0f
            : -1.0f;

        plate.customBottom = editor_.secondBox
            && SendMessageW(editor_.secondBox, BM_GETCHECK, 0, 0)
               == BST_CHECKED;

        plate.top = hexOf(editor_.topColor);
        plate.bottom = hexOf(editor_.bottomColor);
        const bool strokeOverride = editor_.strokeOverrideBox
            && SendMessageW(editor_.strokeOverrideBox, BM_GETCHECK, 0, 0)
               == BST_CHECKED;
        plate.strokeColor = strokeOverride ? hexOf(editor_.strokeColor)
                                           : std::wstring();
        const bool strokeOpacityOverride = editor_.strokeOpacityBox
            && SendMessageW(editor_.strokeOpacityBox, BM_GETCHECK, 0, 0)
               == BST_CHECKED;
        plate.strokeOpacity = strokeOpacityOverride
            ? static_cast<float>(sliderValue(editor_.strokeOpacitySlider)) / 100.0f
            : -1.0f;

        Render();
    };

    /// Puts back the plate and icon the entry had before the dialog
    /// opened. Both are previewed directly on the dock.
    auto restorePlate = [&]()
    {
        if (editor_.item)
        {
            editor_.item->plate = editor_.originalPlate;
            editor_.item->iconSource = editor_.originalIconSource;
            editor_.item->icon.Reset();
            Render();
        }
    };

    switch (message)
    {
    case WM_NCCREATE:
        // Must fall through to DefWindowProc, otherwise the window title
        // passed to CreateWindowExW is never stored and the caption is blank.
        return DefWindowProcW(hwnd, message, wParam, lParam);

    case WM_CREATE:
    {
        if (!hwnd || !editor_.item)
        {
            return -1;
        }

        // Snapshot for the live preview rollback on cancel.
        editor_.originalPlate = editor_.item->plate;
        editor_.originalIconSource = editor_.item->iconSource;

        INITCOMMONCONTROLSEX controls{};
        controls.dwSize = sizeof(controls);
        controls.dwICC = ICC_BAR_CLASSES | ICC_TAB_CLASSES;
        InitCommonControlsEx(&controls);

        // Scale every coordinate for the dialog's monitor DPI.
        const float scale = DialogDpi(hwnd) / 96.0f;

        auto S = [scale](int value) -> int
        {
            return static_cast<int>(std::lround(value * scale));
        };

        auto setFont = [](HWND control, bool bold = false)
        {
            HFONT font = DialogFont(bold);
            if (control && font)
            {
                SendMessageW(control, WM_SETFONT,
                             reinterpret_cast<WPARAM>(font), TRUE);
            }
        };

        const int rows[] = {22, 60, 98, 136};

        auto makeLabel = [&](const wchar_t* text, int x, int y, int w,
                             bool bold = false)
        {
            text = UiText(text);
            HWND control = CreateWindowExW(
                0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                S(x), S(y + 5), S(w), S(20), hwnd, nullptr, instance_, nullptr);
            setFont(control, bold);
            return control;
        };

        auto makeEdit = [&](const std::wstring& value, int id, int x, int y, int w)
        {
            HWND control = CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", value.c_str(),
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
                S(x), S(y), S(w), S(24),
                hwnd, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
                instance_, nullptr);
            setFont(control);
            return control;
        };

        auto makeButton = [&](const wchar_t* text, int id, int x, int y, int w)
        {
            text = UiText(text);
            HWND control = CreateWindowExW(
                0, L"BUTTON", text,
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                S(x), S(y), S(w), S(27),
                hwnd, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
                instance_, nullptr);
            setFont(control);
            return control;
        };

        auto makeCheck = [&](const wchar_t* text, bool checked, int id,
                             int x, int y, int w)
        {
            text = UiText(text);
            HWND control = CreateWindowExW(
                0, L"BUTTON", text,
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
                S(x), S(y), S(w), S(22),
                hwnd, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
                instance_, nullptr);

            if (control)
            {
                SendMessageW(control, BM_SETCHECK,
                             checked ? BST_CHECKED : BST_UNCHECKED, 0);
                setFont(control);
            }

            return control;
        };

        auto makeSlider = [&](int id, int x, int y, int w,
                              int lo, int hi, int value)
        {
            HWND control = CreateWindowExW(
                0, TRACKBAR_CLASSW, nullptr,
                WS_CHILD | WS_VISIBLE | TBS_HORZ | WS_TABSTOP,
                S(x), S(y), S(w), S(28),
                hwnd, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
                instance_, nullptr);

            if (control)
            {
                SendMessageW(control, TBM_SETRANGE, TRUE, MAKELPARAM(lo, hi));
                SendMessageW(control, TBM_SETPOS, TRUE, value);
                setFont(control);
            }

            return control;
        };

        // --- shortcut properties ------------------------------------------
        makeLabel(L"名称", 20, rows[0], 70);
        makeLabel(L"程序", 20, rows[1], 70);
        makeLabel(L"附加命令", 20, rows[2], 70);
        makeLabel(L"图标文件", 20, rows[3], 70);

        editor_.nameEdit = makeEdit(editor_.item->name, kIdName, 96, rows[0], 270);
        editor_.pathEdit = makeEdit(editor_.item->targetPath, kIdPath, 96, rows[1], 270);
        editor_.argumentsEdit = makeEdit(editor_.item->arguments, kIdArguments,
                                         96, rows[2], 270);
        editor_.iconEdit = makeEdit(editor_.item->iconFile, kIdIcon, 96, rows[3], 270);

        makeButton(L"浏览...", kIdBrowsePath, 376, rows[1] - 2, 84);
        makeButton(L"浏览...", kIdBrowseIcon, 376, rows[3] - 2, 84);

        // Group divider: application/arguments above, icon selection below.
        CreateWindowExW(
            0, L"STATIC", nullptr, WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
            S(20), S(126), S(440), S(2), hwnd, nullptr, instance_, nullptr);

        // --- per icon plate -------------------------------------------------
        const PlateStyle& own = editor_.item->plate;
        const IconBackdrop& globalBackdrop = config_.settings.backdrop;

        editor_.enableBox = makeCheck(L"启用圆角底板", own.enabled,
                                      kIdEnable, 20, 190, 200);

        makeLabel(L"图标在底板内的比例", 20, 228, 130);

        const float effectiveScale = own.iconScale > 0.0f
            ? own.iconScale
            : globalBackdrop.iconScale;

        editor_.scaleSlider = makeSlider(
            kIdScale, 158, 224, 140, 50, 150,
            static_cast<int>(std::lround(effectiveScale * 100.0f)));
        editor_.scaleLabel = makeLabel(L"", 306, 228, 60);

        editor_.opacityBox = makeCheck(
            L"自定义底板透明度", own.opacity >= 0.0f,
            kIdPlateOpacityOverride, 20, 260, 160);
        makeLabel(L"底板透明度", 20, 296, 110);
        const float effectiveOpacity = own.opacity >= 0.0f
            ? own.opacity : config_.settings.backdrop.opacity;
        editor_.opacitySlider = makeSlider(
            kIdPlateOpacitySlider, 158, 292, 140, 0, 100,
            static_cast<int>(std::lround(effectiveOpacity * 100.0f)));
        editor_.opacityLabel = makeLabel(L"", 306, 296, 60);

        // Group divider: plate size/opacity above, colour treatment below.
        CreateWindowExW(
            0, L"STATIC", nullptr, WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
            S(20), S(326), S(440), S(2), hwnd, nullptr, instance_, nullptr);

        // --- colour picks ----------------------------------------------------
        // Owner drawn swatches: the control itself previews the colour and
        // clicking it opens the system colour picker.
        auto makeSwatch = [&](int id, int x, int y)
        {
            return CreateWindowExW(
                0, L"BUTTON", nullptr,
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
                S(x), S(y), S(64), S(24),
                hwnd, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
                instance_, nullptr);
        };

        auto colorRefOf = [](const std::wstring& hex,
                             COLORREF fallback) -> COLORREF
        {
            float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
            if (ParseHexColor(hex, r, g, b, a))
            {
                return RGB(static_cast<int>(r * 255.0f + 0.5f),
                           static_cast<int>(g * 255.0f + 0.5f),
                           static_cast<int>(b * 255.0f + 0.5f));
            }

            return fallback;
        };

        editor_.topColor = colorRefOf(own.top, RGB(59, 66, 82));
        editor_.bottomColor = own.customBottom
            ? colorRefOf(own.bottom, derivedColor(editor_.topColor))
            : derivedColor(editor_.topColor);
        editor_.strokeColor = colorRefOf(own.strokeColor, RGB(255, 255, 255));

        makeLabel(L"顶部颜色", 20, 342, 90);
        editor_.topSwatch = makeSwatch(kIdTop, 158, 338);
        editor_.topHex = makeLabel(L"", 234, 342, 170);

        editor_.secondBox = makeCheck(L"自定义第二颜色（渐变）", own.customBottom,
                                      kIdSecond, 20, 378, 240);

        makeLabel(L"底部颜色", 20, 414, 90);
        editor_.bottomSwatch = makeSwatch(kIdBottom, 158, 410);
        editor_.bottomHex = makeLabel(L"", 234, 414, 190);

        // Group divider: fill colours above, rim controls below.
        CreateWindowExW(
            0, L"STATIC", nullptr, WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
            S(20), S(442), S(440), S(2), hwnd, nullptr, instance_, nullptr);

        editor_.strokeOverrideBox = makeCheck(
            L"自定义描边颜色", !own.strokeColor.empty(),
            kIdStrokeOverride, 20, 456, 130);
        editor_.strokeSwatch = makeSwatch(kIdStrokeColor, 158, 454);
        editor_.strokeHex = makeLabel(L"", 234, 458, 190);

        editor_.strokeOpacityBox = makeCheck(
            L"自定义描边透明度", own.strokeOpacity >= 0.0f,
            kIdStrokeOpacityOverride, 20, 492, 145);
        makeLabel(L"描边不透明度", 20, 528, 110);
        const float strokeOpacity = own.strokeOpacity >= 0.0f
            ? own.strokeOpacity : config_.settings.backdrop.strokeOpacity;
        editor_.strokeOpacitySlider = makeSlider(
            kIdStrokeOpacitySlider, 158, 524, 140, 0, 100,
            static_cast<int>(std::lround(strokeOpacity * 100.0f)));
        editor_.strokeOpacityLabel = makeLabel(L"", 306, 528, 60);

        refreshLabels();
        syncAppearance();
        refreshSwatches();

        // --- buttons --------------------------------------------------------
        makeButton(L"保存", kIdSave, 300, 508, 84);
        makeButton(L"取消", kIdCancel, 390, 508, 84);

            HWND tabs = CreateWindowExW(
                0, WC_TABCONTROLW, L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | TCS_TABS,
                S(12), S(6), S(460), S(490), hwnd,
                reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdEditorTabs)),
                instance_, nullptr);
        setFont(tabs);
        if (tabs)
        {
            TCITEMW tab{};
            tab.mask = TCIF_TEXT;
            tab.pszText = const_cast<wchar_t*>(UiText(L"快捷方式"));
            TabCtrl_InsertItem(tabs, 0, &tab);
            tab.pszText = const_cast<wchar_t*>(UiText(L"图标外观"));
            TabCtrl_InsertItem(tabs, 1, &tab);
            TabCtrl_SetCurSel(tabs, 0);

            TagDialogChildrenContext tagContext{
                hwnd, kIdEditorTabs, kIdSave, kIdCancel,
                S(176), S(650), 1, S(38), -S(126), 0};
            EnumChildWindows(hwnd, TagDialogChildren,
                             reinterpret_cast<LPARAM>(&tagContext));
            ShowDialogTabPage(hwnd, 0);
            SetWindowPos(tabs, HWND_BOTTOM, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            makeButton(L"恢复默认值", kIdRestoreDefaults, 20, 508, 120);
        }

        SetFocus(editor_.nameEdit);

        return 0;
    }

    case WM_HSCROLL:
        refreshLabels();
        previewPlate();
        return 0;

    case WM_NOTIFY:
    {
        const auto* header = reinterpret_cast<const NMHDR*>(lParam);
        if (header && header->idFrom == kIdEditorTabs
            && header->code == TCN_SELCHANGE)
        {
            ShowDialogTabPage(hwnd, TabCtrl_GetCurSel(header->hwndFrom));
            return 0;
        }
        break;
    }

    case WM_DRAWITEM:
    {
        const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);

        if (draw->CtlID != kIdTop && draw->CtlID != kIdBottom
            && draw->CtlID != kIdStrokeColor)
        {
            break;
        }

        const bool disabled = (draw->itemState & ODS_DISABLED) != 0;
        const COLORREF fill = disabled
            ? GetSysColor(COLOR_BTNFACE)
            : (draw->CtlID == kIdTop ? editor_.topColor
               : draw->CtlID == kIdBottom ? editor_.bottomColor
                                          : editor_.strokeColor);

        HBRUSH brush = CreateSolidBrush(fill);
        FillRect(draw->hDC, &draw->rcItem, brush);
        DeleteObject(brush);

        FrameRect(draw->hDC, &draw->rcItem,
                  GetSysColorBrush(disabled ? COLOR_GRAYTEXT : COLOR_3DDKSHADOW));

        if ((draw->itemState & ODS_FOCUS) != 0)
        {
            DrawFocusRect(draw->hDC, &draw->rcItem);
        }

        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case kIdRestoreDefaults:
            SendMessageW(editor_.enableBox, BM_SETCHECK, BST_CHECKED, 0);
            SendMessageW(editor_.scaleSlider, TBM_SETPOS, TRUE,
                         static_cast<LPARAM>(std::lround(
                             config_.settings.backdrop.iconScale * 100.0f)));
            SendMessageW(editor_.opacityBox, BM_SETCHECK,
                         BST_UNCHECKED, 0);
            SendMessageW(editor_.opacitySlider, TBM_SETPOS, TRUE,
                         static_cast<LPARAM>(std::lround(
                             config_.settings.backdrop.opacity * 100.0f)));
            SendMessageW(editor_.secondBox, BM_SETCHECK, BST_UNCHECKED, 0);
            SendMessageW(editor_.strokeOverrideBox, BM_SETCHECK,
                         BST_UNCHECKED, 0);
            SendMessageW(editor_.strokeOpacityBox, BM_SETCHECK,
                         BST_UNCHECKED, 0);
            SendMessageW(editor_.strokeOpacitySlider, TBM_SETPOS, TRUE,
                         static_cast<LPARAM>(std::lround(
                             config_.settings.backdrop.strokeOpacity * 100.0f)));
            editor_.topColor = RGB(255, 255, 255);
            editor_.bottomColor = RGB(235, 235, 235);
            editor_.strokeColor = RGB(255, 255, 255);
            refreshLabels();
            syncAppearance();
            refreshSwatches();
            previewPlate();
            return 0;

        case kIdEnable:
        case kIdPlateOpacityOverride:
        case kIdSecond:
        case kIdStrokeOverride:
        case kIdStrokeOpacityOverride:
            syncAppearance();
            refreshSwatches();
            previewPlate();
            return 0;

        case kIdTop:
        case kIdBottom:
        case kIdStrokeColor:
        {
            // System colour picker, seeded with the current colour.
            static COLORREF customColors[16] = {};

            CHOOSECOLORW chooser{};
            chooser.lStructSize = sizeof(chooser);
            chooser.hwndOwner = hwnd;
            chooser.rgbResult = LOWORD(wParam) == kIdTop ? editor_.topColor
                : LOWORD(wParam) == kIdBottom ? editor_.bottomColor
                                              : editor_.strokeColor;
            chooser.lpCustColors = customColors;
            chooser.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR;

            if (ChooseColorW(&chooser))
            {
                if (LOWORD(wParam) == kIdTop)
                {
                    editor_.topColor = chooser.rgbResult;

                    // While the automatic partner is active the bottom
                    // colour follows the top pick.
                    if (!editor_.secondBox
                        || SendMessageW(editor_.secondBox, BM_GETCHECK, 0, 0)
                           != BST_CHECKED)
                    {
                        editor_.bottomColor = derivedColor(editor_.topColor);
                    }
                }
                else if (LOWORD(wParam) == kIdBottom)
                {
                    editor_.bottomColor = chooser.rgbResult;
                }
                else
                {
                    editor_.strokeColor = chooser.rgbResult;
                }

                refreshSwatches();
                previewPlate();
            }

            return 0;
        }

        case kIdBrowsePath:
        {
            const COMDLG_FILTERSPEC filters[] =
            {
                {L"应用程序 (*.exe; *.lnk)", L"*.exe;*.lnk"},
                {L"所有文件 (*.*)", L"*.*"},
            };

            const std::wstring path =
                PickFile(L"选择程序", filters, ARRAYSIZE(filters));

            if (path.empty())
            {
                return 0;
            }

            SetWindowTextW(editor_.pathEdit, path.c_str());

            // Refresh name and icon from the new target, like a shortcut.
            AppInfo info = icons_.Inspect(path, 256);

            if (!info.name.empty())
            {
                SetWindowTextW(editor_.nameEdit, info.name.c_str());
            }

            if (info.icon)
            {
                editor_.pendingIcon = info.icon;
                editor_.pendingIconPath.clear();
                SetWindowTextW(editor_.iconEdit, L"(来自所选程序)");

                editor_.item->iconSource = editor_.pendingIcon;
                editor_.item->icon.Reset();
                Render();
            }

            return 0;
        }

        case kIdBrowseIcon:
        {
            const COMDLG_FILTERSPEC filters[] =
            {
                {L"图片或程序 (*.png; *.exe; *.lnk)", L"*.png;*.exe;*.lnk"},
                {L"图片 (*.png)", L"*.png"},
                {L"所有文件 (*.*)", L"*.*"},
            };

            const std::wstring path =
                PickFile(L"选择图标", filters, ARRAYSIZE(filters));

            if (path.empty())
            {
                return 0;
            }

            ComPtr<IWICBitmap> icon;

            if (GetFileExtension(path) == L".png")
            {
                icon = icons_.LoadFromCache(path);
            }
            else
            {
                AppInfo info = icons_.Inspect(path, 256);
                icon = info.icon;
            }

            if (icon)
            {
                editor_.pendingIcon = icon;
                editor_.pendingIconPath = path;
                SetWindowTextW(editor_.iconEdit, GetFileName(path).c_str());

                editor_.item->iconSource = editor_.pendingIcon;
                editor_.item->icon.Reset();
                Render();
            }

            return 0;
        }

        case kIdSave:
        {
            if (!editor_.item)
            {
                editor_.closed = true;
                return 0;
            }

            DockItem& item = *editor_.item;

            std::wstring name = windowText(editor_.nameEdit);
            std::wstring path = windowText(editor_.pathEdit);
            std::wstring arguments = windowText(editor_.argumentsEdit);

            if (!path.empty() && path != item.targetPath)
            {
                AppInfo info = icons_.Inspect(path, 256);

                item.targetPath = path;
                item.resolvedPath =
                    info.resolvedPath.empty() ? path : info.resolvedPath;
                item.processName = GetFileName(item.resolvedPath);

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
                name = GetFileStem(item.resolvedPath);
            }

            item.name = name;
            item.arguments = arguments;

            if (editor_.pendingIcon)
            {
                item.iconSource = editor_.pendingIcon;
                item.icon.Reset();

                // Persist every chosen icon into LightDock's own cache,
                // including custom PNGs. Otherwise a PNG previews correctly
                // for this session but the old cached icon returns after the
                // next launch.
                icons_.SaveToCache(
                    editor_.pendingIcon.Get(),
                    GetIconCacheDir() + L"\\" + item.iconFile);
            }

            // --- plate look --------------------------------------------------
            PlateStyle& plate = item.plate;

            plate.enabled = editor_.enableBox
                && SendMessageW(editor_.enableBox, BM_GETCHECK, 0, 0)
                   == BST_CHECKED;

            // Pin the ratio only when it deviates from the global default;
            // matching values stay unpinned so global changes keep working.
            const float chosen =
                static_cast<float>(sliderValue(editor_.scaleSlider)) / 100.0f;

            plate.iconScale = std::abs(chosen - config_.settings.backdrop.iconScale)
                < 0.005f ? 0.0f : chosen;

            plate.opacity = editor_.opacityBox
                && SendMessageW(editor_.opacityBox, BM_GETCHECK, 0, 0)
                   == BST_CHECKED
                ? static_cast<float>(sliderValue(editor_.opacitySlider)) / 100.0f
                : -1.0f;

            plate.customBottom = editor_.secondBox
                && SendMessageW(editor_.secondBox, BM_GETCHECK, 0, 0)
                   == BST_CHECKED;

            wchar_t hex[16];

            swprintf(hex, 16, L"#%02X%02X%02X", GetRValue(editor_.topColor),
                     GetGValue(editor_.topColor), GetBValue(editor_.topColor));
            plate.top = hex;

            swprintf(hex, 16, L"#%02X%02X%02X",
                     GetRValue(editor_.bottomColor),
                     GetGValue(editor_.bottomColor),
                     GetBValue(editor_.bottomColor));
            plate.bottom = hex;

            plate.strokeColor = editor_.strokeOverrideBox
                && SendMessageW(editor_.strokeOverrideBox, BM_GETCHECK, 0, 0)
                   == BST_CHECKED
                ? hexOf(editor_.strokeColor) : std::wstring();
            plate.strokeOpacity = editor_.strokeOpacityBox
                && SendMessageW(editor_.strokeOpacityBox, BM_GETCHECK, 0, 0)
                   == BST_CHECKED
                ? static_cast<float>(sliderValue(
                    editor_.strokeOpacitySlider)) / 100.0f
                : -1.0f;

            editor_.saved = true;
            editor_.closed = true;
            return 0;
        }

        case kIdCancel:
            restorePlate();
            editor_.closed = true;
            return 0;
        }

        return 0;

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    {
        // Blend labels and edits into the white dialog background instead of
        // the themed control's gray filler.
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkMode(dc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }

    case WM_CLOSE:
        // Rolling back the live preview: the dock goes back to how it looked
        // before the dialog opened.
        if (!editor_.saved)
        {
            restorePlate();
        }

        editor_.closed = true;
        return 0;

    case WM_DESTROY:
        if (!editor_.saved)
        {
            restorePlate();
        }

        editor_.closed = true;
        return 0;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace ld

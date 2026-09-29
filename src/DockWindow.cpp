#include "DockWindow.h"

namespace ld
{

namespace
{

const wchar_t kWindowClassName[] = L"LightDock_Window_Class";
const wchar_t kHideIndicatorClassName[] = L"LightDock_Hide_Indicator";
constexpr UINT kAppBarCallback = WM_APP + 8;

LRESULT CALLBACK HideIndicatorWndProc(HWND hwnd, UINT message,
                                      WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCHITTEST)
    {
        return HTTRANSPARENT;
    }
    if (message == WM_MOUSEACTIVATE)
    {
        return MA_NOACTIVATE;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

using FnSetProcessDpiAwarenessContext = BOOL(WINAPI*)(HANDLE);
using FnGetDpiForWindow = UINT(WINAPI*)(HWND);

} // namespace

DockWindow::~DockWindow()
{
    Destroy();
}
void DockWindow::EnablePerMonitorDpiAwareness()
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32)
    {
        user32 = LoadLibraryW(L"user32.dll");
    }

    if (user32)
    {
        auto setContext = reinterpret_cast<FnSetProcessDpiAwarenessContext>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"));

        if (setContext)
        {
            // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
            const HANDLE context =
                reinterpret_cast<HANDLE>(static_cast<intptr_t>(-4));

            setContext(context);
            return;
        }
    }

    // Windows 7 / early Windows 10 fallback.
    SetProcessDPIAware();
}

int DockWindow::QueryDpi(HWND hwnd)
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32)
    {
        auto getDpi = reinterpret_cast<FnGetDpiForWindow>(
            GetProcAddress(user32, "GetDpiForWindow"));

        if (getDpi)
        {
            const UINT dpi = getDpi(hwnd);
            if (dpi >= 96 && dpi <= 960)
            {
                return static_cast<int>(dpi);
            }
        }
    }

    return 96;
}

bool DockWindow::Create(HINSTANCE instance, Host* host)
{
    host_ = host;

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = &DockWindow::WndProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kWindowClassName;

    RegisterClassExW(&windowClass);

    hwnd_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kWindowClassName,
        L"LightDock",
        WS_POPUP,
        0, 0, 1, 1,
        nullptr,
        nullptr,
        instance,
        this);

    if (!hwnd_)
    {
        return false;
    }

    // Use the OLE drop target below so DragEnter/DragOver/Drop all arrive on
    // the same live path. The legacy WM_DROPFILES mechanism only reports the
    // final release and cannot drive a preview animation.
    const HRESULT dropResult = RegisterDragDrop(hwnd_, this);
    if (FAILED(dropResult))
    {
        wchar_t message[160]{};
        swprintf(message, 160, L"无法注册 Dock 拖放目标（错误码 0x%08lX）。",
                 static_cast<unsigned long>(dropResult));
        MessageBoxW(hwnd_, message, L"LightDock", MB_OK | MB_ICONERROR);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return false;
    }

    dpi_ = QueryDpi(hwnd_);

    return true;
}

void DockWindow::Destroy()
{
    RemoveTrayIcon();
    RemoveAppBarReservation();

    if (hideIndicator_)
    {
        DestroyWindow(hideIndicator_);
        hideIndicator_ = nullptr;
    }

    if (hideIndicatorDC_)
    {
        if (hideIndicatorOldBitmap_)
        {
            SelectObject(hideIndicatorDC_, hideIndicatorOldBitmap_);
        }
        if (hideIndicatorBitmap_)
        {
            DeleteObject(hideIndicatorBitmap_);
        }
        DeleteDC(hideIndicatorDC_);
    }
    hideIndicatorDC_ = nullptr;
    hideIndicatorBitmap_ = nullptr;
    hideIndicatorOldBitmap_ = nullptr;
    hideIndicatorPixels_ = nullptr;
    hideIndicatorWidth_ = 0;
    hideIndicatorHeight_ = 0;

    if (hwnd_)
    {
        RevokeDragDrop(hwnd_);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

bool DockWindow::SetAppBarReservation(const RECT& monitorRect,
                                     DockEdge edge,
                                     int thickness)
{
    if (!hwnd_ || monitorRect.right <= monitorRect.left
        || monitorRect.bottom <= monitorRect.top)
    {
        return false;
    }

    const int maxThickness = (edge == DockEdge::Left || edge == DockEdge::Right)
        ? monitorRect.right - monitorRect.left
        : monitorRect.bottom - monitorRect.top;
    thickness = (std::clamp)(thickness, 1, maxThickness);

    if (appBarRegistered_
        && appBarHeight_ == thickness
        && appBarEdge_ == edge
        && appBarMonitor_.left == monitorRect.left
        && appBarMonitor_.top == monitorRect.top
        && appBarMonitor_.right == monitorRect.right
        && appBarMonitor_.bottom == monitorRect.bottom)
    {
        return true;
    }

    if (!appBarRegistered_)
    {
        APPBARDATA registration{};
        registration.cbSize = sizeof(registration);
        registration.hWnd = hwnd_;
        registration.uCallbackMessage = kAppBarCallback;
        if (!SHAppBarMessage(ABM_NEW, &registration))
        {
            return false;
        }
        appBarRegistered_ = true;
    }

    APPBARDATA data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    switch (edge)
    {
    case DockEdge::Top: data.uEdge = ABE_TOP; break;
    case DockEdge::Left: data.uEdge = ABE_LEFT; break;
    case DockEdge::Right: data.uEdge = ABE_RIGHT; break;
    case DockEdge::Bottom: default: data.uEdge = ABE_BOTTOM; break;
    }
    data.rc = monitorRect;
    auto applyThickness = [&]()
    {
        switch (edge)
        {
        case DockEdge::Top: data.rc.bottom = data.rc.top + thickness; break;
        case DockEdge::Left: data.rc.right = data.rc.left + thickness; break;
        case DockEdge::Right: data.rc.left = data.rc.right - thickness; break;
        case DockEdge::Bottom: default: data.rc.top = data.rc.bottom - thickness; break;
        }
    };
    applyThickness();

    appBarUpdating_ = true;
    SHAppBarMessage(ABM_QUERYPOS, &data);
    // QUERYPOS may move the proposed edge around another appbar. Keep the
    // requested thickness while honoring the shell-adjusted bottom edge.
    applyThickness();
    if (!SHAppBarMessage(ABM_SETPOS, &data))
    {
        appBarUpdating_ = false;
        return false;
    }
    appBarUpdating_ = false;

    appBarMonitor_ = monitorRect;
    appBarHeight_ = thickness;
    appBarEdge_ = edge;
    return true;
}

void DockWindow::RemoveAppBarReservation()
{
    if (!hwnd_ || !appBarRegistered_)
    {
        return;
    }

    APPBARDATA data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    appBarRegistered_ = false;
    appBarUpdating_ = true;
    SHAppBarMessage(ABM_REMOVE, &data);
    appBarUpdating_ = false;
    appBarHeight_ = 0;
    appBarEdge_ = DockEdge::Bottom;
    appBarMonitor_ = RECT{};
}

void DockWindow::UpdateHideIndicator(bool visible, int x, int y,
                                     int width, int height, BYTE opacity,
                                     COLORREF color)
{
    if (!visible || opacity == 0 || width <= 0 || height <= 0 || !hwnd_)
    {
        if (hideIndicator_)
        {
            ShowWindow(hideIndicator_, SW_HIDE);
        }
        return;
    }

    if (!hideIndicator_)
    {
        HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = HideIndicatorWndProc;
        windowClass.hInstance = instance;
        windowClass.lpszClassName = kHideIndicatorClassName;
        RegisterClassExW(&windowClass);

        hideIndicator_ = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW
                | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
            kHideIndicatorClassName, L"LightDock hidden indicator",
            WS_POPUP, x, y, width, height, nullptr, nullptr, instance, nullptr);
        if (!hideIndicator_)
        {
            return;
        }
    }

    if (width != hideIndicatorWidth_ || height != hideIndicatorHeight_
        || !hideIndicatorDC_ || !hideIndicatorBitmap_ || !hideIndicatorPixels_)
    {
        if (hideIndicatorDC_)
        {
            if (hideIndicatorOldBitmap_)
            {
                SelectObject(hideIndicatorDC_, hideIndicatorOldBitmap_);
            }
            if (hideIndicatorBitmap_)
            {
                DeleteObject(hideIndicatorBitmap_);
                hideIndicatorBitmap_ = nullptr;
            }
        }

        if (!hideIndicatorDC_)
        {
            hideIndicatorDC_ = CreateCompatibleDC(nullptr);
        }
        if (!hideIndicatorDC_)
        {
            ShowWindow(hideIndicator_, SW_HIDE);
            return;
        }

        BITMAPINFO bitmapInfo{};
        bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmapInfo.bmiHeader.biWidth = width;
        bitmapInfo.bmiHeader.biHeight = -height;
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        bitmapInfo.bmiHeader.biCompression = BI_RGB;
        hideIndicatorBitmap_ = CreateDIBSection(
            hideIndicatorDC_, &bitmapInfo, DIB_RGB_COLORS,
            &hideIndicatorPixels_, nullptr, 0);
        if (!hideIndicatorBitmap_ || !hideIndicatorPixels_)
        {
            ShowWindow(hideIndicator_, SW_HIDE);
            return;
        }
        hideIndicatorOldBitmap_ = SelectObject(hideIndicatorDC_,
                                               hideIndicatorBitmap_);
        hideIndicatorWidth_ = width;
        hideIndicatorHeight_ = height;
    }

    // Supersample the capsule edge to get smooth, per-pixel alpha at any DPI.
    constexpr int samplesPerAxis = 4;
    constexpr int sampleCount = samplesPerAxis * samplesPerAxis;
    const double radius = static_cast<double>((std::min)(width, height)) * 0.5;
    const double leftCenter = radius;
    const double rightCenter = static_cast<double>(width) - radius;
    const double topCenter = radius;
    const double bottomCenter = static_cast<double>(height) - radius;
    const BYTE red = GetRValue(color);
    const BYTE green = GetGValue(color);
    const BYTE blue = GetBValue(color);
    auto* pixels = static_cast<std::uint32_t*>(hideIndicatorPixels_);
    for (int py = 0; py < height; ++py)
    {
        for (int px = 0; px < width; ++px)
        {
            int covered = 0;
            for (int sy = 0; sy < samplesPerAxis; ++sy)
            {
                const double sampleY = py + (sy + 0.5) / samplesPerAxis;
                for (int sx = 0; sx < samplesPerAxis; ++sx)
                {
                    const double sampleX = px + (sx + 0.5) / samplesPerAxis;
                    const double centerX = (std::clamp)(
                        sampleX, leftCenter, rightCenter);
                    const double centerY = (std::clamp)(
                        sampleY, topCenter, bottomCenter);
                    const double dx = sampleX - centerX;
                    const double dy = sampleY - centerY;
                    if (dx * dx + dy * dy <= radius * radius)
                    {
                        ++covered;
                    }
                }
            }

            const BYTE alpha = static_cast<BYTE>(
                (static_cast<unsigned int>(opacity) * covered + sampleCount / 2)
                / sampleCount);
            const BYTE premulRed = static_cast<BYTE>(
                (static_cast<unsigned int>(red) * alpha + 127) / 255);
            const BYTE premulGreen = static_cast<BYTE>(
                (static_cast<unsigned int>(green) * alpha + 127) / 255);
            const BYTE premulBlue = static_cast<BYTE>(
                (static_cast<unsigned int>(blue) * alpha + 127) / 255);
            pixels[static_cast<size_t>(py) * width + px]
                = static_cast<std::uint32_t>(premulBlue)
                | (static_cast<std::uint32_t>(premulGreen) << 8)
                | (static_cast<std::uint32_t>(premulRed) << 16)
                | (static_cast<std::uint32_t>(alpha) << 24);
        }
    }

    // Insert immediately below the dock so the dock naturally covers the
    // marker as it slides back into view. It remains click-through.
    SetWindowPos(hideIndicator_, hwnd_, x, y, width, height,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    POINT destination{x, y};
    POINT source{0, 0};
    SIZE size{width, height};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    if (UpdateLayeredWindow(hideIndicator_, nullptr, &destination, &size,
                            hideIndicatorDC_, &source, 0, &blend, ULW_ALPHA))
    {
        ShowWindow(hideIndicator_, SW_SHOWNOACTIVATE);
    }
    else
    {
        ShowWindow(hideIndicator_, SW_HIDE);
    }
}

void DockWindow::SetBounds(int x, int y, int width, int height)
{
    if (!hwnd_)
    {
        return;
    }

    // HWND_TOPMOST was previously paired with SWP_NOZORDER, which makes the
    // HWND_TOPMOST argument a no-op. That allowed Explorer's auto-hidden
    // taskbar to raise itself above LightDock after the dock revealed.
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width, height,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
}

void DockWindow::EnsureTopmost()
{
    if (!hwnd_)
    {
        return;
    }

    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE
                 | SWP_NOOWNERZORDER);
}

RECT DockWindow::GetBounds() const
{
    RECT bounds{};
    if (hwnd_)
    {
        GetWindowRect(hwnd_, &bounds);
    }

    return bounds;
}

bool DockWindow::Present(HDC surfaceDC, int width, int height)
{
    if (!hwnd_ || !surfaceDC)
    {
        return false;
    }

    POINT origin{0, 0};
    SIZE size{width, height};

    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    return UpdateLayeredWindow(
               hwnd_,
               nullptr,
               nullptr,
               &size,
               surfaceDC,
               &origin,
               0,
               &blend,
               ULW_ALPHA) != FALSE;
}

bool DockWindow::AddTrayIcon(HICON icon, const wchar_t* tip)
{
    if (!hwnd_ || !icon)
    {
        return false;
    }

    if (trayAdded_)
    {
        RemoveTrayIcon();
    }

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = 1;
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    data.uCallbackMessage = kTrayCallback;
    data.hIcon = icon;

    if (tip)
    {
        wcsncpy(data.szTip, tip, ARRAYSIZE(data.szTip) - 1);
    }

    trayAdded_ = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
    trayIcon_ = trayAdded_ ? icon : nullptr;

    if (!trayAdded_)
    {
        DestroyIcon(icon);
    }

    return trayAdded_;
}

void DockWindow::RemoveTrayIcon()
{
    if (!trayAdded_ || !hwnd_)
    {
        return;
    }

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = 1;

    Shell_NotifyIconW(NIM_DELETE, &data);
    trayAdded_ = false;

    if (trayIcon_)
    {
        DestroyIcon(trayIcon_);
        trayIcon_ = nullptr;
    }
}

LRESULT CALLBACK DockWindow::WndProc(HWND hwnd, UINT message,
                                     WPARAM wParam, LPARAM lParam)
{
    DockWindow* self = nullptr;

    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<DockWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    else
    {
        self = reinterpret_cast<DockWindow*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (!self)
    {
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    return self->HandleMessage(message, wParam, lParam);
}

LRESULT DockWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_NCCREATE:
        return TRUE;

    case WM_NCHITTEST:
    {
        POINT screen{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

        RECT bounds{};
        GetWindowRect(hwnd_, &bounds);

        const float x = static_cast<float>(screen.x - bounds.left);
        const float y = static_cast<float>(screen.y - bounds.top);

        // During an Explorer drag the cursor may enter the transparent
        // margins before the pointer reaches the visible panel. Keep the
        // whole canvas as a temporary drop target while the left button is
        // held, otherwise OLE reports HTTRANSPARENT and DragOver stops.
        if ((GetKeyState(VK_LBUTTON) & 0x8000) != 0)
        {
            return HTCLIENT;
        }

        // Everything that is not the dock (or a live icon) falls through to
        // whatever is underneath.
        return (host_ && host_->HitTest(x, y)) ? HTCLIENT : HTTRANSPARENT;
    }

    case kAppBarCallback:
        if (lParam == ABN_POSCHANGED && appBarRegistered_
            && host_ && !appBarUpdating_)
        {
            host_->OnAppBarChanged();
        }
        return 0;

    case WM_MOUSEMOVE:
    {
        const float x = static_cast<float>(GET_X_LPARAM(lParam));
        const float y = static_cast<float>(GET_Y_LPARAM(lParam));

        if (host_)
        {
            host_->OnMouseMove(x, y);
        }

        return 0;
    }

    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    {
        const float x = static_cast<float>(GET_X_LPARAM(lParam));
        const float y = static_cast<float>(GET_Y_LPARAM(lParam));

        if (host_)
        {
            const bool down = (message == WM_LBUTTONDOWN);
            const int button = (message == WM_RBUTTONUP) ? 1 : 0;

            host_->OnMouseButton(button, down, x, y);
        }

        return 0;
    }

    case WM_MOUSEACTIVATE:
        // Never steal focus from the application the user is working in.
        return MA_NOACTIVATE;

    case WM_COMMAND:
        if (host_)
        {
            host_->OnCommand(LOWORD(wParam));
        }

        return 0;

    case kTrayCallback:
        if (host_)
        {
            host_->OnTrayNotify(lParam);
        }

        return 0;

    case WM_DROPFILES:
        if (host_)
        {
            host_->OnDropFiles(reinterpret_cast<HDROP>(wParam));
        }

        return 0;

    case WM_TIMER:
        if (host_ && wParam == 0x4C444B31u)
        {
            host_->OnAnimationTimer();
            return 0;
        }

        break;

    case WM_DPICHANGED:
    {
        const int dpi = static_cast<int>(HIWORD(wParam));
        dpi_ = dpi;

        if (host_)
        {
            host_->OnDpiChanged(dpi);
        }

        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        // Painting is fully driven by UpdateLayeredWindow.
        PAINTSTRUCT paint{};
        BeginPaint(hwnd_, &paint);
        EndPaint(hwnd_, &paint);
        return 0;
    }

    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        return TRUE;

    case WM_CLOSE:
        if (host_)
        {
            host_->OnCloseRequested();
        }
        else
        {
            DestroyWindow(hwnd_);
        }
        return 0;

    case WM_DESTROY:
        if (host_)
        {
            host_->OnDestroy();
        }

        return 0;
    }

    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

HRESULT STDMETHODCALLTYPE DockWindow::QueryInterface(REFIID iid, void** object)
{
    if (!object)
    {
        return E_POINTER;
    }

    *object = nullptr;
    if (iid == IID_IUnknown || iid == IID_IDropTarget)
    {
        *object = static_cast<IDropTarget*>(this);
        AddRef();
        return S_OK;
    }

    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE DockWindow::AddRef()
{
    return static_cast<ULONG>(InterlockedIncrement(
        reinterpret_cast<LONG*>(&refCount_)));
}

ULONG STDMETHODCALLTYPE DockWindow::Release()
{
    return static_cast<ULONG>(InterlockedDecrement(
        reinterpret_cast<LONG*>(&refCount_)));
}

void DockWindow::DropPoint(POINTL point, float& x, float& y) const
{
    RECT bounds{};
    GetWindowRect(hwnd_, &bounds);
    x = static_cast<float>(point.x - bounds.left);
    y = static_cast<float>(point.y - bounds.top);
}

std::vector<std::wstring> DockWindow::DropPaths(IDataObject* data) const
{
    std::vector<std::wstring> paths;
    if (!data)
    {
        return paths;
    }

    FORMATETC format{};
    format.cfFormat = CF_HDROP;
    format.dwAspect = DVASPECT_CONTENT;
    format.lindex = -1;
    format.tymed = TYMED_HGLOBAL;

    STGMEDIUM medium{};
    if (FAILED(data->GetData(&format, &medium)))
    {
        return paths;
    }

    const HDROP drop = static_cast<HDROP>(medium.hGlobal);
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0);
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
        paths.push_back(std::move(path));
    }

    ReleaseStgMedium(&medium);
    return paths;
}

HRESULT STDMETHODCALLTYPE DockWindow::DragEnter(IDataObject* data,
                                                  DWORD,
                                                  POINTL point,
                                                  DWORD* effect)
{
    const std::vector<std::wstring> paths = DropPaths(data);
    float x = 0.0f;
    float y = 0.0f;
    DropPoint(point, x, y);

    if (effect)
    {
        *effect = paths.empty() ? DROPEFFECT_NONE : DROPEFFECT_COPY;
    }

    if (!paths.empty() && host_)
    {
        host_->OnExternalDragEnter(paths, x, y);
    }

    return S_OK;
}

HRESULT STDMETHODCALLTYPE DockWindow::DragOver(DWORD,
                                                POINTL point,
                                                DWORD* effect)
{
    if (effect)
    {
        *effect = DROPEFFECT_COPY;
    }

    float x = 0.0f;
    float y = 0.0f;
    DropPoint(point, x, y);
    if (host_)
    {
        host_->OnExternalDragMove(x, y);
    }

    return S_OK;
}

HRESULT STDMETHODCALLTYPE DockWindow::DragLeave()
{
    if (host_)
    {
        host_->OnExternalDragLeave();
    }

    return S_OK;
}

HRESULT STDMETHODCALLTYPE DockWindow::Drop(IDataObject* data,
                                            DWORD,
                                            POINTL point,
                                            DWORD* effect)
{
    const std::vector<std::wstring> paths = DropPaths(data);
    float x = 0.0f;
    float y = 0.0f;
    DropPoint(point, x, y);

    if (effect)
    {
        *effect = paths.empty() ? DROPEFFECT_NONE : DROPEFFECT_COPY;
    }

    if (host_ && !paths.empty())
    {
        // The paths were captured on DragEnter; the app keeps that list for
        // the final insertion, while this call supplies the exact release
        // position.
        host_->OnExternalDrop(x, y);
    }
    else if (host_)
    {
        host_->OnExternalDragLeave();
    }

    return S_OK;
}

} // namespace ld

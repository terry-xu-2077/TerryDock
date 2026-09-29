#pragma once

#include "Common.h"
#include "Config.h"

#include <shellapi.h>

#include <string>
#include <vector>

namespace ld
{

/// The raw Win32 shell: a borderless, topmost, per pixel alpha layered window.
///
/// The HWND is deliberately larger than the visible dock so magnified icons,
/// the launch bounce and the drop shadow have somewhere to live. Transparent
/// parts are made click through via WM_NCHITTEST.
class DockWindow : public IDropTarget
{
public:
    /// Implemented by App.
    class Host
    {
    public:
        virtual ~Host() = default;

        /// Point is in surface (window) pixels.
        virtual bool HitTest(float x, float y) = 0;

        virtual void OnMouseMove(float x, float y) = 0;
        virtual void OnMouseButton(int button, bool down, float x, float y) = 0;
        virtual void OnDpiChanged(int dpi) = 0;
        virtual void OnAppBarChanged() = 0;
        virtual void OnCommand(UINT id) = 0;
        virtual void OnTrayNotify(LPARAM lParam) = 0;
        virtual void OnDropFiles(HDROP drop) = 0;
        virtual void OnExternalDragEnter(const std::vector<std::wstring>& paths,
                                         float x, float y) = 0;
        virtual void OnExternalDragMove(float x, float y) = 0;
        virtual void OnExternalDragLeave() = 0;
        virtual void OnExternalDrop(float x, float y) = 0;
        virtual void OnAnimationTimer() = 0;
        virtual void OnCloseRequested() = 0;
        virtual void OnDestroy() = 0;
    };

    DockWindow() = default;
    ~DockWindow();

    DockWindow(const DockWindow&) = delete;
    DockWindow& operator=(const DockWindow&) = delete;

    /// Must be called before any window is created.
    static void EnablePerMonitorDpiAwareness();

    static int QueryDpi(HWND hwnd);

    bool Create(HINSTANCE instance, Host* host);
    void Destroy();

    HWND Handle() const { return hwnd_; }

    void SetBounds(int x, int y, int width, int height);

    /// Reasserts the layered dock at the topmost z-order without activating it.
    /// Needed when the Windows auto-hidden taskbar raises itself after the
    /// dock has already finished its reveal animation.
    void EnsureTopmost();

    bool SetAppBarReservation(const RECT& monitorRect,
                             DockEdge edge,
                             int thickness);
    void RemoveAppBarReservation();
    void UpdateHideIndicator(bool visible, int x, int y, int width, int height,
                             BYTE opacity, COLORREF color);
    RECT GetBounds() const;

    int GetDpi() const { return dpi_; }
    void SetDpi(int dpi) { dpi_ = dpi; }

    /// Pushes the off screen surface to the desktop.
    bool Present(HDC surfaceDC, int width, int height);

    /// Notification message used by the taskbar icon.
    static constexpr UINT kTrayCallback = WM_APP + 7;

    bool AddTrayIcon(HICON icon, const wchar_t* tip);
    void RemoveTrayIcon();

    // OLE drop target. Explorer keeps this object alive while a drag is over
    // the layered dock window.
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD keyState,
                                        POINTL point, DWORD* effect) override;
    HRESULT STDMETHODCALLTYPE DragOver(DWORD keyState, POINTL point,
                                       DWORD* effect) override;
    HRESULT STDMETHODCALLTYPE DragLeave() override;
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD keyState,
                                   POINTL point, DWORD* effect) override;

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message,
                                    WPARAM wParam, LPARAM lParam);

    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    HWND hwnd_ = nullptr;
    HWND hideIndicator_ = nullptr;
    Host* host_ = nullptr;
    HICON trayIcon_ = nullptr;
    bool trayAdded_ = false;
    bool appBarRegistered_ = false;
    bool appBarUpdating_ = false;
    int hideIndicatorWidth_ = 0;
    int hideIndicatorHeight_ = 0;
    HDC hideIndicatorDC_ = nullptr;
    HBITMAP hideIndicatorBitmap_ = nullptr;
    HGDIOBJ hideIndicatorOldBitmap_ = nullptr;
    void* hideIndicatorPixels_ = nullptr;
    RECT appBarMonitor_{};
    int appBarHeight_ = 0;
    DockEdge appBarEdge_ = DockEdge::Bottom;
    int dpi_ = 96;
    ULONG refCount_ = 1;

    std::vector<std::wstring> DropPaths(IDataObject* data) const;
    void DropPoint(POINTL point, float& x, float& y) const;
};

} // namespace ld

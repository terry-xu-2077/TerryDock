#pragma once

#include "Common.h"
#include "Config.h"

#include <dwmapi.h>

#include <string>
#include <vector>

namespace ld
{

/// Separate non-activating popup used for multi-window management.
///
/// DWM thumbnails cannot be reliably composed into LightDock's
/// UpdateLayeredWindow surface. This ordinary top-level popup gives DWM a
/// native destination HWND while remaining mouse-transparent so the main Dock
/// window keeps all interaction/hit-testing.
class WindowPreview
{
public:
    struct Entry
    {
        HWND hwnd = nullptr;
        std::wstring title;
        bool active = false;
        bool minimized = false;
    };

    WindowPreview() = default;
    ~WindowPreview();

    WindowPreview(const WindowPreview&) = delete;
    WindowPreview& operator=(const WindowPreview&) = delete;

    bool Initialize(HINSTANCE instance, HWND owner);
    void Shutdown();

    void Show(const RECT& screenRect,
              POINT anchorScreen,
              DockEdge dockEdge,
              const std::wstring& applicationName,
              const std::vector<Entry>& entries,
              int hoveredRow,
              float dpiScale,
              float scale,
              float cornerRadius,
              float opacity);
    void Hide();
    void SetHoveredRow(int hoveredRow);

    bool Visible() const { return visible_; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message,
                                    WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK ArrowWndProc(HWND hwnd, UINT message,
                                         WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    void ClearThumbnails();
    void RebuildThumbnails();
    void UpdateThumbnailRects();
    void InvalidateHoverTransition(int oldRow, int newRow);
    void UpdateArrow(const RECT& bodyScreenRect, POINT anchorScreen,
                     DockEdge dockEdge);
    void HideArrow();
    bool EnsureArrowSurface(int width, int height);
    void Paint();

    HINSTANCE instance_ = nullptr;
    HWND owner_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND arrowHwnd_ = nullptr;

    // Ordinary popup HWND is required by DWM thumbnails. Use a D2D DC target
    // only for our own card chrome so thumbnail frames/hover stay antialiased.
    ComPtr<ID2D1Factory> d2dFactory_;
    ComPtr<ID2D1DCRenderTarget> d2dTarget_;

    HDC arrowDC_ = nullptr;
    HBITMAP arrowBitmap_ = nullptr;
    HBITMAP arrowOldBitmap_ = nullptr;
    void* arrowPixels_ = nullptr;
    int arrowWidth_ = 0;
    int arrowHeight_ = 0;

    std::wstring applicationName_;
    std::vector<Entry> entries_;
    std::vector<HTHUMBNAIL> thumbnails_;
    std::vector<RECT> thumbnailRects_;
    int hoveredRow_ = -1;
    float dpiScale_ = 1.0f;
    float menuScale_ = 1.0f;
    float cornerRadius_ = 10.0f;
    float opacity_ = 1.0f;
    DockEdge dockEdge_ = DockEdge::Bottom;
    POINT anchorScreen_{};
    bool nativeRoundedCorners_ = false;
    int lastClientWidth_ = 0;
    int lastClientHeight_ = 0;
    bool visible_ = false;
};

} // namespace ld

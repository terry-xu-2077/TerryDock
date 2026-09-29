#pragma once

#include "Common.h"

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

    bool Initialize(HINSTANCE instance);
    void Shutdown();

    void Show(const RECT& screenRect,
              const std::vector<Entry>& entries,
              int hoveredRow,
              float dpiScale,
              float menuScale,
              float cornerRadius,
              float thumbnailScale);
    void Hide();
    void SetHoveredRow(int hoveredRow);

    bool Visible() const { return visible_; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message,
                                    WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    void ClearThumbnails();
    void RebuildThumbnails();
    void UpdateThumbnailRects();
    void Paint();

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    std::vector<Entry> entries_;
    std::vector<HTHUMBNAIL> thumbnails_;
    std::vector<RECT> thumbnailRects_;
    int hoveredRow_ = -1;
    float dpiScale_ = 1.0f;
    float menuScale_ = 1.0f;
    float cornerRadius_ = 10.0f;
    float thumbnailScale_ = 1.0f;
    bool nativeRoundedCorners_ = false;
    bool visible_ = false;
};

} // namespace ld

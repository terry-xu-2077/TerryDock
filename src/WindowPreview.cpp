#include "WindowPreview.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace ld
{

namespace
{

constexpr wchar_t kWindowPreviewClass[] = L"LightDock_WindowPreview";

int ScalePx(float value, float scale)
{
    return static_cast<int>(std::lround(value * scale));
}

} // namespace

WindowPreview::~WindowPreview()
{
    Shutdown();
}

bool WindowPreview::Initialize(HINSTANCE instance)
{
    if (hwnd_)
    {
        return true;
    }

    instance_ = instance;

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = &WindowPreview::WndProc;
    windowClass.hInstance = instance_;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kWindowPreviewClass;

    RegisterClassExW(&windowClass);

    hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        kWindowPreviewClass,
        L"LightDock window preview",
        WS_POPUP,
        0, 0, 1, 1,
        nullptr,
        nullptr,
        instance_,
        this);

    return hwnd_ != nullptr;
}

void WindowPreview::Shutdown()
{
    ClearThumbnails();

    if (hwnd_)
    {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }

    visible_ = false;
    entries_.clear();
}

void WindowPreview::ClearThumbnails()
{
    for (HTHUMBNAIL thumbnail : thumbnails_)
    {
        if (thumbnail)
        {
            DwmUnregisterThumbnail(thumbnail);
        }
    }

    thumbnails_.clear();
}

void WindowPreview::RebuildThumbnails()
{
    ClearThumbnails();

    if (!hwnd_)
    {
        return;
    }

    thumbnails_.reserve(entries_.size());

    for (const Entry& entry : entries_)
    {
        HTHUMBNAIL thumbnail = nullptr;

        if (entry.hwnd && IsWindow(entry.hwnd)
            && SUCCEEDED(DwmRegisterThumbnail(
                hwnd_, entry.hwnd, &thumbnail)))
        {
            thumbnails_.push_back(thumbnail);
        }
        else
        {
            thumbnails_.push_back(nullptr);
        }
    }

    UpdateThumbnailRects();
}

void WindowPreview::UpdateThumbnailRects()
{
    if (!hwnd_ || entries_.empty())
    {
        return;
    }

    RECT client{};
    GetClientRect(hwnd_, &client);

    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0)
    {
        return;
    }

    const int rowHeight = height / static_cast<int>(entries_.size());
    const int left = ScalePx(10.0f, dpiScale_);
    const int previewSlotWidth = ScalePx(112.0f, dpiScale_);
    const int verticalPad = ScalePx(7.0f, dpiScale_);
    const int maxWidth = ScalePx(104.0f, dpiScale_);
    const int maxHeight = (std::max)(1, rowHeight - verticalPad * 2);

    for (size_t i = 0; i < thumbnails_.size(); ++i)
    {
        HTHUMBNAIL thumbnail = thumbnails_[i];
        if (!thumbnail)
        {
            continue;
        }

        SIZE source{};
        if (FAILED(DwmQueryThumbnailSourceSize(thumbnail, &source))
            || source.cx <= 0 || source.cy <= 0)
        {
            continue;
        }

        const float scale = (std::min)(
            static_cast<float>(maxWidth) / static_cast<float>(source.cx),
            static_cast<float>(maxHeight) / static_cast<float>(source.cy));

        const int drawWidth = (std::max)(
            1, static_cast<int>(std::lround(source.cx * scale)));
        const int drawHeight = (std::max)(
            1, static_cast<int>(std::lround(source.cy * scale)));

        const int rowTop = static_cast<int>(i) * rowHeight;
        const int slotLeft = left;
        const int slotTop = rowTop + (rowHeight - drawHeight) / 2;
        const int drawLeft =
            slotLeft + (previewSlotWidth - drawWidth) / 2;

        DWM_THUMBNAIL_PROPERTIES properties{};
        properties.dwFlags =
            DWM_TNP_RECTDESTINATION
            | DWM_TNP_VISIBLE
            | DWM_TNP_OPACITY
            | DWM_TNP_SOURCECLIENTAREAONLY;
        properties.rcDestination = RECT{
            drawLeft,
            slotTop,
            drawLeft + drawWidth,
            slotTop + drawHeight};
        properties.opacity = 255;
        properties.fVisible = TRUE;
        properties.fSourceClientAreaOnly = FALSE;

        DwmUpdateThumbnailProperties(thumbnail, &properties);
    }
}

void WindowPreview::Show(const RECT& screenRect,
                         const std::vector<Entry>& entries,
                         int hoveredRow,
                         float dpiScale)
{
    if (!hwnd_ && !Initialize(instance_ ? instance_ : GetModuleHandleW(nullptr)))
    {
        return;
    }

    const int width = screenRect.right - screenRect.left;
    const int height = screenRect.bottom - screenRect.top;
    if (width <= 0 || height <= 0 || entries.empty())
    {
        Hide();
        return;
    }

    bool sourcesChanged = entries_.size() != entries.size();
    if (!sourcesChanged)
    {
        for (size_t i = 0; i < entries.size(); ++i)
        {
            if (entries_[i].hwnd != entries[i].hwnd)
            {
                sourcesChanged = true;
                break;
            }
        }
    }

    entries_ = entries;
    hoveredRow_ = hoveredRow;
    dpiScale_ = (std::max)(dpiScale, 1.0f);

    SetWindowPos(
        hwnd_, HWND_TOPMOST,
        screenRect.left, screenRect.top,
        width, height,
        SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);

    const int radius = ScalePx(10.0f, dpiScale_);
    HRGN region = CreateRoundRectRgn(
        0, 0, width + 1, height + 1,
        radius * 2, radius * 2);
    if (region)
    {
        if (!SetWindowRgn(hwnd_, region, TRUE))
        {
            DeleteObject(region);
        }
    }

    if (sourcesChanged)
    {
        RebuildThumbnails();
    }
    else
    {
        UpdateThumbnailRects();
    }

    visible_ = true;
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
}

void WindowPreview::Hide()
{
    if (!hwnd_)
    {
        return;
    }

    if (visible_)
    {
        ShowWindow(hwnd_, SW_HIDE);
    }

    visible_ = false;
    hoveredRow_ = -1;
}

void WindowPreview::SetHoveredRow(int hoveredRow)
{
    if (hoveredRow_ == hoveredRow)
    {
        return;
    }

    hoveredRow_ = hoveredRow;
    if (hwnd_ && visible_)
    {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void WindowPreview::Paint()
{
    if (!hwnd_)
    {
        return;
    }

    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(hwnd_, &paint);
    if (!dc)
    {
        return;
    }

    RECT client{};
    GetClientRect(hwnd_, &client);

    HBRUSH background = CreateSolidBrush(RGB(29, 31, 35));
    FillRect(dc, &client, background);
    DeleteObject(background);

    if (!entries_.empty())
    {
        const int rowHeight =
            (client.bottom - client.top)
            / static_cast<int>(entries_.size());
        const int thumbnailArea =
            ScalePx(132.0f, dpiScale_);
        const int textRightPad =
            ScalePx(14.0f, dpiScale_);
        const int activeRadius =
            ScalePx(3.0f, dpiScale_);

        HFONT font = CreateFontW(
            -ScalePx(16.0f, dpiScale_),
            0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI");

        HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
        SetBkMode(dc, TRANSPARENT);

        for (size_t i = 0; i < entries_.size(); ++i)
        {
            const int top = static_cast<int>(i) * rowHeight;
            const int bottom = top + rowHeight;

            if (static_cast<int>(i) == hoveredRow_)
            {
                RECT rowRect{
                    ScalePx(4.0f, dpiScale_),
                    top + ScalePx(3.0f, dpiScale_),
                    client.right - ScalePx(4.0f, dpiScale_),
                    bottom - ScalePx(3.0f, dpiScale_)};
                HBRUSH hover = CreateSolidBrush(RGB(48, 51, 57));
                FillRect(dc, &rowRect, hover);
                DeleteObject(hover);
            }

            if (entries_[i].active)
            {
                const int cx = ScalePx(9.0f, dpiScale_);
                const int cy = top + rowHeight / 2;
                HBRUSH dot = CreateSolidBrush(RGB(66, 171, 255));
                HGDIOBJ oldBrush = SelectObject(dc, dot);
                HGDIOBJ oldPen = SelectObject(
                    dc, GetStockObject(NULL_PEN));
                Ellipse(dc,
                        cx - activeRadius,
                        cy - activeRadius,
                        cx + activeRadius + 1,
                        cy + activeRadius + 1);
                SelectObject(dc, oldPen);
                SelectObject(dc, oldBrush);
                DeleteObject(dot);
            }

            SetTextColor(
                dc,
                entries_[i].minimized
                    ? RGB(165, 169, 176)
                    : RGB(242, 244, 248));

            RECT textRect{
                thumbnailArea,
                top,
                client.right - textRightPad,
                bottom};

            DrawTextW(
                dc,
                entries_[i].title.c_str(),
                static_cast<int>(entries_[i].title.size()),
                &textRect,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE
                    | DT_END_ELLIPSIS | DT_NOPREFIX);
        }

        if (oldFont)
        {
            SelectObject(dc, oldFont);
        }
        if (font)
        {
            DeleteObject(font);
        }
    }

    HBRUSH border = CreateSolidBrush(RGB(72, 76, 84));
    FrameRect(dc, &client, border);
    DeleteObject(border);

    EndPaint(hwnd_, &paint);
}

LRESULT CALLBACK WindowPreview::WndProc(HWND hwnd, UINT message,
                                        WPARAM wParam, LPARAM lParam)
{
    WindowPreview* self = nullptr;

    if (message == WM_NCCREATE)
    {
        const auto* create =
            reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<WindowPreview*>(
            create->lpCreateParams);
        SetWindowLongPtrW(
            hwnd, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(self));
    }
    else
    {
        self = reinterpret_cast<WindowPreview*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    return self
        ? self->HandleMessage(message, wParam, lParam)
        : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT WindowPreview::HandleMessage(UINT message,
                                     WPARAM wParam,
                                     LPARAM lParam)
{
    switch (message)
    {
    case WM_NCHITTEST:
        // The main layered Dock owns interaction for the menu bounds.
        return HTTRANSPARENT;

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        Paint();
        return 0;

    case WM_SIZE:
        UpdateThumbnailRects();
        return 0;

    default:
        return DefWindowProcW(hwnd_, message, wParam, lParam);
    }
}

} // namespace ld

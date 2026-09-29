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

// MinGW-w64 has shipped dwmapi.h variants with an incorrect
// DwmRegisterThumbnail second-parameter declaration (HWND* instead of HWND).
// Resolve the thumbnail entry points dynamically so the code uses the real
// Windows ABI and remains compatible with both MinGW and MSVC SDK headers.
struct DwmThumbnailApi
{
    using RegisterFn = HRESULT (WINAPI*)(HWND, HWND, PHTHUMBNAIL);
    using UnregisterFn = HRESULT (WINAPI*)(HTHUMBNAIL);
    using QuerySizeFn = HRESULT (WINAPI*)(HTHUMBNAIL, PSIZE);
    using UpdateFn = HRESULT (WINAPI*)(
        HTHUMBNAIL, const DWM_THUMBNAIL_PROPERTIES*);

    RegisterFn registerThumbnail = nullptr;
    UnregisterFn unregisterThumbnail = nullptr;
    QuerySizeFn queryThumbnailSourceSize = nullptr;
    UpdateFn updateThumbnailProperties = nullptr;

    bool Available() const
    {
        return registerThumbnail
            && unregisterThumbnail
            && queryThumbnailSourceSize
            && updateThumbnailProperties;
    }
};

DwmThumbnailApi& ThumbnailApi()
{
    static DwmThumbnailApi api = []()
    {
        DwmThumbnailApi loaded;
        HMODULE module = GetModuleHandleW(L"dwmapi.dll");
        if (!module)
        {
            module = LoadLibraryW(L"dwmapi.dll");
        }

        if (!module)
        {
            return loaded;
        }

        loaded.registerThumbnail =
            reinterpret_cast<DwmThumbnailApi::RegisterFn>(
                GetProcAddress(module, "DwmRegisterThumbnail"));
        loaded.unregisterThumbnail =
            reinterpret_cast<DwmThumbnailApi::UnregisterFn>(
                GetProcAddress(module, "DwmUnregisterThumbnail"));
        loaded.queryThumbnailSourceSize =
            reinterpret_cast<DwmThumbnailApi::QuerySizeFn>(
                GetProcAddress(module, "DwmQueryThumbnailSourceSize"));
        loaded.updateThumbnailProperties =
            reinterpret_cast<DwmThumbnailApi::UpdateFn>(
                GetProcAddress(module, "DwmUpdateThumbnailProperties"));

        return loaded;
    }();

    return api;
}

bool ApplyNativeRoundedCorners(HWND hwnd, float radius)
{
    if (!hwnd)
    {
        return false;
    }

    using SetWindowAttributeFn =
        HRESULT (WINAPI*)(HWND, DWORD, LPCVOID, DWORD);

    HMODULE module = GetModuleHandleW(L"dwmapi.dll");
    if (!module)
    {
        module = LoadLibraryW(L"dwmapi.dll");
    }
    if (!module)
    {
        return false;
    }

    auto setAttribute = reinterpret_cast<SetWindowAttributeFn>(
        GetProcAddress(module, "DwmSetWindowAttribute"));
    if (!setAttribute)
    {
        return false;
    }

    // Values are stable on Windows 11 but may be missing from older MinGW
    // headers, so keep the ABI constants local.
    constexpr DWORD kWindowCornerPreference = 33;
    constexpr DWORD kBorderColor = 34;
    constexpr int kDoNotRound = 1;
    constexpr int kRound = 2;
    constexpr int kRoundSmall = 3;

    const int preference = radius <= 0.5f ? kDoNotRound
        : radius <= 8.0f ? kRoundSmall : kRound;

    const HRESULT cornerResult = setAttribute(
        hwnd, kWindowCornerPreference,
        &preference, sizeof(preference));

    if (FAILED(cornerResult))
    {
        return false;
    }

    const COLORREF border = RGB(184, 199, 214);
    setAttribute(hwnd, kBorderColor, &border, sizeof(border));
    return true;
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
            auto& api = ThumbnailApi();
            if (api.unregisterThumbnail)
            {
                api.unregisterThumbnail(thumbnail);
            }
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

        auto& api = ThumbnailApi();
        if (api.Available()
            && entry.hwnd && IsWindow(entry.hwnd)
            && SUCCEEDED(api.registerThumbnail(
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
    thumbnailRects_.assign(entries_.size(), RECT{});

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

    const float uiScale = dpiScale_ * menuScale_;
    const int itemWidth =
        width / static_cast<int>(entries_.size());
    const int outerPad = ScalePx(8.0f, uiScale);
    const int topPad = ScalePx(8.0f, uiScale);
    const int titleHeight = ScalePx(28.0f, uiScale);
    const int titleGap = ScalePx(4.0f, uiScale);
    const int previewBottom =
        (std::max)(topPad + 1, height - titleHeight - titleGap - outerPad);

    for (size_t i = 0; i < thumbnails_.size(); ++i)
    {
        HTHUMBNAIL thumbnail = thumbnails_[i];
        if (!thumbnail)
        {
            continue;
        }

        SIZE source{};
        auto& api = ThumbnailApi();
        if (!api.queryThumbnailSourceSize
            || FAILED(api.queryThumbnailSourceSize(thumbnail, &source))
            || source.cx <= 0 || source.cy <= 0)
        {
            continue;
        }

        const int cardLeft = static_cast<int>(i) * itemWidth;
        const int cardRight = (i + 1 == entries_.size())
            ? width : cardLeft + itemWidth;

        const int maxWidth = (std::max)(
            1, cardRight - cardLeft - outerPad * 2);
        const int maxHeight = (std::max)(
            1, previewBottom - topPad);

        const float scale = (std::min)(
            static_cast<float>(maxWidth) / static_cast<float>(source.cx),
            static_cast<float>(maxHeight) / static_cast<float>(source.cy));

        const int drawWidth = (std::max)(
            1, static_cast<int>(std::lround(source.cx * scale)));
        const int drawHeight = (std::max)(
            1, static_cast<int>(std::lround(source.cy * scale)));

        const int drawLeft =
            cardLeft + (cardRight - cardLeft - drawWidth) / 2;
        const int drawTop =
            topPad + (maxHeight - drawHeight) / 2;

        const RECT destination{
            drawLeft,
            drawTop,
            drawLeft + drawWidth,
            drawTop + drawHeight};
        thumbnailRects_[i] = destination;

        DWM_THUMBNAIL_PROPERTIES properties{};
        properties.dwFlags =
            DWM_TNP_RECTDESTINATION
            | DWM_TNP_VISIBLE
            | DWM_TNP_OPACITY
            | DWM_TNP_SOURCECLIENTAREAONLY;
        properties.rcDestination = destination;
        properties.opacity = 255;
        properties.fVisible = TRUE;
        properties.fSourceClientAreaOnly = FALSE;

        if (api.updateThumbnailProperties)
        {
            api.updateThumbnailProperties(thumbnail, &properties);
        }
    }
}

void WindowPreview::Show(const RECT& screenRect,
                         const std::vector<Entry>& entries,
                         int hoveredRow,
                         float dpiScale,
                         float menuScale,
                         float cornerRadius,
                         float thumbnailScale)
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
    menuScale_ = ClampF(menuScale, 0.75f, 1.25f);
    cornerRadius_ = ClampF(cornerRadius, 0.0f, 32.0f);
    thumbnailScale_ = ClampF(thumbnailScale, 0.6f, 1.4f);

    SetWindowPos(
        hwnd_, HWND_TOPMOST,
        screenRect.left, screenRect.top,
        width, height,
        SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);

    const int radius = ScalePx(
        cornerRadius_, dpiScale_ * menuScale_);

    // The Dock itself uses per-pixel alpha + Direct2D, so its rounded corners
    // are naturally anti-aliased. This preview is an ordinary HWND because
    // DWM live thumbnails need a native destination window. On Windows 11,
    // let DWM clip and outline that HWND so the corners receive the same
    // compositor antialiasing. Older Windows falls back to the integer HRGN.
    nativeRoundedCorners_ =
        ApplyNativeRoundedCorners(hwnd_, cornerRadius_);

    if (nativeRoundedCorners_)
    {
        SetWindowRgn(hwnd_, nullptr, TRUE);
    }
    else
    {
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

    const float uiScale = dpiScale_ * menuScale_;
    const int radius = (std::max)(
        1, ScalePx(cornerRadius_, uiScale));
    const int diameter = radius * 2;

    // Match DockRenderer::DrawTooltip.
    const COLORREF backgroundColor = RGB(245, 250, 255);
    const COLORREF borderColor = RGB(184, 199, 214);
    const COLORREF textColor = RGB(26, 36, 46);
    const COLORREF mutedTextColor = RGB(112, 124, 136);
    const COLORREF hoverColor = RGB(226, 235, 244);
    const COLORREF activeColor = RGB(56, 173, 255);

    HBRUSH background = CreateSolidBrush(backgroundColor);

    if (nativeRoundedCorners_)
    {
        // DWM supplies the antialiased outer clip and native border.
        FillRect(dc, &client, background);
    }
    else
    {
        HGDIOBJ oldBrush = SelectObject(dc, background);
        HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        RoundRect(dc, 0, 0, client.right, client.bottom,
                  diameter, diameter);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
    }
    DeleteObject(background);

    if (!entries_.empty())
    {
        const int width = client.right - client.left;
        const int height = client.bottom - client.top;
        const int itemWidth =
            width / static_cast<int>(entries_.size());
        const int outerPad = ScalePx(8.0f, uiScale);
        const int titleHeight = ScalePx(28.0f, uiScale);
        const int titleBottomPad = ScalePx(5.0f, uiScale);
        const int activeRadius = ScalePx(3.0f, uiScale);

        HFONT font = CreateFontW(
            -ScalePx(14.0f, uiScale),
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
            const int left = static_cast<int>(i) * itemWidth;
            const int right = (i + 1 == entries_.size())
                ? width : left + itemWidth;

            if (static_cast<int>(i) == hoveredRow_)
            {
                RECT cardRect{
                    left + ScalePx(3.0f, uiScale),
                    ScalePx(3.0f, uiScale),
                    right - ScalePx(3.0f, uiScale),
                    height - ScalePx(3.0f, uiScale)};

                HBRUSH hover = CreateSolidBrush(hoverColor);
                HGDIOBJ previousBrush = SelectObject(dc, hover);
                HGDIOBJ previousPen =
                    SelectObject(dc, GetStockObject(NULL_PEN));
                const int hoverRadius = ScalePx(7.0f, uiScale);
                RoundRect(dc,
                          cardRect.left, cardRect.top,
                          cardRect.right, cardRect.bottom,
                          hoverRadius * 2, hoverRadius * 2);
                SelectObject(dc, previousPen);
                SelectObject(dc, previousBrush);
                DeleteObject(hover);
            }

            if (entries_[i].active)
            {
                const int cx = left + outerPad;
                const int cy = outerPad;
                HBRUSH dot = CreateSolidBrush(activeColor);
                HGDIOBJ previousBrush = SelectObject(dc, dot);
                HGDIOBJ previousPen =
                    SelectObject(dc, GetStockObject(NULL_PEN));
                Ellipse(dc,
                        cx - activeRadius,
                        cy - activeRadius,
                        cx + activeRadius + 1,
                        cy + activeRadius + 1);
                SelectObject(dc, previousPen);
                SelectObject(dc, previousBrush);
                DeleteObject(dot);
            }

            SetTextColor(
                dc,
                entries_[i].minimized
                    ? mutedTextColor : textColor);

            int textLeft = left + outerPad;
            int textRight = right - outerPad;

            // The title is subordinate to the preview: constrain it to the
            // actual rendered thumbnail width, not the whole card width.
            if (i < thumbnailRects_.size())
            {
                const RECT& thumbnail = thumbnailRects_[i];
                if (thumbnail.right > thumbnail.left)
                {
                    textLeft = thumbnail.left;
                    textRight = thumbnail.right;
                }
            }

            RECT textRect{
                textLeft,
                height - titleHeight - titleBottomPad,
                textRight,
                height - titleBottomPad};

            DrawTextW(
                dc,
                entries_[i].title.c_str(),
                static_cast<int>(entries_[i].title.size()),
                &textRect,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE
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

    if (!nativeRoundedCorners_)
    {
        // Win10 fallback: keep an inset outline. The outer HRGN is binary,
        // so only Win11's compositor-native path can be perfectly antialiased.
        const int strokeWidth = (std::max)(1, ScalePx(1.0f, dpiScale_));
        const int inset = (std::max)(1, strokeWidth);
        HPEN borderPen = CreatePen(PS_SOLID, strokeWidth, borderColor);
        HGDIOBJ previousPen = SelectObject(dc, borderPen);
        HGDIOBJ previousBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        RoundRect(dc,
                  inset, inset,
                  client.right - inset,
                  client.bottom - inset,
                  (std::max)(2, diameter - inset * 2),
                  (std::max)(2, diameter - inset * 2));
        SelectObject(dc, previousBrush);
        SelectObject(dc, previousPen);
        DeleteObject(borderPen);
    }

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
            create ? create->lpCreateParams : nullptr);

        if (!self)
        {
            return FALSE;
        }

        // WM_NCCREATE arrives before CreateWindowExW returns, so the member
        // HWND has not yet been assigned by Initialize(). Store it here before
        // forwarding any messages. The previous code called
        // DefWindowProcW(hwnd_ == nullptr, WM_NCCREATE, ...), which made
        // creation fail and caused LightDock to exit immediately at startup.
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(
            hwnd, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(self));
        return TRUE;
    }

    self = reinterpret_cast<WindowPreview*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

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

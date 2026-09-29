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

bool WindowPreview::Initialize(HINSTANCE instance, HWND owner, Host* host)
{
    if (hwnd_)
    {
        host_ = host;
        return true;
    }

    instance_ = instance;
    owner_ = owner;
    host_ = host;

    if (!d2dFactory_)
    {
        D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED,
            IID_PPV_ARGS(d2dFactory_.AddressOf()));
    }

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
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kWindowPreviewClass,
        L"LightDock window preview",
        WS_POPUP,
        0, 0, 1, 1,
        owner_,
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

    d2dTarget_.Reset();
    d2dFactory_.Reset();
    owner_ = nullptr;
    host_ = nullptr;
    visible_ = false;
    hoveredRow_ = -1;
    pressedRow_ = -1;
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

    const int frameInset = ScalePx(5.0f, uiScale);
    const int innerPad = ScalePx(4.0f, uiScale);
    const int topPad = ScalePx(7.0f, uiScale);
    const int titleHeight = ScalePx(20.0f, uiScale);
    const int titleGap = ScalePx(3.0f, uiScale);
    const int previewHeight = ScalePx(85.0f, uiScale);
    const int previewTop = topPad + titleHeight + titleGap;
    const int previewBottom = (std::min)(
        height, previewTop + previewHeight);

    const BYTE thumbnailOpacity = static_cast<BYTE>(std::lround(
        255.0f * ClampF(opacity_, 0.0f, 1.0f)));

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
            1, cardRight - cardLeft - frameInset * 2 - innerPad * 2);
        const int maxHeight = (std::max)(
            1, previewBottom - previewTop - innerPad);

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
            previewTop + (previewBottom - previewTop - drawHeight) / 2;

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
        properties.opacity = thumbnailOpacity;
        properties.fVisible = TRUE;
        properties.fSourceClientAreaOnly = FALSE;

        if (api.updateThumbnailProperties)
        {
            api.updateThumbnailProperties(thumbnail, &properties);
        }
    }
}

void WindowPreview::Show(const RECT& screenRect,
                         const std::wstring& applicationName,
                         const std::vector<Entry>& entries,
                         int hoveredRow,
                         float dpiScale,
                         float scale,
                         float cornerRadius,
                         float opacity)
{
    if (!hwnd_ && !Initialize(
            instance_ ? instance_ : GetModuleHandleW(nullptr),
            owner_,
            host_))
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

    const float nextDpiScale = (std::max)(dpiScale, 1.0f);
    const float nextMenuScale = ClampF(scale, 0.5f, 2.0f);
    const float nextCornerRadius = ClampF(cornerRadius, 0.0f, 40.0f);
    const float nextOpacity = ClampF(opacity, 0.0f, 1.0f);

    bool sourcesChanged = entries_.size() != entries.size();
    bool contentChanged = sourcesChanged
        || applicationName_ != applicationName;

    if (!sourcesChanged)
    {
        for (size_t i = 0; i < entries.size(); ++i)
        {
            if (entries_[i].hwnd != entries[i].hwnd)
            {
                sourcesChanged = true;
                contentChanged = true;
                break;
            }

            if (entries_[i].title != entries[i].title
                || entries_[i].active != entries[i].active
                || entries_[i].minimized != entries[i].minimized)
            {
                contentChanged = true;
            }
        }
    }

    const bool wasVisible = visible_;
    const bool sizeChanged =
        !wasVisible || width != lastClientWidth_ || height != lastClientHeight_;
    const bool hoverChanged = hoveredRow_ != hoveredRow;

    const bool scaleChanged =
        std::fabs(dpiScale_ - nextDpiScale) > 0.0001f
        || std::fabs(menuScale_ - nextMenuScale) > 0.0001f;
    const bool cornerChanged =
        std::fabs(cornerRadius_ - nextCornerRadius) > 0.0001f;
    const bool opacityChanged =
        std::fabs(opacity_ - nextOpacity) > 0.0001f;

    RECT currentBounds{};
    GetWindowRect(hwnd_, &currentBounds);
    const bool positionChanged =
        !wasVisible
        || currentBounds.left != screenRect.left
        || currentBounds.top != screenRect.top;

    const int previousHoveredRow = hoveredRow_;

    applicationName_ = applicationName;
    entries_ = entries;
    hoveredRow_ = hoveredRow;
    dpiScale_ = nextDpiScale;
    menuScale_ = nextMenuScale;
    cornerRadius_ = nextCornerRadius;
    opacity_ = nextOpacity;
    lastClientWidth_ = width;
    lastClientHeight_ = height;

    if (!wasVisible)
    {
        SetWindowPos(
            hwnd_, HWND_TOPMOST,
            screenRect.left, screenRect.top,
            width, height,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    else if (positionChanged || sizeChanged)
    {
        UINT flags = SWP_NOACTIVATE | SWP_NOZORDER;
        if (!sizeChanged)
        {
            flags |= SWP_NOSIZE;
        }

        SetWindowPos(
            hwnd_, nullptr,
            screenRect.left, screenRect.top,
            width, height,
            flags);
    }

    const int radius = ScalePx(
        cornerRadius_, dpiScale_ * menuScale_);

    if (!wasVisible || cornerChanged)
    {
        nativeRoundedCorners_ =
            ApplyNativeRoundedCorners(hwnd_, cornerRadius_);

        if (nativeRoundedCorners_)
        {
            SetWindowRgn(hwnd_, nullptr, TRUE);
        }
    }

    if (!nativeRoundedCorners_
        && (!wasVisible || sizeChanged || cornerChanged))
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

    const bool layoutChanged = sizeChanged || scaleChanged;

    if (sourcesChanged)
    {
        RebuildThumbnails();
    }
    else if (layoutChanged || opacityChanged)
    {
        UpdateThumbnailRects();
    }

    visible_ = true;

    const bool needsFullClientRepaint =
        !wasVisible || contentChanged || layoutChanged
        || cornerChanged || opacityChanged;

    if (needsFullClientRepaint)
    {
        InvalidateRect(hwnd_, nullptr, FALSE);
        if (!wasVisible)
        {
            UpdateWindow(hwnd_);
        }
    }
    else if (hoverChanged)
    {
        InvalidateHoverTransition(previousHoveredRow, hoveredRow_);
    }
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
    pressedRow_ = -1;

    if (GetCapture() == hwnd_)
    {
        ReleaseCapture();
    }
}

void WindowPreview::InvalidateHoverTransition(int oldRow, int newRow)
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

    const float uiScale = dpiScale_ * menuScale_;
    const int itemWidth =
        width / static_cast<int>(entries_.size());
    const int topPad = ScalePx(7.0f, uiScale);
    const int titleHeight = ScalePx(20.0f, uiScale);
    const int titleGap = ScalePx(3.0f, uiScale);
    const int previewHeight = ScalePx(85.0f, uiScale);
    const int frameBottomPad = ScalePx(5.0f, uiScale);
    const int appNameHeight = ScalePx(24.0f, uiScale);
    const int appBottomPad = ScalePx(5.0f, uiScale);
    const int previewTop = topPad + titleHeight + titleGap;
    const int previewBottom = (std::min)(
        height, previewTop + previewHeight);
    const int frameBottom = (std::min)(
        height - appNameHeight - appBottomPad,
        previewBottom + frameBottomPad);

    auto invalidateRow = [&](int row)
    {
        if (row < 0 || row >= static_cast<int>(entries_.size()))
        {
            return;
        }

        // Hover only changes the card outline. Invalidating the whole card
        // made GDI clear and redraw the window title on every left/right
        // hover step, which produced a visible title flash. Repaint only the
        // thin border strips around the rounded card so the title/thumbnail
        // interior stays completely untouched.
        const int frameInset = ScalePx(5.0f, uiScale);
        const int repaintPad = (std::max)(2, ScalePx(3.0f, uiScale));

        const int cardLeft = row * itemWidth;
        const int cardRight = row + 1 == static_cast<int>(entries_.size())
            ? width : (row + 1) * itemWidth;

        const int frameLeft = cardLeft + frameInset;
        const int frameRight = cardRight - frameInset;
        const int frameTop = topPad;

        RECT top{
            (std::max)(0, frameLeft - repaintPad),
            (std::max)(0, frameTop - repaintPad),
            (std::min)(width, frameRight + repaintPad),
            (std::min)(height, frameTop + repaintPad + 1)};

        RECT bottom{
            (std::max)(0, frameLeft - repaintPad),
            (std::max)(0, frameBottom - repaintPad - 1),
            (std::min)(width, frameRight + repaintPad),
            (std::min)(height, frameBottom + repaintPad)};

        RECT left{
            (std::max)(0, frameLeft - repaintPad),
            (std::max)(0, frameTop),
            (std::min)(width, frameLeft + repaintPad + 1),
            (std::min)(height, frameBottom)};

        RECT right{
            (std::max)(0, frameRight - repaintPad - 1),
            (std::max)(0, frameTop),
            (std::min)(width, frameRight + repaintPad),
            (std::min)(height, frameBottom)};

        InvalidateRect(hwnd_, &top, FALSE);
        InvalidateRect(hwnd_, &bottom, FALSE);
        InvalidateRect(hwnd_, &left, FALSE);
        InvalidateRect(hwnd_, &right, FALSE);
    };

    invalidateRow(oldRow);
    if (newRow != oldRow)
    {
        invalidateRow(newRow);
    }
}

void WindowPreview::SetHoveredRow(int hoveredRow)
{
    if (hoveredRow_ == hoveredRow)
    {
        return;
    }

    const int oldRow = hoveredRow_;
    hoveredRow_ = hoveredRow;

    if (hwnd_ && visible_)
    {
        InvalidateHoverTransition(oldRow, hoveredRow_);
    }
}

int WindowPreview::EntryAtPoint(int x, int y) const
{
    if (!hwnd_ || entries_.empty())
    {
        return -1;
    }

    RECT client{};
    GetClientRect(hwnd_, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0
        || x < 0 || x >= width || y < 0 || y >= height)
    {
        return -1;
    }

    const float uiScale = dpiScale_ * menuScale_;
    const int topPad = ScalePx(7.0f, uiScale);
    const int titleHeight = ScalePx(20.0f, uiScale);
    const int titleGap = ScalePx(3.0f, uiScale);
    const int previewHeight = ScalePx(85.0f, uiScale);
    const int frameBottomPad = ScalePx(5.0f, uiScale);
    const int appNameHeight = ScalePx(24.0f, uiScale);
    const int appBottomPad = ScalePx(5.0f, uiScale);
    const int previewTop = topPad + titleHeight + titleGap;
    const int previewBottom = (std::min)(
        height, previewTop + previewHeight);
    const int frameBottom = (std::min)(
        height - appNameHeight - appBottomPad,
        previewBottom + frameBottomPad);

    if (y < topPad || y > frameBottom)
    {
        return -1;
    }

    const int itemWidth =
        width / static_cast<int>(entries_.size());
    if (itemWidth <= 0)
    {
        return -1;
    }

    const int row = x / itemWidth;
    return row >= 0 && row < static_cast<int>(entries_.size())
        ? row : -1;
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

    const COLORREF backgroundColor = RGB(245, 250, 255);
    const COLORREF borderColor = RGB(184, 199, 214);
    const COLORREF textColor = RGB(26, 36, 46);
    const COLORREF mutedTextColor = RGB(112, 124, 136);
    const COLORREF activeColor = RGB(56, 173, 255);

    HBRUSH background = CreateSolidBrush(backgroundColor);

    if (nativeRoundedCorners_)
    {
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

    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    const int itemWidth = entries_.empty()
        ? width : width / static_cast<int>(entries_.size());

    const int topPad = ScalePx(7.0f, uiScale);
    const int frameInset = ScalePx(5.0f, uiScale);
    const int titleHeight = ScalePx(20.0f, uiScale);
    const int titleGap = ScalePx(3.0f, uiScale);
    const int previewHeight = ScalePx(85.0f, uiScale);
    const int frameBottomPad = ScalePx(5.0f, uiScale);
    const int appNameHeight = ScalePx(24.0f, uiScale);
    const int appBottomPad = ScalePx(5.0f, uiScale);
    const int previewTop = topPad + titleHeight + titleGap;
    const int previewBottom = (std::min)(
        height, previewTop + previewHeight);
    const int frameBottom = (std::min)(
        height - appNameHeight - appBottomPad,
        previewBottom + frameBottomPad);

    bool antialiasedChrome = false;
    if (d2dFactory_ && !entries_.empty())
    {
        if (!d2dTarget_)
        {
            const D2D1_RENDER_TARGET_PROPERTIES properties =
                D2D1::RenderTargetProperties(
                    D2D1_RENDER_TARGET_TYPE_DEFAULT,
                    D2D1::PixelFormat(
                        DXGI_FORMAT_B8G8R8A8_UNORM,
                        D2D1_ALPHA_MODE_IGNORE),
                    0.0f, 0.0f,
                    D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE);

            d2dFactory_->CreateDCRenderTarget(
                &properties, d2dTarget_.AddressOf());
        }

        if (d2dTarget_ && SUCCEEDED(d2dTarget_->BindDC(dc, &client)))
        {
            ComPtr<ID2D1SolidColorBrush> frameFillBrush;
            ComPtr<ID2D1SolidColorBrush> frameBorderBrush;
            ComPtr<ID2D1SolidColorBrush> hoverBorderBrush;
            ComPtr<ID2D1SolidColorBrush> activeBrush;

            d2dTarget_->CreateSolidColorBrush(
                D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),
                frameFillBrush.AddressOf());
            d2dTarget_->CreateSolidColorBrush(
                D2D1::ColorF(0.72f, 0.78f, 0.84f, 1.0f),
                frameBorderBrush.AddressOf());
            d2dTarget_->CreateSolidColorBrush(
                D2D1::ColorF(0.22f, 0.68f, 1.0f, 0.95f),
                hoverBorderBrush.AddressOf());
            d2dTarget_->CreateSolidColorBrush(
                D2D1::ColorF(0.22f, 0.68f, 1.0f, 1.0f),
                activeBrush.AddressOf());

            if (frameFillBrush && frameBorderBrush
                && hoverBorderBrush && activeBrush)
            {
                d2dTarget_->BeginDraw();
                d2dTarget_->SetAntialiasMode(
                    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

                const float frameRadius = 9.0f * uiScale;
                const float frameStroke =
                    (std::max)(1.0f, 1.0f * dpiScale_);
                const float hoverStroke =
                    (std::max)(1.5f, 2.0f * dpiScale_);
                const float activeRadiusF = 3.0f * uiScale;

                for (size_t i = 0; i < entries_.size(); ++i)
                {
                    const float left =
                        static_cast<float>(i * itemWidth);
                    const float right = (i + 1 == entries_.size())
                        ? static_cast<float>(width)
                        : static_cast<float>((i + 1) * itemWidth);

                    const D2D1_RECT_F frame = D2D1::RectF(
                        left + static_cast<float>(frameInset),
                        static_cast<float>(topPad),
                        right - static_cast<float>(frameInset),
                        static_cast<float>(frameBottom));

                    d2dTarget_->FillRoundedRectangle(
                        D2D1::RoundedRect(frame, frameRadius, frameRadius),
                        frameFillBrush.Get());
                    d2dTarget_->DrawRoundedRectangle(
                        D2D1::RoundedRect(frame, frameRadius, frameRadius),
                        frameBorderBrush.Get(),
                        frameStroke);

                    if (static_cast<int>(i) == hoveredRow_)
                    {
                        // The hover outline is the card selection state:
                        // include the title and live thumbnail as one unit.
                        d2dTarget_->DrawRoundedRectangle(
                            D2D1::RoundedRect(
                                frame, frameRadius, frameRadius),
                            hoverBorderBrush.Get(),
                            hoverStroke);
                    }

                    if (entries_[i].active)
                    {
                        const D2D1_POINT_2F center = D2D1::Point2F(
                            left + 10.0f * uiScale,
                            static_cast<float>(topPad) + 9.0f * uiScale);
                        d2dTarget_->FillEllipse(
                            D2D1::Ellipse(
                                center, activeRadiusF, activeRadiusF),
                            activeBrush.Get());
                    }
                }

                const HRESULT endResult = d2dTarget_->EndDraw();
                antialiasedChrome = SUCCEEDED(endResult);
                if (endResult == static_cast<HRESULT>(0x8899000CL))
                {
                    d2dTarget_.Reset();
                }
            }
        }
    }

    if (!entries_.empty())
    {
        HFONT titleFont = CreateFontW(
            -ScalePx(13.0f, uiScale),
            0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI");

        // The application label at the bottom of the preview bubble should
        // read at the same visual weight as the normal tooltip bubble. It was
        // still perceptually smaller after inheriting the preview menu scale,
        // so give just this label another 10% size.
        HFONT appFont = CreateFontW(
            -ScalePx(17.0f, uiScale),
            0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI");

        HGDIOBJ oldFont = titleFont
            ? SelectObject(dc, titleFont) : nullptr;
        SetBkMode(dc, TRANSPARENT);

        for (size_t i = 0; i < entries_.size(); ++i)
        {
            const int left = static_cast<int>(i) * itemWidth;
            const int right = (i + 1 == entries_.size())
                ? width : left + itemWidth;

            if (!antialiasedChrome)
            {
                RECT frame{
                    left + frameInset,
                    topPad,
                    right - frameInset,
                    frameBottom};
                HBRUSH frameFill = CreateSolidBrush(RGB(255, 255, 255));
                HPEN framePen = CreatePen(PS_SOLID, 1, borderColor);
                HGDIOBJ previousBrush = SelectObject(dc, frameFill);
                HGDIOBJ previousPen = SelectObject(dc, framePen);
                const int frameRadius = ScalePx(9.0f, uiScale);
                RoundRect(dc,
                          frame.left, frame.top,
                          frame.right, frame.bottom,
                          frameRadius * 2, frameRadius * 2);
                SelectObject(dc, previousPen);
                SelectObject(dc, previousBrush);
                DeleteObject(framePen);
                DeleteObject(frameFill);
            }

            SetTextColor(
                dc,
                entries_[i].minimized
                    ? mutedTextColor : textColor);

            int textLeft = left + frameInset + ScalePx(4.0f, uiScale);
            int textRight = right - frameInset - ScalePx(4.0f, uiScale);

            if (i < thumbnailRects_.size())
            {
                const RECT& thumbnail = thumbnailRects_[i];
                if (thumbnail.right > thumbnail.left)
                {
                    textLeft = thumbnail.left;
                    textRight = thumbnail.right;
                }
            }

            RECT titleRect{
                textLeft,
                topPad,
                textRight,
                topPad + titleHeight};

            DrawTextW(
                dc,
                entries_[i].title.c_str(),
                static_cast<int>(entries_[i].title.size()),
                &titleRect,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE
                    | DT_END_ELLIPSIS | DT_NOPREFIX);
        }

        if (appFont)
        {
            SelectObject(dc, appFont);
        }

        SetTextColor(dc, textColor);
        RECT appNameRect{
            ScalePx(8.0f, uiScale),
            frameBottom,
            width - ScalePx(8.0f, uiScale),
            height - appBottomPad};
        DrawTextW(
            dc,
            applicationName_.c_str(),
            static_cast<int>(applicationName_.size()),
            &appNameRect,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE
                | DT_END_ELLIPSIS | DT_NOPREFIX);

        if (oldFont)
        {
            SelectObject(dc, oldFont);
        }
        if (titleFont)
        {
            DeleteObject(titleFont);
        }
        if (appFont)
        {
            DeleteObject(appFont);
        }
    }

    if (!nativeRoundedCorners_)
    {
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
        // DWM thumbnails live in this dedicated popup, so the popup must own
        // its clicks too. Hover remains non-activating via WM_MOUSEACTIVATE.
        return HTCLIENT;

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_LBUTTONDOWN:
    {
        pressedRow_ = EntryAtPoint(
            GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        if (pressedRow_ >= 0)
        {
            SetCapture(hwnd_);
        }
        return 0;
    }

    case WM_LBUTTONUP:
    {
        const int releasedRow = EntryAtPoint(
            GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        const int pressedRow = pressedRow_;
        pressedRow_ = -1;

        if (GetCapture() == hwnd_)
        {
            ReleaseCapture();
        }

        if (pressedRow >= 0
            && releasedRow == pressedRow
            && host_)
        {
            host_->OnPreviewWindowActivated(pressedRow);
        }
        return 0;
    }

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

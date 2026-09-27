#include "DockRenderer.h"

namespace ld
{

namespace
{

/// Signed distance to a rounded rectangle (negative inside).
float RoundedRectDistance(float px, float py, const D2D1_RECT_F& rect, float radius)
{
    const float halfWidth = (rect.right - rect.left) * 0.5f;
    const float halfHeight = (rect.bottom - rect.top) * 0.5f;

    const float cx = rect.left + halfWidth;
    const float cy = rect.top + halfHeight;
    const float r = (std::min)(radius, (std::min)(halfWidth, halfHeight));

    const float dx = std::fabs(px - cx) - (halfWidth - r);
    const float dy = std::fabs(py - cy) - (halfHeight - r);

    const float ox = (std::max)(dx, 0.0f);
    const float oy = (std::max)(dy, 0.0f);

    return std::sqrt(ox * ox + oy * oy)
        + (std::min)((std::max)(dx, dy), 0.0f) - r;
}

/// D2DERR_RECREATE_TARGET, spelled out so we do not depend on d2derr.h.
constexpr HRESULT kRecreateTarget = static_cast<HRESULT>(0x8899000CL);

/// Separable box blur with edge clamping. Three passes approximate a Gaussian.
void BoxBlurHorizontal(float* data, int width, int height, int radius)
{
    if (width <= 0 || height <= 0 || radius <= 0)
    {
        return;
    }

    std::vector<float> temp(static_cast<size_t>(width));
    const float inverse = 1.0f / static_cast<float>(radius * 2 + 1);

    for (int y = 0; y < height; ++y)
    {
        float* row = data + static_cast<size_t>(y) * static_cast<size_t>(width);

        double acc = static_cast<double>(row[0]) * static_cast<double>(radius + 1);
        for (int i = 1; i <= radius; ++i)
        {
            const int index = (std::min)(i, width - 1);
            acc += row[index];
        }

        for (int x = 0; x < width; ++x)
        {
            temp[static_cast<size_t>(x)] = static_cast<float>(acc * inverse);

            const int outIndex = (std::max)(x - radius, 0);
            const int inIndex = (std::min)(x + radius + 1, width - 1);

            acc -= row[outIndex];
            acc += row[inIndex];
        }

        std::memcpy(row, temp.data(), static_cast<size_t>(width) * sizeof(float));
    }
}

void BoxBlurVertical(float* data, int width, int height, int radius)
{
    if (width <= 0 || height <= 0 || radius <= 0)
    {
        return;
    }

    std::vector<float> temp(static_cast<size_t>(height));
    const float inverse = 1.0f / static_cast<float>(radius * 2 + 1);

    for (int x = 0; x < width; ++x)
    {
        double acc = static_cast<double>(data[x]) * static_cast<double>(radius + 1);
        for (int i = 1; i <= radius; ++i)
        {
            const int index = (std::min)(i, height - 1);
            acc += data[static_cast<size_t>(index) * static_cast<size_t>(width) + x];
        }

        for (int y = 0; y < height; ++y)
        {
            temp[static_cast<size_t>(y)] = static_cast<float>(acc * inverse);

            const int outIndex = (std::max)(y - radius, 0);
            const int inIndex = (std::min)(y + radius + 1, height - 1);

            acc -= data[static_cast<size_t>(outIndex) * static_cast<size_t>(width) + x];
            acc += data[static_cast<size_t>(inIndex) * static_cast<size_t>(width) + x];
        }

        for (int y = 0; y < height; ++y)
        {
            data[static_cast<size_t>(y) * static_cast<size_t>(width) + x] =
                temp[static_cast<size_t>(y)];
        }
    }
}

} // namespace

DockRenderer::~DockRenderer()
{
    Shutdown();
}

bool DockRenderer::Initialize()
{
    if (FAILED(CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(wic_.AddressOf()))))
    {
        return false;
    }

    if (FAILED(D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(d2d_.AddressOf()))))
    {
        return false;
    }

    if (FAILED(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(dwrite_.AddressOf()))))
    {
        return false;
    }

    screenDC_ = GetDC(nullptr);
    return d2d_ && wic_;
}

void DockRenderer::Shutdown()
{
    ReleaseSurface();

    if (screenDC_)
    {
        ReleaseDC(nullptr, screenDC_);
        screenDC_ = nullptr;
    }

    shadow_.Reset();
    rt_.Reset();
    dwrite_.Reset();
    d2d_.Reset();
    wic_.Reset();
}

void DockRenderer::ReleaseSurface()
{
    if (rt_)
    {
        rt_.Reset();
    }

    backgroundBrush_.Reset();
    panelGradient_.Reset();
    edgeBrush_.Reset();
    rimBrush_.Reset();
    indicatorBrush_.Reset();
    plateSolid_.Reset();
    plateGradient_.Reset();
    plateReady_ = false;
    shadow_.Reset();

    if (memoryDC_)
    {
        if (previousBitmap_)
        {
            SelectObject(memoryDC_, previousBitmap_);
            previousBitmap_ = nullptr;
        }

        DeleteDC(memoryDC_);
        memoryDC_ = nullptr;
    }

    if (dib_)
    {
        DeleteObject(dib_);
        dib_ = nullptr;
        dibBits_ = nullptr;
    }

    width_ = 0;
    height_ = 0;
    brushDirty_ = true;
    shadowDirty_ = true;
}

bool DockRenderer::Resize(int width, int height)
{
    if (width <= 0 || height <= 0 || !d2d_)
    {
        return false;
    }

    if (rt_ && width == width_ && height == height_)
    {
        return true;
    }

    ReleaseSurface();

    HDC reference = screenDC_ ? screenDC_ : GetDC(nullptr);
    if (!reference)
    {
        return false;
    }

    memoryDC_ = CreateCompatibleDC(reference);
    if (!memoryDC_)
    {
        if (!screenDC_)
        {
            ReleaseDC(nullptr, reference);
        }

        return false;
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height; // top down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    dib_ = CreateDIBSection(
        reference, &info, DIB_RGB_COLORS, &dibBits_, nullptr, 0);

    if (!screenDC_)
    {
        ReleaseDC(nullptr, reference);
    }

    if (!dib_)
    {
        ReleaseSurface();
        return false;
    }

    if (dibBits_)
    {
        std::memset(dibBits_, 0, static_cast<size_t>(width) * height * 4);
    }

    previousBitmap_ = SelectObject(memoryDC_, dib_);

    const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f,
        96.0f);

    if (FAILED(d2d_->CreateDCRenderTarget(&properties, rt_.AddressOf())))
    {
        ReleaseSurface();
        return false;
    }

    const RECT surface{0, 0, width, height};
    if (FAILED(rt_->BindDC(memoryDC_, &surface)))
    {
        ReleaseSurface();
        return false;
    }

    rt_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    width_ = width;
    height_ = height;
    brushDirty_ = true;
    shadowDirty_ = true;

    return true;
}

void DockRenderer::SetOrientation(DockEdge edge,
                                  float logicalWidth,
                                  float logicalHeight)
{
    edge_ = edge;
    logicalWidth_ = logicalWidth;
    logicalHeight_ = logicalHeight;
}

bool DockRenderer::BeginDraw()
{
    if (!rt_)
    {
        return false;
    }

    rt_->BeginDraw();
    rt_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    const DockTransform transform(edge_, logicalWidth_, logicalHeight_);
    rt_->SetTransform(transform.Matrix());

    drawing_ = true;

    if (shadowDirty_)
    {
        BuildShadow();
    }

    EnsureBrushes();

    return true;
}

bool DockRenderer::EndDraw()
{
    if (!rt_ || !drawing_)
    {
        return false;
    }

    drawing_ = false;

    const HRESULT hr = rt_->EndDraw(nullptr, nullptr);

    if (hr == kRecreateTarget)
    {
        const int width = width_;
        const int height = height_;
        ReleaseSurface();
        return Resize(width, height);
    }

    return SUCCEEDED(hr);
}

void DockRenderer::SetPanelStyle(float shadowBaseWidth,
                                 float panelHeight,
                                 float cornerRadius,
                                 float shadowOpacity,
                                 float shadowBlur,
                                 float shadowOffsetY)
{
    if (shadowBaseWidth != shadowBaseWidth_
        || panelHeight != panelHeight_
        || cornerRadius != cornerRadius_
        || shadowOpacity != shadowOpacity_
        || shadowBlur != shadowBlur_
        || shadowOffsetY != shadowOffsetY_)
    {
        brushDirty_ = true;
        shadowDirty_ = true;
    }

    shadowBaseWidth_ = shadowBaseWidth;
    panelHeight_ = panelHeight;
    cornerRadius_ = cornerRadius;
    shadowOpacity_ = shadowOpacity;
    shadowBlur_ = shadowBlur;
    shadowOffsetY_ = shadowOffsetY;
}

void DockRenderer::EnsureBrushes()
{
    if (!rt_ || !brushDirty_)
    {
        return;
    }

    backgroundBrush_.Reset();
    panelGradient_.Reset();
    edgeBrush_.Reset();
    rimBrush_.Reset();
    indicatorBrush_.Reset();

    // Built-in look: plain white. The panel opacity (a separate knob) decides
    // how much of the wallpaper shows through.
    rt_->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),
        backgroundBrush_.AddressOf());

    if (panelCustom_)
    {
        const D2D1_GRADIENT_STOP bgStops[] =
        {
            {0.0f, panelTop_},
            {1.0f, panelBottom_},
        };

        ComPtr<ID2D1GradientStopCollection> bgCollection;
        if (SUCCEEDED(rt_->CreateGradientStopCollection(
                bgStops,
                static_cast<UINT32>(ARRAYSIZE(bgStops)),
                D2D1_GAMMA_2_2,
                D2D1_EXTEND_MODE_CLAMP,
                bgCollection.AddressOf())))
        {
            rt_->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(
                    D2D1::Point2F(0.0f, 0.0f),
                    D2D1::Point2F(0.0f, panelHeight_)),
                bgCollection.Get(),
                panelGradient_.AddressOf());
        }
    }

    // The running dots must read against the panel: dark on a light panel,
    // near white on a dark one.
    const bool lightPanel = panelLuminance_ > 0.6f;

    rt_->CreateSolidColorBrush(
        lightPanel
            ? D2D1::ColorF(0.16f, 0.17f, 0.20f, 0.85f)
            : D2D1::ColorF(0.92f, 0.94f, 0.99f, 1.0f),
        indicatorBrush_.AddressOf());

    // Panel outline: one uniform hairline around the whole panel. No gradient
    // and no extra top highlight — every side gets exactly the same colour
    // and the same 1px weight, matching the clean macOS look.
    rt_->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),
        edgeBrush_.AddressOf());

    rt_->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), rimBrush_.AddressOf());

    brushDirty_ = false;
}

void DockRenderer::BuildShadow()
{
    shadow_.Reset();
    shadowDirty_ = true;

    if (!rt_ || width_ == 0)
    {
        return;
    }

    const float panelWidth = shadowBaseWidth_;
    const float panelHeight = panelHeight_;

    if (panelWidth <= 0.0f || panelHeight <= 0.0f)
    {
        return;
    }

    const float spread = 1.0f;
    const int extent = static_cast<int>(
        std::ceil(shadowBlur_ * 2.0f + std::fabs(shadowOffsetY_) + spread + 2.0f));

    const int bitmapWidth = static_cast<int>(std::ceil(panelWidth)) + extent * 2;
    const int bitmapHeight = static_cast<int>(std::ceil(panelHeight)) + extent * 2;

    if (bitmapWidth <= 0 || bitmapHeight <= 0)
    {
        return;
    }

    const D2D1_RECT_F shape = D2D1::RectF(
        static_cast<float>(extent) - spread,
        static_cast<float>(extent) - spread + shadowOffsetY_,
        static_cast<float>(extent) + panelWidth + spread,
        static_cast<float>(extent) + panelHeight + spread);

    std::vector<float> alpha(
        static_cast<size_t>(bitmapWidth) * static_cast<size_t>(bitmapHeight), 0.0f);

    for (int y = 0; y < bitmapHeight; ++y)
    {
        const float py = static_cast<float>(y) + 0.5f;

        for (int x = 0; x < bitmapWidth; ++x)
        {
            const float px = static_cast<float>(x) + 0.5f;
            const float distance =
                RoundedRectDistance(px, py, shape, cornerRadius_ + spread);

            const float coverage = ClampF(0.5f - distance, 0.0f, 1.0f);

            alpha[static_cast<size_t>(y) * static_cast<size_t>(bitmapWidth)
                  + static_cast<size_t>(x)] = coverage * shadowOpacity_;
        }
    }

    int radius = static_cast<int>(std::round(shadowBlur_ * 0.5f));
    if (radius < 1)
    {
        radius = 1;
    }

    for (int pass = 0; pass < 3; ++pass)
    {
        BoxBlurHorizontal(alpha.data(), bitmapWidth, bitmapHeight, radius);
        BoxBlurVertical(alpha.data(), bitmapWidth, bitmapHeight, radius);
    }

    // Black shadow: premultiplied RGB stays zero, only alpha matters.
    std::vector<uint32_t> pixels(alpha.size(), 0);
    for (size_t i = 0; i < alpha.size(); ++i)
    {
        float a = ClampF(alpha[i], 0.0f, 1.0f) * 255.0f + 0.5f;
        if (a < 0.0f)
        {
            a = 0.0f;
        }

        if (a > 255.0f)
        {
            a = 255.0f;
        }

        pixels[i] = static_cast<uint32_t>(a) << 24;
    }

    const D2D1_BITMAP_PROPERTIES properties = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

    if (SUCCEEDED(rt_->CreateBitmap(
            D2D1::SizeU(static_cast<UINT32>(bitmapWidth),
                        static_cast<UINT32>(bitmapHeight)),
            pixels.data(),
            static_cast<UINT32>(bitmapWidth * 4),
            &properties,
            shadow_.AddressOf())))
    {
        shadowWidth_ = bitmapWidth;
        shadowHeight_ = bitmapHeight;
        shadowExtent_ = static_cast<float>(extent);

        // The side slices must contain the whole rounded corner plus the
        // shadow spilling past it, otherwise stretching would bend them.
        shadowSegment_ = (std::min)(
            shadowExtent_ + cornerRadius_ + shadowBlur_ * 1.5f,
            static_cast<float>(bitmapWidth) * 0.45f);

        shadowDirty_ = false;
    }
}

void DockRenderer::DrawShadow(float panelX, float panelY, float panelWidth)
{
    if (!rt_ || !shadow_)
    {
        return;
    }

    const float sourceWidth = static_cast<float>(shadowWidth_);
    const float sourceHeight = static_cast<float>(shadowHeight_);

    const float left = panelX - shadowExtent_;
    const float top = panelY - shadowExtent_;
    const float width = panelWidth + shadowExtent_ * 2.0f;
    const float segment = shadowSegment_;

    if (width <= segment * 2.0f + 1.0f)
    {
        // Degenerate: just stretch the whole thing.
        rt_->DrawBitmap(
            shadow_.Get(),
            D2D1::RectF(left, top, left + width, top + sourceHeight),
            1.0f,
            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        return;
    }

    // Horizontal nine slicing: the rounded ends are drawn at 1:1 and only the
    // straight middle band is stretched, so the shadow can follow the panel
    // width without being re-blurred every frame.
    D2D1_RECT_F source = D2D1::RectF(0.0f, 0.0f, segment, sourceHeight);
    rt_->DrawBitmap(
        shadow_.Get(),
        D2D1::RectF(left, top, left + segment, top + sourceHeight),
        1.0f,
        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
        &source);

    source = D2D1::RectF(segment, 0.0f, sourceWidth - segment, sourceHeight);
    rt_->DrawBitmap(
        shadow_.Get(),
        D2D1::RectF(left + segment, top,
                    left + width - segment, top + sourceHeight),
        1.0f,
        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
        &source);

    source = D2D1::RectF(sourceWidth - segment, 0.0f, sourceWidth, sourceHeight);
    rt_->DrawBitmap(
        shadow_.Get(),
        D2D1::RectF(left + width - segment, top,
                    left + width, top + sourceHeight),
        1.0f,
        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
        &source);
}

void DockRenderer::SetPanelBackground(const D2D1_COLOR_F& top,
                                      const D2D1_COLOR_F& bottom,
                                      bool custom)
{
    if (custom == panelCustom_
        && top.r == panelTop_.r && top.g == panelTop_.g
        && top.b == panelTop_.b && top.a == panelTop_.a
        && bottom.r == panelBottom_.r && bottom.g == panelBottom_.g
        && bottom.b == panelBottom_.b && bottom.a == panelBottom_.a)
    {
        return;
    }

    panelTop_ = top;
    panelBottom_ = bottom;
    panelCustom_ = custom;

    // Built-in look is plain white; with a custom gradient, average the two
    // stops. Rec.709 luma is good enough to tell "light panel" from "dark".
    const D2D1_COLOR_F& a = custom ? top : D2D1::ColorF(1.0f, 1.0f, 1.0f);
    const D2D1_COLOR_F& b = custom ? bottom : a;

    panelLuminance_ = custom
        ? 0.5f * (0.2126f * a.r + 0.7152f * a.g + 0.0722f * a.b
                  + 0.2126f * b.r + 0.7152f * b.g + 0.0722f * b.b)
        : 1.0f;

    brushDirty_ = true;
}

void DockRenderer::DrawBackground(float panelX,
                                  float panelY,
                                  float panelWidth,
                                  float backgroundOpacity,
                                  float borderOpacity)
{
    if (!rt_ || !backgroundBrush_)
    {
        return;
    }

    const D2D1_RECT_F rect = D2D1::RectF(
        panelX, panelY, panelX + panelWidth, panelY + panelHeight_);

    const D2D1_ROUNDED_RECT panel =
        D2D1::RoundedRect(rect, cornerRadius_, cornerRadius_);

    if (panelCustom_ && panelGradient_)
    {
        panelGradient_->SetOpacity(ClampF(backgroundOpacity, 0.0f, 1.0f));
        panelGradient_->SetTransform(
            D2D1::Matrix3x2F::Translation(0.0f, panelY));

        rt_->FillRoundedRectangle(panel, panelGradient_.Get());
    }
    else if (backgroundBrush_)
    {
        backgroundBrush_->SetOpacity(ClampF(backgroundOpacity, 0.0f, 1.0f));
        rt_->FillRoundedRectangle(panel, backgroundBrush_.Get());
    }

    if (edgeBrush_)
    {
        // One uniform hairline, same colour and weight on every side.
        edgeBrush_->SetOpacity(ClampF(borderOpacity, 0.0f, 1.0f));

        // A two-device-pixel outline reads consistently on all four sides;
        // the old 1px antialiased stroke made the top edge look stronger
        // than the vertical and bottom edges on common DPI scales.
        constexpr float borderWidth = 2.0f;
        const float inset = borderWidth * 0.5f;
        const D2D1_RECT_F stroke = D2D1::RectF(
            rect.left + inset,
            rect.top + inset,
            rect.right - inset,
            rect.bottom - inset);

        rt_->DrawRoundedRectangle(
            D2D1::RoundedRect(stroke,
                              (std::max)(cornerRadius_ - inset, 0.0f),
                              (std::max)(cornerRadius_ - inset, 0.0f)),
            edgeBrush_.Get(),
            borderWidth);
    }
}

void DockRenderer::EnsurePlateBrush(bool gradient,
                                    const D2D1_COLOR_F& top,
                                    const D2D1_COLOR_F& bottom)
{
    if (!rt_)
    {
        return;
    }

    if (plateReady_
        && gradient == plateGradientActive_
        && top.r == plateTop_.r && top.g == plateTop_.g && top.b == plateTop_.b
        && bottom.r == plateBottom_.r && bottom.g == plateBottom_.g
        && bottom.b == plateBottom_.b)
    {
        return;
    }

    plateSolid_.Reset();
    plateGradient_.Reset();
    plateReady_ = false;

    if (gradient)
    {
        const D2D1_GRADIENT_STOP plateStops[] =
        {
            {0.0f, top},
            {1.0f, bottom},
        };

        ComPtr<ID2D1GradientStopCollection> collection;
        if (SUCCEEDED(rt_->CreateGradientStopCollection(
                plateStops,
                static_cast<UINT32>(ARRAYSIZE(plateStops)),
                D2D1_GAMMA_2_2,
                D2D1_EXTEND_MODE_CLAMP,
                collection.AddressOf())))
        {
            // The brush lives in a 0..1 vertical space and is mapped onto the
            // plate by the draw call, so it can be shared by every icon.
            rt_->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(
                    D2D1::Point2F(0.0f, 0.0f),
                    D2D1::Point2F(0.0f, 1.0f)),
                collection.Get(),
                plateGradient_.AddressOf());
        }
    }
    else
    {
        rt_->CreateSolidColorBrush(top, plateSolid_.AddressOf());
    }

    plateTop_ = top;
    plateBottom_ = bottom;
    plateGradientActive_ = gradient;
    plateReady_ = true;
}

void DockRenderer::DrawIconBackdrop(const D2D1_RECT_F& rect,
                                    float radius,
                                    float opacity,
                                    bool gradient,
                                    const D2D1_COLOR_F& top,
                                    const D2D1_COLOR_F& bottom)
{
    if (!rt_)
    {
        return;
    }

    EnsurePlateBrush(gradient, top, bottom);

    const D2D1_ROUNDED_RECT plate = D2D1::RoundedRect(rect, radius, radius);

    if (gradient && plateGradient_)
    {
        plateGradient_->SetOpacity(ClampF(opacity, 0.0f, 1.0f));
        plateGradient_->SetTransform(
            D2D1::Matrix3x2F(1.0f, 0.0f, 0.0f, rect.bottom - rect.top,
                             0.0f, rect.top));

        rt_->FillRoundedRectangle(plate, plateGradient_.Get());
        return;
    }

    if (plateSolid_)
    {
        plateSolid_->SetOpacity(ClampF(opacity, 0.0f, 1.0f));
        rt_->FillRoundedRectangle(plate, plateSolid_.Get());
    }
}

void DockRenderer::DrawIndicator(float x, float y, float diameter)
{
    if (!rt_ || !indicatorBrush_)
    {
        return;
    }

    const float radius = diameter * 0.5f;

    rt_->FillEllipse(
        D2D1::Ellipse(D2D1::Point2F(x + radius, y + radius), radius, radius),
        indicatorBrush_.Get());
}

void DockRenderer::DrawIcon(ID2D1Bitmap* bitmap, const D2D1_RECT_F& destination)
{
    if (!rt_ || !bitmap)
    {
        return;
    }

    const DockTransform transform(edge_, logicalWidth_, logicalHeight_);
    const D2D1_MATRIX_3X2_F previous = transform.Matrix();
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    rt_->DrawBitmap(bitmap, transform.ToPhysical(destination), 1.0f,
                    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    rt_->SetTransform(previous);
}

void DockRenderer::DrawTooltip(const std::wstring& text,
                               float centerX,
                               float bottom,
                               float scale,
                               float dpiScale,
                               float cornerRadius,
                               float opacity,
                               bool arrowUp)
{
    if (!rt_ || !dwrite_ || text.empty())
    {
        return;
    }

    const float safeScale = ClampF(scale, 0.5f, 2.0f);
    const float fontSize = 14.0f * dpiScale * safeScale;
    const float padX = 12.0f * dpiScale * safeScale;
    const float padY = 7.0f * dpiScale * safeScale;
    const float tail = 8.0f * dpiScale * safeScale;
    const float sideTail = 9.0f * dpiScale * safeScale;
    const float sideGap = 10.0f * dpiScale * safeScale;

    ComPtr<IDWriteTextFormat> format;
    if (FAILED(dwrite_->CreateTextFormat(
            L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            fontSize, L"", format.AddressOf())))
    {
        return;
    }

    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(
            text.c_str(), static_cast<UINT32>(text.size()), format.Get(),
            1600.0f, 400.0f, layout.AddressOf())))
    {
        return;
    }

    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics)))
    {
        return;
    }

    // Constrain the drawable layout to the measured width; otherwise its
    // 1600px alignment box centres the text far outside the visible bubble.
    const float textWidth = (std::max)(metrics.width, 1.0f);
    const float textHeight = (std::max)(metrics.height, 1.0f);
    layout->SetMaxWidth(textWidth);
    layout->SetMaxHeight(textHeight);

    const float width = textWidth + padX * 2.0f;
    const float height = textHeight + padY * 2.0f;
    const bool vertical = edge_ == DockEdge::Left || edge_ == DockEdge::Right;
    const float bodyBottom = arrowUp ? bottom : bottom - tail;
    D2D1_RECT_F body{};
    float bubbleCenterX = centerX;
    float bubbleCenterY = bodyBottom - height * 0.5f;
    if (vertical)
    {
        const float anchorX = edge_ == DockEdge::Left
            ? bottom + sideGap : logicalHeight_ - bottom - sideGap;
        bubbleCenterX = edge_ == DockEdge::Left
            ? anchorX + sideTail + width * 0.5f
            : anchorX - sideTail - width * 0.5f;
        bubbleCenterY = centerX;
        body = D2D1::RectF(bubbleCenterX - width * 0.5f,
                           bubbleCenterY - height * 0.5f,
                           bubbleCenterX + width * 0.5f,
                           bubbleCenterY + height * 0.5f);
    }
    else if (arrowUp)
    {
        body = D2D1::RectF(centerX - width * 0.5f,
                           bottom + tail,
                           centerX + width * 0.5f,
                           bottom + tail + height);
    }
    else
    {
        body = D2D1::RectF(centerX - width * 0.5f,
                           bodyBottom - height,
                           centerX + width * 0.5f,
                           bodyBottom);
    }

    ComPtr<ID2D1SolidColorBrush> fill;
    ComPtr<ID2D1SolidColorBrush> border;
    ComPtr<ID2D1SolidColorBrush> ink;
    const float alpha = ClampF(opacity, 0.0f, 1.0f);
    rt_->CreateSolidColorBrush(
        D2D1::ColorF(0.96f, 0.98f, 1.0f, 0.92f * alpha), fill.AddressOf());
    rt_->CreateSolidColorBrush(
        D2D1::ColorF(0.72f, 0.78f, 0.84f, 0.95f * alpha), border.AddressOf());
    rt_->CreateSolidColorBrush(
        D2D1::ColorF(0.10f, 0.14f, 0.18f, 0.96f * alpha), ink.AddressOf());

    if (!fill || !border || !ink)
    {
        return;
    }

    if (vertical)
    {
        rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    }

    // Match the dock panel's global corner radius rather than using an
    // oversized pill shape of our own.
    const float radius = ClampF(cornerRadius, 0.0f,
        (std::min)(width, height) * 0.5f);
    rt_->FillRoundedRectangle(
        D2D1::RoundedRect(body, radius, radius), fill.Get());
    rt_->DrawRoundedRectangle(
        D2D1::RoundedRect(body, radius, radius), border.Get(),
        1.0f * dpiScale);

    ComPtr<ID2D1PathGeometry> tailGeometry;
    if (d2d_->CreatePathGeometry(tailGeometry.AddressOf()) == S_OK)
    {
        ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(tailGeometry->Open(sink.AddressOf())))
        {
            if (!vertical)
            {
                if (arrowUp)
                {
                    sink->BeginFigure(
                        D2D1::Point2F(centerX - tail, body.top),
                        D2D1_FIGURE_BEGIN_FILLED);
                    sink->AddLine(D2D1::Point2F(centerX, bottom));
                    sink->AddLine(D2D1::Point2F(centerX + tail, body.top));
                }
                else
                {
                    sink->BeginFigure(
                        D2D1::Point2F(centerX - tail, bodyBottom),
                        D2D1_FIGURE_BEGIN_FILLED);
                    sink->AddLine(D2D1::Point2F(centerX, bodyBottom + tail));
                    sink->AddLine(D2D1::Point2F(centerX + tail, bodyBottom));
                }
            }
            else if (edge_ == DockEdge::Left)
            {
                sink->BeginFigure(
                    D2D1::Point2F(body.left, bubbleCenterY - sideTail),
                    D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(D2D1::Point2F(bottom + sideGap, bubbleCenterY));
                sink->AddLine(D2D1::Point2F(
                    body.left, bubbleCenterY + sideTail));
            }
            else
            {
                sink->BeginFigure(
                    D2D1::Point2F(body.right, bubbleCenterY - sideTail),
                    D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(D2D1::Point2F(
                                            logicalHeight_ - bottom - sideGap,
                                            bubbleCenterY));
                sink->AddLine(D2D1::Point2F(body.right,
                                            bubbleCenterY + sideTail));
            }
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            rt_->FillGeometry(tailGeometry.Get(), fill.Get());
        }
    }

    const D2D1_RECT_F textRect = D2D1::RectF(
        body.left + padX, body.top + padY,
        body.right - padX, body.bottom - padY);
    rt_->DrawTextLayout(
        D2D1::Point2F(textRect.left, textRect.top), layout.Get(), ink.Get(),
        D2D1_DRAW_TEXT_OPTIONS_CLIP);

    if (vertical)
    {
        rt_->SetTransform(DockTransform(edge_, logicalWidth_, logicalHeight_)
                              .Matrix());
    }
}

void DockRenderer::DrawPlateRim(const D2D1_RECT_F& rect,
                                float radius,
                                float opacity,
                                float thickness,
                                const D2D1_COLOR_F& color)
{
    if (!rt_ || thickness <= 0.0f)
    {
        return;
    }

    const float width = rect.right - rect.left;
    const float height = rect.bottom - rect.top;

    if (width < 3.0f || height < 3.0f)
    {
        return;
    }

    const float widthPx = ClampF(thickness, 0.1f, 8.0f);
    const float inset = widthPx * 0.5f;
    const float r = (std::max)(radius - inset, 0.0f);
    if (!rimBrush_)
    {
        return;
    }
    rimBrush_->SetColor(color);
    rimBrush_->SetOpacity(ClampF(opacity, 0.0f, 1.0f));
    const D2D1_RECT_F stroke = D2D1::RectF(
        rect.left + inset, rect.top + inset,
        rect.right - inset, rect.bottom - inset);
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(stroke, r, r),
                              rimBrush_.Get(), widthPx);
}

} // namespace ld

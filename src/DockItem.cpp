#include "DockItem.h"

namespace ld
{

namespace
{

/// Coverage of a rounded rectangle at pixel centre (px, py), 0..1.
float RoundedCoverage(float px, float py, float width, float height, float radius)
{
    const float cx = width * 0.5f;
    const float cy = height * 0.5f;
    const float r = (std::min)(radius, (std::min)(cx, cy));

    const float dx = std::fabs(px - cx) - (cx - r);
    const float dy = std::fabs(py - cy) - (cy - r);

    const float ox = (std::max)(dx, 0.0f);
    const float oy = (std::max)(dy, 0.0f);

    const float distance = std::sqrt(ox * ox + oy * oy)
        + (std::min)((std::max)(dx, dy), 0.0f) - r;

    return (std::max)(0.0f, (std::min)(1.0f, 0.5f - distance));
}

/// Multiplies the alpha (and, because the data is premultiplied, the colour)
/// of every pixel by a rounded rectangle mask. Done once at upload time so
/// drawing stays a plain bitmap blit.
ComPtr<IWICBitmap> ApplyRoundedMask(IWICImagingFactory* wic,
                                    IWICBitmap* source,
                                    float cornerFraction)
{
    if (!wic || !source || cornerFraction <= 0.0f)
    {
        return {};
    }

    unsigned int width = 0;
    unsigned int height = 0;
    if (FAILED(source->GetSize(&width, &height)) || width == 0 || height == 0)
    {
        return {};
    }

    ComPtr<IWICBitmap> destination;
    if (FAILED(wic->CreateBitmapFromSource(
            source, WICBitmapCacheOnLoad, destination.AddressOf())))
    {
        return {};
    }

    WICRect whole{0, 0, static_cast<INT>(width), static_cast<INT>(height)};

    ComPtr<IWICBitmapLock> lock;
    if (FAILED(destination->Lock(&whole, WICBitmapLockWrite, lock.AddressOf())))
    {
        return {};
    }

    unsigned int stride = 0;
    unsigned char* data = nullptr;
    unsigned int dataSize = 0;

    if (FAILED(lock->GetStride(&stride))
        || FAILED(lock->GetDataPointer(&dataSize, &data)))
    {
        return {};
    }

    const float radius = cornerFraction * static_cast<float>(width);

    for (unsigned int y = 0; y < height; ++y)
    {
        unsigned char* row = data + static_cast<size_t>(y) * stride;

        for (unsigned int x = 0; x < width; ++x)
        {
            const float coverage = RoundedCoverage(
                static_cast<float>(x) + 0.5f,
                static_cast<float>(y) + 0.5f,
                static_cast<float>(width),
                static_cast<float>(height),
                radius);

            if (coverage >= 1.0f)
            {
                continue;
            }

            unsigned char* pixel = row + static_cast<size_t>(x) * 4;

            // Premultiplied BGRA: scaling every channel scales alpha too.
            for (int c = 0; c < 4; ++c)
            {
                pixel[c] = static_cast<unsigned char>(
                    static_cast<float>(pixel[c]) * coverage + 0.5f);
            }
        }
    }

    lock.Reset();

    return destination;
}

} // namespace

bool DockItem::EnsureIconBitmap(ID2D1RenderTarget* target,
                                IWICImagingFactory* wic,
                                unsigned int displaySize,
                                float cornerFraction)
{
    if (!iconSource || !target || !wic || displaySize == 0)
    {
        return false;
    }

    unsigned int sourceWidth = 0;
    unsigned int sourceHeight = 0;
    if (FAILED(iconSource->GetSize(&sourceWidth, &sourceHeight)) || sourceWidth == 0)
    {
        return false;
    }

    // Never upscale: a 256px source stays 256px, a tiny source stays tiny.
    const unsigned int wanted =
        (displaySize < sourceWidth) ? displaySize : sourceWidth;

    if (icon)
    {
        const D2D1_SIZE_F current = icon->GetSize();
        if (std::fabs(current.width - static_cast<float>(wanted)) < 0.5f
            && std::fabs(iconCornerFraction - cornerFraction) < 0.0005f)
        {
            return true;
        }

        icon.Reset();
    }

    iconCornerFraction = cornerFraction;

    ComPtr<IWICBitmap> upload;

    if (wanted < sourceWidth)
    {
        // Let WIC do a high quality downscale once instead of asking D2D to
        // minify a 256px icon every frame (which aliases badly).
        ComPtr<IWICBitmapScaler> scaler;
        if (SUCCEEDED(wic->CreateBitmapScaler(scaler.AddressOf()))
            && SUCCEEDED(scaler->Initialize(
                iconSource.Get(),
                static_cast<UINT>(displaySize),
                static_cast<UINT>(displaySize),
                WICBitmapInterpolationModeCubic)))
        {
            wic->CreateBitmapFromSource(
                scaler.Get(), WICBitmapCacheOnLoad, upload.AddressOf());
        }
    }


    ComPtr<IWICBitmap> masked = ApplyRoundedMask(wic, upload ? upload.Get() : iconSource.Get(),
                                                 cornerFraction);

    IWICBitmapSource* source = masked
        ? masked.Get()
        : (upload ? upload.Get() : iconSource.Get());

    const HRESULT hr = target->CreateBitmapFromWicBitmap(
        source, nullptr, icon.AddressOf());

    return SUCCEEDED(hr);
}

} // namespace ld

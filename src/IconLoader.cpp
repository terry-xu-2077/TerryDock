#include "IconLoader.h"

#include "Utils.h"

#include <ocidl.h>
#include <shlobj.h>
#include <commoncontrols.h>

namespace ld
{

namespace
{

constexpr unsigned int kPreferredIconSize = 256;

/// Windows ships PrivateExtractIconsW without a public header.
extern "C" __declspec(dllimport) UINT __stdcall PrivateExtractIconsW(
    const wchar_t* fileName, int iconIndex, int cxIcon, int cyIcon,
    HICON* phicon, UINT* piconid, UINT nIcons, UINT flags);

std::wstring FallbackName(const std::wstring& path)
{
    std::wstring name = GetFileStem(path);
    return name.empty() ? path : name;
}

/// Largest opaque run inside the bitmap: the size of the actual artwork
/// before any upscaling. An icon whose artwork is 32px inside a 256px
/// canvas was stretched by the shell and will look blurry on the dock.
unsigned int OpaqueContentSize(IWICBitmap* source)
{
    if (!source)
    {
        return 0;
    }

    unsigned int width = 0;
    unsigned int height = 0;
    if (FAILED(source->GetSize(&width, &height)) || width == 0 || height == 0)
    {
        return 0;
    }

    WICRect whole{0, 0, static_cast<INT>(width), static_cast<INT>(height)};

    ComPtr<IWICBitmapLock> lock;
    if (FAILED(source->Lock(&whole, WICBitmapLockRead, lock.AddressOf())))
    {
        return 0;
    }

    unsigned int stride = 0;
    unsigned char* data = nullptr;
    unsigned int dataSize = 0;

    if (FAILED(lock->GetStride(&stride))
        || FAILED(lock->GetDataPointer(&dataSize, &data)))
    {
        return 0;
    }

    int minX = static_cast<int>(width);
    int minY = static_cast<int>(height);
    int maxX = -1;
    int maxY = -1;

    for (unsigned int y = 0; y < height; ++y)
    {
        const unsigned char* row = data + static_cast<size_t>(y) * stride;

        for (unsigned int x = 0; x < width; ++x)
        {
            if (row[x * 4 + 3] > 12)
            {
                if (static_cast<int>(x) < minX)
                {
                    minX = static_cast<int>(x);
                }

                if (static_cast<int>(x) > maxX)
                {
                    maxX = static_cast<int>(x);
                }

                if (static_cast<int>(y) < minY)
                {
                    minY = static_cast<int>(y);
                }

                if (static_cast<int>(y) > maxY)
                {
                    maxY = static_cast<int>(y);
                }
            }
        }
    }

    if (maxX < 0 || maxY < 0)
    {
        return 0;
    }

    const unsigned int contentWidth =
        static_cast<unsigned int>(maxX - minX + 1);
    const unsigned int contentHeight =
        static_cast<unsigned int>(maxY - minY + 1);

    return (std::max)(contentWidth, contentHeight);
}

/// Side length of an HICON's colour bitmap, via GetIconInfo.
unsigned int HiconNativeSize(HICON icon)
{
    ICONINFO info{};
    if (!GetIconInfo(icon, &info))
    {
        return 0;
    }

    BITMAP bm{};
    unsigned int native = 0;

    if (info.hbmColor
        && GetObjectW(info.hbmColor, sizeof(bm), &bm) == sizeof(bm))
    {
        native = static_cast<unsigned int>(bm.bmWidth);
    }

    if (info.hbmColor)
    {
        DeleteObject(info.hbmColor);
    }

    if (info.hbmMask)
    {
        DeleteObject(info.hbmMask);
    }

    return native;
}

} // namespace

IconLoader::IconLoader() = default;

bool IconLoader::Initialize()
{
    if (wic_)
    {
        return true;
    }

    const HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(wic_.AddressOf()));

    return SUCCEEDED(hr);
}

bool IconLoader::IsCircularIcon(IWICBitmap* source) const
{
    if (!source)
    {
        return false;
    }

    unsigned int width = 0;
    unsigned int height = 0;
    if (FAILED(source->GetSize(&width, &height)) || width < 8 || height < 8)
    {
        return false;
    }

    WICRect whole{0, 0, static_cast<INT>(width), static_cast<INT>(height)};
    ComPtr<IWICBitmapLock> lock;
    if (FAILED(source->Lock(&whole, WICBitmapLockRead, lock.AddressOf())))
    {
        return false;
    }

    unsigned int stride = 0;
    unsigned char* data = nullptr;
    unsigned int dataSize = 0;
    if (FAILED(lock->GetStride(&stride))
        || FAILED(lock->GetDataPointer(&dataSize, &data)))
    {
        return false;
    }

    constexpr unsigned char kAlphaThreshold = 28;
    int minX = static_cast<int>(width);
    int minY = static_cast<int>(height);
    int maxX = -1;
    int maxY = -1;

    for (unsigned int y = 0; y < height; ++y)
    {
        const unsigned char* row = data + static_cast<size_t>(y) * stride;
        for (unsigned int x = 0; x < width; ++x)
        {
            if (row[x * 4 + 3] <= kAlphaThreshold)
            {
                continue;
            }

            minX = (std::min)(minX, static_cast<int>(x));
            minY = (std::min)(minY, static_cast<int>(y));
            maxX = (std::max)(maxX, static_cast<int>(x));
            maxY = (std::max)(maxY, static_cast<int>(y));
        }
    }

    if (maxX < minX || maxY < minY)
    {
        return false;
    }

    const float boxWidth = static_cast<float>(maxX - minX + 1);
    const float boxHeight = static_cast<float>(maxY - minY + 1);
    const float aspect = boxWidth / boxHeight;
    if (aspect < 0.92f || aspect > 1.08f)
    {
        return false;
    }

    const float centerX = (static_cast<float>(minX + maxX) + 1.0f) * 0.5f;
    const float centerY = (static_cast<float>(minY + maxY) + 1.0f) * 0.5f;
    const float radiusX = boxWidth * 0.5f;
    const float radiusY = boxHeight * 0.5f;

    constexpr int kBins = 72;
    float radialMax[kBins]{};

    for (int y = minY; y <= maxY; ++y)
    {
        const unsigned char* row =
            data + static_cast<size_t>(y) * stride;

        for (int x = minX; x <= maxX; ++x)
        {
            if (row[x * 4 + 3] <= kAlphaThreshold)
            {
                continue;
            }

            const float nx =
                (static_cast<float>(x) + 0.5f - centerX) / radiusX;
            const float ny =
                (static_cast<float>(y) + 0.5f - centerY) / radiusY;
            const float radius = std::sqrt(nx * nx + ny * ny);
            float angle = std::atan2(ny, nx);
            if (angle < 0.0f)
            {
                angle += 2.0f * kPi;
            }

            int bin = static_cast<int>(
                angle / (2.0f * kPi) * static_cast<float>(kBins));
            bin = (std::clamp)(bin, 0, kBins - 1);
            radialMax[bin] = (std::max)(radialMax[bin], radius);
        }
    }

    float sum = 0.0f;
    float minRadius = std::numeric_limits<float>::max();
    float maxRadius = 0.0f;

    for (float radius : radialMax)
    {
        // Missing directions mean the outer silhouette is not a closed circle.
        if (radius < 0.78f)
        {
            return false;
        }

        sum += radius;
        minRadius = (std::min)(minRadius, radius);
        maxRadius = (std::max)(maxRadius, radius);
    }

    const float mean = sum / static_cast<float>(kBins);
    float variance = 0.0f;
    for (float radius : radialMax)
    {
        const float delta = radius - mean;
        variance += delta * delta;
    }
    variance /= static_cast<float>(kBins);
    const float deviation = std::sqrt(variance);

    // A rasterised circle stays close to radius 1 in every direction.
    // Rounded squares and arbitrary logos show much larger angular variation.
    return mean >= 0.90f && mean <= 1.10f
        && deviation <= 0.065f
        && (maxRadius - minRadius) <= 0.20f;
}

AppInfo IconLoader::Inspect(const std::wstring& path, unsigned int iconSize)
{
    AppInfo info;
    info.resolvedPath = path;

    const std::wstring extension = GetFileExtension(path);
    std::wstring shortcutName;

    if (extension == L".lnk")
    {
        std::wstring target;
        std::wstring arguments;

        if (ResolveShortcut(path, target, arguments) && !target.empty())
        {
            info.resolvedPath = target;
            info.arguments = arguments;
            // For shortcuts, the label belongs to the shortcut the user
            // dragged in, not the executable it points to.
            shortcutName = GetFileStem(path);
        }
    }

    info.processName = GetFileName(info.resolvedPath);

    // Prefer the localized description baked into the executable's version
    // resource ("文件资源管理器" beats "explorer.exe"); the shell display
    // name is only the file name for plain executables.
    info.name = shortcutName;

    if (info.name.empty())
    {
        info.name = GetFileDescription(info.resolvedPath);
    }

    if (info.name.empty())
    {
        info.name = GetShellDisplayName(info.resolvedPath);
    }

    if (info.name.empty())
    {
        info.name = FallbackName(info.resolvedPath);
    }

    const unsigned int size = iconSize == 0 ? kPreferredIconSize : iconSize;
    ComPtr<IWICBitmap> icon = ExtractIconBitmap(info.resolvedPath, size);

    if (!icon)
    {
        // A shortcut pointing at something exotic: fall back to the shortcut.
        icon = ExtractIconBitmap(path, size);
    }

    if (icon)
    {
        icon = NormalizeContent(icon.Get(), size);
    }

    info.icon = std::move(icon);

    return info;
}

ComPtr<IWICBitmap> IconLoader::NormalizeContent(IWICBitmap* source,
                                                unsigned int targetSize)
{
    if (!wic_ || !source || targetSize == 0)
    {
        return {};
    }

    unsigned int width = 0;
    unsigned int height = 0;
    if (FAILED(source->GetSize(&width, &height)) || width == 0 || height == 0)
    {
        return {};
    }

    // --- measure the opaque content --------------------------------------
    WICRect whole{0, 0, static_cast<INT>(width), static_cast<INT>(height)};

    ComPtr<IWICBitmapLock> lock;
    if (FAILED(source->Lock(&whole, WICBitmapLockRead, lock.AddressOf())))
    {
        return {};
    }

    unsigned int stride = 0;
    unsigned char* data = nullptr;
    unsigned int dataSize = 0;

    if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&dataSize, &data)))
    {
        return {};
    }

    int minX = static_cast<int>(width);
    int minY = static_cast<int>(height);
    int maxX = -1;
    int maxY = -1;

    for (unsigned int y = 0; y < height; ++y)
    {
        const unsigned char* row = data + static_cast<size_t>(y) * stride;

        for (unsigned int x = 0; x < width; ++x)
        {
            // 32bppPBGRA: alpha is the fourth byte.
            if (row[x * 4 + 3] > 12)
            {
                if (static_cast<int>(x) < minX)
                {
                    minX = static_cast<int>(x);
                }

                if (static_cast<int>(x) > maxX)
                {
                    maxX = static_cast<int>(x);
                }

                if (static_cast<int>(y) < minY)
                {
                    minY = static_cast<int>(y);
                }

                if (static_cast<int>(y) > maxY)
                {
                    maxY = static_cast<int>(y);
                }
            }
        }
    }

    lock.Reset();

    if (maxX < 0 || maxY < 0)
    {
        return {}; // fully transparent, nothing worth keeping
    }

    const int contentWidth = maxX - minX + 1;
    const int contentHeight = maxY - minY + 1;

    // Content already fills the canvas: nothing to do.
    if (contentWidth >= static_cast<int>(width * 88 / 100)
        && contentHeight >= static_cast<int>(height * 88 / 100))
    {
        ComPtr<IWICBitmap> keep;
        wic_->CreateBitmapFromSource(
            source, WICBitmapCacheOnLoad, keep.AddressOf());
        return keep;
    }

    // --- crop, then scale back up ----------------------------------------
    // Keep the crop square so the icon is not stretched later: centre the
    // content in a side x side window.
    int side = (std::max)(contentWidth, contentHeight);
    side = (std::min)(side, static_cast<int>((std::min)(width, height)));

    const int centerContentX = (minX + maxX + 1) / 2;
    const int centerContentY = (minY + maxY + 1) / 2;

    int cropX = centerContentX - side / 2;
    int cropY = centerContentY - side / 2;

    cropX = (std::max)(0, (std::min)(cropX, static_cast<int>(width) - side));
    cropY = (std::max)(0, (std::min)(cropY, static_cast<int>(height) - side));

    WICRect crop{cropX, cropY, side, side};

    ComPtr<IWICBitmapClipper> clipper;
    if (FAILED(wic_->CreateBitmapClipper(clipper.AddressOf()))
        || FAILED(clipper->Initialize(source, &crop)))
    {
        return {};
    }

    if (side == static_cast<int>(targetSize))
    {
        ComPtr<IWICBitmap> cropped;
        wic_->CreateBitmapFromSource(
            clipper.Get(), WICBitmapCacheOnLoad, cropped.AddressOf());
        return cropped;
    }

    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(wic_->CreateBitmapScaler(scaler.AddressOf()))
        || FAILED(scaler->Initialize(
            clipper.Get(),
            targetSize,
            targetSize,
            WICBitmapInterpolationModeFant)))
    {
        return {};
    }

    ComPtr<IWICBitmap> result;
    wic_->CreateBitmapFromSource(
        scaler.Get(), WICBitmapCacheOnLoad, result.AddressOf());

    return result;
}

ComPtr<IWICBitmap> IconLoader::ExtractIconBitmap(const std::wstring& path,
                                                 unsigned int size)
{
    // Prefer native resource pixels: PrivateExtractIconsW probes descending
    // standard sizes and only accepts a hit when the returned bitmap is
    // exactly the requested size, so a legacy 32px icon comes back as crisp
    // 32px artwork (NormalizeContent upscales it with WIC) instead of the
    // image list's blurry GDI stretch onto a 256px canvas.
    if (ComPtr<IWICBitmap> native = ExtractViaPrivateIcons(path, size))
    {
        if (OpaqueContentSize(native.Get()) >= 16)
        {
            return native;
        }
    }

    // Modern applications carry real 256px art; the shell paths deliver it.
    // A stretched small bitmap (opaque artwork far below the canvas) is
    // rejected here so the native pixels above win instead.
    if (ComPtr<IWICBitmap> jumbo = ExtractViaJumboList(path, size))
    {
        if (OpaqueContentSize(jumbo.Get()) >= 96)
        {
            return jumbo;
        }
    }

    if (ComPtr<IWICBitmap> shell = ExtractViaShellItem(path, size))
    {
        return shell;
    }

    if (ComPtr<IWICBitmap> handle = ExtractViaIconHandle(path, size))
    {
        return handle;
    }

    return ExtractViaPrivateIcons(path, size);
}

ComPtr<IWICBitmap> IconLoader::ExtractViaPrivateIcons(const std::wstring& path,
                                                      unsigned int size)
{
    if (!wic_ || path.empty() || size == 0)
    {
        return {};
    }

    // Probe the standard icon sizes from large to small. When a size is
    // missing PrivateExtractIconsW silently scales a neighbour, so a probe
    // only counts as a native hit when the returned HICON matches exactly.
    static const int kSizes[] = {256, 128, 96, 64, 48, 32};

    for (int probe : kSizes)
    {
        if (probe > static_cast<int>(size))
        {
            continue;
        }

        HICON icon = nullptr;
        UINT id = 0;

        if (PrivateExtractIconsW(path.c_str(), 0, probe, probe,
                                 &icon, &id, 1, 0) != 1 || icon == nullptr)
        {
            continue;
        }

        const unsigned int native = HiconNativeSize(icon);
        DestroyIcon(icon);

        if (native != static_cast<unsigned int>(probe))
        {
            continue;
        }

        HICON exact = nullptr;
        if (PrivateExtractIconsW(path.c_str(), 0, probe, probe,
                                 &exact, &id, 1, 0) != 1 || exact == nullptr)
        {
            continue;
        }

        ComPtr<IWICBitmap> bitmap = BitmapFromHIcon(exact, native);
        DestroyIcon(exact);

        if (bitmap)
        {
            return bitmap;
        }
    }

    return {};
}

ComPtr<IWICBitmap> IconLoader::ExtractViaJumboList(const std::wstring& path,
                                                   unsigned int size)
{
    if (!wic_ || path.empty())
    {
        return {};
    }

    ComPtr<IImageList> imageList;
    if (FAILED(SHGetImageList(SHIL_JUMBO, IID_PPV_ARGS(imageList.AddressOf()))))
    {
        return {};
    }

    SHFILEINFOW info{};
    if (SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info),
                       SHGFI_SYSICONINDEX) == 0)
    {
        return {};
    }

    HICON icon = nullptr;
    if (FAILED(imageList->GetIcon(info.iIcon,
                                  ILD_TRANSPARENT | ILD_SCALE, &icon))
        || icon == nullptr)
    {
        return {};
    }

    ComPtr<IWICBitmap> bitmap = BitmapFromHIcon(icon, size);
    DestroyIcon(icon);

    return bitmap;
}

ComPtr<IWICBitmap> IconLoader::ExtractViaShellItem(const std::wstring& path,
                                                   unsigned int size)
{
    if (!wic_ || path.empty())
    {
        return {};
    }

    ComPtr<IShellItem> shellItem;
    if (FAILED(SHCreateItemFromParsingName(
            path.c_str(), nullptr, IID_PPV_ARGS(shellItem.AddressOf()))))
    {
        return {};
    }

    ComPtr<IShellItemImageFactory> factory;
    if (FAILED(shellItem->QueryInterface(IID_PPV_ARGS(factory.AddressOf()))))
    {
        return {};
    }

    HBITMAP native = nullptr;
    const SIZE request{static_cast<LONG>(size), static_cast<LONG>(size)};

    const HRESULT hr = factory->GetImage(
        request, SIIGBF_RESIZETOFIT | SIIGBF_BIGGERSIZEOK | SIIGBF_ICONONLY, &native);

    if (FAILED(hr) || native == nullptr)
    {
        if (native)
        {
            DeleteObject(native);
        }

        return {};
    }

    ComPtr<IWICBitmap> bitmap = BitmapFromHBitmap(native);
    DeleteObject(native);

    return bitmap;
}

ComPtr<IWICBitmap> IconLoader::BitmapFromHIcon(HICON icon, unsigned int size)
{
    if (!wic_ || !icon || size == 0)
    {
        return {};
    }

    HDC screen = GetDC(nullptr);
    if (!screen)
    {
        return {};
    }

    HDC memory = CreateCompatibleDC(screen);

    ComPtr<IWICBitmap> bitmap;

    if (memory)
    {
        BITMAPINFO header{};
        header.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        header.bmiHeader.biWidth = static_cast<LONG>(size);
        header.bmiHeader.biHeight = -static_cast<LONG>(size);
        header.bmiHeader.biPlanes = 1;
        header.bmiHeader.biBitCount = 32;
        header.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP dib = CreateDIBSection(
            memory, &header, DIB_RGB_COLORS, &bits, nullptr, 0);

        if (dib)
        {
            HGDIOBJ previous = SelectObject(memory, dib);

            // Zero the surface first so untouched pixels stay transparent.
            if (bits)
            {
                std::memset(bits, 0, static_cast<size_t>(size) * size * 4);
            }

            DrawIconEx(memory, 0, 0, icon,
                       static_cast<int>(size), static_cast<int>(size),
                       0, nullptr, DI_NORMAL);

            SelectObject(memory, previous);
            bitmap = BitmapFromHBitmap(dib);
            DeleteObject(dib);
        }

        DeleteDC(memory);
    }

    ReleaseDC(nullptr, screen);

    return bitmap;
}

ComPtr<IWICBitmap> IconLoader::ExtractViaIconHandle(const std::wstring& path,
                                                    unsigned int size)
{
    if (!wic_ || path.empty())
    {
        return {};
    }

    SHFILEINFOW info{};
    const DWORD_PTR result = SHGetFileInfoW(
        path.c_str(),
        0,
        &info,
        sizeof(info),
        SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES);

    if (result == 0 || info.hIcon == nullptr)
    {
        return {};
    }

    ComPtr<IWICBitmap> bitmap = BitmapFromHIcon(info.hIcon, size);

    DestroyIcon(info.hIcon);

    return bitmap;
}

ComPtr<IWICBitmap> IconLoader::BitmapFromHBitmap(HBITMAP bitmap)
{
    if (!wic_ || !bitmap)
    {
        return {};
    }

    ComPtr<IWICBitmap> source;
    const HRESULT hr = wic_->CreateBitmapFromHBITMAP(
        bitmap, nullptr, WICBitmapUsePremultipliedAlpha, source.AddressOf());

    if (FAILED(hr) || !source)
    {
        return {};
    }

    // Normalise to premultiplied BGRA so D2D can consume it directly.
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wic_->CreateFormatConverter(converter.AddressOf())))
    {
        return source;
    }

    if (FAILED(converter->Initialize(
            source.Get(),
            format,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeMedianCut)))
    {
        return source;
    }

    ComPtr<IWICBitmap> normalized;
    if (FAILED(wic_->CreateBitmapFromSource(
            converter.Get(), WICBitmapCacheOnLoad, normalized.AddressOf())))
    {
        return source;
    }

    return normalized;
}

ComPtr<IWICBitmap> IconLoader::LoadFromCache(const std::wstring& file)
{
    if (!wic_ || file.empty() || !FileExists(file))
    {
        return {};
    }

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wic_->CreateDecoderFromFilename(
            file.c_str(),
            nullptr,
            GENERIC_READ,
            WICDecodeMetadataCacheOnLoad,
            decoder.AddressOf())))
    {
        return {};
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.AddressOf())))
    {
        return {};
    }

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wic_->CreateFormatConverter(converter.AddressOf())))
    {
        return {};
    }

    if (FAILED(converter->Initialize(
            frame.Get(),
            GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeMedianCut)))
    {
        return {};
    }

    ComPtr<IWICBitmap> bitmap;
    wic_->CreateBitmapFromSource(
        converter.Get(), WICBitmapCacheOnLoad, bitmap.AddressOf());

    return bitmap;
}

bool IconLoader::SaveToCache(IWICBitmap* bitmap, const std::wstring& file)
{
    if (!wic_ || !bitmap || file.empty())
    {
        return false;
    }

    EnsureDirectoryExists(GetIconCacheDir());

    ComPtr<IWICStream> stream;
    if (FAILED(wic_->CreateStream(stream.AddressOf())))
    {
        return false;
    }

    if (FAILED(stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE)))
    {
        return false;
    }

    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(wic_->CreateEncoder(
            GUID_ContainerFormatPng, nullptr, encoder.AddressOf())))
    {
        return false;
    }

    if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)))
    {
        return false;
    }

    ComPtr<IWICBitmapFrameEncode> frameEncode;
    ComPtr<IPropertyBag2> options;
    if (FAILED(encoder->CreateNewFrame(frameEncode.AddressOf(), options.AddressOf())))
    {
        return false;
    }

    if (FAILED(frameEncode->Initialize(options.Get())))
    {
        return false;
    }

    unsigned int width = 0;
    unsigned int height = 0;
    bitmap->GetSize(&width, &height);

    if (FAILED(frameEncode->SetSize(width, height)))
    {
        return false;
    }

    WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
    if (FAILED(frameEncode->SetPixelFormat(&format)))
    {
        return false;
    }

    if (FAILED(frameEncode->WriteSource(bitmap, nullptr)))
    {
        return false;
    }

    if (FAILED(frameEncode->Commit()))
    {
        return false;
    }

    return SUCCEEDED(encoder->Commit());
}

bool ResolveShortcut(const std::wstring& linkPath,
                     std::wstring& targetPath,
                     std::wstring& arguments)
{
    ComPtr<IShellLinkW> shellLink;
    if (FAILED(CoCreateInstance(
            CLSID_ShellLink,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(shellLink.AddressOf()))))
    {
        return false;
    }

    ComPtr<IPersistFile> persistFile;
    if (FAILED(shellLink->QueryInterface(IID_PPV_ARGS(persistFile.AddressOf()))))
    {
        return false;
    }

    if (FAILED(persistFile->Load(linkPath.c_str(), STGM_READ)))
    {
        return false;
    }

    wchar_t target[MAX_PATH * 2]{};
    if (SUCCEEDED(shellLink->GetPath(target, ARRAYSIZE(target), nullptr, 0))
        && target[0] != L'\0')
    {
        targetPath = target;
    }

    wchar_t args[MAX_PATH * 2]{};
    if (SUCCEEDED(shellLink->GetArguments(args, ARRAYSIZE(args))))
    {
        arguments = args;
    }

    return !targetPath.empty();
}

std::wstring GetShellDisplayName(const std::wstring& path)
{
    if (path.empty())
    {
        return {};
    }

    // Shell's "normal display" name is the friendly product name when the
    // executable advertises one, otherwise the file name.
    ComPtr<IShellItem> shellItem;
    if (SUCCEEDED(SHCreateItemFromParsingName(
            path.c_str(), nullptr, IID_PPV_ARGS(shellItem.AddressOf()))))
    {
        wchar_t* displayName = nullptr;
        if (SUCCEEDED(shellItem->GetDisplayName(SIGDN_NORMALDISPLAY, &displayName))
            && displayName != nullptr)
        {
            std::wstring result(displayName);
            CoTaskMemFree(displayName);

            if (!result.empty())
            {
                return result;
            }
        }
    }

    SHFILEINFOW info{};
    if (SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info),
                       SHGFI_DISPLAYNAME) != 0)
    {
        if (info.szDisplayName[0] != L'\0')
        {
            return std::wstring(info.szDisplayName);
        }
    }

    return FallbackName(path);
}

} // namespace ld

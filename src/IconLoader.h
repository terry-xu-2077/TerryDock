#pragma once

#include "Common.h"

#include <string>

namespace ld
{

/// Everything extracted from a file the user dropped into the dock.
struct AppInfo
{
    std::wstring name;
    std::wstring resolvedPath;
    std::wstring arguments;
    std::wstring processName;

    /// Null when no icon could be extracted.
    ComPtr<IWICBitmap> icon;
};

class IconLoader
{
public:
    IconLoader();

    bool Initialize();

    IWICImagingFactory* Factory() const { return wic_.Get(); }

    /// Resolves shortcuts, reads the display name and extracts the icon.
    AppInfo Inspect(const std::wstring& path, unsigned int iconSize);

    /// Loads a cached icon from disk.
    ComPtr<IWICBitmap> LoadFromCache(const std::wstring& file);

    /// Stores an icon in the cache directory as PNG.
    bool SaveToCache(IWICBitmap* bitmap, const std::wstring& file);

    /// Returns true when the alpha silhouette is a near-perfect circle.
    /// Used only when an app is first added so LightDock can choose sensible
    /// automatic plate defaults without overriding later user customization.
    bool IsCircularIcon(IWICBitmap* source) const;

private:
    ComPtr<IWICBitmap> ExtractIconBitmap(const std::wstring& path,
                                         unsigned int size);

    /// Best source for 256px icons: the shell's jumbo image list. May also
    /// return an upscaled tiny bitmap, which ExtractIconBitmap scores out.
    ComPtr<IWICBitmap> ExtractViaJumboList(const std::wstring& path,
                                           unsigned int size);

    ComPtr<IWICBitmap> ExtractViaShellItem(const std::wstring& path,
                                           unsigned int size);

    /// Direct resource extraction without the shell image list.
    ComPtr<IWICBitmap> ExtractViaPrivateIcons(const std::wstring& path,
                                              unsigned int size);

    ComPtr<IWICBitmap> ExtractViaIconHandle(const std::wstring& path,
                                            unsigned int size);

    /// Renders an HICON into a transparent 32bpp DIB and wraps it in WIC.
    ComPtr<IWICBitmap> BitmapFromHIcon(HICON icon, unsigned int size);

    ComPtr<IWICBitmap> BitmapFromHBitmap(HBITMAP bitmap);

    /// Shell happily hands back a 256x256 canvas with a 48px icon sitting in
    /// the middle of it. This measures the opaque content, crops to it and
    /// scales it back up, so a small source icon still fills the dock slot.
    ComPtr<IWICBitmap> NormalizeContent(IWICBitmap* source,
                                        unsigned int targetSize);

    ComPtr<IWICImagingFactory> wic_;
};

/// Resolves a .lnk to its target. Returns the input unchanged for other files.
bool ResolveShortcut(const std::wstring& linkPath,
                     std::wstring& targetPath,
                     std::wstring& arguments);

/// Shell display name ("After Effects"), falling back to the file stem.
std::wstring GetShellDisplayName(const std::wstring& path);

} // namespace ld

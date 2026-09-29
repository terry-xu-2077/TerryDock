#pragma once

#include "Animation.h"
#include "Common.h"
#include "Config.h"

#include <string>

namespace ld
{

enum class DockItemKind
{
    /// User configured/persisted launcher.
    Pinned,

    /// Built-in Windows shell shortcuts that always live on the left.
    StartButton,
    SearchButton,

    /// Temporary icon for an unpinned app that currently owns a taskbar window.
    RunningTransient,
};

/// A single entry in the dock.
///
/// The transform applied when drawing is intentionally composed of three
/// independent parts so that hover magnification and launch bounce never
/// overwrite each other:
///
///     final position = layout position (x / baselineBottom)
///                    + bounce offset (vertical only)
///                    + scale (bottom anchored growth)
struct DockItem
{
    DockItemKind kind = DockItemKind::Pinned;

    std::wstring id;
    std::wstring name;

    /// Path the user picked (.exe or .lnk). This is what gets launched.
    std::wstring targetPath;

    /// Real executable behind a shortcut.
    std::wstring resolvedPath;

    std::wstring arguments;
    std::wstring processName;

    /// Icon cache file (relative to the icon cache directory).
    std::wstring iconFile;

    /// Look overrides; inherits from the global backdrop while `custom` is
    /// false, so only entries the user actually tweaked differ.
    PlateStyle plate;

    /// Decoded icon kept on the CPU side so it survives render target resets.
    ComPtr<IWICBitmap> iconSource;

    /// Render target bound bitmap, rebuilt whenever the surface is resized.
    ComPtr<ID2D1Bitmap> icon;

    /// The corner fraction the cached bitmap was masked with. Part of the
    /// cache key: two items can sit at the same size but different radii.
    float iconCornerFraction = -1.0f;
    float iconBitmapScale = 0.0f;

    // --- state -------------------------------------------------------------
    bool running = false;
    bool launching = false;

    /// Seconds since the launch was requested.
    double launchElapsed = 0.0;

    // --- layout (surface pixels) -------------------------------------------
    /// Left edge of the rendered icon.
    float x = 0.0f;

    /// Rendered icon size (iconSize * scale).
    float size = 0.0f;

    /// Horizontal centre of the rendered icon.
    float centerX = 0.0f;

    /// Horizontal centre this icon would have at scale 1.0.
    ///
    /// Magnification is measured against this fixed point, never against the
    /// animated position: measuring against the moving centre creates a
    /// feedback loop and the icons visibly jitter under the cursor.
    float baseCenterX = 0.0f;

    /// Bottom edge of the icon at scale 1.0. Icons grow upwards from here.
    float baselineBottom = 0.0f;

    // --- animation ---------------------------------------------------------
    Spring scaleSpring;
    Spring bounceSpring;

    float scale = 1.0f;

    /// Vertical offset in pixels, negative moves the icon up.
    float bounceOffset = 0.0f;

    /// Countdown to the next bounce impulse while launching.
    float bounceTimer = 0.0f;

    /// Rebuilds `icon` from `iconSource` at the requested display size.
    ///
    /// @param cornerFraction corner radius as a fraction of the bitmap width.
    ///                       Pass 0 to leave the icon shape untouched.
    bool EnsureIconBitmap(ID2D1RenderTarget* target,
                          IWICImagingFactory* wic,
                          unsigned int displaySize,
                          float cornerFraction,
                          float bitmapScale = 1.0f);
};

} // namespace ld

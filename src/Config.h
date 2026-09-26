#pragma once

#include "Common.h"

#include <string>
#include <vector>

namespace ld
{

/// The plate look of a single dock entry.
///
/// The corner radius (and therefore the rounded clipping of full bleed
/// icons) always follows the global backdrop, matching macOS where the
/// tile shape is a dock-wide constant. So do plate opacity and the
/// icon-to-plate *default*: `iconScale` of 0 means "follow the global
/// default", any other value pins this one icon.
struct PlateStyle
{
    /// Whether this icon draws its rounded plate at all.
    bool enabled = true;

    /// Icon-to-plate ratio; 0 follows the global backdrop default.
    float iconScale = 0.0f;

    /// When false the gradient's second colour is derived from `top`
    /// automatically (a slightly darker offset). When true `bottom` is used.
    bool customBottom = false;

    /// "#RGB", "#RRGGBB" or "#RRGGBBAA".
    std::wstring top = L"#3B4252";

    /// Only used while `customBottom` is true.
    std::wstring bottom = L"#22262F";

    /// Optional per-icon override for the plate's inner rim. Empty uses white.
    std::wstring strokeColor;

    /// Negative inherits the global rim opacity; otherwise 0..1.
    float strokeOpacity = -1.0f;
};

struct AppEntry
{
    std::wstring id;
    std::wstring name;

    /// Original path selected by the user: an .exe or a .lnk.
    std::wstring targetPath;

    /// Resolved executable (for .lnk this is the shortcut target).
    std::wstring resolvedPath;

    std::wstring arguments;
    std::wstring processName;

    /// Icon cache file name inside %APPDATA%\LightDock\icons.
    std::wstring iconFile;

    /// Look overrides for this entry's icon plate.
    PlateStyle plate;
};

/// How the panel reacts while the icons are magnified.
enum class PanelMode
{
    /// The panel hugs the icon row, so it grows and shrinks continuously
    /// with the magnification.
    Elastic,

    /// macOS behaviour: as soon as the pointer enters the dock the panel
    /// expands once to the width the fully magnified row needs, the icons
    /// then scale strictly inside it, and the panel collapses again when
    /// the pointer leaves.
    Fixed,
    Static,
};

/// Shared plate parameters. Deliberately has no colours: the plate colour
/// belongs to each icon (the global layer only shapes the tiles), and no
/// on/off switch either — that lives on every icon itself.
struct IconBackdrop
{
    /// Corner radius in design pixels (scaled by DPI like everything else).
    float cornerRadius = 12.0f;

    float opacity = 0.92f;
    float strokeWidth = 2.0f;
    float strokeOpacity = 1.0f;

    /// How much of the plate the icon itself occupies by default. Below 1.0
    /// the plate shows as a border around the icon.
    float iconScale = 0.86f;
};

struct DockSettings
{
    int   iconSize = 52;
    int   iconSpacing = 14;
    float overallScale = 1.0f;
    /// When enabled, the dock overlays the desktop instead of reserving
    /// its bottom strip. Fullscreen apps hide the dock regardless.
    bool autoHide = false;
    int autoHideDelayMs = 250;
    float autoHideSpeed = 1.0f;
    float magnification = 1.6f;
    float tooltipOpacity = 0.92f;
    float tooltipFadeSeconds = 0.18f;
    float tooltipScale = 1.0f;

    IconBackdrop backdrop;

    PanelMode panelMode = PanelMode::Fixed;

    float backgroundOpacity = 0.60f;
    float cornerRadius = 18.0f;
    float borderOpacity = 0.45f;
    float shadowOpacity = 0.30f;

    /// Panel background. Empty colours keep the built-in white look;
    /// when both are set the panel blends vertically from `backgroundTop`
    /// to `backgroundBottom`.
    std::wstring backgroundTop;
    std::wstring backgroundBottom;

    /// Device name of the display the dock lives on. Empty = primary.
    std::wstring monitor;
};

struct DockConfig
{
    DockSettings settings;
    std::vector<AppEntry> apps;
};

class Config
{
public:
    /// Directory that holds config.json (created when missing).
    static std::wstring Directory();

    static std::wstring FilePath();

    /// Loads the config, falling back to defaults when the file is missing
    /// or malformed.
    static DockConfig Load();

    static bool Save(const DockConfig& config);
};

} // namespace ld

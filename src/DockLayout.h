#pragma once

#include "Common.h"
#include "Config.h"

#include <vector>

namespace ld
{

class DockItem;

/// All values are already scaled to device pixels.
struct LayoutMetrics
{
    float dpiScale = 1.0f;
    bool anchorIconsAtTop = false;

    float iconSize = 64.0f;
    float spacing = 10.0f;

    float paddingX = 12.0f;
    float paddingY = 8.0f;

    /// Diameter of the running indicator dot. It lives inside the bottom
    /// padding strip and reserves no row space of its own.
    float indicatorHeight = 4.0f;

    float cornerRadius = 18.0f;
    float magnification = 1.6f;

    /// Corner radius of the plate behind each icon, already DPI scaled.
    float backdropRadius = 0.0f;

    /// Gaussian falloff expressed as a multiple of the cell width.
    float sigmaFactor = 0.9f;

    /// Extra margin around the dock that still reacts to the mouse.
    float hitMargin = 6.0f;
};

/// Static geometry: the size of the layered window and the vertical layout of
/// the panel. Nothing here depends on the current magnification.
struct DockGeometry
{
    int surfaceWidth = 0;
    int surfaceHeight = 0;

    /// Panel width with every icon at scale 1.0.
    float basePanelWidth = 0.0f;

    /// Widest the panel can ever get (every icon at full magnification).
    /// Used once to bake the drop shadow, and as the target of the macOS
    /// style "expand once on hover" behaviour.
    float maxPanelWidth = 0.0f;

    float panelHeight = 0.0f;
    float panelY = 0.0f;

    /// Bottom edge of an icon rendered at scale 1.0. The icons are
    /// vertically centred in the panel: equal padding above and below.
    float iconBaselineBottom = 0.0f;

    /// Centre line of the running indicator dots (inside the bottom padding).
    float indicatorCenterY = 0.0f;
};

/// Per frame result: the panel grows and shrinks with the icon row.
struct LayoutFrame
{
    float panelX = 0.0f;
    float panelWidth = 0.0f;
};

/// Extra room the window needs so magnified icons, bounce and shadow are
/// never clipped by the HWND rectangle.
struct SurfaceMargins
{
    float left = 0.0f;
    float right = 0.0f;
    float top = 0.0f;
    float bottom = 0.0f;
};

LayoutMetrics MakeMetrics(const DockSettings& settings, float dpiScale);

DockGeometry ComputeGeometry(int itemCount, const LayoutMetrics& metrics,
                             DockEdge edge = DockEdge::Bottom);

SurfaceMargins ComputeMargins(const LayoutMetrics& metrics,
                              DockEdge edge = DockEdge::Bottom);

/// Writes the scale target of every item from the cursor position.
///
/// @param mouseX    Cursor X the caller wants to be the centre of the
///                  magnification. It must be eased/frozen by the caller:
///                  feeding the raw cursor in lets one fast sideways flick
///                  collapse the whole row within a single frame.
/// @param presence  How much of the magnification to apply, 0..1. The caller
///                  eases this value so entering feels instant and leaving
///                  fades out instead of snapping back. It already carries
///                  the "pointer left the row" information, so no spatial
///                  edge factor is applied on top of it.
void UpdateScaleTargets(std::vector<DockItem*>& items,
                        float mouseX,
                        float presence,
                        const LayoutMetrics& metrics);

/// Horizontal span in which the pointer drives the dock: the un-magnified
/// row widened by hitMargin, plus `fadeDistance` -- the band outside those
/// bounds over which the magnification eases back down so leaving the dock
/// never snaps.
void RowInfluenceBounds(const DockGeometry& geometry,
                        size_t itemCount,
                        const LayoutMetrics& metrics,
                        float& coreLeft,
                        float& coreRight,
                        float& fadeDistance);

/// How strongly the pointer drives the dock at this X: 1 fully inside the
/// row, smoothly down to 0 across the fade band, 0 beyond it.
float RowInfluenceFade(float mouseX,
                       float coreLeft,
                       float coreRight,
                       float fadeDistance);

/// Places every item using its current animated scale and returns the panel
/// rectangle that wraps the resulting row.
///
/// @param panelWidth When positive the panel is forced to that width and the
///                   row is centred inside it (macOS "expand once" mode).
///                   Pass a value <= 0 to make the panel hug the row instead.
/// @param presence Pointer influence, 0..1 (see UpdateScaleTargets).
LayoutFrame ApplyLayout(std::vector<DockItem*>& items,
                        const DockGeometry& geometry,
                        float panelWidth,
                        float mouseX,
                        float presence,
                        const LayoutMetrics& metrics);

/// True when the point is inside the panel (expanded by hitMargin).
bool HitTestDock(float px, float py,
                 const LayoutFrame& frame,
                 const DockGeometry& geometry,
                 const LayoutMetrics& metrics);

} // namespace ld

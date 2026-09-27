#include "DockLayout.h"

#include "DockItem.h"

namespace ld
{

namespace
{

/// Distance from a point to a rounded rectangle, negative inside.
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

} // namespace

LayoutMetrics MakeMetrics(const DockSettings& settings, float dpiScale)
{
    LayoutMetrics metrics;
    metrics.dpiScale = dpiScale;
    // All edges except the bottom expand away from their screen edge.
    // For vertical docks, the logical top maps to the outer (screen-facing)
    // icon edge after rotation, keeping magnification anchored correctly.
    metrics.anchorIconsAtTop = settings.dockEdge != DockEdge::Bottom;

    metrics.iconSize = static_cast<float>(settings.iconSize) * dpiScale;
    metrics.spacing = static_cast<float>(settings.iconSpacing) * dpiScale;
    metrics.cornerRadius = settings.cornerRadius * dpiScale;
    metrics.magnification = settings.panelMode == PanelMode::Static
        ? 1.0f
        : settings.magnification;
    // Kept for reference: the rounded clip is applied per icon while
    // rendering, using the global corner radius.
    metrics.backdropRadius = settings.backdrop.cornerRadius * dpiScale;

    metrics.paddingX = 12.0f * dpiScale;
    metrics.paddingY = 8.0f * dpiScale;

    metrics.indicatorHeight = 4.0f * dpiScale;
    metrics.hitMargin = 6.0f * dpiScale;

    return metrics;
}

SurfaceMargins ComputeMargins(const LayoutMetrics& metrics, DockEdge edge)
{
    // Room above the panel: a magnified icon grows upwards and a launching
    // icon additionally jumps. Horizontal room is only needed for the shadow,
    // because the widest possible row is already baked into the surface.
    const float scaleOverflow = (metrics.magnification - 1.0f) * metrics.iconSize;
    const float jumpRoom = 30.0f * metrics.dpiScale;
    const float shadowExtent = 26.0f * metrics.dpiScale;
    const bool verticalDock = edge == DockEdge::Left || edge == DockEdge::Right;
    const float tooltipRoom = (verticalDock ? 500.0f : 48.0f)
        * metrics.dpiScale;

    SurfaceMargins margins;
    // Keep enough horizontal canvas for the name bubble above either outer
    // icon. The dock panel remains centred; only transparent surface space
    // grows so natural one-line labels are not clipped at the HWND edges.
    const float tooltipSideRoom = 200.0f * metrics.dpiScale;
    margins.left = (std::max)(shadowExtent, tooltipSideRoom);
    margins.right = (std::max)(shadowExtent, tooltipSideRoom);
    if (edge == DockEdge::Top || edge == DockEdge::Left
        || edge == DockEdge::Right)
    {
        margins.top = shadowExtent;
        margins.bottom = scaleOverflow + jumpRoom + shadowExtent * 0.6f
            + tooltipRoom;
    }
    else
    {
        margins.top = scaleOverflow + jumpRoom + shadowExtent * 0.6f
            + tooltipRoom;
        margins.bottom = shadowExtent;
    }

    return margins;
}

DockGeometry ComputeGeometry(int itemCount, const LayoutMetrics& metrics,
                             DockEdge edge)
{
    const int safeCount = (itemCount > 0) ? itemCount : 1;
    const float count = static_cast<float>(safeCount);

    const float baseRowWidth =
        count * metrics.iconSize + (count - 1.0f) * metrics.spacing;

    const float maxRowWidth =
        count * metrics.iconSize * metrics.magnification
        + (count - 1.0f) * metrics.spacing;

    DockGeometry geometry;
    geometry.basePanelWidth = baseRowWidth + metrics.paddingX * 2.0f;
    geometry.maxPanelWidth = maxRowWidth + metrics.paddingX * 2.0f;

    // The icon row is vertically centred: equal padding above and below.
    // The running indicator dots live inside the bottom padding strip and
    // reserve no space of their own.
    geometry.panelHeight = metrics.paddingY * 2.0f + metrics.iconSize;

    const SurfaceMargins margins = ComputeMargins(metrics, edge);

    geometry.surfaceWidth = static_cast<int>(
        std::ceil(geometry.maxPanelWidth + margins.left + margins.right));
    geometry.surfaceHeight = static_cast<int>(
        std::ceil(geometry.panelHeight + margins.top + margins.bottom));

    geometry.panelY = margins.top;
    geometry.iconBaselineBottom =
        geometry.panelY + geometry.panelHeight - metrics.paddingY;
    geometry.indicatorCenterY =
        geometry.iconBaselineBottom + metrics.paddingY * 0.5f;

    return geometry;
}

void RowInfluenceBounds(const DockGeometry& geometry,
                        size_t itemCount,
                        const LayoutMetrics& metrics,
                        float& coreLeft,
                        float& coreRight,
                        float& fadeDistance)
{
    const size_t safeCount = (itemCount > 0) ? itemCount : 1;

    const float baseRowWidth = static_cast<float>(safeCount) * metrics.iconSize
        + static_cast<float>(safeCount - 1) * metrics.spacing;
    const float baseContentLeft =
        (static_cast<float>(geometry.surfaceWidth) - baseRowWidth) * 0.5f;

    coreLeft = baseContentLeft - metrics.hitMargin;
    coreRight = baseContentLeft + baseRowWidth + metrics.hitMargin;
    fadeDistance = metrics.paddingX;
}

float RowInfluenceFade(float mouseX,
                       float coreLeft,
                       float coreRight,
                       float fadeDistance)
{
    const float beyond = (mouseX < coreLeft) ? coreLeft - mouseX
        : (mouseX > coreRight) ? mouseX - coreRight
        : 0.0f;

    if (beyond <= 0.0f || fadeDistance <= 0.0f)
    {
        return beyond <= 0.0f ? 1.0f : 0.0f;
    }

    // Smoothstep down to zero: no discontinuity at the boundary, so the
    // spring never gets a step change to react to.
    const float t = ClampF(beyond / fadeDistance, 0.0f, 1.0f);

    return 1.0f - t * t * (3.0f - 2.0f * t);
}

void UpdateScaleTargets(std::vector<DockItem*>& items,
                        float mouseX,
                        float presence,
                        const LayoutMetrics& metrics)
{
    const float cell = metrics.iconSize + metrics.spacing;
    const float sigma = metrics.sigmaFactor * cell;
    const float twoSigmaSq = 2.0f * sigma * sigma;
    const float maxExtra = metrics.magnification - 1.0f;

    // Outside the icon row the dock does not react: without this the
    // magnified state would cling to the panel edges and only release after
    // the cursor crosses the whole padding. The caller already eased the raw
    // influence into `presence`, so multiplying by the raw edge factor again
    // would re-introduce the very step the easing removes — a fast sideways
    // flick would collapse the row in a single frame.
    const float strength = presence;

    for (DockItem* item : items)
    {
        if (strength <= 0.0f)
        {
            item->scaleSpring.target = 1.0f;
            continue;
        }

        // Measure against where the icon actually is this frame, not its
        // resting slot. In Fixed mode the row spreads into the expanded
        // panel, so the outer icons sit far away from their base centres —
        // measuring from the base would leave them unable to magnify at all.
        const float distance = mouseX - item->centerX;
        const float falloff = std::exp(-(distance * distance) / twoSigmaSq);

        item->scaleSpring.target = 1.0f + maxExtra * falloff * strength;
    }
}

LayoutFrame ApplyLayout(std::vector<DockItem*>& items,
                        const DockGeometry& geometry,
                        float panelWidth,
                        float mouseX,
                        float presence,
                        const LayoutMetrics& metrics)
{
    LayoutFrame frame;

    const float cell = metrics.iconSize + metrics.spacing;
    const float surfaceWidth = static_cast<float>(geometry.surfaceWidth);

    if (items.empty())
    {
        // Empty dock: a single empty slot, centred.
        frame.panelWidth = metrics.iconSize + metrics.paddingX * 2.0f;
        frame.panelX = (surfaceWidth - frame.panelWidth) * 0.5f;
        return frame;
    }

    const size_t count = items.size();
    const bool fixedPanel = panelWidth > 0.0f;

    const float baseRowWidth =
        static_cast<float>(count) * metrics.iconSize
        + static_cast<float>(count - 1) * metrics.spacing;

    float rowWidth = 0.0f;
    for (DockItem* item : items)
    {
        rowWidth += metrics.iconSize * item->scale;
    }
    rowWidth += metrics.spacing * static_cast<float>(count - 1);

    // The un-magnified row is centred in the window. Its centre is the one
    // fixed reference point that scale targets are measured against.
    const float baseContentLeft = (surfaceWidth - baseRowWidth) * 0.5f;

    float rowLeft = baseContentLeft;

    // Gap between neighbouring icons. The Fixed mode stretches this so the
    // row uses the width its panel just gained; Elastic keeps the base gap.
    float gap = metrics.spacing;

    if (fixedPanel)
    {
        // macOS style: the panel expands once, and the row spreads to use
        // the width it just gained. The slack between the expanded panel
        // and the currently magnified row is shared out between the gaps,
        // so an enlarging icon pushes its neighbours outward into the
        // reserved side space instead of leaving the row huddled in the
        // middle with dead margins on both ends.
        const float panelX = (surfaceWidth - panelWidth) * 0.5f;
        const float available = panelWidth - metrics.paddingX * 2.0f;
        const float slack = available - rowWidth;

        if (slack > 0.5f && count > 1)
        {
            gap += slack / static_cast<float>(count - 1);
            rowLeft = panelX + metrics.paddingX;
        }
        else
        {
            rowLeft = panelX + (panelWidth - rowWidth) * 0.5f;
        }
    }
    else if (presence > 0.0f)
    {
        // The anchor is clamped to the icon row on purpose: past either end
        // it degenerates and the whole row would chase the cursor towards the
        // edge (the old "sticks to your mouse" feeling). The resulting shift
        // is then faded out with the same release curve as the scales, so
        // the row glides back to centre instead of jumping.
        const float anchorX =
            ClampF(mouseX, baseContentLeft, baseContentLeft + baseRowWidth);

        const float local = anchorX - baseContentLeft;

        const float column = local / cell;
        int index = static_cast<int>(std::floor(column));
        if (index >= static_cast<int>(count))
        {
            index = static_cast<int>(count) - 1;
        }

        const float fraction = column - static_cast<float>(index);

        float offset = 0.0f;
        for (int i = 0; i < index; ++i)
        {
            offset += metrics.iconSize * items[static_cast<size_t>(i)]->scale
                + metrics.spacing;
        }

        offset += fraction
            * (metrics.iconSize * items[static_cast<size_t>(index)]->scale
               + metrics.spacing);

        const float anchored = anchorX - offset;

        // The row may drift sideways while the icons swell. Saturate it
        // smoothly instead of clamping, which would make the panel snap.
        const float limit = (metrics.magnification - 1.0f) * metrics.iconSize;
        const float shift = limit > 0.0f
            ? limit * std::tanh((anchored - baseContentLeft) / limit)
            : 0.0f;

        // `presence` is already the time-eased influence, so the drift fades
        // out on exactly the same curve as the icon scales. No extra spatial
        // factor here: the anchor is held steady through the release (see
        // App::Tick), so this is a clean fade back to centre.
        rowLeft = baseContentLeft + shift * presence;
    }

    float cursor = rowLeft;
    for (size_t i = 0; i < count; ++i)
    {
        DockItem* item = items[i];
        const float size = metrics.iconSize * item->scale;

        item->size = size;
        item->x = cursor;
        item->centerX = cursor + size * 0.5f;
        item->baseCenterX =
            baseContentLeft + metrics.iconSize * 0.5f
            + static_cast<float>(i) * cell;
        item->baselineBottom = metrics.anchorIconsAtTop
            ? geometry.panelY + metrics.paddingY + size
            : geometry.iconBaselineBottom;

        cursor += size + gap;
    }

    if (fixedPanel)
    {
        frame.panelWidth = panelWidth;
        frame.panelX = (surfaceWidth - panelWidth) * 0.5f;
    }
    else
    {
        // Elastic: the panel simply wraps whatever the row turned into.
        frame.panelWidth = rowWidth + metrics.paddingX * 2.0f;
        frame.panelX = rowLeft - metrics.paddingX;
    }

    return frame;
}

bool HitTestDock(float px, float py,
                 const LayoutFrame& frame,
                 const DockGeometry& geometry,
                 const LayoutMetrics& metrics)
{
    const D2D1_RECT_F rect = D2D1::RectF(
        frame.panelX - metrics.hitMargin,
        geometry.panelY - metrics.hitMargin,
        frame.panelX + frame.panelWidth + metrics.hitMargin,
        geometry.panelY + geometry.panelHeight + metrics.hitMargin);

    return RoundedRectDistance(px, py, rect, metrics.cornerRadius) <= 0.0f;
}

} // namespace ld

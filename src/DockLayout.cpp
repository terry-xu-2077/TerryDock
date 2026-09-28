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

float MaximumMagnifiedRowWidth(int itemCount, const LayoutMetrics& metrics)
{
    const int safeCount = (std::max)(itemCount, 1);
    const float count = static_cast<float>(safeCount);
    const float cell = metrics.iconSize + metrics.spacing;
    const float baseRowWidth =
        count * metrics.iconSize + (count - 1.0f) * metrics.spacing;

    const float maxExtraScale = (std::max)(0.0f, metrics.magnification - 1.0f);
    if (maxExtraScale <= 0.0f || safeCount <= 0)
    {
        return baseRowWidth;
    }

    const float sigma = metrics.sigmaFactor * cell;
    const float twoSigmaSq = 2.0f * sigma * sigma;
    constexpr float kMinimumVisibleScaleChange = 0.015f;

    // The influence is a smooth sum of Gaussians. Sample densely across the
    // resting icon centres once when geometry is rebuilt; this is far more
    // accurate than assuming every icon reaches maximum scale simultaneously,
    // while remaining trivial work even for a very large dock.
    const int intervals = (std::max)(1, (safeCount - 1) * 32);
    const float firstCenter = metrics.iconSize * 0.5f;
    const float lastCenter =
        firstCenter + static_cast<float>(safeCount - 1) * cell;

    float widest = baseRowWidth;
    for (int sample = 0; sample <= intervals; ++sample)
    {
        const float t = static_cast<float>(sample)
            / static_cast<float>(intervals);
        const float mouseX = firstCenter + (lastCenter - firstCenter) * t;

        float width = baseRowWidth;
        for (int i = 0; i < safeCount; ++i)
        {
            const float center =
                firstCenter + static_cast<float>(i) * cell;
            const float distance = mouseX - center;
            const float falloff =
                std::exp(-(distance * distance) / twoSigmaSq);
            const float scaleChange = maxExtraScale * falloff;

            if (scaleChange >= kMinimumVisibleScaleChange)
            {
                width += metrics.iconSize * scaleChange;
            }
        }

        widest = (std::max)(widest, width);
    }

    return widest;
}

void ResolveFixedIconPositions(const std::vector<DockItem*>& items,
                               float baseContentLeft,
                               float panelX,
                               float panelWidth,
                               const LayoutMetrics& metrics,
                               std::vector<float>& lefts)
{
    const size_t count = items.size();
    lefts.assign(count, 0.0f);
    if (count == 0)
    {
        return;
    }

    const float cell = metrics.iconSize + metrics.spacing;

    std::vector<float> widths(count, metrics.iconSize);
    std::vector<float> offsets(count, 0.0f);
    std::vector<float> desired(count, 0.0f);

    for (size_t i = 0; i < count; ++i)
    {
        widths[i] = metrics.iconSize * items[i]->scale;
        if (i > 0)
        {
            offsets[i] = offsets[i - 1]
                + widths[i - 1] + metrics.spacing;
        }

        const float baseCenter =
            baseContentLeft + metrics.iconSize * 0.5f
            + static_cast<float>(i) * cell;

        // Every icon wants to grow around its own resting centre.
        desired[i] = baseCenter - widths[i] * 0.5f - offsets[i];
    }

    // Project the desired positions onto the non-overlap constraints with
    // isotonic regression (PAVA). Unlike rebuilding the whole row from its
    // changing total width, this is anchored to the immutable resting slots:
    // only icons that are actually pushed by a growing neighbour move.
    struct Block
    {
        size_t first = 0;
        size_t last = 0;
        float sum = 0.0f;
        float weight = 0.0f;

        float Mean() const
        {
            return weight > 0.0f ? sum / weight : 0.0f;
        }
    };

    std::vector<Block> blocks;
    blocks.reserve(count);

    for (size_t i = 0; i < count; ++i)
    {
        blocks.push_back(Block{i, i, desired[i], 1.0f});

        while (blocks.size() >= 2)
        {
            Block& right = blocks.back();
            Block& left = blocks[blocks.size() - 2];
            if (left.Mean() <= right.Mean())
            {
                break;
            }

            Block merged;
            merged.first = left.first;
            merged.last = right.last;
            merged.sum = left.sum + right.sum;
            merged.weight = left.weight + right.weight;

            blocks.pop_back();
            blocks.back() = merged;
        }
    }

    std::vector<float> solved(count, 0.0f);
    for (const Block& block : blocks)
    {
        const float value = block.Mean();
        for (size_t i = block.first; i <= block.last; ++i)
        {
            solved[i] = value;
        }
    }

    for (size_t i = 0; i < count; ++i)
    {
        lefts[i] = solved[i] + offsets[i];
    }

    // The accurate fixed panel has enough total width for the resolved row.
    // Near the first/last icon the optimal local solution can be asymmetric,
    // so translate it as a whole only when it would cross the panel's inner
    // padding. This translation is continuous and does not relayout gaps.
    const float innerLeft = panelX + metrics.paddingX;
    const float innerRight = panelX + panelWidth - metrics.paddingX;
    const float rowLeft = lefts.front();
    const float rowRight = lefts.back() + widths.back();

    float shift = 0.0f;
    if (rowLeft < innerLeft)
    {
        shift = innerLeft - rowLeft;
    }
    if (rowRight + shift > innerRight)
    {
        shift += innerRight - (rowRight + shift);
    }

    if (std::fabs(shift) > 0.0001f)
    {
        for (float& left : lefts)
        {
            left += shift;
        }
    }
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

    const float widestAnimatedRow =
        MaximumMagnifiedRowWidth(safeCount, metrics);

    DockGeometry geometry;
    geometry.basePanelWidth = baseRowWidth + metrics.paddingX * 2.0f;

    // Fixed mode should reserve only the width that the Gaussian hover can
    // actually produce. A tiny safety margin covers spring overshoot and
    // sub-pixel rounding without creating the large empty wings seen in the
    // previous count * iconSize * constant approximation.
    const float fixedSafety = 4.0f * metrics.dpiScale;
    geometry.fixedPanelWidth =
        widestAnimatedRow + metrics.paddingX * 2.0f + fixedSafety;

    // Surface/shadow allocation gets a little more headroom than the visible
    // panel, but this no longer affects the panel the user sees.
    const float renderSafety = 8.0f * metrics.dpiScale;
    geometry.maxPanelWidth =
        geometry.fixedPanelWidth + renderSafety;

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
                        const LayoutMetrics& metrics,
                        bool useBaseCenters)
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
    constexpr float kMinimumVisibleScaleChange = 0.015f;

    for (DockItem* item : items)
    {
        if (strength <= 0.0f)
        {
            item->scaleSpring.target = 1.0f;
            continue;
        }

        // Animated modes use the stable resting centre so magnification never
        // feeds back into its own hit calculation as neighbouring icons move.
        const float referenceCenter =
            useBaseCenters ? item->baseCenterX : item->centerX;
        const float distance = mouseX - referenceCenter;
        const float falloff = std::exp(-(distance * distance) / twoSigmaSq);
        const float scaleChange = maxExtra * falloff * strength;

        // Gaussian falloff has infinite tails. Ignore changes too small to
        // see so distant icons remain exactly at their resting scale instead
        // of subtly moving the fixed layout on every hover frame.
        item->scaleSpring.target = scaleChange < kMinimumVisibleScaleChange
            ? 1.0f : 1.0f + scaleChange;
    }
}

LayoutFrame ApplyLayout(std::vector<DockItem*>& items,
                        const DockGeometry& geometry,
                        float panelWidth,
                        float mouseX,
                        float presence,
                        const LayoutMetrics& metrics)
{
    (void)mouseX;
    (void)presence;

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

    if (fixedPanel)
    {
        const float panelX = (surfaceWidth - panelWidth) * 0.5f;
        std::vector<float> lefts;
        ResolveFixedIconPositions(
            items, baseContentLeft, panelX, panelWidth, metrics, lefts);

        for (size_t i = 0; i < count; ++i)
        {
            DockItem* item = items[i];
            const float size = metrics.iconSize * item->scale;

            item->size = size;
            item->x = lefts[i];
            item->centerX = lefts[i] + size * 0.5f;
            item->baseCenterX =
                baseContentLeft + metrics.iconSize * 0.5f
                + static_cast<float>(i) * cell;
            item->baselineBottom = metrics.anchorIconsAtTop
                ? geometry.panelY + metrics.paddingY + size
                : geometry.iconBaselineBottom;
        }
    }
    else
    {
        // Elastic deliberately re-centres the changing row because its panel
        // breathes with the row. This behaviour already feels correct and is
        // kept separate from the fixed-mode solver above.
        float cursor = (surfaceWidth - rowWidth) * 0.5f;

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

            cursor += size + metrics.spacing;
        }
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

#pragma once

#include "Config.h"

namespace ld
{

/// Maps the dock's stable, horizontal layout space onto its screen edge.
/// Input and layout code always use logical coordinates; only the window,
/// renderer and pointer boundary convert to/from physical coordinates.
class DockTransform
{
public:
    DockTransform(DockEdge edge = DockEdge::Bottom,
                  float logicalWidth = 0.0f,
                  float logicalHeight = 0.0f)
        : edge_(edge), width_(logicalWidth), height_(logicalHeight) {}

    bool Vertical() const
    {
        return edge_ == DockEdge::Left || edge_ == DockEdge::Right;
    }

    float PhysicalWidth() const { return Vertical() ? height_ : width_; }
    float PhysicalHeight() const { return Vertical() ? width_ : height_; }

    D2D1_POINT_2F ToPhysical(float x, float y) const
    {
        switch (edge_)
        {
        case DockEdge::Left: return D2D1::Point2F(y, x);
        case DockEdge::Right: return D2D1::Point2F(height_ - y, x);
        case DockEdge::Top:
        case DockEdge::Bottom:
        default: return D2D1::Point2F(x, y);
        }
    }

    D2D1_POINT_2F ToLogical(float x, float y) const
    {
        switch (edge_)
        {
        case DockEdge::Left: return D2D1::Point2F(y, x);
        case DockEdge::Right: return D2D1::Point2F(y, height_ - x);
        case DockEdge::Top:
        case DockEdge::Bottom:
        default: return D2D1::Point2F(x, y);
        }
    }

    D2D1_RECT_F ToPhysical(const D2D1_RECT_F& rect) const
    {
        switch (edge_)
        {
        case DockEdge::Left:
            return D2D1::RectF(rect.top, rect.left, rect.bottom, rect.right);
        case DockEdge::Right:
            return D2D1::RectF(height_ - rect.bottom, rect.left,
                               height_ - rect.top, rect.right);
        case DockEdge::Top:
        case DockEdge::Bottom:
        default: return rect;
        }
    }

    D2D1_MATRIX_3X2_F Matrix() const
    {
        switch (edge_)
        {
        case DockEdge::Left: return D2D1::Matrix3x2F(0, 1, 1, 0, 0, 0);
        case DockEdge::Right:
            return D2D1::Matrix3x2F(0, 1, -1, 0, height_, 0);
        case DockEdge::Top:
        case DockEdge::Bottom:
        default: return D2D1::Matrix3x2F::Identity();
        }
    }

private:
    DockEdge edge_;
    float width_;
    float height_;
};

} // namespace ld

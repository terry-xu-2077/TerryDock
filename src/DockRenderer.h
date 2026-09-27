#pragma once

#include "Common.h"
#include "DockTransform.h"

#include <vector>

namespace ld
{

/// Owns the off screen 32bpp premultiplied surface that is handed to
/// UpdateLayeredWindow.
///
/// Pipeline:  HWND (WS_EX_LAYERED)
///              -> Direct2D DC render target bound to a DIB section
///              -> UpdateLayeredWindow
///
/// Nothing is drawn with GDI and no child controls exist, so the window keeps
/// true per pixel alpha and clean anti aliased rounded corners.
class DockRenderer
{
public:
    ~DockRenderer();

    bool Initialize();
    void Shutdown();

    /// Recreates the DIB + Direct2D render target for a new surface size.
    bool Resize(int width, int height);
    void SetOrientation(DockEdge edge, float logicalWidth, float logicalHeight);

    bool BeginDraw();
    bool EndDraw();

    ID2D1RenderTarget* Target() const { return rt_.Get(); }
    IWICImagingFactory* Wic() const { return wic_.Get(); }

    HDC SurfaceDC() const { return memoryDC_; }
    int Width() const { return width_; }
    int Height() const { return height_; }

    /// Panel style. The shadow is baked once for the widest panel the dock
    /// can ever reach and then stretched horizontally as the panel breathes.
    void SetPanelStyle(float shadowBaseWidth,
                       float panelHeight,
                       float cornerRadius,
                       float shadowOpacity,
                       float shadowBlur,
                       float shadowOffsetY);

    /// Each icon brings its own plate look, so the colours arrive per call.
    /// The brushes are rebuilt only when those colours actually change.
    void DrawIconBackdrop(const D2D1_RECT_F& rect,
                          float radius,
                          float opacity,
                          bool gradient,
                          const D2D1_COLOR_F& top,
                          const D2D1_COLOR_F& bottom);

    void DrawShadow(float panelX, float panelY, float panelWidth);
    void DrawBackground(float panelX,
                        float panelY,
                        float panelWidth,
                        float backgroundOpacity,
                        float borderOpacity);

    /// Custom panel background. Pass an empty alpha (a < 0) to fall back to
    /// the built-in white; otherwise the panel blends vertically from
    /// `top` to `bottom`.
    void SetPanelBackground(const D2D1_COLOR_F& top, const D2D1_COLOR_F& bottom,
                            bool custom);
    void DrawIndicator(float x, float y, float diameter);
    void DrawIcon(ID2D1Bitmap* bitmap, const D2D1_RECT_F& destination);

    void DrawTooltip(const std::wstring& text,
                     float centerX,
                     float bottom,
                      float scale,
                      float dpiScale,
                      float cornerRadius,
                      float opacity,
                      bool arrowUp = false);

    /// Thin inner highlight rim along a plate's rounded edge. Drawn on top
    /// of the icon so full bleed tiles (clipped by the same radius) get the
    /// macOS style bright edge. The gradient runs top bright to bottom dim,
    /// like a light source above the dock.
    void DrawPlateRim(const D2D1_RECT_F& rect, float radius, float opacity,
                      float thickness, const D2D1_COLOR_F& color);

private:
    void ReleaseSurface();
    void EnsureBrushes();
    void EnsurePlateBrush(bool gradient,
                          const D2D1_COLOR_F& top,
                          const D2D1_COLOR_F& bottom);
    void BuildShadow();

    ComPtr<ID2D1Factory> d2d_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<IWICImagingFactory> wic_;

    HDC screenDC_ = nullptr;
    HDC memoryDC_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ previousBitmap_ = nullptr;
    void* dibBits_ = nullptr;

    int width_ = 0;
    int height_ = 0;

    ComPtr<ID2D1DCRenderTarget> rt_;

    ComPtr<ID2D1SolidColorBrush> backgroundBrush_;
    /// Custom panel background gradient (see SetPanelBackground).
    ComPtr<ID2D1LinearGradientBrush> panelGradient_;
    /// Panel outline: one uniform hairline all the way around.
    ComPtr<ID2D1SolidColorBrush> edgeBrush_;
    /// Icon plate rim highlight, same idea but tuned for tile sized rects.
    ComPtr<ID2D1SolidColorBrush> rimBrush_;
    ComPtr<ID2D1SolidColorBrush> indicatorBrush_;
    /// Plate brushes, rebuilt whenever an icon asks for different colours.
    ComPtr<ID2D1SolidColorBrush> plateSolid_;
    ComPtr<ID2D1LinearGradientBrush> plateGradient_;
    D2D1_COLOR_F plateTop_{};
    D2D1_COLOR_F plateBottom_{};
    bool plateGradientActive_ = true;
    bool plateReady_ = false;

    ComPtr<ID2D1Bitmap> shadow_;
    int shadowWidth_ = 0;
    int shadowHeight_ = 0;

    D2D1_COLOR_F panelTop_{};
    D2D1_COLOR_F panelBottom_{};
    bool panelCustom_ = false;

    /// Perceived brightness of the effective panel (0..1). Drives the
    /// running indicator colour: dark dot on a light panel, light on dark.
    float panelLuminance_ = 1.0f;

    /// Half of the (shadow bitmap - base panel) difference, in both axes.
    float shadowExtent_ = 0.0f;

    /// Width of the two fixed side slices used for horizontal nine slicing.
    float shadowSegment_ = 0.0f;

    float shadowBaseWidth_ = 0.0f;
    float panelHeight_ = 0.0f;
    float cornerRadius_ = 18.0f;
    float shadowOpacity_ = 0.30f;
    float shadowBlur_ = 14.0f;
    float shadowOffsetY_ = 5.0f;

    bool brushDirty_ = true;
    bool shadowDirty_ = true;
    bool drawing_ = false;
    DockEdge edge_ = DockEdge::Bottom;
    float logicalWidth_ = 0.0f;
    float logicalHeight_ = 0.0f;
};

} // namespace ld

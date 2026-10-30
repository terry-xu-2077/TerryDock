#pragma once

// MacUi: a tiny, dependency-free UI layer for LightDock's secondary windows.
//
// Everything here is drawn with Direct2D + DirectWrite (already used by the
// dock itself), so the look is consistent and anti-aliased without pulling in
// any UI framework. Only text input uses a native EDIT control, hosted inside
// a self-drawn field.
//
//   * TrackMenu  - macOS style popup menu (rounded, soft shadow, submenus,
//                  check marks, icons, keyboard navigation).
//   * Form       - macOS "System Settings" style window: optional sidebar or
//                  segmented navigation, grouped rounded cards with rows
//                  (switch, slider, popup button, colour well, text field),
//                  and a footer with push buttons.
//
// Both follow the Windows light/dark app theme.

#include "Common.h"

#include <functional>
#include <string>
#include <vector>

namespace ld::ui
{

// ---------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------

struct Palette
{
    bool dark = false;

    D2D1_COLOR_F windowBg;
    D2D1_COLOR_F sidebarBg;
    D2D1_COLOR_F hairline;
    D2D1_COLOR_F card;
    D2D1_COLOR_F cardBorder;
    D2D1_COLOR_F separator;
    D2D1_COLOR_F text;
    D2D1_COLOR_F secondaryText;
    D2D1_COLOR_F tertiaryText;
    D2D1_COLOR_F accent;
    D2D1_COLOR_F accentPressed;
    D2D1_COLOR_F onAccent;
    D2D1_COLOR_F controlBg;
    D2D1_COLOR_F controlPressed;
    D2D1_COLOR_F controlBorder;
    D2D1_COLOR_F shadow;
    D2D1_COLOR_F fieldBg;
    D2D1_COLOR_F fieldBorder;
    D2D1_COLOR_F fill;          // switch off, slider track, segmented track
    D2D1_COLOR_F knob;
    D2D1_COLOR_F hover;
    D2D1_COLOR_F segmentSelected;

    D2D1_COLOR_F menuBg;
    D2D1_COLOR_F menuBorder;
    D2D1_COLOR_F menuInnerBorder;
    D2D1_COLOR_F menuSeparator;
    D2D1_COLOR_F menuText;
    D2D1_COLOR_F menuDisabledText;
    D2D1_COLOR_F menuHeaderText;
};

/// Reads "AppsUseLightTheme" from the registry.
bool SystemUsesDarkTheme();

const Palette& GetPalette(bool dark);

/// Dark caption + caption colour matching the window background
/// (Windows 11; harmlessly ignored on Windows 10).
void ApplyWindowChrome(HWND hwnd, const Palette& palette);

/// Converts any WIC source to a premultiplied D2D bitmap for `target`.
ComPtr<ID2D1Bitmap> CreateBitmapFromWic(ID2D1RenderTarget* target,
                                        IWICBitmapSource* source);

// ---------------------------------------------------------------------------
// Popup menu
// ---------------------------------------------------------------------------

struct MenuItem
{
    enum class Type
    {
        Command,
        Separator,
        Header,
    };

    Type type = Type::Command;
    UINT id = 0;
    std::wstring text;
    ComPtr<IWICBitmapSource> icon;
    bool checked = false;
    bool enabled = true;
    std::vector<MenuItem> children;

    static MenuItem Command(UINT id, std::wstring text, bool enabled = true,
                            bool checked = false);
    static MenuItem Submenu(std::wstring text, std::vector<MenuItem> children);
    static MenuItem Separator();
    static MenuItem Header(std::wstring text);
};

struct MenuOptions
{
    /// Open towards the left / above the anchor (tray menu).
    bool alignRight = false;
    bool alignBottom = false;

    /// Popup-button behaviour: place the menu so this item sits on top of
    /// the button (anchor = button top-left, anchorHeight = button height).
    int alignItem = -1;
    int anchorHeight = 0;

    /// Minimum menu width in physical pixels.
    int minWidth = 0;

    /// Called on every loop iteration while the menu is open (lets the dock
    /// keep animating underneath).
    std::function<void()> idle;
};

/// Shows a menu and blocks until it is dismissed. Returns the chosen id, or 0.
UINT TrackMenu(HWND owner, const std::vector<MenuItem>& items, POINT anchor,
               const MenuOptions& options = {});

// ---------------------------------------------------------------------------
// Form
// ---------------------------------------------------------------------------

enum class Navigation
{
    None,
    Sidebar,
    Segmented,
};

enum class ButtonRole
{
    Normal,
    Primary,
};

class Form
{
public:
    enum class Kind
    {
        Switch,
        Slider,
        Choice,
        Color,
        Edit,
        Info,
        Header,
        Paragraph,
    };

    struct Control
    {
        Kind kind = Kind::Info;
        std::wstring label;
        std::wstring sublabel;

        bool* boolValue = nullptr;
        int* intValue = nullptr;
        COLORREF* colorValue = nullptr;
        std::wstring* textValue = nullptr;

        int minimum = 0;
        int maximum = 100;
        int step = 1;
        std::function<std::wstring(int)> format;

        std::vector<std::wstring> options;

        /// Colour well caption / info value / header subtitle.
        std::function<std::wstring()> caption;

        /// Returns false to grey the row out.
        std::function<bool()> enabled;

        /// Optional trailing button next to a text field.
        std::wstring buttonText;
        std::function<void()> onButton;
        bool readOnly = false;

        /// Header image and title, queried on every paint so callers can
        /// swap them live.
        std::function<IWICBitmapSource*()> image;
        std::function<std::wstring()> dynamicLabel;
        float imageSize = 56.0f;

        // --- internal -------------------------------------------------------
        HWND edit = nullptr;
        int page = 0;
        D2D1_RECT_F row{};
        D2D1_RECT_F box{};
        D2D1_RECT_F button{};
        float knob = 0.0f;
        IWICBitmapSource* cachedSource = nullptr;
        ComPtr<ID2D1Bitmap> cachedBitmap;
    };

    struct Options
    {
        HWND owner = nullptr;
        HINSTANCE instance = nullptr;
        std::wstring title;
        float width = 640.0f;    // client size in DIPs
        float height = 480.0f;  // <= 0: fit the content
        Navigation navigation = Navigation::None;
        HICON icon = nullptr;

        /// Work area to centre on (physical pixels). Empty = owner monitor.
        RECT center{};
    };

    Form() = default;
    ~Form();

    Form(const Form&) = delete;
    Form& operator=(const Form&) = delete;

    // --- building (before Create) ------------------------------------------
    int AddPage(std::wstring title, wchar_t glyph = 0,
                D2D1_COLOR_F tint = D2D1::ColorF(0x8E8E93));
    void AddSection(std::wstring title = {}, std::wstring footnote = {});

    Control& AddSwitch(std::wstring label, bool* value,
                       std::wstring sublabel = {});
    Control& AddSlider(std::wstring label, int* value, int minimum,
                       int maximum, std::function<std::wstring(int)> format,
                       int step = 1);
    Control& AddChoice(std::wstring label, int* value,
                       std::vector<std::wstring> options);
    Control& AddColor(std::wstring label, COLORREF* value,
                      std::function<std::wstring()> caption = {});
    Control& AddEdit(std::wstring label, std::wstring* value,
                     std::wstring buttonText = {},
                     std::function<void()> onButton = {});
    Control& AddInfo(std::wstring label, std::function<std::wstring()> value);
    Control& AddHeader(std::function<IWICBitmapSource*()> image,
                       std::function<std::wstring()> title,
                       std::function<std::wstring()> subtitle,
                       float imageSize = 56.0f);
    Control& AddParagraph(std::wstring text);

    void AddButton(int id, std::wstring text,
                   ButtonRole role = ButtonRole::Normal,
                   bool alignLeft = false);

    /// Live callbacks.
    std::function<void(Control&)> onChange;
    std::function<void(int id)> onButton;
    std::function<void()> onClose;   // caption X / Esc

    // --- lifetime ----------------------------------------------------------
    bool Create(const Options& options);
    void Destroy();
    HWND Handle() const { return hwnd_; }

    void SelectPage(int page);

    /// Re-reads all bound values (e.g. after "restore defaults").
    void Refresh();

    /// Updates a text field and its bound string.
    void SetText(Control& control, const std::wstring& text);

private:
    struct Section
    {
        std::wstring title;
        std::wstring footnote;
        std::vector<Control*> rows;
        bool plain = false;

        D2D1_RECT_F card{};
        D2D1_RECT_F titleRect{};
        D2D1_RECT_F footRect{};
    };

    struct Page
    {
        std::wstring title;
        wchar_t glyph = 0;
        D2D1_COLOR_F tint{};
        std::vector<Section> sections;
        float contentHeight = 0.0f;
        float scroll = 0.0f;
        D2D1_RECT_F nav{};
    };

    struct Button
    {
        int id = 0;
        std::wstring text;
        ButtonRole role = ButtonRole::Normal;
        bool alignLeft = false;
        D2D1_RECT_F rect{};
    };

    enum class Target
    {
        None,
        Control,
        ControlButton,
        Nav,
        Footer,
        Thumb,
    };

    struct Hit
    {
        Target target = Target::None;
        int index = -1;

        bool operator==(const Hit& other) const
        {
            return target == other.target && index == other.index;
        }
        bool operator!=(const Hit& other) const { return !(*this == other); }
    };

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message,
                                       WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK EditSubclassProc(HWND hwnd, UINT message,
                                             WPARAM wParam, LPARAM lParam,
                                             UINT_PTR id, DWORD_PTR data);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    Control& NewControl(Kind kind, std::wstring label);
    Section& CurrentSection();

    void CreateTextFormats();
    void CreateEditFonts();
    void Layout();
    void LayoutPage(Page& page);
    void PositionEdits();
    void UpdateTheme();

    void Paint();
    void Draw();
    void DrawSidebar();
    void DrawSegmented();
    void DrawPage(const Page& page);
    void DrawControl(Control& control);
    void DrawFooter();
    void DrawScrollbar(const Page& page);
    void DrawFocusRing(const D2D1_RECT_F& rect, float radius);

    void FillRound(const D2D1_RECT_F& rect, float radius,
                   const D2D1_COLOR_F& color);
    void StrokeRound(const D2D1_RECT_F& rect, float radius,
                     const D2D1_COLOR_F& color, float width = 1.0f);
    void FillRect(const D2D1_RECT_F& rect, const D2D1_COLOR_F& color);
    void Line(float x0, float y0, float x1, float y1,
              const D2D1_COLOR_F& color, float width = 1.0f);
    void Text(const std::wstring& text, IDWriteTextFormat* format,
              const D2D1_RECT_F& rect, const D2D1_COLOR_F& color,
              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);
    float MeasureWidth(const std::wstring& text, IDWriteTextFormat* format);
    float MeasureHeight(const std::wstring& text, IDWriteTextFormat* format,
                        float width);

    bool IsEnabled(const Control& control) const;
    D2D1_RECT_F Viewport() const;
    float PageOffset(const Page& page) const;
    D2D1_RECT_F ToClient(const Page& page, const D2D1_RECT_F& rect) const;
    D2D1_RECT_F ThumbRect(const Page& page) const;

    Hit HitTest(float x, float y) const;
    void OnMouseDown(float x, float y);
    void OnMouseMove(float x, float y);
    void OnMouseUp(float x, float y);
    void OnWheel(int delta);
    void OnKey(WPARAM key);
    void Activate(const Hit& hit);
    void SetSliderFromX(Control& control, float x);
    void SetScroll(Page& page, float value);
    void EnsureVisible(Control& control);
    void ChangeFocus(int direction);
    std::vector<Hit> FocusOrder() const;
    void Notify(Control& control);
    void StartAnimation();
    bool StepAnimation();
    void SyncEditsFromValues();
    void Invalidate();

    float Px(float dip) const { return dip * scale_; }
    float Dip(float px) const { return px / scale_; }

    HWND hwnd_ = nullptr;
    Options options_;
    std::vector<Page> pages_;
    std::vector<std::unique_ptr<Control>> controls_;
    std::vector<Button> buttons_;
    int page_ = 0;

    float scale_ = 1.0f;
    float width_ = 0.0f;   // client DIPs
    float height_ = 0.0f;
    bool dark_ = false;
    const Palette* palette_ = nullptr;
    HBRUSH fieldBrush_ = nullptr;
    HFONT editFont_ = nullptr;
    int editLineHeight_ = 16;
    bool syncing_ = false;

    ComPtr<ID2D1DCRenderTarget> target_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<ID2D1StrokeStyle> roundStroke_;
    ComPtr<IDWriteTextFormat> body_;
    ComPtr<IDWriteTextFormat> bodyRight_;
    ComPtr<IDWriteTextFormat> small_;
    ComPtr<IDWriteTextFormat> sectionTitle_;
    ComPtr<IDWriteTextFormat> pageTitle_;
    ComPtr<IDWriteTextFormat> headerTitle_;
    ComPtr<IDWriteTextFormat> buttonText_;
    ComPtr<IDWriteTextFormat> glyph_;
    ComPtr<IDWriteTextFormat> wrap_;

    HDC backDc_ = nullptr;
    HBITMAP backBitmap_ = nullptr;
    HGDIOBJ backOld_ = nullptr;
    int backWidth_ = 0;
    int backHeight_ = 0;

    Hit hover_;
    Hit pressed_;
    Hit focus_;
    bool focusVisible_ = false;
    bool tracking_ = false;
    float thumbGrab_ = 0.0f;
    float scrollbarAlpha_ = 0.0f;
    ULONGLONG scrollbarShownAt_ = 0;
    bool timerRunning_ = false;
};

} // namespace ld::ui

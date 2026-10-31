#pragma once

// Launcher: LightDock's own Start and Search popups.
//
// Windows 11 draws its Start menu and search flyout in the shell's
// composition layer and ignores attempts to move them, so they can never
// open next to a Dock icon. The launcher reads the same "All apps" list
// (shell:AppsFolder: desktop shortcuts and packaged apps) and shows it in a
// popup that points at the icon that opened it:
//
//   * Start  - search field + alphabetical app grid + settings/power footer.
//   * Search - Spotlight style: a large search field with ranked results.
//
// Drawn with Direct2D on a layered window (rounded body + tail as one
// shape), matching MacUi menus. Text input is self-drawn and keeps a system
// caret so the IME composition/candidate windows follow it.

#include "Common.h"
#include "Config.h"
#include "MacUi.h"

#include <shobjidl.h>

#include <string>
#include <vector>

namespace ld
{

/// The "All apps" list as the Start menu sees it.
class AppCatalog
{
public:
    struct App
    {
        std::wstring name;
        std::wstring lowerName;
        std::wstring parsingName;   // shell:AppsFolder\<parsingName>
        std::wstring targetPath;    // .exe behind a desktop shortcut, if any
        std::wstring lowerTarget;   // file name of targetPath, lower case
        ComPtr<IShellItem> item;
        ComPtr<IWICBitmapSource> icon;
        bool iconLoaded = false;
        int category = -1;          // AppCatalog::Category

        // Icon analysis (see AppCatalog::Icon).
        bool tile = false;          // already a full-bleed square tile
        bool lightGlyph = false;    // mostly white artwork: use a dark plate
        bool coloredPlate = false;  // logo on its own solid tile colour
        D2D1_COLOR_F plateColor{};
        WICRect crop{};             // square around the visible artwork
    };

    /// Smart groups, in display order. Anything the rules cannot place
    /// lands in Other.
    enum Category : int
    {
        Design,
        Development,
        Office,
        AI,
        Communication,
        Media,
        Games,
        Network,
        System,
        Other,
        CategoryCount,
    };
    static const wchar_t* CategoryTitle(int category);

    /// Re-enumerates when stale (or always when `force`). Keeps already
    /// loaded icons for apps that are still present.
    void Refresh(bool force = false);

    std::vector<App>& Apps() { return apps_; }
    bool Stale() const { return apps_.empty() || GetTickCount64() - refreshedAt_ > 30000; }

    /// Loads (once) and returns the app's icon.
    IWICBitmapSource* Icon(App& app);

    static bool Launch(const App& app, bool elevated = false);

    IWICImagingFactory* Wic() const { return wic_.Get(); }

private:
    std::vector<App> apps_;
    ULONGLONG refreshedAt_ = 0;
    ComPtr<IWICImagingFactory> wic_;
};

class Launcher
{
public:
    enum class Mode
    {
        Start,
        Search,
    };

    class Host
    {
    public:
        virtual ~Host() = default;
        virtual const wchar_t* LauncherText(const wchar_t* chinese) const = 0;
        virtual void OnLauncherPin(const std::wstring& path) = 0;
        virtual void OnLauncherSystemStart() = 0;
        virtual void OnLauncherVisibilityChanged(bool visible) = 0;

        /// Start panel pinned row (AppsFolder parsing names, in order).
        virtual std::vector<std::wstring> LauncherPins() const = 0;
        virtual void SetLauncherPins(const std::vector<std::wstring>& pins) = 0;

        /// Apps hidden from the groups (shown in the collapsed "Hidden" row).
        virtual std::vector<std::wstring> LauncherHidden() const = 0;
        virtual void SetLauncherHidden(const std::vector<std::wstring>& hidden) = 0;
    };

    Launcher() = default;
    ~Launcher();

    Launcher(const Launcher&) = delete;
    Launcher& operator=(const Launcher&) = delete;

    void Initialize(HWND owner, Host* host);
    void Shutdown();

    /// Opens next to `icon` (screen pixels) on a dock anchored at `edge`.
    void Show(Mode mode, const RECT& icon, DockEdge edge);
    void Hide();

    bool Visible() const { return visible_; }
    Mode CurrentMode() const { return mode_; }

    /// When the popup last closed (GetTickCount64). Lets the Dock tell a
    /// "click the icon again to close" from a fresh open.
    ULONGLONG HiddenAt() const { return hiddenAt_; }

private:
    enum class Target
    {
        None,
        Field,
        Item,
        SystemStart,
        Settings,
        Power,
        PinToggle,
        GroupHeader,
    };

    struct Hit
    {
        Target target = Target::None;
        int index = -1;
        bool operator==(const Hit& o) const
        {
            return target == o.target && index == o.index;
        }
    };

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message,
                                       WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    const wchar_t* T(const wchar_t* chinese) const;

    bool GridMode() const;
    void UpdateResults();
    void ComputeLayout();
    void Place();
    bool EnsureSurface();
    void Render();
    void Present();

    void DrawField();
    void DrawGrid();
    void DrawList();
    void DrawFooter();
    void DrawIcon(AppCatalog::App& app, const D2D1_RECT_F& rect);
    ID2D1Bitmap* ScaledIcon(AppCatalog::App& app, int pixels);
    D2D1_RECT_F SnapToPixels(const D2D1_RECT_F& rect) const;
    void LayoutFooter();

    struct Cell
    {
        int app = -1;              // index into catalog_.Apps()
        D2D1_RECT_F rect{};        // content coordinates
        bool pinned = false;
    };

    /// A titled block of the grid. The pinned row has a plain title; groups
    /// (and "Hidden") have a clickable header row and start collapsed.
    struct Section
    {
        std::wstring title;
        float top = 0.0f;
        int group = -1;            // -1 pinned, CategoryCount = hidden
        bool expanded = true;
        int count = 0;
        std::vector<int> preview;  // first few apps, shown when collapsed
        D2D1_RECT_F header{};      // content coordinates (groups only)
    };

    static constexpr int kHiddenGroup = AppCatalog::CategoryCount;
    bool IsHidden(int app) const;
    void ToggleHidden(int app);
    void ToggleGroup(int section, bool fromSticky);
    int StickySection() const;
    void DrawGroupHeader(const Section& section, const D2D1_RECT_F& rect,
                         bool hot);

    void BuildGrid();
    std::vector<int> PinnedApps() const;
    bool IsPinned(int app) const;
    void TogglePin(int app);
    D2D1_RECT_F PinButtonRect(int index) const;
    void UpdateDrag(float contentX, float contentY);
    void FinishDrag();
    int AppAt(int index) const;

    D2D1_RECT_F ContentRect() const;
    D2D1_RECT_F ItemRect(int index) const;   // content coordinates
    int VisibleCount() const;
    float ContentHeight() const;
    void Scroll(float delta);
    void EnsureSelectionVisible();
    Hit HitTest(float x, float y) const;

    void MoveSelection(int dx, int dy);
    void LaunchSelected(bool elevated = false);
    void ShowItemMenu(int index, POINT screen);
    void ShowPowerMenu();
    void InsertText(const std::wstring& text);
    void Backspace();

    HWND owner_ = nullptr;
    HWND hwnd_ = nullptr;
    Host* host_ = nullptr;

    AppCatalog catalog_;
    std::vector<int> results_;      // indices into catalog_.Apps()

    // Start grid: pinned row + smart groups.
    std::vector<std::wstring> pins_;
    std::vector<std::wstring> hidden_;
    /// Expanded groups (session only; everything starts collapsed).
    std::vector<bool> expanded_ =
        std::vector<bool>(AppCatalog::CategoryCount + 1, false);
    std::vector<Cell> cells_;
    std::vector<Section> sections_;
    D2D1_RECT_F pinnedArea_{};      // content coordinates
    D2D1_RECT_F pinHint_{};
    float gridHeight_ = 0.0f;

    // Dragging a cell into / within the pinned row.
    bool dragging_ = false;
    int dragApp_ = -1;
    bool dragFromPinned_ = false;
    int dragSlot_ = -1;
    D2D1_POINT_2F pressPoint_{};    // content coordinates
    D2D1_POINT_2F dragPoint_{};     // content coordinates
    std::wstring query_;
    int selected_ = 0;
    float scroll_ = 0.0f;
    Hit hover_;
    Hit pressed_;

    Mode mode_ = Mode::Start;
    DockEdge edge_ = DockEdge::Bottom;
    RECT icon_{};
    bool visible_ = false;
    bool dark_ = false;
    const ui::Palette* palette_ = nullptr;
    ULONGLONG hiddenAt_ = 0;
    ULONGLONG shownAt_ = 0;
    BYTE alpha_ = 0;
    bool caretOn_ = true;
    bool iconsPending_ = false;
    int iconBudget_ = 0;
    bool menuOpen_ = false;

    // Geometry (DIPs unless noted).
    float scale_ = 1.0f;
    float bodyW_ = 0.0f;
    float bodyH_ = 0.0f;
    D2D1_RECT_F body_{};           // in window DIPs
    D2D1_POINT_2F tip_{};          // in window DIPs
    POINT windowPos_{};            // physical
    SIZE windowSize_{};            // physical

    HDC surfaceDc_ = nullptr;
    HBITMAP surfaceBitmap_ = nullptr;
    HGDIOBJ surfaceOld_ = nullptr;
    SIZE surfaceSize_{};
    ComPtr<ID2D1DCRenderTarget> target_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    struct ScaledBitmap
    {
        IWICBitmapSource* source = nullptr;
        int pixels = 0;
        ComPtr<ID2D1Bitmap> bitmap;
    };
    std::vector<ScaledBitmap> bitmaps_;

    // Footer buttons (body coordinates).
    D2D1_RECT_F systemButton_{};
    D2D1_RECT_F settingsButton_{};
    D2D1_RECT_F powerButton_{};

    ComPtr<IDWriteTextFormat> fieldFormat_;
    ComPtr<IDWriteTextFormat> bigFieldFormat_;
    ComPtr<IDWriteTextFormat> itemFormat_;
    ComPtr<IDWriteTextFormat> gridFormat_;
    ComPtr<IDWriteTextFormat> sectionFormat_;
    ComPtr<IDWriteTextFormat> smallFormat_;
    ComPtr<IDWriteTextFormat> glyphFormat_;
    ComPtr<IDWriteTextFormat> bigGlyphFormat_;
    ComPtr<IDWriteTextFormat> pinGlyphFormat_;
    ComPtr<IDWriteTextFormat> groupFormat_;
};

} // namespace ld

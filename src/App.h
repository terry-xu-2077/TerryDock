#pragma once

#include "Common.h"

#include "AppLauncher.h"
#include "Config.h"
#include "DockItem.h"
#include "DockLayout.h"
#include "DockRenderer.h"
#include "DockTransform.h"
#include "DockWindow.h"
#include "WindowPreview.h"
#include "IconLoader.h"
#include "ProcessMonitor.h"

#include <chrono>
#include <memory>
#include <vector>

namespace ld
{

/// Wires configuration, icons, layout, animation and the window together and
/// owns the message loop.
class App : public DockWindow::Host, public WindowPreview::Host
{
public:
    App() = default;

    int Run(HINSTANCE instance);

    // --- DockWindow::Host --------------------------------------------------
    bool HitTest(float x, float y) override;
    void OnMouseMove(float x, float y) override;
    void OnMouseButton(int button, bool down, float x, float y) override;
    void OnDpiChanged(int dpi) override;
    void OnAppBarChanged() override;
    void OnCommand(UINT id) override;
    void OnTrayNotify(LPARAM lParam) override;
    void OnDropFiles(HDROP drop) override;
    void OnExternalDragEnter(const std::vector<std::wstring>& paths,
                             float x, float y) override;
    void OnExternalDragMove(float x, float y) override;
    void OnExternalDragLeave() override;
    void OnExternalDrop(float x, float y) override;
    void OnAnimationTimer() override;
    void OnDestroy() override;
    void OnCloseRequested() override;

    // --- WindowPreview::Host -----------------------------------------------
    void OnPreviewWindowActivated(int row) override;

private:
    enum : UINT
    {
        kMenuOpen = 1,
        kMenuRemove = 2,
        kMenuAdd = 3,
        kMenuExit = 4,
        kMenuEdit = 5,
        kMenuSettings = 6,
        kMenuLanguageChinese = 7,
        kMenuLanguageEnglish = 8,
        kMenuAbout = 9,
        kMenuOpenFolder = 10,
        kMenuPinRunning = 11,
        kMenuAddRunningBase = 1000,

        /// Monitor entries start here; kMenuMonitorBase + index.
        kMenuMonitorBase = 50000,
    };

    /// One entry per attached display.
    struct MonitorTarget
    {
        HMONITOR handle = nullptr;
        std::wstring device;
        RECT rect{};
        bool primary = false;
    };

    /// One taskbar-style top-level window. Dynamic running items are keyed by
    /// HWND rather than executable path so multiple Explorer/Chrome windows
    /// remain distinct and clicking an icon can activate the exact window.
    struct RunningWindowInfo
    {
        HWND hwnd = nullptr;
        std::wstring path;
        std::wstring title;

        /// Windows taskbar identity for packaged/UWP apps. Unlike the host
        /// executable path this stays unique when ApplicationFrameHost.exe
        /// owns windows for several applications.
        std::wstring appUserModelId;

        /// Localized shell display name resolved from appUserModelId.
        std::wstring applicationName;
    };

    enum
    {
        kIdleIntervalMs = 50,
    };

    bool Initialize(HINSTANCE instance);
    void Shutdown();

    // --- data --------------------------------------------------------------
    void LoadItems();
    void RebuildItemPointers();
    void AddApplication(const std::wstring& path);
    void FinishExternalDrag(bool commit, float x, float y);
    void RemoveApplication(size_t index);
    void LaunchApplication(size_t index);
    void RefreshRunningApplications();
    void BeginShellPopupPlacement(bool search, float physicalX, float physicalY);
    void UpdateShellPopupPlacement();
    void SaveConfiguration() const;

    // --- geometry ----------------------------------------------------------
    void RebuildMetrics();
    void UpdateSurfaceAndGeometry();
    void RepositionWindow();
    void UpdateDockWindowPosition();
    void CheckFullscreen();
    void SetFullscreenVisibilityTarget(float target);
    void CheckDisplayChange();
    void DetectRefreshRate();

    // --- monitor -----------------------------------------------------------
    void RefreshMonitors();
    RECT CurrentWorkArea() const;
    void SelectMonitor(int index);
    void AppendMonitorMenu(HMENU menu);

    // --- loop --------------------------------------------------------------
    void Tick(double dt);
    void Render();
    void PollProcesses();
    void CheckPointer();
    float HoverStrength() const;
    void WakeAnimation();
    void RequestExit();

    // --- input -------------------------------------------------------------
    int IndexAtPoint(float x, float y) const;
    int WindowMenuRowAt(float physicalX, float physicalY) const;
    bool PointInWindowMenu(float physicalX, float physicalY) const;
    void ShowContextMenu(int index, float x, float y);
    void ShowTrayMenu();
    void HandleMenuCommand(UINT id);
    const wchar_t* UiText(const wchar_t* chinese) const;
    std::vector<RunningWindowInfo> FindRunningTaskbarWindows() const;
    std::vector<std::wstring> FindRunningTaskbarApplications() const;
    void AddRunningApplication(size_t index);
    std::vector<std::wstring> PickApplicationFiles();
    void FinishIconDrag(float x, float y);

    /// Draws a small "dock" glyph at runtime: no .ico resource to maintain.
    static HICON MakeTrayIcon(int size);

    /// Per application editor: name, target, arguments, icon.
    bool EditItem(size_t index);

    /// How the dock behaves: panel mode, icon size, spacing, magnification,
    /// plus the global plate defaults (shape only, never colours).
    bool ShowSettings();

    /// File picker with a custom filter and title.
    std::wstring PickFile(const wchar_t* title,
                          const COMDLG_FILTERSPEC* filters,
                          UINT filterCount);
    std::vector<std::wstring> PickFiles(const wchar_t* title,
                                        const COMDLG_FILTERSPEC* filters,
                                        UINT filterCount,
                                        bool allowMultiSelect);

    HINSTANCE instance_ = nullptr;

    /// Editor state, only valid while EditItem is running.
    struct ItemEditor
    {
        HWND dialog = nullptr;
        HWND nameEdit = nullptr;
        HWND pathEdit = nullptr;
        HWND argumentsEdit = nullptr;
        HWND iconEdit = nullptr;

        // Plate look for this one icon. The corner radius is deliberately
        // absent: rounded clipping follows the global backdrop.
        HWND enableBox = nullptr;
        HWND scaleSlider = nullptr;
        HWND scaleLabel = nullptr;
        HWND opacityBox = nullptr;
        HWND opacitySlider = nullptr;
        HWND opacityLabel = nullptr;

        /// Owner drawn colour preview buttons; clicking one opens the
        /// system colour picker.
        HWND topSwatch = nullptr;
        HWND topHex = nullptr;
        HWND secondBox = nullptr;
        HWND bottomSwatch = nullptr;
        HWND bottomHex = nullptr;
        HWND strokeOverrideBox = nullptr;
        HWND strokeSwatch = nullptr;
        HWND strokeHex = nullptr;
        HWND strokeOpacityBox = nullptr;
        HWND strokeOpacitySlider = nullptr;
        HWND strokeOpacityLabel = nullptr;

        COLORREF topColor = 0;
        COLORREF bottomColor = 0;
        COLORREF strokeColor = RGB(255, 255, 255);

        DockItem* item = nullptr;

        /// Plate look and icon source when the dialog opened. Live preview
        /// writes straight into the item so the dock repaints immediately;
        /// cancelling restores both snapshots.
        PlateStyle originalPlate;
        ComPtr<IWICBitmap> originalIconSource;

        std::wstring pendingIconPath;
        ComPtr<IWICBitmap> pendingIcon;

        bool saved = false;
        bool closed = false;
    };

    /// "Dock 设置" dialog. Single global settings window: dock behaviour
    /// plus the global plate shape defaults.
    struct SettingsEditor
    {
        struct ScrollChild
        {
            HWND hwnd = nullptr;
            int x = 0;
            int y = 0;
            int page = 0;
        };

        HWND dialog = nullptr;
        HWND modeCombo = nullptr;
        HWND edgeCombo = nullptr;
        HWND tabControl = nullptr;
        HWND pageViewport = nullptr;
        std::vector<ScrollChild> scrollChildren;
        int pageContentBottom[4]{};
        int pageScrollPosition = 0;
        HWND spacingSlider = nullptr;
        HWND spacingLabel = nullptr;
        HWND magnifySlider = nullptr;
        HWND magnifyLabel = nullptr;

        // Global plate defaults: shape only, never colours.
        HWND cornerSlider = nullptr;
        HWND cornerLabel = nullptr;
        HWND scaleSlider = nullptr;
        HWND scaleLabel = nullptr;
        HWND opacitySlider = nullptr;
        HWND opacityLabel = nullptr;
        HWND strokeWidthSlider = nullptr;
        HWND strokeWidthLabel = nullptr;
        HWND strokeOpacitySlider = nullptr;
        HWND strokeOpacityLabel = nullptr;
        HWND overallScaleSlider = nullptr;
        HWND overallScaleLabel = nullptr;
        HWND autoHideBox = nullptr;
        HWND autoHideDelaySlider = nullptr;
        HWND autoHideDelayLabel = nullptr;
        HWND autoHideSpeedSlider = nullptr;
        HWND autoHideSpeedLabel = nullptr;
        HWND dockCornerSlider = nullptr;
        HWND dockCornerLabel = nullptr;

        // Custom panel background gradient.
        HWND customBox = nullptr;
        HWND topSwatch = nullptr;
        HWND topHex = nullptr;
        HWND bottomSwatch = nullptr;
        HWND bottomHex = nullptr;
        HWND bgOpacitySlider = nullptr;
        HWND bgOpacityLabel = nullptr;
        HWND tooltipOpacitySlider = nullptr;
        HWND tooltipOpacityLabel = nullptr;
        HWND tooltipFadeSlider = nullptr;
        HWND tooltipFadeLabel = nullptr;
        HWND tooltipScaleSlider = nullptr;
        HWND tooltipScaleLabel = nullptr;
        HWND tooltipCornerSlider = nullptr;
        HWND tooltipCornerLabel = nullptr;

        COLORREF topColor = 0;
        COLORREF bottomColor = 0;

        /// Settings when the dialog opened; cancelling rolls back to these
        /// (the live preview writes straight into the running config).
        DockSettings original;

        bool closed = false;
        bool saved = false;
    };

    ItemEditor editor_;
    SettingsEditor settings_;

    static LRESULT CALLBACK EditorProc(HWND hwnd, UINT message,
                                       WPARAM wParam, LPARAM lParam);
    LRESULT HandleEditorMessage(HWND hwnd, UINT message,
                                WPARAM wParam, LPARAM lParam);

    static LRESULT CALLBACK SettingsProc(HWND hwnd, UINT message,
                                         WPARAM wParam, LPARAM lParam);
    LRESULT HandleSettingsMessage(HWND hwnd, UINT message,
                                  WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK SettingsViewportProc(HWND hwnd, UINT message,
                                                 WPARAM wParam, LPARAM lParam);
    LRESULT HandleSettingsViewportMessage(HWND hwnd, UINT message,
                                          WPARAM wParam, LPARAM lParam);
    void SetSettingsPageScroll(int position);

    /// Runs a nested modal loop for `window` until `closed` flips.
    void RunModal(HWND window, const bool& closed);

    /// Applies current settings to layout and persists them.
    void ApplySettings();

    DockConfig config_;
    LayoutMetrics metrics_;
    DockGeometry geometry_;
    DockTransform dockTransform_;
    LayoutFrame frame_;

    std::vector<MonitorTarget> monitors_;
    int monitorIndex_ = 0;

    std::vector<std::unique_ptr<DockItem>> items_;
    std::vector<DockItem*> pointers_;
    std::vector<std::wstring> menuRunningCandidates_;
    std::vector<HBITMAP> menuRunningBitmaps_;

    DockWindow window_;
    WindowPreview windowPreview_;
    DockRenderer renderer_;
    IconLoader icons_;

    bool mouseActive_ = false;
    float mouseX_ = 0.0f;
    float mouseY_ = 0.0f;
    float mousePhysicalX_ = 0.0f;
    float mousePhysicalY_ = 0.0f;

    int pressedIndex_ = -1;
    int draggingIndex_ = -1;
    float dragStartX_ = 0.0f;
    float dragStartY_ = 0.0f;

    /// Drag recognition is based on the physical screen pointer, never on the
    /// animated icon/dock geometry. This prevents hover/bounce/repositioning
    /// from turning a stationary click into a reorder drag.
    POINT dragStartScreen_{};

    float dragGrabX_ = 0.0f;
    float dragGrabY_ = 0.0f;
    float dragLift_ = 0.0f;
    float dragPlaceholderAmount_ = 0.0f;
    int dragTargetIndex_ = -1;
    int dragVisualTargetIndex_ = -1;
    int menuIndex_ = -1;

    bool shellPopupPlacementPending_ = false;
    bool shellPopupSearch_ = false;
    POINT shellPopupAnchorScreen_{};
    std::chrono::steady_clock::time_point shellPopupPlacementStarted_{};

    bool externalDragActive_ = false;
    std::vector<std::wstring> externalDragPaths_;
    std::chrono::steady_clock::time_point lastExternalDragFrame_{};
    float tooltipPresence_ = 0.0f;
    std::wstring tooltipItemId_;
    float tooltipOpacity_ = 0.0f;

    /// Hover-expanded window menu for apps that own multiple top-level windows.
    std::wstring windowMenuItemId_;
    float windowMenuLeaveElapsed_ = 0.0f;
    bool windowMenuVisible_ = false;
    int windowMenuHoveredRow_ = -1;
    int windowMenuPressedRow_ = -1;
    int windowMenuVisibleRows_ = 0;
    float windowMenuItemWidth_ = 0.0f;
    D2D1_RECT_F windowMenuBounds_{};

    /// Width of the panel in macOS "expand once" mode, animated.
    Spring panelWidth_;
    Spring fullscreenVisibility_;
    float hideAnimationStart_ = 1.0f;
    float hideAnimationTarget_ = 1.0f;
    double hideAnimationElapsed_ = 0.0;
    bool hideAnimationActive_ = false;
    Spring hideIndicatorVisibility_;
    bool fullscreenActive_ = false;
    int fullscreenCandidateChecks_ = 0;
    std::chrono::steady_clock::time_point hideDeadline_{};
    bool autoHideHidePending_ = false;
    bool settingsWindowOpen_ = false;

    /// True while a modal editor is up: the dock stops reacting to the mouse.
    bool modal_ = false;

    /// Eased pointer influence actually applied this frame (0..1).
    float hoverPresence_ = 0.0f;

    /// Cursor X that shapes the row (magnification centre and row drift).
    /// It tracks the real cursor while the pointer still has influence, and
    /// freezes at the last influential position once it leaves. Without the
    /// freeze, flicking the cursor sideways teleports the anchor to the
    /// clamped end of the row before the release starts, so the row jumps and
    /// then snaps back — while leaving vertically only fades and looks fine.
    float anchorX_ = 0.0f;

    // TEMP-DIAG-BEGIN
    float diagMouseX_ = -1.0f;
    // TEMP-DIAG-END

    bool animating_ = false;
    bool needsRender_ = true;
    bool running_ = false;
    bool exitRequested_ = false;

    float dpiScale_ = 1.0f;
    float dockScale_ = 1.0f;
    int frameIntervalMs_ = 16;

    RECT lastWorkArea_{};
    int lastVisibleTaskbarInset_ = -1;

    std::chrono::steady_clock::time_point lastFrame_;
    std::chrono::steady_clock::time_point lastPoll_;
    std::chrono::steady_clock::time_point lastDisplayCheck_;
    std::chrono::steady_clock::time_point lastFullscreenCheck_;
};

} // namespace ld

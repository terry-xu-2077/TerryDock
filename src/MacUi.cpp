#include "MacUi.h"

#include <commctrl.h>
#include <dwmapi.h>

namespace ld::ui
{

namespace
{

// ---------------------------------------------------------------------------
// Shared factories
// ---------------------------------------------------------------------------

ID2D1Factory* D2DFactory()
{
    static ComPtr<ID2D1Factory> factory;
    if (!factory)
    {
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                          __uuidof(ID2D1Factory), nullptr,
                          reinterpret_cast<void**>(factory.AddressOf()));
    }
    return factory.Get();
}

IDWriteFactory* DWFactory()
{
    static ComPtr<IDWriteFactory> factory;
    if (!factory)
    {
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(factory.AddressOf()));
    }
    return factory.Get();
}

IWICImagingFactory* WicFactory()
{
    static ComPtr<IWICImagingFactory> factory;
    if (!factory)
    {
        CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                         CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    }
    return factory.Get();
}

const wchar_t* UserLocale()
{
    static wchar_t locale[LOCALE_NAME_MAX_LENGTH] = {};
    if (!locale[0]
        && GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH) == 0)
    {
        wcscpy(locale, L"en-us");
    }
    return locale;
}

/// Segoe Fluent Icons on Windows 11, Segoe MDL2 Assets on Windows 10.
const wchar_t* IconFontFamily()
{
    static const wchar_t* family = []() -> const wchar_t*
    {
        ComPtr<IDWriteFontCollection> fonts;
        if (DWFactory()
            && SUCCEEDED(DWFactory()->GetSystemFontCollection(&fonts, FALSE)))
        {
            UINT32 index = 0;
            BOOL exists = FALSE;
            fonts->FindFamilyName(L"Segoe Fluent Icons", &index, &exists);
            if (exists)
            {
                return L"Segoe Fluent Icons";
            }
        }
        return L"Segoe MDL2 Assets";
    }();
    return family;
}

ComPtr<IDWriteTextFormat> MakeFormat(float size, DWRITE_FONT_WEIGHT weight,
                                     bool wrap = false,
                                     const wchar_t* family = L"Segoe UI")
{
    ComPtr<IDWriteTextFormat> format;
    if (!DWFactory()
        || FAILED(DWFactory()->CreateTextFormat(
            family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, UserLocale(), &format)))
    {
        return format;
    }

    format->SetParagraphAlignment(wrap ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR
                                       : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP
                                 : DWRITE_WORD_WRAPPING_NO_WRAP);
    if (!wrap)
    {
        DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> ellipsis;
        DWFactory()->CreateEllipsisTrimmingSign(format.Get(), &ellipsis);
        format->SetTrimming(&trimming, ellipsis.Get());
    }
    return format;
}

ID2D1StrokeStyle* RoundStroke()
{
    static ComPtr<ID2D1StrokeStyle> style;
    if (!style && D2DFactory())
    {
        D2DFactory()->CreateStrokeStyle(
            D2D1::StrokeStyleProperties(
                D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
            nullptr, 0, &style);
    }
    return style.Get();
}

int DpiForMonitor(HMONITOR monitor)
{
    using Fn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static Fn fn = []() -> Fn
    {
        HMODULE shcore = LoadLibraryW(L"shcore.dll");
        return shcore ? reinterpret_cast<Fn>(
                            GetProcAddress(shcore, "GetDpiForMonitor"))
                      : nullptr;
    }();

    UINT x = 96, y = 96;
    if (fn && monitor && SUCCEEDED(fn(monitor, 0, &x, &y)) && x > 0)
    {
        return static_cast<int>(x);
    }

    HDC dc = GetDC(nullptr);
    const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    ReleaseDC(nullptr, dc);
    return dpi > 0 ? dpi : 96;
}

int DpiForWindow(HWND hwnd)
{
    using Fn = UINT(WINAPI*)(HWND);
    static Fn fn = []() -> Fn
    {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        return user32 ? reinterpret_cast<Fn>(
                            GetProcAddress(user32, "GetDpiForWindow"))
                      : nullptr;
    }();
    if (fn && hwnd)
    {
        const UINT dpi = fn(hwnd);
        if (dpi > 0)
        {
            return static_cast<int>(dpi);
        }
    }
    return DpiForMonitor(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST));
}

void AdjustForDpi(RECT& rect, DWORD style, DWORD exStyle, int dpi)
{
    using Fn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static Fn fn = []() -> Fn
    {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        return user32 ? reinterpret_cast<Fn>(
                            GetProcAddress(user32, "AdjustWindowRectExForDpi"))
                      : nullptr;
    }();
    if (fn)
    {
        fn(&rect, style, FALSE, exStyle, static_cast<UINT>(dpi));
    }
    else
    {
        AdjustWindowRectEx(&rect, style, FALSE, exStyle);
    }
}

constexpr D2D1_COLOR_F Rgb(int r, int g, int b, float a = 1.0f)
{
    return D2D1_COLOR_F{r / 255.0f, g / 255.0f, b / 255.0f, a};
}

D2D1_COLOR_F Fade(D2D1_COLOR_F color, float alpha)
{
    color.a *= alpha;
    return color;
}

D2D1_COLOR_F Mix(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t)
{
    return D2D1_COLOR_F{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                        a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

D2D1_COLOR_F FromColorRef(COLORREF color)
{
    return Rgb(GetRValue(color), GetGValue(color), GetBValue(color));
}

COLORREF ToColorRef(const D2D1_COLOR_F& color)
{
    return RGB(static_cast<int>(color.r * 255.0f + 0.5f),
               static_cast<int>(color.g * 255.0f + 0.5f),
               static_cast<int>(color.b * 255.0f + 0.5f));
}

D2D1_RECT_F Inflate(const D2D1_RECT_F& rect, float dx, float dy)
{
    return D2D1::RectF(rect.left - dx, rect.top - dy,
                       rect.right + dx, rect.bottom + dy);
}

D2D1_RECT_F Offset(const D2D1_RECT_F& rect, float dx, float dy)
{
    return D2D1::RectF(rect.left + dx, rect.top + dy,
                       rect.right + dx, rect.bottom + dy);
}

bool Contains(const D2D1_RECT_F& rect, float x, float y)
{
    return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
}

float Width(const D2D1_RECT_F& rect) { return rect.right - rect.left; }
float Height(const D2D1_RECT_F& rect) { return rect.bottom - rect.top; }

/// Simple premultiplied 32bpp DIB + memory DC.
struct Surface
{
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ old = nullptr;
    int width = 0;
    int height = 0;

    bool Resize(int w, int h)
    {
        if (dc && w == width && h == height)
        {
            return true;
        }
        Release();

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = w;
        info.bmiHeader.biHeight = -h;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        dc = CreateCompatibleDC(nullptr);
        bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits,
                                  nullptr, 0);
        if (!dc || !bitmap)
        {
            Release();
            return false;
        }
        old = SelectObject(dc, bitmap);
        width = w;
        height = h;
        return true;
    }

    void Release()
    {
        if (dc && old)
        {
            SelectObject(dc, old);
        }
        if (bitmap)
        {
            DeleteObject(bitmap);
        }
        if (dc)
        {
            DeleteDC(dc);
        }
        dc = nullptr;
        bitmap = nullptr;
        old = nullptr;
        width = height = 0;
    }
};

void FillRoundRect(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush,
                   const D2D1_RECT_F& rect, float radius,
                   const D2D1_COLOR_F& color)
{
    brush->SetColor(color);
    rt->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush);
}

/// Cheap soft shadow: stacked, slightly growing translucent round rects.
void SoftShadow(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush,
                const D2D1_RECT_F& rect, float radius, float spread,
                float offsetY, float strength)
{
    const int steps = std::max(2, static_cast<int>(spread));
    for (int i = steps; i >= 1; --i)
    {
        const float grow = static_cast<float>(i);
        FillRoundRect(rt, brush,
                      Offset(Inflate(rect, grow, grow), 0.0f, offsetY),
                      radius + grow, D2D1::ColorF(0, 0, 0,
                          strength / static_cast<float>(steps)));
    }
}

void DrawCheck(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush,
               float cx, float cy, float size, const D2D1_COLOR_F& color,
               float width)
{
    brush->SetColor(color);
    const float s = size / 2.0f;
    rt->DrawLine(D2D1::Point2F(cx - s, cy + 0.05f * size),
                 D2D1::Point2F(cx - s * 0.25f, cy + s * 0.75f),
                 brush, width, RoundStroke());
    rt->DrawLine(D2D1::Point2F(cx - s * 0.25f, cy + s * 0.75f),
                 D2D1::Point2F(cx + s, cy - s * 0.8f),
                 brush, width, RoundStroke());
}

void DrawChevron(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush,
                 float cx, float cy, float size, int direction,
                 const D2D1_COLOR_F& color, float width)
{
    // direction: 0 right, 1 up, 2 down
    brush->SetColor(color);
    const float s = size / 2.0f;
    D2D1_POINT_2F a, b, c;
    if (direction == 0)
    {
        a = D2D1::Point2F(cx - s * 0.5f, cy - s);
        b = D2D1::Point2F(cx + s * 0.5f, cy);
        c = D2D1::Point2F(cx - s * 0.5f, cy + s);
    }
    else if (direction == 1)
    {
        a = D2D1::Point2F(cx - s, cy + s * 0.5f);
        b = D2D1::Point2F(cx, cy - s * 0.5f);
        c = D2D1::Point2F(cx + s, cy + s * 0.5f);
    }
    else
    {
        a = D2D1::Point2F(cx - s, cy - s * 0.5f);
        b = D2D1::Point2F(cx, cy + s * 0.5f);
        c = D2D1::Point2F(cx + s, cy - s * 0.5f);
    }
    rt->DrawLine(a, b, brush, width, RoundStroke());
    rt->DrawLine(b, c, brush, width, RoundStroke());
}

Palette MakeLight()
{
    Palette p;
    p.dark = false;
    p.windowBg = Rgb(242, 242, 245);
    p.sidebarBg = Rgb(232, 232, 236);
    p.hairline = Rgb(0, 0, 0, 0.10f);
    p.card = Rgb(255, 255, 255);
    p.cardBorder = Rgb(0, 0, 0, 0.07f);
    p.separator = Rgb(0, 0, 0, 0.08f);
    p.text = Rgb(29, 29, 31);
    p.secondaryText = Rgb(110, 110, 115);
    p.tertiaryText = Rgb(160, 160, 166);
    p.accent = Rgb(0, 122, 255);
    p.accentPressed = Rgb(0, 98, 214);
    p.onAccent = Rgb(255, 255, 255);
    p.controlBg = Rgb(255, 255, 255);
    p.controlPressed = Rgb(232, 232, 236);
    p.controlBorder = Rgb(0, 0, 0, 0.14f);
    p.shadow = Rgb(0, 0, 0, 0.08f);
    p.fieldBg = Rgb(255, 255, 255);
    p.fieldBorder = Rgb(0, 0, 0, 0.16f);
    p.fill = Rgb(120, 120, 128, 0.20f);
    p.knob = Rgb(255, 255, 255);
    p.hover = Rgb(0, 0, 0, 0.06f);
    p.segmentSelected = Rgb(255, 255, 255);

    p.menuBg = Rgb(248, 248, 250, 0.985f);
    p.menuBorder = Rgb(0, 0, 0, 0.16f);
    p.menuInnerBorder = Rgb(255, 255, 255, 0.0f);
    p.menuSeparator = Rgb(0, 0, 0, 0.10f);
    p.menuText = Rgb(29, 29, 31);
    p.menuDisabledText = Rgb(0, 0, 0, 0.28f);
    p.menuHeaderText = Rgb(0, 0, 0, 0.45f);
    return p;
}

Palette MakeDark()
{
    Palette p;
    p.dark = true;
    p.windowBg = Rgb(30, 30, 32);
    p.sidebarBg = Rgb(38, 38, 41);
    p.hairline = Rgb(0, 0, 0, 0.55f);
    p.card = Rgb(44, 44, 47);
    p.cardBorder = Rgb(255, 255, 255, 0.06f);
    p.separator = Rgb(255, 255, 255, 0.08f);
    p.text = Rgb(236, 236, 240);
    p.secondaryText = Rgb(152, 152, 158);
    p.tertiaryText = Rgb(110, 110, 116);
    p.accent = Rgb(10, 132, 255);
    p.accentPressed = Rgb(0, 106, 220);
    p.onAccent = Rgb(255, 255, 255);
    p.controlBg = Rgb(78, 78, 82);
    p.controlPressed = Rgb(98, 98, 103);
    p.controlBorder = Rgb(255, 255, 255, 0.08f);
    p.shadow = Rgb(0, 0, 0, 0.30f);
    p.fieldBg = Rgb(32, 32, 34);
    p.fieldBorder = Rgb(255, 255, 255, 0.14f);
    p.fill = Rgb(120, 120, 128, 0.36f);
    p.knob = Rgb(236, 236, 240);
    p.hover = Rgb(255, 255, 255, 0.07f);
    p.segmentSelected = Rgb(99, 99, 104);

    p.menuBg = Rgb(42, 42, 45, 0.985f);
    p.menuBorder = Rgb(0, 0, 0, 0.70f);
    p.menuInnerBorder = Rgb(255, 255, 255, 0.13f);
    p.menuSeparator = Rgb(255, 255, 255, 0.12f);
    p.menuText = Rgb(236, 236, 240);
    p.menuDisabledText = Rgb(255, 255, 255, 0.28f);
    p.menuHeaderText = Rgb(255, 255, 255, 0.45f);
    return p;
}

} // namespace

// ---------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------

bool SystemUsesDarkTheme()
{
    DWORD value = 1;
    DWORD size = sizeof(value);
    if (RegGetValueW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size)
        != ERROR_SUCCESS)
    {
        return false;
    }
    return value == 0;
}

const Palette& GetPalette(bool dark)
{
    static const Palette light = MakeLight();
    static const Palette darkPalette = MakeDark();
    return dark ? darkPalette : light;
}

void ApplyWindowChrome(HWND hwnd, const Palette& palette)
{
    if (!hwnd)
    {
        return;
    }

    const BOOL dark = palette.dark ? TRUE : FALSE;
    // 20 = DWMWA_USE_IMMERSIVE_DARK_MODE (19 on early Windows 10 builds).
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark))))
    {
        DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
    }

    // 35 = DWMWA_CAPTION_COLOR, 36 = DWMWA_TEXT_COLOR (Windows 11).
    const COLORREF caption = ToColorRef(palette.windowBg);
    DwmSetWindowAttribute(hwnd, 35, &caption, sizeof(caption));
    const COLORREF text = ToColorRef(palette.text);
    DwmSetWindowAttribute(hwnd, 36, &text, sizeof(text));
}

ComPtr<ID2D1Bitmap> CreateBitmapFromWic(ID2D1RenderTarget* target,
                                        IWICBitmapSource* source)
{
    ComPtr<ID2D1Bitmap> bitmap;
    if (!target || !source || !WicFactory())
    {
        return bitmap;
    }

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(WicFactory()->CreateFormatConverter(&converter))
        || FAILED(converter->Initialize(
            source, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
            nullptr, 0.0, WICBitmapPaletteTypeCustom)))
    {
        return bitmap;
    }

    target->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &bitmap);
    return bitmap;
}

// ---------------------------------------------------------------------------
// Menu
// ---------------------------------------------------------------------------

MenuItem MenuItem::Command(UINT id, std::wstring text, bool enabled,
                           bool checked)
{
    MenuItem item;
    item.id = id;
    item.text = std::move(text);
    item.enabled = enabled;
    item.checked = checked;
    return item;
}

MenuItem MenuItem::Submenu(std::wstring text, std::vector<MenuItem> children)
{
    MenuItem item;
    item.text = std::move(text);
    item.children = std::move(children);
    item.enabled = !item.children.empty();
    return item;
}

MenuItem MenuItem::Separator()
{
    MenuItem item;
    item.type = Type::Separator;
    item.enabled = false;
    return item;
}

MenuItem MenuItem::Header(std::wstring text)
{
    MenuItem item;
    item.type = Type::Header;
    item.text = std::move(text);
    item.enabled = false;
    return item;
}

namespace
{

constexpr float kMenuMargin = 14.0f;   // shadow area around the body
constexpr float kMenuPad = 5.0f;
constexpr float kMenuItemH = 24.0f;
constexpr float kMenuSepH = 9.0f;
constexpr float kMenuHeaderH = 22.0f;
constexpr float kMenuRadius = 9.0f;
constexpr float kMenuItemRadius = 5.0f;
constexpr wchar_t kMenuClass[] = L"LightDock_MacMenu";

LRESULT CALLBACK MenuWindowProc(HWND hwnd, UINT message, WPARAM wParam,
                                LPARAM lParam)
{
    switch (message)
    {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        return TRUE;
    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

bool Selectable(const MenuItem& item)
{
    return item.type == MenuItem::Type::Command && item.enabled;
}

class MenuSession
{
public:
    MenuSession(HWND owner, const MenuOptions& options)
        : owner_(owner), options_(options)
    {
        dark_ = SystemUsesDarkTheme();
        palette_ = &GetPalette(dark_);
        text_ = MakeFormat(13.0f, DWRITE_FONT_WEIGHT_NORMAL);
        header_ = MakeFormat(11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    }

    ~MenuSession()
    {
        while (!levels_.empty())
        {
            PopLevel();
        }
    }

    UINT Run(const std::vector<MenuItem>& items, POINT anchor)
    {
        RegisterClassOnce();
        if (!OpenLevel(items, anchor, -1, nullptr))
        {
            return 0;
        }

        ReleaseCapture();
        const HWND initialForeground = GetForegroundWindow();
        const DWORD thread = GetCurrentThreadId();
        bool buttonWasDown = AnyButtonDown();
        POINT lastCursor{-100000, -100000};

        while (!done_)
        {
            MsgWaitForMultipleObjectsEx(0, nullptr, 15, QS_ALLINPUT,
                                        MWMO_INPUTAVAILABLE);
            MSG message{};
            while (!done_ && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                if (message.message == WM_QUIT)
                {
                    PostQuitMessage(static_cast<int>(message.wParam));
                    Finish(0);
                    break;
                }

                if ((message.message >= WM_MOUSEFIRST
                     && message.message <= WM_MOUSELAST)
                    || (message.message >= WM_NCMOUSEMOVE
                        && message.message <= WM_NCXBUTTONDBLCLK))
                {
                    HandleMouse(message);
                    continue;
                }

                if (message.message == WM_KEYDOWN
                    || message.message == WM_SYSKEYDOWN)
                {
                    HandleKey(message.wParam);
                    continue;
                }

                if (message.message == WM_KEYUP || message.message == WM_CHAR
                    || message.message == WM_SYSKEYUP
                    || message.message == WM_SYSCHAR)
                {
                    continue;
                }

                TranslateMessage(&message);
                DispatchMessageW(&message);
            }

            if (done_)
            {
                break;
            }

            POINT cursor{};
            GetCursorPos(&cursor);
            if (cursor.x != lastCursor.x || cursor.y != lastCursor.y)
            {
                lastCursor = cursor;
                HoverAt(cursor);
            }

            // Clicks in other applications never reach our queue.
            const bool down = AnyButtonDown();
            if (down && !buttonWasDown && LevelAt(cursor) < 0)
            {
                Finish(0);
                break;
            }
            buttonWasDown = down;

            const HWND foreground = GetForegroundWindow();
            if (foreground && foreground != initialForeground
                && GetWindowThreadProcessId(foreground, nullptr) != thread)
            {
                Finish(0);
                break;
            }

            ProcessPending();
            ProcessFades();

            if (options_.idle)
            {
                options_.idle();
            }
        }

        while (!levels_.empty())
        {
            PopLevel();
        }
        return result_;
    }

private:
    struct Level
    {
        const std::vector<MenuItem>* items = nullptr;
        HWND hwnd = nullptr;
        float scale = 1.0f;
        POINT position{};       // window top-left (physical)
        SIZE size{};            // window size (physical)
        RECT body{};            // body in screen pixels
        float bodyW = 0.0f;     // DIPs
        float bodyH = 0.0f;
        float textLeft = 0.0f;
        bool hasCheck = false;
        bool hasIcon = false;
        bool hasChildren = false;
        std::vector<D2D1_RECT_F> rects;   // relative to body (DIPs)
        std::vector<ComPtr<ID2D1Bitmap>> icons;
        int hover = -1;
        int childFor = -1;
        ULONGLONG shownAt = 0;
        BYTE alpha = 0;
        Surface surface;
        ComPtr<ID2D1DCRenderTarget> target;
        ComPtr<ID2D1SolidColorBrush> brush;
    };

    struct Pending
    {
        bool active = false;
        int level = -1;
        int index = -1;
        ULONGLONG at = 0;
    };

    static bool AnyButtonDown()
    {
        return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0
            || (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0
            || (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
    }

    static void RegisterClassOnce()
    {
        static bool registered = false;
        if (registered)
        {
            return;
        }
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = MenuWindowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = kMenuClass;
        RegisterClassExW(&windowClass);
        registered = true;
    }

    float Measure(const std::wstring& text, IDWriteTextFormat* format) const
    {
        ComPtr<IDWriteTextLayout> layout;
        if (!format || FAILED(DWFactory()->CreateTextLayout(
                text.c_str(), static_cast<UINT32>(text.size()), format,
                4000.0f, 100.0f, &layout)))
        {
            return 0.0f;
        }
        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);
        return metrics.widthIncludingTrailingWhitespace;
    }

    bool OpenLevel(const std::vector<MenuItem>& items, POINT anchor,
                   int parentIndex, const Level* parent)
    {
        auto level = std::make_unique<Level>();
        level->items = &items;

        HMONITOR monitor = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        GetMonitorInfoW(monitor, &info);
        const RECT bounds = info.rcMonitor;
        level->scale = static_cast<float>(DpiForMonitor(monitor)) / 96.0f;
        const float s = level->scale;

        // --- measure ------------------------------------------------------
        float textW = 0.0f;
        for (const MenuItem& item : items)
        {
            if (item.type == MenuItem::Type::Command)
            {
                textW = std::max(textW, Measure(item.text, text_.Get()));
                level->hasCheck = level->hasCheck || item.checked;
                level->hasIcon = level->hasIcon || static_cast<bool>(item.icon);
                level->hasChildren =
                    level->hasChildren || !item.children.empty();
            }
            else if (item.type == MenuItem::Type::Header)
            {
                textW = std::max(textW, Measure(item.text, header_.Get()));
            }
        }

        level->textLeft = kMenuPad + 10.0f
            + (level->hasCheck ? 16.0f : 0.0f)
            + (level->hasIcon ? 24.0f : 0.0f);
        float width = level->textLeft + std::ceil(textW) + 28.0f
            + (level->hasChildren ? 14.0f : 0.0f) + kMenuPad;
        width = std::max(width, 168.0f);
        if (!parent && options_.minWidth > 0)
        {
            width = std::max(width, options_.minWidth / s + 2.0f * kMenuPad);
        }
        width = std::min(width, 520.0f);

        float y = kMenuPad;
        for (const MenuItem& item : items)
        {
            const float h = item.type == MenuItem::Type::Separator ? kMenuSepH
                : item.type == MenuItem::Type::Header ? kMenuHeaderH
                                                      : kMenuItemH;
            level->rects.push_back(D2D1::RectF(kMenuPad, y, width - kMenuPad,
                                               y + h));
            y += h;
        }
        level->bodyW = width;
        level->bodyH = y + kMenuPad;

        const int bodyWpx = static_cast<int>(std::ceil(level->bodyW * s));
        const int bodyHpx = static_cast<int>(std::ceil(level->bodyH * s));
        const int margin = static_cast<int>(std::ceil(kMenuMargin * s));

        // --- position (body, screen pixels) --------------------------------
        int x = anchor.x;
        int top = anchor.y;
        if (parent)
        {
            const D2D1_RECT_F& item = parent->rects[parentIndex];
            x = parent->body.right - static_cast<int>(3 * s);
            top = parent->body.top
                + static_cast<int>(std::lround((item.top - kMenuPad) * parent->scale));
            if (x + bodyWpx > bounds.right)
            {
                x = parent->body.left - bodyWpx + static_cast<int>(3 * s);
            }
        }
        else
        {
            if (options_.alignItem >= 0
                && options_.alignItem < static_cast<int>(items.size()))
            {
                const D2D1_RECT_F& item = level->rects[options_.alignItem];
                const float itemCenter = (item.top + item.bottom) * 0.5f * s;
                top = anchor.y + options_.anchorHeight / 2
                    - static_cast<int>(std::lround(itemCenter));
                x = anchor.x - static_cast<int>(std::lround(
                    (level->textLeft - 10.0f) * s));
            }
            else
            {
                if (options_.alignRight)
                {
                    x = anchor.x - bodyWpx;
                }
                if (options_.alignBottom)
                {
                    top = anchor.y - bodyHpx;
                }
                if (x + bodyWpx > bounds.right)
                {
                    x = anchor.x - bodyWpx;
                }
                if (top + bodyHpx > bounds.bottom)
                {
                    top = anchor.y - bodyHpx;
                }
            }
        }

        x = std::clamp(x, static_cast<int>(bounds.left),
                       std::max(static_cast<int>(bounds.left),
                                static_cast<int>(bounds.right) - bodyWpx));
        top = std::clamp(top, static_cast<int>(bounds.top),
                         std::max(static_cast<int>(bounds.top),
                                  static_cast<int>(bounds.bottom) - bodyHpx));

        level->body = RECT{x, top, x + bodyWpx, top + bodyHpx};
        level->position = POINT{x - margin, top - margin};
        level->size = SIZE{bodyWpx + 2 * margin, bodyHpx + 2 * margin};

        level->hwnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
            kMenuClass, L"", WS_POPUP,
            level->position.x, level->position.y,
            level->size.cx, level->size.cy,
            owner_, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!level->hwnd || !level->surface.Resize(level->size.cx,
                                                   level->size.cy))
        {
            if (level->hwnd)
            {
                DestroyWindow(level->hwnd);
            }
            level->surface.Release();
            return false;
        }

        const D2D1_RENDER_TARGET_PROPERTIES properties =
            D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                  D2D1_ALPHA_MODE_PREMULTIPLIED),
                96.0f * s, 96.0f * s);
        if (FAILED(D2DFactory()->CreateDCRenderTarget(&properties,
                                                      &level->target))
            || FAILED(level->target->CreateSolidColorBrush(
                D2D1::ColorF(0, 0, 0), &level->brush)))
        {
            DestroyWindow(level->hwnd);
            level->surface.Release();
            return false;
        }
        level->target->SetTextAntialiasMode(
            D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

        level->icons.resize(items.size());
        for (size_t i = 0; i < items.size(); ++i)
        {
            if (items[i].icon)
            {
                level->icons[i] = CreateBitmapFromWic(level->target.Get(),
                                                      items[i].icon.Get());
            }
        }

        level->shownAt = GetTickCount64();
        level->alpha = 0;
        levels_.push_back(std::move(level));
        Render(*levels_.back());
        ShowWindow(levels_.back()->hwnd, SW_SHOWNOACTIVATE);
        return true;
    }

    void PopLevel()
    {
        if (levels_.empty())
        {
            return;
        }
        Level& level = *levels_.back();
        if (level.hwnd)
        {
            DestroyWindow(level.hwnd);
        }
        level.target.Reset();
        level.surface.Release();
        levels_.pop_back();
        if (!levels_.empty())
        {
            levels_.back()->childFor = -1;
        }
    }

    void CloseAbove(int level)
    {
        while (static_cast<int>(levels_.size()) > level + 1)
        {
            PopLevel();
        }
    }

    void Render(Level& level)
    {
        ID2D1DCRenderTarget* rt = level.target.Get();
        ID2D1SolidColorBrush* brush = level.brush.Get();
        const Palette& p = *palette_;

        RECT bind{0, 0, level.size.cx, level.size.cy};
        if (FAILED(rt->BindDC(level.surface.dc, &bind)))
        {
            return;
        }

        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(0, 0, 0, 0));

        const float s = level.scale;
        const float m = std::ceil(kMenuMargin * s) / s;
        rt->SetTransform(D2D1::Matrix3x2F::Translation(m, m));

        const D2D1_RECT_F body = D2D1::RectF(0, 0, level.bodyW, level.bodyH);
        SoftShadow(rt, brush, body, kMenuRadius, 12.0f, 4.0f,
                   p.dark ? 0.55f : 0.20f);
        FillRoundRect(rt, brush, body, kMenuRadius, p.menuBg);

        brush->SetColor(p.menuBorder);
        rt->DrawRoundedRectangle(
            D2D1::RoundedRect(Inflate(body, -0.5f / s, -0.5f / s),
                              kMenuRadius, kMenuRadius),
            brush, 1.0f / s);
        if (p.menuInnerBorder.a > 0.0f)
        {
            brush->SetColor(p.menuInnerBorder);
            rt->DrawRoundedRectangle(
                D2D1::RoundedRect(Inflate(body, -1.5f / s, -1.5f / s),
                                  kMenuRadius - 1.0f, kMenuRadius - 1.0f),
                brush, 1.0f / s);
        }

        const std::vector<MenuItem>& items = *level.items;
        for (size_t i = 0; i < items.size(); ++i)
        {
            const MenuItem& item = items[i];
            const D2D1_RECT_F& r = level.rects[i];
            const float cy = (r.top + r.bottom) * 0.5f;

            if (item.type == MenuItem::Type::Separator)
            {
                brush->SetColor(p.menuSeparator);
                rt->FillRectangle(D2D1::RectF(r.left + 9.0f, cy - 0.5f / s,
                                              r.right - 9.0f, cy + 0.5f / s),
                                  brush);
                continue;
            }

            if (item.type == MenuItem::Type::Header)
            {
                brush->SetColor(p.menuHeaderText);
                rt->DrawText(item.text.c_str(),
                             static_cast<UINT32>(item.text.size()),
                             header_.Get(),
                             D2D1::RectF(r.left + 9.0f, r.top + 2.0f,
                                         r.right - 9.0f, r.bottom),
                             brush);
                continue;
            }

            const bool highlighted = item.enabled
                && (level.hover == static_cast<int>(i)
                    || level.childFor == static_cast<int>(i));
            if (highlighted)
            {
                FillRoundRect(rt, brush, r, kMenuItemRadius, p.accent);
            }

            const D2D1_COLOR_F color = !item.enabled ? p.menuDisabledText
                : highlighted ? p.onAccent : p.menuText;

            float x = r.left + 10.0f;
            if (level.hasCheck)
            {
                if (item.checked)
                {
                    DrawCheck(rt, brush, x + 5.0f, cy, 9.0f, color, 1.6f);
                }
                x += 16.0f;
            }
            if (level.hasIcon)
            {
                if (level.icons[i])
                {
                    rt->DrawBitmap(level.icons[i].Get(),
                                   D2D1::RectF(x, cy - 8.0f, x + 16.0f,
                                               cy + 8.0f),
                                   item.enabled ? 1.0f : 0.4f);
                }
                x += 24.0f;
            }

            brush->SetColor(color);
            const float right = r.right - (level.hasChildren ? 22.0f : 10.0f);
            rt->DrawText(item.text.c_str(),
                         static_cast<UINT32>(item.text.size()), text_.Get(),
                         D2D1::RectF(x, r.top, right, r.bottom), brush,
                         D2D1_DRAW_TEXT_OPTIONS_CLIP);

            if (!item.children.empty())
            {
                DrawChevron(rt, brush, r.right - 11.0f, cy, 8.0f, 0, color,
                            1.5f);
            }
        }

        if (FAILED(rt->EndDraw()))
        {
            return;
        }

        Present(level);
    }

    void Present(Level& level)
    {
        BLENDFUNCTION blend{AC_SRC_OVER, 0, level.alpha, AC_SRC_ALPHA};
        POINT source{0, 0};
        HDC screen = GetDC(nullptr);
        UpdateLayeredWindow(level.hwnd, screen, &level.position, &level.size,
                            level.surface.dc, &source, 0, &blend, ULW_ALPHA);
        ReleaseDC(nullptr, screen);
    }

    void ProcessFades()
    {
        const ULONGLONG now = GetTickCount64();
        for (auto& level : levels_)
        {
            if (level->alpha == 255)
            {
                continue;
            }
            const float t = std::min(
                1.0f, static_cast<float>(now - level->shownAt) / 110.0f);
            const float eased = 1.0f - (1.0f - t) * (1.0f - t);
            level->alpha = static_cast<BYTE>(std::lround(eased * 255.0f));
            BLENDFUNCTION blend{AC_SRC_OVER, 0, level->alpha, AC_SRC_ALPHA};
            UpdateLayeredWindow(level->hwnd, nullptr, nullptr, nullptr,
                                nullptr, nullptr, 0, &blend, ULW_ALPHA);
        }
    }

    int LevelAt(POINT point) const
    {
        for (int i = static_cast<int>(levels_.size()) - 1; i >= 0; --i)
        {
            if (PtInRect(&levels_[i]->body, point))
            {
                return i;
            }
        }
        return -1;
    }

    int ItemAt(int levelIndex, POINT point) const
    {
        const Level& level = *levels_[levelIndex];
        const float x = (point.x - level.body.left) / level.scale;
        const float y = (point.y - level.body.top) / level.scale;
        for (size_t i = 0; i < level.rects.size(); ++i)
        {
            if (Contains(level.rects[i], x, y)
                || (y >= level.rects[i].top && y < level.rects[i].bottom
                    && x >= 0 && x < level.bodyW))
            {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    void SetHover(int levelIndex, int index)
    {
        Level& level = *levels_[levelIndex];
        if (index >= 0 && !Selectable((*level.items)[index]))
        {
            index = -1;
        }
        if (level.hover == index)
        {
            return;
        }
        level.hover = index;
        Render(level);
    }

    void HoverAt(POINT point)
    {
        const int levelIndex = LevelAt(point);
        if (levelIndex < 0)
        {
            Level& deepest = *levels_.back();
            if (deepest.childFor < 0 && deepest.hover >= 0)
            {
                deepest.hover = -1;
                Render(deepest);
            }
            return;
        }

        // Arriving in a submenu cancels the pending close of that submenu.
        if (pendingClose_.active && levelIndex > pendingClose_.level)
        {
            pendingClose_.active = false;
        }
        if (levelIndex > 0)
        {
            Level& parent = *levels_[levelIndex - 1];
            if (parent.hover != parent.childFor)
            {
                parent.hover = parent.childFor;
                Render(parent);
            }
        }

        const int index = ItemAt(levelIndex, point);
        Level& level = *levels_[levelIndex];
        const int previous = level.hover;
        SetHover(levelIndex, index);
        if (level.hover == previous)
        {
            return;
        }

        const ULONGLONG now = GetTickCount64();
        if (level.childFor >= 0 && level.childFor != level.hover)
        {
            pendingClose_ = Pending{true, levelIndex, -1, now + 220};
        }

        pendingOpen_.active = false;
        if (level.hover >= 0 && level.childFor != level.hover
            && !(*level.items)[level.hover].children.empty())
        {
            pendingOpen_ = Pending{true, levelIndex, level.hover,
                                   now + (level.childFor >= 0 ? 220 : 90)};
        }
    }

    void ProcessPending()
    {
        const ULONGLONG now = GetTickCount64();
        if (pendingClose_.active && now >= pendingClose_.at)
        {
            pendingClose_.active = false;
            if (pendingClose_.level < static_cast<int>(levels_.size()))
            {
                CloseAbove(pendingClose_.level);
                Render(*levels_[pendingClose_.level]);
            }
        }
        if (pendingOpen_.active && now >= pendingOpen_.at)
        {
            pendingOpen_.active = false;
            if (pendingOpen_.level < static_cast<int>(levels_.size())
                && levels_[pendingOpen_.level]->hover == pendingOpen_.index)
            {
                OpenChild(pendingOpen_.level, pendingOpen_.index, false);
            }
        }
    }

    void OpenChild(int levelIndex, int index, bool selectFirst)
    {
        CloseAbove(levelIndex);
        Level& level = *levels_[levelIndex];
        const MenuItem& item = (*level.items)[index];
        if (item.children.empty() || !item.enabled)
        {
            return;
        }
        level.childFor = index;
        level.hover = index;
        Render(level);
        // Anchor on the parent item so the submenu picks the right monitor.
        const POINT anchor{
            level.body.right,
            level.body.top + static_cast<LONG>(std::lround(
                level.rects[index].top * level.scale))};
        if (OpenLevel(item.children, anchor, index, &level) && selectFirst)
        {
            MoveHover(1);
        }
    }

    void MoveHover(int direction)
    {
        Level& level = *levels_.back();
        const int count = static_cast<int>(level.items->size());
        int index = level.hover;
        for (int step = 0; step < count; ++step)
        {
            index = index < 0 ? (direction > 0 ? 0 : count - 1)
                              : (index + direction + count) % count;
            if (Selectable((*level.items)[index]))
            {
                SetHover(static_cast<int>(levels_.size()) - 1, index);
                return;
            }
        }
    }

    void Activate(int levelIndex, int index)
    {
        if (levelIndex < 0 || index < 0)
        {
            return;
        }
        const MenuItem& item = (*levels_[levelIndex]->items)[index];
        if (!Selectable(item))
        {
            return;
        }
        if (!item.children.empty())
        {
            OpenChild(levelIndex, index, false);
            return;
        }
        Finish(item.id);
    }

    void HandleMouse(const MSG& message)
    {
        POINT point = message.pt;
        switch (message.message)
        {
        case WM_MOUSEMOVE:
        case WM_NCMOUSEMOVE:
            HoverAt(point);
            break;

        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_NCLBUTTONDOWN:
        case WM_NCRBUTTONDOWN:
        case WM_NCMBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDBLCLK:
        {
            const int levelIndex = LevelAt(point);
            if (levelIndex < 0)
            {
                Finish(0);
                break;
            }
            downInside_ = true;
            const int index = ItemAt(levelIndex, point);
            if (index >= 0 && !(*levels_[levelIndex]->items)[index].children.empty())
            {
                OpenChild(levelIndex, index, false);
            }
            break;
        }

        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_NCLBUTTONUP:
        case WM_NCRBUTTONUP:
        {
            const int levelIndex = LevelAt(point);
            if (downInside_ && levelIndex >= 0)
            {
                const int index = ItemAt(levelIndex, point);
                if (index >= 0
                    && (*levels_[levelIndex]->items)[index].children.empty())
                {
                    Activate(levelIndex, index);
                }
            }
            downInside_ = false;
            break;
        }

        default:
            break;
        }
    }

    void HandleKey(WPARAM key)
    {
        Level& level = *levels_.back();
        const int levelIndex = static_cast<int>(levels_.size()) - 1;
        switch (key)
        {
        case VK_DOWN:
            MoveHover(1);
            break;
        case VK_UP:
            MoveHover(-1);
            break;
        case VK_RIGHT:
            if (level.hover >= 0
                && !(*level.items)[level.hover].children.empty())
            {
                OpenChild(levelIndex, level.hover, true);
            }
            break;
        case VK_LEFT:
            if (levels_.size() > 1)
            {
                PopLevel();
                Render(*levels_.back());
            }
            break;
        case VK_RETURN:
        case VK_SPACE:
            if (level.hover >= 0)
            {
                if (!(*level.items)[level.hover].children.empty())
                {
                    OpenChild(levelIndex, level.hover, true);
                }
                else
                {
                    Activate(levelIndex, level.hover);
                }
            }
            break;
        case VK_ESCAPE:
            if (levels_.size() > 1)
            {
                PopLevel();
                Render(*levels_.back());
            }
            else
            {
                Finish(0);
            }
            break;
        case VK_MENU:
        case VK_F10:
        case VK_TAB:
            Finish(0);
            break;
        default:
            break;
        }
    }

    void Finish(UINT result)
    {
        result_ = result;
        done_ = true;
    }

    HWND owner_ = nullptr;
    MenuOptions options_;
    bool dark_ = false;
    const Palette* palette_ = nullptr;
    ComPtr<IDWriteTextFormat> text_;
    ComPtr<IDWriteTextFormat> header_;
    std::vector<std::unique_ptr<Level>> levels_;
    Pending pendingOpen_;
    Pending pendingClose_;
    bool downInside_ = false;
    bool done_ = false;
    UINT result_ = 0;
};

} // namespace

UINT TrackMenu(HWND owner, const std::vector<MenuItem>& items, POINT anchor,
               const MenuOptions& options)
{
    if (items.empty() || !D2DFactory() || !DWFactory())
    {
        return 0;
    }
    MenuSession session(owner, options);
    return session.Run(items, anchor);
}

// ---------------------------------------------------------------------------
// Form
// ---------------------------------------------------------------------------

namespace
{

constexpr wchar_t kFormClass[] = L"LightDock_MacForm";
constexpr float kSidebarW = 196.0f;
constexpr float kFooterH = 58.0f;
constexpr float kTitleBandH = 54.0f;
constexpr float kSegmentBandH = 58.0f;
constexpr float kPagePadX = 26.0f;
constexpr float kRowPadX = 14.0f;
constexpr float kCardRadius = 10.0f;
constexpr float kMaxContentW = 620.0f;
constexpr UINT_PTR kAnimTimer = 1;

} // namespace

Form::~Form()
{
    Destroy();
    if (fieldBrush_)
    {
        DeleteObject(fieldBrush_);
    }
    if (editFont_)
    {
        DeleteObject(editFont_);
    }
    if (backDc_)
    {
        SelectObject(backDc_, backOld_);
        DeleteDC(backDc_);
    }
    if (backBitmap_)
    {
        DeleteObject(backBitmap_);
    }
}

int Form::AddPage(std::wstring title, wchar_t glyph, D2D1_COLOR_F tint)
{
    Page page;
    page.title = std::move(title);
    page.glyph = glyph;
    page.tint = tint;
    pages_.push_back(std::move(page));
    return static_cast<int>(pages_.size()) - 1;
}

void Form::AddSection(std::wstring title, std::wstring footnote)
{
    if (pages_.empty())
    {
        AddPage(L"");
    }
    Section section;
    section.title = std::move(title);
    section.footnote = std::move(footnote);
    pages_.back().sections.push_back(std::move(section));
}

Form::Section& Form::CurrentSection()
{
    if (pages_.empty() || pages_.back().sections.empty())
    {
        AddSection();
    }
    return pages_.back().sections.back();
}

Form::Control& Form::NewControl(Kind kind, std::wstring label)
{
    auto control = std::make_unique<Control>();
    control->kind = kind;
    control->label = std::move(label);
    Section& section = CurrentSection();
    control->page = static_cast<int>(pages_.size()) - 1;
    if (kind == Kind::Header || kind == Kind::Paragraph)
    {
        section.plain = true;
    }
    section.rows.push_back(control.get());
    controls_.push_back(std::move(control));
    return *controls_.back();
}

Form::Control& Form::AddSwitch(std::wstring label, bool* value,
                               std::wstring sublabel)
{
    Control& control = NewControl(Kind::Switch, std::move(label));
    control.boolValue = value;
    control.sublabel = std::move(sublabel);
    control.knob = value && *value ? 1.0f : 0.0f;
    return control;
}

Form::Control& Form::AddSlider(std::wstring label, int* value, int minimum,
                               int maximum,
                               std::function<std::wstring(int)> format,
                               int step)
{
    Control& control = NewControl(Kind::Slider, std::move(label));
    control.intValue = value;
    control.minimum = minimum;
    control.maximum = std::max(maximum, minimum + 1);
    control.format = std::move(format);
    control.step = std::max(1, step);
    return control;
}

Form::Control& Form::AddChoice(std::wstring label, int* value,
                               std::vector<std::wstring> options)
{
    Control& control = NewControl(Kind::Choice, std::move(label));
    control.intValue = value;
    control.options = std::move(options);
    return control;
}

Form::Control& Form::AddColor(std::wstring label, COLORREF* value,
                              std::function<std::wstring()> caption)
{
    Control& control = NewControl(Kind::Color, std::move(label));
    control.colorValue = value;
    control.caption = std::move(caption);
    return control;
}

Form::Control& Form::AddEdit(std::wstring label, std::wstring* value,
                             std::wstring buttonText,
                             std::function<void()> onButton)
{
    Control& control = NewControl(Kind::Edit, std::move(label));
    control.textValue = value;
    control.buttonText = std::move(buttonText);
    control.onButton = std::move(onButton);
    return control;
}

Form::Control& Form::AddInfo(std::wstring label,
                             std::function<std::wstring()> value)
{
    Control& control = NewControl(Kind::Info, std::move(label));
    control.caption = std::move(value);
    return control;
}

Form::Control& Form::AddHeader(std::function<IWICBitmapSource*()> image,
                               std::function<std::wstring()> title,
                               std::function<std::wstring()> subtitle,
                               float imageSize)
{
    Control& control = NewControl(Kind::Header, L"");
    control.image = std::move(image);
    control.dynamicLabel = std::move(title);
    control.caption = std::move(subtitle);
    control.imageSize = imageSize;
    return control;
}

Form::Control& Form::AddParagraph(std::wstring text)
{
    return NewControl(Kind::Paragraph, std::move(text));
}

void Form::AddButton(int id, std::wstring text, ButtonRole role,
                     bool alignLeft)
{
    Button button;
    button.id = id;
    button.text = std::move(text);
    button.role = role;
    button.alignLeft = alignLeft;
    buttons_.push_back(std::move(button));
}

// --- lifetime ---------------------------------------------------------------

void Form::CreateTextFormats()
{
    body_ = MakeFormat(13.0f, DWRITE_FONT_WEIGHT_NORMAL);
    bodyRight_ = MakeFormat(13.0f, DWRITE_FONT_WEIGHT_NORMAL);
    if (bodyRight_)
    {
        bodyRight_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    small_ = MakeFormat(11.5f, DWRITE_FONT_WEIGHT_NORMAL);
    sectionTitle_ = MakeFormat(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    pageTitle_ = MakeFormat(19.0f, DWRITE_FONT_WEIGHT_BOLD);
    headerTitle_ = MakeFormat(16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    buttonText_ = MakeFormat(13.0f, DWRITE_FONT_WEIGHT_NORMAL);
    if (buttonText_)
    {
        buttonText_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    glyph_ = MakeFormat(12.0f, DWRITE_FONT_WEIGHT_NORMAL, false,
                        IconFontFamily());
    if (glyph_)
    {
        glyph_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    wrap_ = MakeFormat(12.5f, DWRITE_FONT_WEIGHT_NORMAL, true);
}

void Form::CreateEditFonts()
{
    if (editFont_)
    {
        DeleteObject(editFont_);
    }
    editFont_ = CreateFontW(
        -static_cast<int>(std::lround(13.0f * scale_)), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");

    HDC dc = GetDC(nullptr);
    HGDIOBJ old = SelectObject(dc, editFont_);
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    editLineHeight_ = std::max(8, static_cast<int>(metrics.tmHeight));

    for (auto& control : controls_)
    {
        if (control->edit)
        {
            SendMessageW(control->edit, WM_SETFONT,
                         reinterpret_cast<WPARAM>(editFont_), TRUE);
        }
    }
}

bool Form::Create(const Options& options)
{
    options_ = options;
    if (pages_.empty())
    {
        AddPage(L"");
    }
    if (!D2DFactory() || !DWFactory())
    {
        return false;
    }

    static bool registered = false;
    if (!registered)
    {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = &Form::WindowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = kFormClass;
        RegisterClassExW(&windowClass);
        registered = true;
    }

    dark_ = SystemUsesDarkTheme();
    palette_ = &GetPalette(dark_);
    fieldBrush_ = CreateSolidBrush(ToColorRef(palette_->fieldBg));
    CreateTextFormats();

    RECT work = options.center;
    if (work.right <= work.left || work.bottom <= work.top)
    {
        HMONITOR monitor = MonitorFromWindow(options.owner,
                                             MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        GetMonitorInfoW(monitor, &info);
        work = info.rcWork;
    }
    HMONITOR monitor = MonitorFromRect(&work, MONITOR_DEFAULTTONEAREST);
    const int dpi = DpiForMonitor(monitor);
    scale_ = static_cast<float>(dpi) / 96.0f;

    width_ = options.width;
    height_ = options.height > 0.0f ? options.height : 2000.0f;
    if (options.height <= 0.0f)
    {
        for (Page& page : pages_)
        {
            LayoutPage(page);
        }
        float content = 0.0f;
        for (const Page& page : pages_)
        {
            content = std::max(content, page.contentHeight);
        }
        height_ = content + (buttons_.empty() ? 0.0f : kFooterH)
            + Viewport().top;
        options_.height = height_;
    }

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU
        | WS_CLIPCHILDREN;
    const DWORD exStyle = options.icon ? 0 : WS_EX_DLGMODALFRAME;
    RECT frame{0, 0, static_cast<LONG>(std::lround(Px(width_))),
               static_cast<LONG>(std::lround(Px(height_)))};
    AdjustForDpi(frame, style, exStyle, dpi);
    const int w = frame.right - frame.left;
    const int h = frame.bottom - frame.top;
    const int workW = static_cast<int>(work.right - work.left);
    const int workH = static_cast<int>(work.bottom - work.top);
    const int x = static_cast<int>(work.left) + (workW - w) / 2;
    const int y = static_cast<int>(work.top) + std::max(0, (workH - h) / 2);

    hwnd_ = CreateWindowExW(exStyle, kFormClass, options.title.c_str(), style,
                            x, y, w, h, options.owner, nullptr,
                            options.instance ? options.instance
                                             : GetModuleHandleW(nullptr),
                            this);
    if (!hwnd_)
    {
        return false;
    }

    if (options.icon)
    {
        SendMessageW(hwnd_, WM_SETICON, ICON_SMALL,
                     reinterpret_cast<LPARAM>(options.icon));
        SendMessageW(hwnd_, WM_SETICON, ICON_BIG,
                     reinterpret_cast<LPARAM>(options.icon));
    }

    // The window may have landed on a monitor with another DPI.
    const int actualDpi = DpiForWindow(hwnd_);
    if (actualDpi != dpi)
    {
        scale_ = static_cast<float>(actualDpi) / 96.0f;
        RECT resized{0, 0, static_cast<LONG>(std::lround(Px(width_))),
                     static_cast<LONG>(std::lround(Px(height_)))};
        AdjustForDpi(resized, style, exStyle, actualDpi);
        SetWindowPos(hwnd_, nullptr, 0, 0, resized.right - resized.left,
                     resized.bottom - resized.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    const D2D1_RENDER_TARGET_PROPERTIES properties =
        D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                              D2D1_ALPHA_MODE_IGNORE),
            96.0f * scale_, 96.0f * scale_);
    if (FAILED(D2DFactory()->CreateDCRenderTarget(&properties, &target_))
        || FAILED(target_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0),
                                                 &brush_)))
    {
        Destroy();
        return false;
    }

    CreateEditFonts();
    for (auto& control : controls_)
    {
        if (control->kind != Kind::Edit)
        {
            continue;
        }
        DWORD editStyle = WS_CHILD | ES_AUTOHSCROLL | WS_TABSTOP;
        if (control->readOnly)
        {
            editStyle |= ES_READONLY;
        }
        control->edit = CreateWindowExW(
            0, L"EDIT", control->textValue ? control->textValue->c_str() : L"",
            editStyle, 0, 0, 10, 10, hwnd_, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        if (control->edit)
        {
            SendMessageW(control->edit, WM_SETFONT,
                         reinterpret_cast<WPARAM>(editFont_), TRUE);
            SendMessageW(control->edit, EM_SETMARGINS,
                         EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
            SetWindowSubclass(control->edit, &Form::EditSubclassProc, 1,
                              reinterpret_cast<DWORD_PTR>(this));
        }
    }

    ApplyWindowChrome(hwnd_, *palette_);
    Layout();
    SelectPage(std::clamp(page_, 0, static_cast<int>(pages_.size()) - 1));
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);
    return true;
}

void Form::Destroy()
{
    if (hwnd_)
    {
        HWND hwnd = hwnd_;
        if (timerRunning_)
        {
            KillTimer(hwnd, kAnimTimer);
            timerRunning_ = false;
        }
        DestroyWindow(hwnd);
        hwnd_ = nullptr;
    }
    for (auto& control : controls_)
    {
        control->edit = nullptr;
        control->cachedBitmap.Reset();
        control->cachedSource = nullptr;
    }
}

void Form::SelectPage(int page)
{
    if (page < 0 || page >= static_cast<int>(pages_.size()))
    {
        return;
    }
    page_ = page;
    if (focus_.target == Target::Control
        || focus_.target == Target::ControlButton)
    {
        focus_ = Hit{};
    }
    if (hwnd_)
    {
        if (GetFocus() && GetParent(GetFocus()) == hwnd_)
        {
            SetFocus(hwnd_);
        }
        PositionEdits();
        Invalidate();
    }
}

void Form::Refresh()
{
    for (auto& control : controls_)
    {
        if (control->kind == Kind::Switch && control->boolValue)
        {
            control->knob = *control->boolValue ? 1.0f : 0.0f;
        }
    }
    SyncEditsFromValues();
    PositionEdits();
    Invalidate();
}

void Form::SetText(Control& control, const std::wstring& text)
{
    if (control.textValue)
    {
        *control.textValue = text;
    }
    if (control.edit)
    {
        syncing_ = true;
        SetWindowTextW(control.edit, text.c_str());
        syncing_ = false;
    }
    Invalidate();
}

void Form::SyncEditsFromValues()
{
    syncing_ = true;
    for (auto& control : controls_)
    {
        if (control->edit && control->textValue)
        {
            SetWindowTextW(control->edit, control->textValue->c_str());
        }
    }
    syncing_ = false;
}

// --- layout -----------------------------------------------------------------

D2D1_RECT_F Form::Viewport() const
{
    float left = 0.0f;
    float top = 0.0f;
    if (options_.navigation == Navigation::Sidebar)
    {
        left = kSidebarW;
        top = kTitleBandH;
    }
    else if (options_.navigation == Navigation::Segmented)
    {
        top = kSegmentBandH;
    }
    const float bottom = height_ - (buttons_.empty() ? 0.0f : kFooterH);
    return D2D1::RectF(left, top, width_, std::max(top, bottom));
}

float Form::MeasureWidth(const std::wstring& text, IDWriteTextFormat* format)
{
    ComPtr<IDWriteTextLayout> layout;
    if (text.empty() || !format
        || FAILED(DWFactory()->CreateTextLayout(
            text.c_str(), static_cast<UINT32>(text.size()), format, 4000.0f,
            200.0f, &layout)))
    {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    return std::ceil(metrics.widthIncludingTrailingWhitespace);
}

float Form::MeasureHeight(const std::wstring& text, IDWriteTextFormat* format,
                          float width)
{
    ComPtr<IDWriteTextLayout> layout;
    if (text.empty() || !format
        || FAILED(DWFactory()->CreateTextLayout(
            text.c_str(), static_cast<UINT32>(text.size()), format,
            std::max(1.0f, width), 4000.0f, &layout)))
    {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    return std::ceil(metrics.height);
}

void Form::LayoutPage(Page& page)
{
    const D2D1_RECT_F viewport = Viewport();
    const float available = Width(viewport) - 2.0f * kPagePadX;
    const float innerW = std::min(available, kMaxContentW);
    const float x0 = (Width(viewport) - innerW) * 0.5f;
    float y = options_.navigation == Navigation::None ? 22.0f : 6.0f;

    for (Section& section : page.sections)
    {
        if (!section.title.empty())
        {
            section.titleRect = D2D1::RectF(x0 + 4.0f, y, x0 + innerW, y + 18.0f);
            y += 24.0f;
        }

        const float cardTop = y;
        for (Control* control : section.rows)
        {
            Control& c = *control;
            float h = 40.0f;
            switch (c.kind)
            {
            case Kind::Slider: h = 44.0f; break;
            case Kind::Edit: h = 46.0f; break;
            case Kind::Header: h = c.imageSize + 16.0f; break;
            case Kind::Paragraph:
                h = MeasureHeight(c.label, wrap_.Get(), innerW - 8.0f) + 10.0f;
                break;
            default: break;
            }
            if (!c.sublabel.empty())
            {
                h += 14.0f;
            }

            c.row = D2D1::RectF(x0, y, x0 + innerW, y + h);
            const float right = c.row.right - kRowPadX;
            const float cy = (c.row.top + c.row.bottom) * 0.5f;
            const float labelW = MeasureWidth(c.label, body_.Get());
            const float labelRight = c.row.left + kRowPadX + labelW;

            switch (c.kind)
            {
            case Kind::Switch:
                c.box = D2D1::RectF(right - 38.0f, cy - 11.0f, right, cy + 11.0f);
                break;
            case Kind::Slider:
            {
                const float valueW = 58.0f;
                const float trackRight = right - valueW - 10.0f;
                const float trackW = std::clamp(
                    trackRight - labelRight - 24.0f, 90.0f, 230.0f);
                c.box = D2D1::RectF(trackRight - trackW, cy - 11.0f,
                                    trackRight, cy + 11.0f);
                break;
            }
            case Kind::Choice:
            {
                float optionW = 0.0f;
                for (const std::wstring& option : c.options)
                {
                    optionW = std::max(optionW, MeasureWidth(option, body_.Get()));
                }
                const float w = std::clamp(optionW + 44.0f, 110.0f, 300.0f);
                c.box = D2D1::RectF(right - w, cy - 12.0f, right, cy + 12.0f);
                break;
            }
            case Kind::Color:
                c.box = D2D1::RectF(right - 46.0f, cy - 12.0f, right, cy + 12.0f);
                break;
            case Kind::Edit:
            {
                float fieldRight = right;
                if (!c.buttonText.empty())
                {
                    const float bw = std::max(
                        76.0f, MeasureWidth(c.buttonText, buttonText_.Get())
                                   + 28.0f);
                    c.button = D2D1::RectF(right - bw, cy - 13.0f, right,
                                           cy + 13.0f);
                    fieldRight = c.button.left - 8.0f;
                }
                c.box = D2D1::RectF(c.row.left + kRowPadX + 96.0f, cy - 14.0f,
                                    fieldRight, cy + 14.0f);
                break;
            }
            case Kind::Header:
                c.box = D2D1::RectF(c.row.left + 4.0f, c.row.top + 8.0f,
                                    c.row.left + 4.0f + c.imageSize,
                                    c.row.top + 8.0f + c.imageSize);
                break;
            default:
                c.box = D2D1::RectF(right, cy, right, cy);
                break;
            }
            y += h;
        }

        section.card = D2D1::RectF(x0, cardTop, x0 + innerW, y);
        if (!section.footnote.empty())
        {
            const float fh = MeasureHeight(section.footnote, small_.Get(),
                                           innerW - 8.0f);
            section.footRect = D2D1::RectF(x0 + 4.0f, y + 7.0f,
                                           x0 + innerW - 4.0f, y + 7.0f + fh);
            y += 7.0f + fh;
        }
        y += 22.0f;
    }

    page.contentHeight = y - 22.0f + 22.0f;
}

void Form::Layout()
{
    if (!hwnd_)
    {
        return;
    }
    RECT client{};
    GetClientRect(hwnd_, &client);
    width_ = Dip(static_cast<float>(client.right));
    height_ = Dip(static_cast<float>(client.bottom));

    if (options_.navigation == Navigation::Sidebar)
    {
        float y = 12.0f;
        for (Page& page : pages_)
        {
            page.nav = D2D1::RectF(10.0f, y, kSidebarW - 10.0f, y + 32.0f);
            y += 34.0f;
        }
    }
    else if (options_.navigation == Navigation::Segmented)
    {
        float segmentW = 0.0f;
        for (const Page& page : pages_)
        {
            segmentW = std::max(segmentW,
                                MeasureWidth(page.title, buttonText_.Get()) + 36.0f);
        }
        const float total = segmentW * static_cast<float>(pages_.size());
        float x = (width_ - total) * 0.5f;
        for (Page& page : pages_)
        {
            page.nav = D2D1::RectF(x, 16.0f, x + segmentW, 42.0f);
            x += segmentW;
        }
    }

    const float buttonY = height_ - kFooterH + (kFooterH - 28.0f) * 0.5f;
    float right = width_ - 20.0f;
    for (auto it = buttons_.rbegin(); it != buttons_.rend(); ++it)
    {
        if (it->alignLeft)
        {
            continue;
        }
        const float w = std::max(88.0f,
                                 MeasureWidth(it->text, buttonText_.Get()) + 34.0f);
        it->rect = D2D1::RectF(right - w, buttonY, right, buttonY + 28.0f);
        right -= w + 10.0f;
    }
    float left = options_.navigation == Navigation::Sidebar
        ? kSidebarW + 20.0f : 20.0f;
    for (Button& button : buttons_)
    {
        if (!button.alignLeft)
        {
            continue;
        }
        const float w = std::max(88.0f,
                                 MeasureWidth(button.text, buttonText_.Get()) + 34.0f);
        button.rect = D2D1::RectF(left, buttonY, left + w, buttonY + 28.0f);
        left += w + 10.0f;
    }

    for (Page& page : pages_)
    {
        LayoutPage(page);
        SetScroll(page, page.scroll);
    }
    PositionEdits();
}

float Form::PageOffset(const Page& page) const
{
    return Viewport().top - page.scroll;
}

D2D1_RECT_F Form::ToClient(const Page& page, const D2D1_RECT_F& rect) const
{
    const D2D1_RECT_F viewport = Viewport();
    return Offset(rect, viewport.left, PageOffset(page));
}

void Form::PositionEdits()
{
    if (!hwnd_)
    {
        return;
    }
    const D2D1_RECT_F viewport = Viewport();
    for (auto& control : controls_)
    {
        if (!control->edit)
        {
            continue;
        }
        const Page& page = pages_[control->page];
        const D2D1_RECT_F field = ToClient(page, control->box);
        const bool visible = control->page == page_
            && field.top >= viewport.top - 0.5f
            && field.bottom <= viewport.bottom + 0.5f;

        const int left = static_cast<int>(std::lround(Px(field.left + 9.0f)));
        const int right = static_cast<int>(std::lround(Px(field.right - 9.0f)));
        const int centre = static_cast<int>(std::lround(
            Px((field.top + field.bottom) * 0.5f)));
        const int top = centre - editLineHeight_ / 2;

        EnableWindow(control->edit, IsEnabled(*control) ? TRUE : FALSE);
        SetWindowPos(control->edit, nullptr, left, top,
                     std::max(1, right - left), editLineHeight_,
                     SWP_NOZORDER | SWP_NOACTIVATE
                         | (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
        if (!visible && GetFocus() == control->edit)
        {
            SetFocus(hwnd_);
        }
    }
}

void Form::UpdateTheme()
{
    dark_ = SystemUsesDarkTheme();
    palette_ = &GetPalette(dark_);
    if (fieldBrush_)
    {
        DeleteObject(fieldBrush_);
    }
    fieldBrush_ = CreateSolidBrush(ToColorRef(palette_->fieldBg));
    ApplyWindowChrome(hwnd_, *palette_);
    RedrawWindow(hwnd_, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

// --- drawing helpers ---------------------------------------------------------

void Form::FillRound(const D2D1_RECT_F& rect, float radius,
                     const D2D1_COLOR_F& color)
{
    brush_->SetColor(color);
    target_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius),
                                  brush_.Get());
}

void Form::StrokeRound(const D2D1_RECT_F& rect, float radius,
                       const D2D1_COLOR_F& color, float width)
{
    const float w = width / scale_;
    brush_->SetColor(color);
    target_->DrawRoundedRectangle(
        D2D1::RoundedRect(Inflate(rect, -w * 0.5f, -w * 0.5f), radius, radius),
        brush_.Get(), w);
}

void Form::FillRect(const D2D1_RECT_F& rect, const D2D1_COLOR_F& color)
{
    brush_->SetColor(color);
    target_->FillRectangle(rect, brush_.Get());
}

void Form::Line(float x0, float y0, float x1, float y1,
                const D2D1_COLOR_F& color, float width)
{
    brush_->SetColor(color);
    target_->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1),
                      brush_.Get(), width / scale_);
}

void Form::Text(const std::wstring& text, IDWriteTextFormat* format,
                const D2D1_RECT_F& rect, const D2D1_COLOR_F& color,
                DWRITE_TEXT_ALIGNMENT align)
{
    if (text.empty() || !format)
    {
        return;
    }
    const DWRITE_TEXT_ALIGNMENT previous = format->GetTextAlignment();
    if (previous != align)
    {
        format->SetTextAlignment(align);
    }
    brush_->SetColor(color);
    target_->DrawText(text.c_str(), static_cast<UINT32>(text.size()), format,
                      rect, brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    if (previous != align)
    {
        format->SetTextAlignment(previous);
    }
}

void Form::DrawFocusRing(const D2D1_RECT_F& rect, float radius)
{
    brush_->SetColor(Fade(palette_->accent, 0.45f));
    target_->DrawRoundedRectangle(
        D2D1::RoundedRect(Inflate(rect, 2.0f, 2.0f), radius + 2.0f,
                          radius + 2.0f),
        brush_.Get(), 3.0f);
}

bool Form::IsEnabled(const Control& control) const
{
    return !control.enabled || control.enabled();
}

// --- painting ------------------------------------------------------------------

void Form::Paint()
{
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd_, &ps);
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int w = std::max(1, static_cast<int>(client.right));
    const int h = std::max(1, static_cast<int>(client.bottom));

    if (!backDc_ || w != backWidth_ || h != backHeight_)
    {
        if (backDc_)
        {
            SelectObject(backDc_, backOld_);
            DeleteDC(backDc_);
        }
        if (backBitmap_)
        {
            DeleteObject(backBitmap_);
        }
        backDc_ = CreateCompatibleDC(dc);
        backBitmap_ = CreateCompatibleBitmap(dc, w, h);
        backOld_ = SelectObject(backDc_, backBitmap_);
        backWidth_ = w;
        backHeight_ = h;
    }

    RECT bind{0, 0, w, h};
    if (target_ && SUCCEEDED(target_->BindDC(backDc_, &bind)))
    {
        target_->BeginDraw();
        Draw();
        if (target_->EndDraw() == static_cast<HRESULT>(D2DERR_RECREATE_TARGET))
        {
            brush_.Reset();
            target_.Reset();
            const D2D1_RENDER_TARGET_PROPERTIES properties =
                D2D1::RenderTargetProperties(
                    D2D1_RENDER_TARGET_TYPE_DEFAULT,
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                      D2D1_ALPHA_MODE_IGNORE),
                    96.0f * scale_, 96.0f * scale_);
            if (SUCCEEDED(D2DFactory()->CreateDCRenderTarget(&properties,
                                                             &target_)))
            {
                target_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &brush_);
            }
            for (auto& control : controls_)
            {
                control->cachedBitmap.Reset();
                control->cachedSource = nullptr;
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    BitBlt(dc, 0, 0, w, h, backDc_, 0, 0, SRCCOPY);
    EndPaint(hwnd_, &ps);
}

void Form::Draw()
{
    const Palette& p = *palette_;
    target_->SetTransform(D2D1::Matrix3x2F::Identity());
    target_->Clear(p.windowBg);

    if (pages_.empty())
    {
        return;
    }
    Page& page = pages_[page_];
    const D2D1_RECT_F viewport = Viewport();

    if (options_.navigation == Navigation::Sidebar)
    {
        DrawSidebar();
        Text(page.title, pageTitle_.Get(),
             D2D1::RectF(viewport.left + kPagePadX, 10.0f,
                         viewport.right - kPagePadX, kTitleBandH - 6.0f),
             p.text);
    }
    else if (options_.navigation == Navigation::Segmented)
    {
        DrawSegmented();
    }

    if (page.scroll > 0.5f
        && options_.navigation != Navigation::None)
    {
        FillRect(D2D1::RectF(viewport.left, viewport.top - 1.0f / scale_,
                             viewport.right, viewport.top),
                 p.hairline);
    }

    target_->PushAxisAlignedClip(viewport, D2D1_ANTIALIAS_MODE_ALIASED);
    target_->SetTransform(
        D2D1::Matrix3x2F::Translation(viewport.left, PageOffset(page)));
    DrawPage(page);
    target_->SetTransform(D2D1::Matrix3x2F::Identity());
    target_->PopAxisAlignedClip();

    DrawScrollbar(page);
    DrawFooter();
}

void Form::DrawSidebar()
{
    const Palette& p = *palette_;
    FillRect(D2D1::RectF(0, 0, kSidebarW, height_), p.sidebarBg);
    FillRect(D2D1::RectF(kSidebarW - 1.0f / scale_, 0, kSidebarW, height_),
             p.hairline);

    for (size_t i = 0; i < pages_.size(); ++i)
    {
        const Page& page = pages_[i];
        const bool selected = static_cast<int>(i) == page_;
        const bool hovered = hover_.target == Target::Nav
            && hover_.index == static_cast<int>(i);
        if (selected)
        {
            FillRound(page.nav, 7.0f, p.accent);
        }
        else if (hovered)
        {
            FillRound(page.nav, 7.0f, p.hover);
        }

        const float cy = (page.nav.top + page.nav.bottom) * 0.5f;
        float textLeft = page.nav.left + 10.0f;
        if (page.glyph)
        {
            const D2D1_RECT_F tile = D2D1::RectF(page.nav.left + 7.0f, cy - 11.0f,
                                                 page.nav.left + 29.0f, cy + 11.0f);
            FillRound(tile, 6.0f, page.tint);
            const wchar_t glyph[2] = {page.glyph, 0};
            Text(glyph, glyph_.Get(), tile, D2D1::ColorF(1, 1, 1, 1),
                 DWRITE_TEXT_ALIGNMENT_CENTER);
            textLeft = tile.right + 10.0f;
        }
        Text(page.title, body_.Get(),
             D2D1::RectF(textLeft, page.nav.top, page.nav.right - 8.0f,
                         page.nav.bottom),
             selected ? p.onAccent : p.text);
    }
}

void Form::DrawSegmented()
{
    const Palette& p = *palette_;
    if (pages_.empty())
    {
        return;
    }
    const D2D1_RECT_F track = Inflate(
        D2D1::RectF(pages_.front().nav.left, pages_.front().nav.top,
                    pages_.back().nav.right, pages_.back().nav.bottom),
        2.0f, 2.0f);
    FillRound(track, 8.0f, p.fill);

    for (size_t i = 0; i < pages_.size(); ++i)
    {
        const Page& page = pages_[i];
        const bool selected = static_cast<int>(i) == page_;
        if (selected)
        {
            FillRound(Offset(page.nav, 0.0f, 0.5f), 6.0f, p.shadow);
            FillRound(page.nav, 6.0f, p.segmentSelected);
        }
        else if (i > 0 && static_cast<int>(i) != page_ + 1)
        {
            Line(page.nav.left, page.nav.top + 6.0f, page.nav.left,
                 page.nav.bottom - 6.0f, p.separator);
        }
        Text(page.title, buttonText_.Get(), page.nav, p.text,
             DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}

void Form::DrawPage(const Page& page)
{
    const Palette& p = *palette_;
    for (const Section& section : page.sections)
    {
        if (!section.title.empty())
        {
            Text(section.title, sectionTitle_.Get(), section.titleRect,
                 p.secondaryText);
        }
        if (!section.plain && !section.rows.empty())
        {
            if (!p.dark)
            {
                FillRound(Offset(section.card, 0.0f, 0.5f), kCardRadius,
                          Rgb(0, 0, 0, 0.03f));
            }
            FillRound(section.card, kCardRadius, p.card);
            StrokeRound(section.card, kCardRadius, p.cardBorder);
        }

        for (size_t i = 0; i < section.rows.size(); ++i)
        {
            Control& control = *section.rows[i];
            DrawControl(control);
            if (!section.plain && i + 1 < section.rows.size())
            {
                FillRect(D2D1::RectF(control.row.left + kRowPadX,
                                     control.row.bottom - 1.0f / scale_,
                                     control.row.right, control.row.bottom),
                         p.separator);
            }
        }

        if (!section.footnote.empty())
        {
            brush_->SetColor(p.secondaryText);
            ComPtr<IDWriteTextLayout> layout;
            if (SUCCEEDED(DWFactory()->CreateTextLayout(
                    section.footnote.c_str(),
                    static_cast<UINT32>(section.footnote.size()), small_.Get(),
                    Width(section.footRect), Height(section.footRect) + 4.0f,
                    &layout)))
            {
                layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
                layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                target_->DrawTextLayout(
                    D2D1::Point2F(section.footRect.left, section.footRect.top),
                    layout.Get(), brush_.Get());
            }
        }
    }
}

void Form::DrawControl(Control& c)
{
    const Palette& p = *palette_;
    const bool enabled = IsEnabled(c);
    const float a = enabled ? 1.0f : 0.4f;
    const float cy = (c.row.top + c.row.bottom) * 0.5f;
    const bool focused = focusVisible_ && focus_.index >= 0
        && focus_.target == Target::Control
        && controls_[focus_.index].get() == &c;
    const bool pressed = pressed_.target == Target::Control
        && pressed_.index >= 0 && controls_[pressed_.index].get() == &c;

    // --- label --------------------------------------------------------------
    if (c.kind != Kind::Header && c.kind != Kind::Paragraph)
    {
        const float labelRight = c.kind == Kind::Info
            ? c.row.right - kRowPadX
            : std::max(c.row.left + 40.0f, c.box.left - 12.0f);
        if (c.sublabel.empty())
        {
            Text(c.label, body_.Get(),
                 D2D1::RectF(c.row.left + kRowPadX, c.row.top,
                             c.kind == Kind::Edit ? c.box.left - 8.0f : labelRight,
                             c.row.bottom),
                 Fade(p.text, a));
        }
        else
        {
            Text(c.label, body_.Get(),
                 D2D1::RectF(c.row.left + kRowPadX, cy - 18.0f, labelRight,
                             cy + 1.0f),
                 Fade(p.text, a));
            Text(c.sublabel, small_.Get(),
                 D2D1::RectF(c.row.left + kRowPadX, cy + 1.0f, labelRight,
                             cy + 18.0f),
                 Fade(p.secondaryText, a));
        }
    }

    switch (c.kind)
    {
    case Kind::Switch:
    {
        const D2D1_COLOR_F track = Mix(p.fill, p.accent, c.knob);
        FillRound(c.box, Height(c.box) * 0.5f, Fade(track, a));
        const float d = Height(c.box) - 4.0f;
        const float x = c.box.left + 2.0f + c.knob * (Width(c.box) - 4.0f - d);
        const D2D1_ELLIPSE knob = D2D1::Ellipse(
            D2D1::Point2F(x + d * 0.5f, cy), d * 0.5f, d * 0.5f);
        brush_->SetColor(Fade(Rgb(0, 0, 0, 0.18f), a));
        target_->FillEllipse(D2D1::Ellipse(
            D2D1::Point2F(knob.point.x, knob.point.y + 0.6f),
            knob.radiusX + 0.6f, knob.radiusY + 0.6f), brush_.Get());
        brush_->SetColor(Fade(p.knob, enabled ? 1.0f : 0.7f));
        target_->FillEllipse(knob, brush_.Get());
        if (focused)
        {
            DrawFocusRing(c.box, Height(c.box) * 0.5f);
        }
        break;
    }

    case Kind::Slider:
    {
        const int value = c.intValue ? *c.intValue : c.minimum;
        const float t = static_cast<float>(value - c.minimum)
            / static_cast<float>(c.maximum - c.minimum);
        const float r = 9.0f;
        const float x = c.box.left + r + ClampF(t, 0.0f, 1.0f)
            * (Width(c.box) - 2.0f * r);
        FillRound(D2D1::RectF(c.box.left, cy - 2.0f, c.box.right, cy + 2.0f),
                  2.0f, Fade(p.fill, a));
        FillRound(D2D1::RectF(c.box.left, cy - 2.0f, x, cy + 2.0f), 2.0f,
                  Fade(p.accent, a));

        brush_->SetColor(Fade(Rgb(0, 0, 0, p.dark ? 0.45f : 0.16f), a));
        target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, cy + 0.7f),
                                           r + 0.7f, r + 0.7f),
                             brush_.Get());
        brush_->SetColor(pressed ? Mix(p.knob, p.controlPressed, 0.6f) : p.knob);
        target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, cy), r, r),
                             brush_.Get());
        brush_->SetColor(Fade(p.controlBorder, a));
        target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x, cy), r - 0.25f,
                                           r - 0.25f),
                             brush_.Get(), 0.5f);
        if (focused)
        {
            DrawFocusRing(D2D1::RectF(x - r, cy - r, x + r, cy + r), r);
        }

        const std::wstring text = c.format ? c.format(value)
                                           : std::to_wstring(value);
        Text(text, bodyRight_.Get(),
             D2D1::RectF(c.box.right + 8.0f, c.row.top,
                         c.row.right - kRowPadX, c.row.bottom),
             Fade(p.secondaryText, a), DWRITE_TEXT_ALIGNMENT_TRAILING);
        break;
    }

    case Kind::Choice:
    {
        FillRound(Offset(c.box, 0.0f, 0.5f), 6.0f, Fade(p.shadow, a));
        FillRound(c.box, 6.0f, Fade(pressed ? p.controlPressed : p.controlBg, a));
        StrokeRound(c.box, 6.0f, Fade(p.controlBorder, a));
        const int index = c.intValue ? *c.intValue : -1;
        const std::wstring text = index >= 0
            && index < static_cast<int>(c.options.size())
            ? c.options[index] : std::wstring();
        Text(text, body_.Get(),
             D2D1::RectF(c.box.left + 10.0f, c.box.top, c.box.right - 26.0f,
                         c.box.bottom),
             Fade(p.text, a));
        const D2D1_RECT_F knob = D2D1::RectF(c.box.right - 20.0f, c.box.top + 3.0f,
                                             c.box.right - 3.0f, c.box.bottom - 3.0f);
        FillRound(knob, 4.0f, Fade(p.accent, a));
        const float kx = (knob.left + knob.right) * 0.5f;
        DrawChevron(target_.Get(), brush_.Get(), kx, cy - 3.0f, 6.0f, 1,
                    Fade(p.onAccent, a), 1.4f);
        DrawChevron(target_.Get(), brush_.Get(), kx, cy + 3.0f, 6.0f, 2,
                    Fade(p.onAccent, a), 1.4f);
        if (focused)
        {
            DrawFocusRing(c.box, 6.0f);
        }
        break;
    }

    case Kind::Color:
    {
        if (c.caption)
        {
            Text(c.caption(), bodyRight_.Get(),
                 D2D1::RectF(c.box.left - 200.0f, c.row.top, c.box.left - 10.0f,
                             c.row.bottom),
                 Fade(p.secondaryText, a), DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
        FillRound(Offset(c.box, 0.0f, 0.5f), 6.0f, Fade(p.shadow, a));
        FillRound(c.box, 6.0f, Fade(pressed ? p.controlPressed : p.controlBg, a));
        StrokeRound(c.box, 6.0f, Fade(p.controlBorder, a));
        const D2D1_RECT_F inner = Inflate(c.box, -3.5f, -3.5f);
        FillRound(inner, 3.5f,
                  Fade(FromColorRef(c.colorValue ? *c.colorValue : 0), a));
        StrokeRound(inner, 3.5f, Fade(Rgb(0, 0, 0, 0.12f), a));
        if (focused)
        {
            DrawFocusRing(c.box, 6.0f);
        }
        break;
    }

    case Kind::Edit:
    {
        const bool editFocused = c.edit && GetFocus() == c.edit;
        FillRound(c.box, 6.0f, Fade(p.fieldBg, enabled ? 1.0f : 0.6f));
        StrokeRound(c.box, 6.0f, editFocused ? p.accent : Fade(p.fieldBorder, a),
                    editFocused ? 1.5f : 1.0f);
        if (editFocused)
        {
            DrawFocusRing(c.box, 6.0f);
        }

        if (!c.buttonText.empty())
        {
            const bool buttonPressed = pressed_.target == Target::ControlButton
                && pressed_.index >= 0 && controls_[pressed_.index].get() == &c;
            const bool buttonFocused = focusVisible_
                && focus_.target == Target::ControlButton
                && focus_.index >= 0 && controls_[focus_.index].get() == &c;
            FillRound(Offset(c.button, 0.0f, 0.5f), 6.0f, Fade(p.shadow, a));
            FillRound(c.button, 6.0f,
                      Fade(buttonPressed ? p.controlPressed : p.controlBg, a));
            StrokeRound(c.button, 6.0f, Fade(p.controlBorder, a));
            Text(c.buttonText, buttonText_.Get(), c.button, Fade(p.text, a),
                 DWRITE_TEXT_ALIGNMENT_CENTER);
            if (buttonFocused)
            {
                DrawFocusRing(c.button, 6.0f);
            }
        }
        break;
    }

    case Kind::Info:
        if (c.caption)
        {
            const float labelW = MeasureWidth(c.label, body_.Get());
            Text(c.caption(), bodyRight_.Get(),
                 D2D1::RectF(c.row.left + kRowPadX + labelW + 16.0f, c.row.top,
                             c.row.right - kRowPadX, c.row.bottom),
                 Fade(p.secondaryText, a), DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
        break;

    case Kind::Header:
    {
        IWICBitmapSource* source = c.image ? c.image() : nullptr;
        if (source != c.cachedSource)
        {
            c.cachedSource = source;
            c.cachedBitmap = CreateBitmapFromWic(target_.Get(), source);
        }
        float textLeft = c.row.left + 4.0f;
        if (c.cachedBitmap)
        {
            target_->DrawBitmap(c.cachedBitmap.Get(), c.box, 1.0f,
                                D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            textLeft = c.box.right + 14.0f;
        }
        const std::wstring title = c.dynamicLabel ? c.dynamicLabel() : c.label;
        const std::wstring subtitle = c.caption ? c.caption() : std::wstring();
        const float hc = (c.box.top + c.box.bottom) * 0.5f;
        if (subtitle.empty())
        {
            Text(title, headerTitle_.Get(),
                 D2D1::RectF(textLeft, c.box.top, c.row.right - 4.0f, c.box.bottom),
                 p.text);
        }
        else
        {
            Text(title, headerTitle_.Get(),
                 D2D1::RectF(textLeft, hc - 22.0f, c.row.right - 4.0f, hc),
                 p.text);
            Text(subtitle, small_.Get(),
                 D2D1::RectF(textLeft, hc + 1.0f, c.row.right - 4.0f, hc + 19.0f),
                 p.secondaryText);
        }
        break;
    }

    case Kind::Paragraph:
    {
        brush_->SetColor(p.text);
        ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(DWFactory()->CreateTextLayout(
                c.label.c_str(), static_cast<UINT32>(c.label.size()),
                wrap_.Get(), Width(c.row) - 8.0f, Height(c.row), &layout)))
        {
            target_->DrawTextLayout(D2D1::Point2F(c.row.left + 4.0f,
                                                  c.row.top + 2.0f),
                                    layout.Get(), brush_.Get());
        }
        break;
    }
    }
}

void Form::DrawFooter()
{
    if (buttons_.empty())
    {
        return;
    }
    const Palette& p = *palette_;
    const float top = height_ - kFooterH;
    const float left = options_.navigation == Navigation::Sidebar ? kSidebarW : 0.0f;
    FillRect(D2D1::RectF(left, top, width_, top + 1.0f / scale_), p.hairline);

    for (size_t i = 0; i < buttons_.size(); ++i)
    {
        const Button& button = buttons_[i];
        const bool pressed = pressed_.target == Target::Footer
            && pressed_.index == static_cast<int>(i)
            && hover_ == pressed_;
        const bool focused = focusVisible_ && focus_.target == Target::Footer
            && focus_.index == static_cast<int>(i);

        if (button.role == ButtonRole::Primary)
        {
            FillRound(Offset(button.rect, 0.0f, 0.5f), 6.0f, p.shadow);
            FillRound(button.rect, 6.0f, pressed ? p.accentPressed : p.accent);
            Text(button.text, buttonText_.Get(), button.rect, p.onAccent,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        else
        {
            FillRound(Offset(button.rect, 0.0f, 0.5f), 6.0f, p.shadow);
            FillRound(button.rect, 6.0f, pressed ? p.controlPressed : p.controlBg);
            StrokeRound(button.rect, 6.0f, p.controlBorder);
            Text(button.text, buttonText_.Get(), button.rect, p.text,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        if (focused)
        {
            DrawFocusRing(button.rect, 6.0f);
        }
    }
}

D2D1_RECT_F Form::ThumbRect(const Page& page) const
{
    const D2D1_RECT_F viewport = Viewport();
    const float viewH = Height(viewport);
    if (page.contentHeight <= viewH + 0.5f)
    {
        return D2D1::RectF(0, 0, 0, 0);
    }
    const float trackTop = viewport.top + 4.0f;
    const float trackH = viewH - 8.0f;
    const float thumbH = std::max(32.0f, trackH * viewH / page.contentHeight);
    const float maxScroll = page.contentHeight - viewH;
    const float y = trackTop + (trackH - thumbH) * (page.scroll / maxScroll);
    return D2D1::RectF(viewport.right - 10.0f, y, viewport.right - 3.0f,
                       y + thumbH);
}

void Form::DrawScrollbar(const Page& page)
{
    const D2D1_RECT_F thumb = ThumbRect(page);
    if (Width(thumb) <= 0.0f || scrollbarAlpha_ <= 0.01f)
    {
        return;
    }
    const D2D1_COLOR_F color = palette_->dark ? Rgb(255, 255, 255, 0.35f)
                                              : Rgb(0, 0, 0, 0.32f);
    FillRound(Inflate(thumb, -1.0f, 0.0f), 3.0f,
              Fade(color, scrollbarAlpha_));
}

// --- input -----------------------------------------------------------------------

Form::Hit Form::HitTest(float x, float y) const
{
    for (size_t i = 0; i < buttons_.size(); ++i)
    {
        if (Contains(buttons_[i].rect, x, y))
        {
            return Hit{Target::Footer, static_cast<int>(i)};
        }
    }

    if (options_.navigation != Navigation::None)
    {
        for (size_t i = 0; i < pages_.size(); ++i)
        {
            if (Contains(pages_[i].nav, x, y))
            {
                return Hit{Target::Nav, static_cast<int>(i)};
            }
        }
    }

    if (pages_.empty())
    {
        return {};
    }
    const Page& page = pages_[page_];
    const D2D1_RECT_F viewport = Viewport();
    if (!Contains(viewport, x, y))
    {
        return {};
    }

    const D2D1_RECT_F thumb = ThumbRect(page);
    if (Width(thumb) > 0.0f && x >= thumb.left - 3.0f)
    {
        return Hit{Target::Thumb, 0};
    }

    const float px = x - viewport.left;
    const float py = y - PageOffset(page);
    for (size_t i = 0; i < controls_.size(); ++i)
    {
        const Control& c = *controls_[i];
        if (c.page != page_ || !IsEnabled(c))
        {
            continue;
        }
        switch (c.kind)
        {
        case Kind::Slider:
            if (Contains(D2D1::RectF(c.box.left - 6.0f, c.row.top,
                                     c.box.right + 6.0f, c.row.bottom), px, py))
            {
                return Hit{Target::Control, static_cast<int>(i)};
            }
            break;
        case Kind::Switch:
        case Kind::Choice:
        case Kind::Color:
            if (Contains(Inflate(c.box, 3.0f, 3.0f), px, py))
            {
                return Hit{Target::Control, static_cast<int>(i)};
            }
            break;
        case Kind::Edit:
            if (!c.buttonText.empty() && Contains(c.button, px, py))
            {
                return Hit{Target::ControlButton, static_cast<int>(i)};
            }
            if (Contains(c.box, px, py))
            {
                return Hit{Target::Control, static_cast<int>(i)};
            }
            break;
        default:
            break;
        }
    }
    return {};
}

void Form::SetSliderFromX(Control& control, float x)
{
    if (!control.intValue)
    {
        return;
    }
    const Page& page = pages_[control.page];
    const D2D1_RECT_F box = ToClient(page, control.box);
    const float r = 9.0f;
    const float t = ClampF((x - box.left - r) / std::max(1.0f, Width(box) - 2 * r),
                           0.0f, 1.0f);
    int value = control.minimum
        + static_cast<int>(std::lround(t * (control.maximum - control.minimum)));
    if (control.step > 1)
    {
        value = control.minimum
            + ((value - control.minimum + control.step / 2) / control.step)
                * control.step;
    }
    value = std::clamp(value, control.minimum, control.maximum);
    if (value != *control.intValue)
    {
        *control.intValue = value;
        Notify(control);
    }
}

void Form::OnMouseDown(float x, float y)
{
    const Hit hit = HitTest(x, y);
    pressed_ = hit;
    focusVisible_ = false;
    SetCapture(hwnd_);

    if (hit.target == Target::Control)
    {
        Control& control = *controls_[hit.index];
        if (control.kind == Kind::Edit)
        {
            SetFocus(control.edit);
            SendMessageW(control.edit, EM_SETSEL, 0, -1);
            pressed_ = Hit{};
            ReleaseCapture();
        }
        else
        {
            SetFocus(hwnd_);
            focus_ = hit;
            if (control.kind == Kind::Slider)
            {
                SetSliderFromX(control, x);
            }
        }
    }
    else if (hit.target == Target::Thumb)
    {
        const D2D1_RECT_F thumb = ThumbRect(pages_[page_]);
        thumbGrab_ = Contains(thumb, thumb.left + 1.0f, y) ? y - thumb.top
                                                           : Height(thumb) * 0.5f;
        OnMouseMove(x, y);
    }
    else if (hit.target == Target::Nav)
    {
        SetFocus(hwnd_);
        SelectPage(hit.index);
        pressed_ = Hit{};
    }
    else
    {
        SetFocus(hwnd_);
        if (hit.target != Target::None)
        {
            focus_ = hit;
        }
    }
    Invalidate();
}

void Form::OnMouseMove(float x, float y)
{
    if (!tracking_)
    {
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd_, 0};
        TrackMouseEvent(&track);
        tracking_ = true;
    }

    if (pressed_.target == Target::Control)
    {
        Control& control = *controls_[pressed_.index];
        if (control.kind == Kind::Slider)
        {
            SetSliderFromX(control, x);
            return;
        }
    }

    if (pressed_.target == Target::Thumb)
    {
        Page& page = pages_[page_];
        const D2D1_RECT_F viewport = Viewport();
        const D2D1_RECT_F thumb = ThumbRect(page);
        const float trackH = Height(viewport) - 8.0f - Height(thumb);
        const float t = ClampF((y - thumbGrab_ - viewport.top - 4.0f)
                                   / std::max(1.0f, trackH),
                               0.0f, 1.0f);
        SetScroll(page, t * (page.contentHeight - Height(viewport)));
        return;
    }

    const Hit hit = HitTest(x, y);
    if (hit != hover_)
    {
        hover_ = hit;
        Invalidate();
    }

    // Reveal the overlay scrollbar while hovering near it.
    const D2D1_RECT_F viewport = Viewport();
    if (!pages_.empty() && x > viewport.right - 16.0f && Contains(viewport, x, y)
        && Width(ThumbRect(pages_[page_])) > 0.0f)
    {
        scrollbarShownAt_ = GetTickCount64();
        if (scrollbarAlpha_ < 1.0f)
        {
            StartAnimation();
        }
    }
}

void Form::OnMouseUp(float x, float y)
{
    const Hit pressed = pressed_;
    pressed_ = Hit{};
    if (GetCapture() == hwnd_)
    {
        ReleaseCapture();
    }

    if (pressed.target == Target::Footer || pressed.target == Target::ControlButton
        || (pressed.target == Target::Control
            && controls_[pressed.index]->kind != Kind::Slider))
    {
        if (HitTest(x, y) == pressed)
        {
            Activate(pressed);
        }
    }
    Invalidate();
}

void Form::OnWheel(int delta)
{
    if (pages_.empty())
    {
        return;
    }
    Page& page = pages_[page_];
    SetScroll(page, page.scroll - static_cast<float>(delta) / WHEEL_DELTA * 48.0f);
}

void Form::SetScroll(Page& page, float value)
{
    const float maxScroll = std::max(0.0f, page.contentHeight
                                               - Height(Viewport()));
    const float clamped = ClampF(value, 0.0f, maxScroll);
    if (std::abs(clamped - page.scroll) < 0.01f)
    {
        page.scroll = clamped;
        return;
    }
    page.scroll = clamped;
    if (&page == &pages_[page_] && maxScroll > 0.0f)
    {
        scrollbarShownAt_ = GetTickCount64();
        scrollbarAlpha_ = 1.0f;
        StartAnimation();
    }
    PositionEdits();
    Invalidate();
}

void Form::EnsureVisible(Control& control)
{
    if (control.page != page_)
    {
        return;
    }
    Page& page = pages_[page_];
    const float viewH = Height(Viewport());
    if (control.row.top < page.scroll)
    {
        SetScroll(page, control.row.top - 8.0f);
    }
    else if (control.row.bottom > page.scroll + viewH)
    {
        SetScroll(page, control.row.bottom - viewH + 8.0f);
    }
}

std::vector<Form::Hit> Form::FocusOrder() const
{
    std::vector<Hit> order;
    if (!pages_.empty())
    {
        for (const Section& section : pages_[page_].sections)
        {
            for (const Control* control : section.rows)
            {
                if (!IsEnabled(*control))
                {
                    continue;
                }
                int index = -1;
                for (size_t i = 0; i < controls_.size(); ++i)
                {
                    if (controls_[i].get() == control)
                    {
                        index = static_cast<int>(i);
                        break;
                    }
                }
                switch (control->kind)
                {
                case Kind::Switch:
                case Kind::Slider:
                case Kind::Choice:
                case Kind::Color:
                    order.push_back(Hit{Target::Control, index});
                    break;
                case Kind::Edit:
                    order.push_back(Hit{Target::Control, index});
                    if (!control->buttonText.empty())
                    {
                        order.push_back(Hit{Target::ControlButton, index});
                    }
                    break;
                default:
                    break;
                }
            }
        }
    }
    for (size_t i = 0; i < buttons_.size(); ++i)
    {
        order.push_back(Hit{Target::Footer, static_cast<int>(i)});
    }
    return order;
}

void Form::ChangeFocus(int direction)
{
    const std::vector<Hit> order = FocusOrder();
    if (order.empty())
    {
        return;
    }
    int current = -1;
    for (size_t i = 0; i < order.size(); ++i)
    {
        if (order[i] == focus_)
        {
            current = static_cast<int>(i);
            break;
        }
    }
    const int count = static_cast<int>(order.size());
    const int next = current < 0 ? (direction > 0 ? 0 : count - 1)
                                 : (current + direction + count) % count;
    focus_ = order[next];
    focusVisible_ = true;

    if (focus_.target == Target::Control || focus_.target == Target::ControlButton)
    {
        Control& control = *controls_[focus_.index];
        EnsureVisible(control);
        if (focus_.target == Target::Control && control.kind == Kind::Edit
            && control.edit)
        {
            SetFocus(control.edit);
            SendMessageW(control.edit, EM_SETSEL, 0, -1);
        }
        else
        {
            SetFocus(hwnd_);
        }
    }
    else
    {
        SetFocus(hwnd_);
    }
    Invalidate();
}

void Form::OnKey(WPARAM key)
{
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

    switch (key)
    {
    case VK_TAB:
        if (control && options_.navigation != Navigation::None)
        {
            const int count = static_cast<int>(pages_.size());
            SelectPage((page_ + (shift ? count - 1 : 1)) % count);
        }
        else
        {
            ChangeFocus(shift ? -1 : 1);
        }
        return;

    case VK_ESCAPE:
        if (onClose)
        {
            onClose();
        }
        return;

    case VK_RETURN:
        if (focusVisible_ && focus_.target == Target::Footer)
        {
            Activate(focus_);
            return;
        }
        for (const Button& button : buttons_)
        {
            if (button.role == ButtonRole::Primary && onButton)
            {
                onButton(button.id);
                return;
            }
        }
        return;

    case VK_SPACE:
        if (focus_.target != Target::None && focus_.target != Target::Nav)
        {
            if (!(focus_.target == Target::Control
                  && controls_[focus_.index]->kind == Kind::Slider))
            {
                Activate(focus_);
            }
        }
        return;

    case VK_LEFT:
    case VK_RIGHT:
    case VK_UP:
    case VK_DOWN:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_HOME:
    case VK_END:
        break;

    default:
        return;
    }

    if (focus_.target == Target::Control)
    {
        Control& c = *controls_[focus_.index];
        if (c.kind == Kind::Slider && c.intValue)
        {
            const int big = std::max(c.step, (c.maximum - c.minimum) / 10);
            int value = *c.intValue;
            switch (key)
            {
            case VK_LEFT: case VK_DOWN: value -= c.step; break;
            case VK_RIGHT: case VK_UP: value += c.step; break;
            case VK_PRIOR: value += big; break;
            case VK_NEXT: value -= big; break;
            case VK_HOME: value = c.minimum; break;
            case VK_END: value = c.maximum; break;
            default: break;
            }
            value = std::clamp(value, c.minimum, c.maximum);
            if (value != *c.intValue)
            {
                *c.intValue = value;
                Notify(c);
            }
            focusVisible_ = true;
            return;
        }
        if (c.kind == Kind::Choice && c.intValue && !c.options.empty()
            && (key == VK_UP || key == VK_DOWN))
        {
            const int count = static_cast<int>(c.options.size());
            *c.intValue = std::clamp(*c.intValue + (key == VK_DOWN ? 1 : -1),
                                     0, count - 1);
            Notify(c);
            return;
        }
    }

    // Scroll the page with the keyboard when nothing else wants the keys.
    if (!pages_.empty())
    {
        Page& page = pages_[page_];
        const float viewH = Height(Viewport());
        switch (key)
        {
        case VK_UP: SetScroll(page, page.scroll - 40.0f); break;
        case VK_DOWN: SetScroll(page, page.scroll + 40.0f); break;
        case VK_PRIOR: SetScroll(page, page.scroll - viewH); break;
        case VK_NEXT: SetScroll(page, page.scroll + viewH); break;
        case VK_HOME: SetScroll(page, 0.0f); break;
        case VK_END: SetScroll(page, page.contentHeight); break;
        default: break;
        }
    }
}

void Form::Activate(const Hit& hit)
{
    switch (hit.target)
    {
    case Target::Footer:
        if (onButton)
        {
            onButton(buttons_[hit.index].id);
        }
        return;

    case Target::Nav:
        SelectPage(hit.index);
        return;

    case Target::ControlButton:
    {
        Control& control = *controls_[hit.index];
        if (control.onButton && IsEnabled(control))
        {
            control.onButton();
            Invalidate();
        }
        return;
    }

    case Target::Control:
        break;

    default:
        return;
    }

    Control& control = *controls_[hit.index];
    if (!IsEnabled(control))
    {
        return;
    }

    switch (control.kind)
    {
    case Kind::Switch:
        if (control.boolValue)
        {
            *control.boolValue = !*control.boolValue;
            Notify(control);
            StartAnimation();
        }
        break;

    case Kind::Choice:
    {
        if (!control.intValue || control.options.empty())
        {
            break;
        }
        std::vector<MenuItem> items;
        for (size_t i = 0; i < control.options.size(); ++i)
        {
            items.push_back(MenuItem::Command(
                static_cast<UINT>(i + 1), control.options[i], true,
                static_cast<int>(i) == *control.intValue));
        }
        const D2D1_RECT_F box = ToClient(pages_[control.page], control.box);
        POINT anchor{static_cast<LONG>(std::lround(Px(box.left))),
                     static_cast<LONG>(std::lround(Px(box.top)))};
        ClientToScreen(hwnd_, &anchor);
        MenuOptions options;
        options.alignItem = *control.intValue;
        options.anchorHeight = static_cast<int>(std::lround(Px(Height(box))));
        options.minWidth = static_cast<int>(std::lround(Px(Width(box))));
        const UINT chosen = TrackMenu(hwnd_, items, anchor, options);
        if (chosen > 0 && static_cast<int>(chosen - 1) != *control.intValue)
        {
            *control.intValue = static_cast<int>(chosen - 1);
            Notify(control);
        }
        Invalidate();
        break;
    }

    case Kind::Color:
    {
        if (!control.colorValue)
        {
            break;
        }
        static COLORREF customColors[16] = {};
        CHOOSECOLORW chooser{};
        chooser.lStructSize = sizeof(chooser);
        chooser.hwndOwner = hwnd_;
        chooser.rgbResult = *control.colorValue;
        chooser.lpCustColors = customColors;
        chooser.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR;
        if (ChooseColorW(&chooser))
        {
            *control.colorValue = chooser.rgbResult;
            Notify(control);
        }
        break;
    }

    case Kind::Edit:
        if (control.edit)
        {
            SetFocus(control.edit);
        }
        break;

    default:
        break;
    }
}

void Form::Notify(Control& control)
{
    if (onChange)
    {
        onChange(control);
    }
    if (hwnd_)
    {
        PositionEdits();
        Invalidate();
    }
}

void Form::StartAnimation()
{
    if (hwnd_ && !timerRunning_)
    {
        SetTimer(hwnd_, kAnimTimer, 15, nullptr);
        timerRunning_ = true;
    }
}

bool Form::StepAnimation()
{
    bool active = false;
    for (auto& control : controls_)
    {
        if (control->kind != Kind::Switch || !control->boolValue)
        {
            continue;
        }
        const float target = *control->boolValue ? 1.0f : 0.0f;
        if (std::abs(control->knob - target) > 0.001f)
        {
            const float step = 15.0f / 140.0f;
            control->knob = target > control->knob
                ? std::min(target, control->knob + step)
                : std::max(target, control->knob - step);
            active = true;
        }
    }

    const ULONGLONG elapsed = GetTickCount64() - scrollbarShownAt_;
    float alpha = 0.0f;
    if (elapsed < 900)
    {
        alpha = 1.0f;
    }
    else if (elapsed < 1150)
    {
        alpha = 1.0f - static_cast<float>(elapsed - 900) / 250.0f;
    }
    if (pressed_.target == Target::Thumb)
    {
        alpha = 1.0f;
        scrollbarShownAt_ = GetTickCount64();
    }
    if (alpha != scrollbarAlpha_ || alpha > 0.0f)
    {
        active = active || alpha > 0.0f;
        scrollbarAlpha_ = alpha;
    }
    return active;
}

void Form::Invalidate()
{
    if (hwnd_)
    {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

// --- window procedure --------------------------------------------------------

LRESULT CALLBACK Form::WindowProc(HWND hwnd, UINT message, WPARAM wParam,
                                  LPARAM lParam)
{
    Form* self = nullptr;
    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<Form*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self)
        {
            self->hwnd_ = hwnd;
        }
    }
    else
    {
        self = reinterpret_cast<Form*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (!self || self->hwnd_ != hwnd)
    {
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return self->HandleMessage(message, wParam, lParam);
}

LRESULT CALLBACK Form::EditSubclassProc(HWND hwnd, UINT message, WPARAM wParam,
                                        LPARAM lParam, UINT_PTR, DWORD_PTR data)
{
    auto* self = reinterpret_cast<Form*>(data);
    if (message == WM_KEYDOWN
        && (wParam == VK_TAB || wParam == VK_RETURN || wParam == VK_ESCAPE))
    {
        if (self && self->hwnd_)
        {
            if (wParam == VK_TAB)
            {
                for (size_t i = 0; i < self->controls_.size(); ++i)
                {
                    if (self->controls_[i]->edit == hwnd)
                    {
                        self->focus_ = Hit{Target::Control, static_cast<int>(i)};
                    }
                }
            }
            self->OnKey(wParam);
        }
        return 0;
    }
    if (message == WM_CHAR && (wParam == L'\t' || wParam == L'\r'
                               || wParam == 0x1B))
    {
        return 0;
    }
    if (message == WM_NCDESTROY)
    {
        RemoveWindowSubclass(hwnd, &Form::EditSubclassProc, 1);
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

LRESULT Form::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_PAINT:
        Paint();
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_SIZE:
        Layout();
        Invalidate();
        return 0;

    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        OnMouseDown(Dip(static_cast<float>(GET_X_LPARAM(lParam))),
                    Dip(static_cast<float>(GET_Y_LPARAM(lParam))));
        return 0;

    case WM_MOUSEMOVE:
        OnMouseMove(Dip(static_cast<float>(GET_X_LPARAM(lParam))),
                    Dip(static_cast<float>(GET_Y_LPARAM(lParam))));
        return 0;

    case WM_LBUTTONUP:
        OnMouseUp(Dip(static_cast<float>(GET_X_LPARAM(lParam))),
                  Dip(static_cast<float>(GET_Y_LPARAM(lParam))));
        return 0;

    case WM_CAPTURECHANGED:
        if (pressed_.target != Target::None
            && reinterpret_cast<HWND>(lParam) != hwnd_)
        {
            pressed_ = Hit{};
            Invalidate();
        }
        return 0;

    case WM_MOUSELEAVE:
        tracking_ = false;
        if (hover_.target != Target::None)
        {
            hover_ = Hit{};
            Invalidate();
        }
        return 0;

    case WM_MOUSEWHEEL:
        OnWheel(GET_WHEEL_DELTA_WPARAM(wParam));
        return 0;

    case WM_KEYDOWN:
        OnKey(wParam);
        return 0;

    case WM_CHAR:
        return 0;

    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS;

    case WM_TIMER:
        if (wParam == kAnimTimer)
        {
            if (!StepAnimation())
            {
                KillTimer(hwnd_, kAnimTimer);
                timerRunning_ = false;
            }
            Invalidate();
        }
        return 0;

    case WM_COMMAND:
    {
        const HWND source = reinterpret_cast<HWND>(lParam);
        const WORD code = HIWORD(wParam);
        for (size_t i = 0; i < controls_.size(); ++i)
        {
            Control& control = *controls_[i];
            if (!source || control.edit != source)
            {
                continue;
            }
            if (code == EN_CHANGE && !syncing_)
            {
                if (control.textValue)
                {
                    const int length = GetWindowTextLengthW(source);
                    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
                    const int written = GetWindowTextW(source, text.data(),
                                                       length + 1);
                    text.resize(written > 0 ? static_cast<size_t>(written) : 0);
                    *control.textValue = text;
                }
                if (onChange)
                {
                    onChange(control);
                }
                Invalidate();
            }
            else if (code == EN_SETFOCUS)
            {
                focus_ = Hit{Target::Control, static_cast<int>(i)};
                Invalidate();
            }
            else if (code == EN_KILLFOCUS)
            {
                Invalidate();
            }
            break;
        }
        return 0;
    }

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
    {
        HDC dc = reinterpret_cast<HDC>(wParam);
        const HWND control = reinterpret_cast<HWND>(lParam);
        const bool enabled = IsWindowEnabled(control) != FALSE;
        SetTextColor(dc, ToColorRef(enabled ? palette_->text
                                            : palette_->tertiaryText));
        SetBkColor(dc, ToColorRef(palette_->fieldBg));
        return reinterpret_cast<LRESULT>(fieldBrush_);
    }

    case WM_SETTINGCHANGE:
        if (lParam && wcscmp(reinterpret_cast<const wchar_t*>(lParam),
                             L"ImmersiveColorSet") == 0)
        {
            UpdateTheme();
        }
        break;

    case WM_DPICHANGED:
    {
        scale_ = static_cast<float>(HIWORD(wParam)) / 96.0f;
        if (target_)
        {
            target_->SetDpi(96.0f * scale_, 96.0f * scale_);
        }
        CreateEditFonts();
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
        Invalidate();
        return 0;
    }

    case WM_CLOSE:
        if (onClose)
        {
            onClose();
        }
        return 0;

    case WM_NCDESTROY:
    {
        HWND hwnd = hwnd_;
        hwnd_ = nullptr;
        timerRunning_ = false;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    default:
        break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

} // namespace ld::ui

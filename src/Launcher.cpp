#include "Launcher.h"

#include "AppLauncher.h"

#include <shellapi.h>
#include <shlobj.h>

namespace ld
{

namespace
{

// Local copies so the build does not depend on which GUIDs a given MinGW
// libuuid happens to export.
const GUID kFolderIdAppsFolder =
    {0x1e87508d, 0x89c2, 0x42f0, {0x8a, 0x7e, 0x64, 0x5a, 0x0f, 0x50, 0xca, 0x58}};
const GUID kBhidEnumItems =
    {0x94f60519, 0x2850, 0x4924, {0xaa, 0x5a, 0xd1, 0x5e, 0x84, 0x86, 0x80, 0x39}};
const PROPERTYKEY kLinkTargetParsingPath =
    {{0xb9b4b3fc, 0x2b51, 0x4a42, {0xb5, 0xd8, 0x32, 0x41, 0x46, 0xaf, 0xcf, 0x25}}, 2};

constexpr wchar_t kLauncherClass[] = L"LightDock_Launcher";
constexpr UINT_PTR kFrameTimer = 1;
constexpr UINT_PTR kCaretTimer = 2;
constexpr UINT kRefreshMessage = WM_APP + 1;

constexpr float kMargin = 24.0f;      // shadow room around the body
constexpr float kRadius = 14.0f;
constexpr float kTailLength = 10.0f;
constexpr float kTailHalf = 10.0f;
constexpr float kTipGap = 10.0f;
constexpr float kPad = 14.0f;

// Start layout
constexpr float kStartWidth = 560.0f;
constexpr float kStartHeight = 600.0f;
constexpr float kStartFieldH = 34.0f;
constexpr float kFooterH = 50.0f;
constexpr int kGridColumns = 5;
constexpr float kCellH = 106.0f;
constexpr float kGridTitleH = 28.0f;
constexpr float kGroupHeaderH = 40.0f;

// Search layout
constexpr float kSearchWidth = 640.0f;
constexpr float kSearchFieldH = 54.0f;
constexpr float kRowH = 44.0f;
constexpr int kSearchRows = 8;

std::wstring Lower(const std::wstring& text)
{
    if (text.empty())
    {
        return text;
    }
    std::wstring result(text.size(), L'\0');
    const int written = LCMapStringEx(
        LOCALE_NAME_USER_DEFAULT, LCMAP_LOWERCASE, text.c_str(),
        static_cast<int>(text.size()), result.data(),
        static_cast<int>(result.size()), nullptr, nullptr, 0);
    if (written <= 0)
    {
        return text;
    }
    result.resize(static_cast<size_t>(written));
    return result;
}

std::wstring FileNameOf(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

/// "{KNOWNFOLDERID}\rest" -> absolute path (AppsFolder parsing names of
/// desktop apps use this form).
std::wstring ExpandKnownFolderPath(const std::wstring& parsingName)
{
    if (parsingName.size() < 40 || parsingName[0] != L'{')
    {
        return {};
    }
    const size_t close = parsingName.find(L'}');
    if (close == std::wstring::npos || close + 1 >= parsingName.size()
        || parsingName[close + 1] != L'\\')
    {
        return {};
    }

    GUID id{};
    const std::wstring guid = parsingName.substr(0, close + 1);
    if (FAILED(CLSIDFromString(guid.c_str(), &id)))
    {
        return {};
    }

    PWSTR folder = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &folder)))
    {
        result = std::wstring(folder) + parsingName.substr(close + 1);
    }
    CoTaskMemFree(folder);
    return result;
}

bool IsExecutablePath(const std::wstring& path)
{
    const std::wstring lower = Lower(path);
    return lower.size() > 4
        && lower.compare(lower.size() - 4, 4, L".exe") == 0
        && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool IsWebAddress(const std::wstring& lower)
{
    return lower.rfind(L"http://", 0) == 0 || lower.rfind(L"https://", 0) == 0
        || lower.rfind(L"mailto:", 0) == 0 || lower.rfind(L"www.", 0) == 0;
}

bool HasDocumentExtension(const std::wstring& lower)
{
    static const wchar_t* const kDocuments[] =
    {
        L".pdf", L".txt", L".rtf", L".md", L".htm", L".html", L".mht",
        L".chm", L".hlp", L".doc", L".docx", L".xls", L".xlsx", L".ppt",
        L".pptx", L".xml", L".json", L".ini", L".log", L".cfg", L".nfo",
        L".url", L".website", L".lnk", L".ico", L".png", L".jpg",
    };
    const size_t dot = lower.find_last_of(L'.');
    const size_t slash = lower.find_last_of(L"\\/");
    if (dot == std::wstring::npos
        || (slash != std::wstring::npos && dot < slash))
    {
        return false;
    }
    const std::wstring extension = lower.substr(dot);
    for (const wchar_t* candidate : kDocuments)
    {
        if (extension == candidate)
        {
            return true;
        }
    }
    return false;
}

/// True for entries that start a program: packaged apps, executables and
/// app protocols (steam://...). False for manuals, readmes and web links
/// that installers also drop into the Start menu.
bool IsProgramEntry(const std::wstring& parsingName, const std::wstring& resolved,
                    std::wstring& url)
{
    const std::wstring name = Lower(parsingName);
    const std::wstring target = Lower(resolved);

    // Folder shortcuts ("KeyShot Resources") open Explorer, not a program.
    if (!resolved.empty())
    {
        const DWORD attributes = GetFileAttributesW(resolved.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES
            && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            return false;
        }
    }

    if (IsWebAddress(name) || IsWebAddress(target))
    {
        return false;
    }

    // Internet shortcuts: keep app protocols, drop web pages.
    if (target.size() > 4 && target.compare(target.size() - 4, 4, L".url") == 0)
    {
        wchar_t buffer[2048] = {};
        GetPrivateProfileStringW(L"InternetShortcut", L"URL", L"", buffer,
                                 ARRAYSIZE(buffer), resolved.c_str());
        const std::wstring lowerUrl = Lower(buffer);
        url = buffer;
        return !lowerUrl.empty() && !IsWebAddress(lowerUrl)
            && lowerUrl.rfind(L"file:", 0) != 0;
    }

    if (HasDocumentExtension(target)
        || (target.empty() && HasDocumentExtension(name)))
    {
        return false;
    }
    return true;
}

// --- smart groups ------------------------------------------------------------

bool IsAsciiWordChar(wchar_t c)
{
    return (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9');
}

/// Plain English keywords ("qq", "photos", "edge") must match a whole word
/// so they do not fire inside longer names ("Photoshop" is not "photos").
/// Keywords with punctuation ("steam://", "\\steamapps\\") and Chinese
/// keywords match as substrings.
bool HasKeyword(const std::wstring& haystack, const wchar_t* keyword)
{
    const size_t length = wcslen(keyword);
    bool wholeWord = length > 0;
    for (size_t i = 0; i < length; ++i)
    {
        wholeWord = wholeWord && IsAsciiWordChar(keyword[i]);
    }

    size_t at = haystack.find(keyword);
    while (at != std::wstring::npos)
    {
        if (!wholeWord)
        {
            return true;
        }
        const bool left = at == 0 || !IsAsciiWordChar(haystack[at - 1]);
        const bool right = at + length >= haystack.size()
            || !IsAsciiWordChar(haystack[at + length]);
        if (left && right)
        {
            return true;
        }
        at = haystack.find(keyword, at + 1);
    }
    return false;
}

struct CategoryRule
{
    int category;
    std::vector<const wchar_t*> keywords;
};

/// Checked in order; the first rule with a matching keyword wins. Order
/// matters: "QQ音乐" is media before "qq" can make it a chat app, Acrobat
/// is office before "adobe" makes it a design tool.
const std::vector<CategoryRule>& CategoryRules()
{
    static const std::vector<CategoryRule> rules =
    {
        {AppCatalog::Games, {
            L"steam://", L"com.epicgames.launcher://", L"uplay://",
            L"battlenet://", L"origin://", L"goggalaxy://", L"\\steamapps\\",
            L"\\epic games\\", L"\\riot games\\", L"\\wegame", L"steam",
            L"epic games", L"battle.net", L"wegame", L"ubisoft", L"ea app",
            L"gog galaxy", L"xbox", L"minecraft", L"game", L"games", L"游戏",
            L"原神", L"genshin", L"英雄联盟", L"league of legends"}},
        {AppCatalog::AI, {
            L"chatgpt", L"openai", L"claude", L"anthropic", L"codex",
            L"copilot", L"deepseek", L"kimi", L"豆包", L"doubao", L"通义",
            L"qwen", L"文心", L"gemini", L"ollama", L"lm studio", L"comfyui",
            L"stable diffusion", L"midjourney", L"perplexity"}},
        {AppCatalog::Media, {
            L"music", L"音乐", L"video", L"视频", L"player", L"播放器",
            L"抖音", L"douyin", L"tiktok", L"bilibili", L"哔哩", L"spotify",
            L"netflix", L"vlc", L"potplayer", L"foobar", L"itunes",
            L"cloudmusic", L"网易云", L"kugou", L"酷狗", L"kuwo", L"酷我",
            L"爱奇艺", L"iqiyi", L"优酷", L"youku", L"twitch", L"photos",
            L"照片", L"camera", L"相机", L"直播", L"影音", L"电影", L"podcast"}},
        {AppCatalog::Communication, {
            L"wechat", L"weixin", L"微信", L"qq", L"tim", L"telegram",
            L"discord", L"slack", L"teams", L"zoom", L"skype", L"钉钉",
            L"dingtalk", L"飞书", L"feishu", L"lark", L"whatsapp", L"line",
            L"signal", L"mail", L"邮件", L"foxmail", L"thunderbird",
            L"腾讯会议", L"wemeet", L"wxwork", L"phone link", L"手机连接"}},
        {AppCatalog::Office, {
            L"acrobat", L"pdf", L"word", L"excel", L"powerpoint", L"onenote",
            L"outlook", L"access", L"publisher", L"office", L"wps",
            L"notion", L"obsidian", L"typora", L"evernote", L"印象笔记",
            L"有道", L"youdao", L"xmind", L"notepad", L"记事本", L"便笺",
            L"sticky notes", L"calendar", L"日历", L"to do", L"todo",
            L"翻译", L"translate", L"词典", L"dictionary", L"文档",
            L"表格", L"幻灯片"}},
        {AppCatalog::Design, {
            L"adobe", L"photoshop", L"illustrator", L"after effects",
            L"premiere", L"lightroom", L"indesign", L"audition", L"blender",
            L"maya", L"3ds max", L"cinema 4d", L"c4d", L"houdini", L"zbrush",
            L"substance", L"figma", L"sketch", L"krita", L"gimp",
            L"coreldraw", L"affinity", L"davinci", L"resolve", L"capcut",
            L"剪映", L"pureref", L"unreal", L"marmoset", L"keyshot",
            L"rhino", L"autocad", L"sketchup", L"clip studio", L"toon boom",
            L"spine", L"aseprite", L"obs", L"nuke", L"character creator",
            L"iclone", L"topaz", L"paint", L"画图", L"maxon", L"autodesk",
            L"sidefx", L"foundry", L"pixologic", L"设计"}},
        {AppCatalog::Development, {
            L"visual studio", L"vs code", L"vscode", L"jetbrains",
            L"intellij", L"pycharm", L"webstorm", L"clion", L"rider",
            L"goland", L"android studio", L"python", L"node.js", L"nodejs",
            L"git", L"github", L"powershell", L"terminal", L"终端", L"cmd",
            L"命令提示符", L"command prompt", L"wsl", L"ubuntu", L"docker",
            L"postman", L"sublime", L"cmake", L"nsight", L"cuda", L"unity",
            L"godot", L"eclipse", L"qt creator", L"mingw", L"msys", L"conda",
            L"jupyter", L"sql", L"navicat", L"dbeaver", L"wireshark",
            L"fiddler", L"cursor", L"windows kits", L"sdk", L"developer",
            L"debug", L"开发"}},
        {AppCatalog::Network, {
            L"chrome", L"edge", L"firefox", L"browser", L"浏览器", L"opera",
            L"brave", L"vivaldi", L"vpn", L"radmin", L"parsec", L"parsecvdisplay", L"todesk",
            L"anydesk", L"teamviewer", L"remote desktop", L"远程", L"sunlogin",
            L"向日葵", L"clash", L"v2ray", L"proxy", L"download", L"下载",
            L"迅雷", L"thunder", L"idm", L"qbittorrent", L"utorrent", L"网盘",
            L"onedrive", L"dropbox", L"google drive", L"filezilla",
            L"winscp", L"putty", L"xshell", L"ssh"}},
        {AppCatalog::System, {
            L"nvidia", L"realtek", L"intel", L"amd", L"advanced micro devices",
            L"logitech", L"razer", L"corsair", L"asus", L"msi", L"dell",
            L"lenovo", L"synaptics", L"control panel", L"控制面板",
            L"settings", L"设置", L"task manager", L"cleanmymac", L"任务管理器", L"registry",
            L"注册表", L"odbc", L"driver", L"驱动", L"clean", L"defender",
            L"security", L"安全", L"backup", L"备份", L"7-zip", L"winrar",
            L"bandizip", L"everything", L"disk", L"磁盘", L"system", L"系统",
            L"snipping", L"截图", L"calculator", L"计算器", L"character map",
            L"magnifier", L"放大镜", L"narrator", L"讲述人", L"keyboard",
            L"recovery", L"恢复", L"update", L"更新", L"store", L"商店",
            L"clock", L"时钟", L"工具", L"tools", L"utility", L"manager",
            L"管理器", L"microsoft.windows"}},
    };
    return rules;
}

std::wstring FileCompany(const std::wstring& path)
{
    if (path.empty())
    {
        return {};
    }
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (size == 0)
    {
        return {};
    }
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()))
    {
        return {};
    }

    struct Translation
    {
        WORD language;
        WORD codePage;
    };
    Translation* translations = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation",
                        reinterpret_cast<void**>(&translations), &length)
        || length < sizeof(Translation))
    {
        return {};
    }

    wchar_t key[64];
    swprintf(key, 64, L"\\StringFileInfo\\%04x%04x\\CompanyName",
             translations[0].language, translations[0].codePage);
    wchar_t* value = nullptr;
    UINT valueLength = 0;
    if (VerQueryValueW(data.data(), key, reinterpret_cast<void**>(&value),
                       &valueLength)
        && value && valueLength > 0)
    {
        return value;
    }
    return {};
}

int Classify(const std::wstring& name, const std::wstring& parsingName,
             const std::wstring& resolved, const std::wstring& url,
             const std::wstring& executable)
{
    const std::wstring haystack = Lower(name) + L" | " + Lower(parsingName)
        + L" | " + Lower(resolved) + L" | " + Lower(url) + L" | "
        + Lower(FileCompany(executable));

    for (const CategoryRule& rule : CategoryRules())
    {
        for (const wchar_t* keyword : rule.keywords)
        {
            if (HasKeyword(haystack, keyword))
            {
                return rule.category;
            }
        }
    }

    // Anything else that lives in the Windows folder is a system tool.
    wchar_t windows[MAX_PATH] = {};
    const UINT written = GetWindowsDirectoryW(windows, MAX_PATH);
    if (written > 0 && !resolved.empty())
    {
        const std::wstring folder = Lower(std::wstring(windows, written)) + L"\\";
        if (Lower(resolved).rfind(folder, 0) == 0)
        {
            return AppCatalog::System;
        }
    }
    return AppCatalog::Other;
}

D2D1_RECT_F Inset(const D2D1_RECT_F& r, float dx, float dy)
{
    return D2D1::RectF(r.left + dx, r.top + dy, r.right - dx, r.bottom - dy);
}

bool Contains(const D2D1_RECT_F& r, float x, float y)
{
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

D2D1_COLOR_F WithAlpha(D2D1_COLOR_F color, float alpha)
{
    color.a *= alpha;
    return color;
}

ComPtr<ID2D1PathGeometry> BubbleGeometry(const D2D1_RECT_F& body, float radius,
                                         DockEdge edge, D2D1_POINT_2F tip,
                                         float halfBase)
{
    ComPtr<ID2D1PathGeometry> geometry;
    ID2D1Factory* factory = ui::Direct2D();
    if (!factory || FAILED(factory->CreatePathGeometry(&geometry)))
    {
        return {};
    }
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geometry->Open(&sink)))
    {
        return {};
    }

    const float l = body.left, t = body.top, r = body.right, b = body.bottom;
    const float rad = (std::min)(radius, (std::min)(r - l, b - t) * 0.5f);
    auto along = [&](float value, float from, float to)
    {
        const float lo = from + rad + halfBase;
        const float hi = to - rad - halfBase;
        return lo <= hi ? ClampF(value, lo, hi) : (from + to) * 0.5f;
    };
    auto corner = [&](float x, float y)
    {
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(x, y),
                                      D2D1::SizeF(rad, rad), 0.0f,
                                      D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                      D2D1_ARC_SIZE_SMALL));
    };

    // The tail sits on the side facing the Dock.
    sink->BeginFigure(D2D1::Point2F(l + rad, t), D2D1_FIGURE_BEGIN_FILLED);
    if (edge == DockEdge::Top)
    {
        const float c = along(tip.x, l, r);
        sink->AddLine(D2D1::Point2F(c - halfBase, t));
        sink->AddLine(tip);
        sink->AddLine(D2D1::Point2F(c + halfBase, t));
    }
    sink->AddLine(D2D1::Point2F(r - rad, t));
    corner(r, t + rad);
    if (edge == DockEdge::Right)
    {
        const float c = along(tip.y, t, b);
        sink->AddLine(D2D1::Point2F(r, c - halfBase));
        sink->AddLine(tip);
        sink->AddLine(D2D1::Point2F(r, c + halfBase));
    }
    sink->AddLine(D2D1::Point2F(r, b - rad));
    corner(r - rad, b);
    if (edge == DockEdge::Bottom)
    {
        const float c = along(tip.x, l, r);
        sink->AddLine(D2D1::Point2F(c + halfBase, b));
        sink->AddLine(tip);
        sink->AddLine(D2D1::Point2F(c - halfBase, b));
    }
    sink->AddLine(D2D1::Point2F(l + rad, b));
    corner(l, b - rad);
    if (edge == DockEdge::Left)
    {
        const float c = along(tip.y, t, b);
        sink->AddLine(D2D1::Point2F(l, c + halfBase));
        sink->AddLine(tip);
        sink->AddLine(D2D1::Point2F(l, c - halfBase));
    }
    sink->AddLine(D2D1::Point2F(l, t + rad));
    corner(l + rad, t);
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close()))
    {
        return {};
    }
    return geometry;
}

void ShellOpen(const wchar_t* file, const wchar_t* parameters = nullptr,
               int show = SW_SHOWNORMAL)
{
    AllowSetForegroundWindow(ASFW_ANY);
    ShellExecuteW(nullptr, L"open", file, parameters, nullptr, show);
}

} // namespace

// ---------------------------------------------------------------------------
// AppCatalog
// ---------------------------------------------------------------------------

void AppCatalog::Refresh(bool force)
{
    const ULONGLONG now = GetTickCount64();
    if (!force && !apps_.empty() && now - refreshedAt_ < 30000)
    {
        return;
    }

    ComPtr<IShellItem> folder;
    if (FAILED(SHGetKnownFolderItem(kFolderIdAppsFolder, KF_FLAG_DEFAULT,
                                    nullptr, IID_PPV_ARGS(&folder))))
    {
        return;
    }
    ComPtr<IEnumShellItems> items;
    if (FAILED(folder->BindToHandler(nullptr, kBhidEnumItems,
                                     IID_PPV_ARGS(&items))))
    {
        return;
    }

    std::vector<App> fresh;
    while (true)
    {
        ComPtr<IShellItem> item;
        ULONG fetched = 0;
        if (items->Next(1, &item, &fetched) != S_OK || fetched == 0)
        {
            break;
        }

        App app;
        PWSTR text = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &text)) && text)
        {
            app.name = text;
        }
        CoTaskMemFree(text);
        text = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &text)) && text)
        {
            app.parsingName = text;
        }
        CoTaskMemFree(text);

        if (app.name.empty() || app.parsingName.empty())
        {
            continue;
        }

        app.lowerName = Lower(app.name);

        // Uninstallers are listed in AppsFolder but are not apps anyone
        // wants to launch from a dock.
        if (app.lowerName.find(L"uninstall") != std::wstring::npos
            || app.name.find(L"卸载") != std::wstring::npos)
        {
            continue;
        }

        ComPtr<IShellItem2> item2;
        PWSTR target = nullptr;
        std::wstring resolved;
        if (SUCCEEDED(item->QueryInterface(IID_PPV_ARGS(&item2)))
            && SUCCEEDED(item2->GetString(kLinkTargetParsingPath, &target))
            && target)
        {
            resolved = target;
        }
        CoTaskMemFree(target);
        if (resolved.empty())
        {
            resolved = ExpandKnownFolderPath(app.parsingName);
        }

        // The Start menu folders also hold manuals, readmes and web links.
        // Only keep things that start a program.
        std::wstring url;
        if (!IsProgramEntry(app.parsingName, resolved, url))
        {
            continue;
        }
        app.targetPath = IsExecutablePath(resolved) ? resolved : std::wstring();
        app.lowerTarget = Lower(FileNameOf(app.targetPath));
        app.item = item;

        // Keep icons and groups that were already worked out for this app.
        for (App& old : apps_)
        {
            if (old.parsingName == app.parsingName)
            {
                app.icon = old.icon;
                app.iconLoaded = old.iconLoaded;
                app.category = old.category;
                break;
            }
        }
        if (app.category < 0)
        {
            app.category = Classify(app.name, app.parsingName, resolved, url,
                                    app.targetPath);
        }

        bool duplicate = false;
        for (const App& existing : fresh)
        {
            if (existing.parsingName == app.parsingName)
            {
                duplicate = true;
                break;
            }
        }
        if (!duplicate)
        {
            fresh.push_back(std::move(app));
        }
    }

    std::sort(fresh.begin(), fresh.end(), [](const App& a, const App& b)
    {
        return CompareStringEx(LOCALE_NAME_USER_DEFAULT,
                               NORM_IGNORECASE | SORT_DIGITSASNUMBERS,
                               a.name.c_str(), static_cast<int>(a.name.size()),
                               b.name.c_str(), static_cast<int>(b.name.size()),
                               nullptr, nullptr, 0) == CSTR_LESS_THAN;
    });

    apps_ = std::move(fresh);
    refreshedAt_ = now;
}

const wchar_t* AppCatalog::CategoryTitle(int category)
{
    static const wchar_t* const titles[CategoryCount] =
    {
        L"设计创作", L"开发工具", L"办公效率", L"AI 助手", L"通讯社交",
        L"影音娱乐", L"游戏", L"网络与远程", L"系统工具", L"其他",
    };
    return category >= 0 && category < CategoryCount ? titles[category]
                                                     : titles[Other];
}

IWICBitmapSource* AppCatalog::Icon(App& app)
{
    if (app.iconLoaded)
    {
        return app.icon.Get();
    }
    app.iconLoaded = true;

    if (!wic_)
    {
        CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                         CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_));
    }
    ComPtr<IShellItemImageFactory> factory;
    if (!wic_ || !app.item
        || FAILED(app.item->QueryInterface(IID_PPV_ARGS(&factory))))
    {
        return nullptr;
    }

    // 128px source: big enough to downscale cleanly to every size the
    // panel draws (grid, list and the small group previews).
    HBITMAP bitmap = nullptr;
    if (FAILED(factory->GetImage(SIZE{128, 128},
                                 SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK,
                                 &bitmap))
        || !bitmap)
    {
        return nullptr;
    }

    ComPtr<IWICBitmap> wicBitmap;
    if (SUCCEEDED(wic_->CreateBitmapFromHBITMAP(
            bitmap, nullptr, WICBitmapUsePremultipliedAlpha, &wicBitmap)))
    {
        wicBitmap->QueryInterface(IID_PPV_ARGS(&app.icon));
    }
    DeleteObject(bitmap);

    // Analyse the artwork once: where it is, whether it is already a
    // full-bleed tile (then it only gets rounded corners) or a free-form
    // glyph (then it sits on a rounded plate), and whether it is so light
    // that a white plate would swallow it.
    UINT width = 0, height = 0;
    if (app.icon && SUCCEEDED(app.icon->GetSize(&width, &height))
        && width > 0 && height > 0)
    {
        std::vector<BYTE> pixels(static_cast<size_t>(width) * height * 4);
        ComPtr<IWICFormatConverter> converter;
        if (SUCCEEDED(wic_->CreateFormatConverter(&converter))
            && SUCCEEDED(converter->Initialize(
                app.icon.Get(), GUID_WICPixelFormat32bppPBGRA,
                WICBitmapDitherTypeNone, nullptr, 0.0,
                WICBitmapPaletteTypeCustom))
            && SUCCEEDED(converter->CopyPixels(
                nullptr, width * 4, static_cast<UINT>(pixels.size()),
                pixels.data())))
        {
            int left = static_cast<int>(width), top = static_cast<int>(height);
            int right = -1, bottom = -1;
            for (UINT y = 0; y < height; ++y)
            {
                for (UINT x = 0; x < width; ++x)
                {
                    if (pixels[(static_cast<size_t>(y) * width + x) * 4 + 3] > 24)
                    {
                        left = (std::min)(left, static_cast<int>(x));
                        top = (std::min)(top, static_cast<int>(y));
                        right = (std::max)(right, static_cast<int>(x));
                        bottom = (std::max)(bottom, static_cast<int>(y));
                    }
                }
            }

            if (right >= left && bottom >= top)
            {
                const int boxW = right - left + 1;
                const int boxH = bottom - top + 1;
                int opaque = 0;
                double luminance = 0.0;
                int counted = 0;
                for (int y = top; y <= bottom; ++y)
                {
                    for (int x = left; x <= right; ++x)
                    {
                        const BYTE* px = &pixels[(static_cast<size_t>(y) * width + x) * 4];
                        if (px[3] > 200)
                        {
                            ++opaque;
                        }
                        if (px[3] > 128)
                        {
                            const double a = px[3] / 255.0;
                            luminance += (0.0722 * px[0] + 0.7152 * px[1]
                                          + 0.2126 * px[2]) / 255.0 / a;
                            ++counted;
                        }
                    }
                }
                const double coverage = static_cast<double>(opaque)
                    / (static_cast<double>(boxW) * boxH);
                const double aspect = static_cast<double>(boxW) / boxH;
                app.tile = coverage > 0.90 && aspect > 0.9 && aspect < 1.1;
                app.lightGlyph = !app.tile && counted > 0
                    && luminance / counted > 0.82;

                // Square crop centred on the artwork, kept inside the image.
                const int side = (std::max)(boxW, boxH);
                int cropX = left + boxW / 2 - side / 2;
                int cropY = top + boxH / 2 - side / 2;
                cropX = std::clamp(cropX, 0, static_cast<int>(width) - side);
                cropY = std::clamp(cropY, 0, static_cast<int>(height) - side);
                app.crop = WICRect{cropX, cropY, side, side};

                // Store apps (ChatGPT, Claude, ...) come as a small logo on a
                // solid tile colour. Clipping that tile leaves the logo tiny:
                // instead keep the tile colour as the plate and zoom in on
                // the logo itself.
                if (app.tile)
                {
                    auto at = [&](int x, int y)
                    {
                        return &pixels[(static_cast<size_t>(y) * width + x) * 4];
                    };
                    // Sample the middle of each edge, a few pixels inside, so
                    // rounded corners and antialiasing do not interfere.
                    double sum[3] = {};
                    std::vector<const BYTE*> samples;
                    const int inset = 3;
                    for (int k = boxW / 5; k < boxW * 4 / 5; ++k)
                    {
                        samples.push_back(at(left + k, top + inset));
                        samples.push_back(at(left + k, bottom - inset));
                    }
                    for (int k = boxH / 5; k < boxH * 4 / 5; ++k)
                    {
                        samples.push_back(at(left + inset, top + k));
                        samples.push_back(at(right - inset, top + k));
                    }
                    for (const BYTE* px : samples)
                    {
                        for (int c = 0; c < 3; ++c)
                        {
                            sum[c] += px[c];
                        }
                    }
                    const double count = samples.empty() ? 1.0 : samples.size();
                    const double mean[3] = {sum[0] / count, sum[1] / count,
                                            sum[2] / count};
                    size_t uniform = 0;
                    for (const BYTE* px : samples)
                    {
                        if (std::abs(px[0] - mean[0]) < 12
                            && std::abs(px[1] - mean[1]) < 12
                            && std::abs(px[2] - mean[2]) < 12)
                        {
                            ++uniform;
                        }
                    }

                    if (!samples.empty() && uniform >= samples.size() * 95 / 100)
                    {
                        int logoL = right, logoT = bottom, logoR = left, logoB = top;
                        for (int y = top; y <= bottom; ++y)
                        {
                            for (int x = left; x <= right; ++x)
                            {
                                const BYTE* px = at(x, y);
                                if (px[3] < 200)
                                {
                                    continue;
                                }
                                const double difference = std::abs(px[0] - mean[0])
                                    + std::abs(px[1] - mean[1])
                                    + std::abs(px[2] - mean[2]);
                                if (difference > 48.0)
                                {
                                    logoL = (std::min)(logoL, x);
                                    logoT = (std::min)(logoT, y);
                                    logoR = (std::max)(logoR, x);
                                    logoB = (std::max)(logoB, y);
                                }
                            }
                        }
                        const int logoSide = (std::max)(logoR - logoL + 1,
                                                        logoB - logoT + 1);
                        if (logoR >= logoL && logoB >= logoT
                            && logoSide < side * 8 / 10)
                        {
                            const int padded = (std::min)(
                                side, logoSide + (std::max)(2, logoSide / 12));
                            int x = (logoL + logoR + 1) / 2 - padded / 2;
                            int y = (logoT + logoB + 1) / 2 - padded / 2;
                            x = std::clamp(x, left, right + 1 - padded);
                            y = std::clamp(y, top, bottom + 1 - padded);
                            app.crop = WICRect{x, y, padded, padded};
                            app.tile = false;
                            app.coloredPlate = true;
                            app.plateColor = D2D1::ColorF(
                                static_cast<float>(mean[2] / 255.0),
                                static_cast<float>(mean[1] / 255.0),
                                static_cast<float>(mean[0] / 255.0));
                        }
                    }
                }
            }
        }
    }
    if (app.crop.Width <= 0)
    {
        app.crop = WICRect{0, 0, static_cast<INT>(width), static_cast<INT>(height)};
    }
    return app.icon.Get();
}

bool AppCatalog::Launch(const App& app, bool elevated)
{
    AllowSetForegroundWindow(ASFW_ANY);

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.nShow = SW_SHOWNORMAL;
    std::wstring file;
    if (elevated && !app.targetPath.empty())
    {
        info.lpVerb = L"runas";
        file = app.targetPath;
    }
    else
    {
        file = L"shell:AppsFolder\\" + app.parsingName;
    }
    info.lpFile = file.c_str();
    return ShellExecuteExW(&info) != FALSE;
}

// ---------------------------------------------------------------------------
// Launcher
// ---------------------------------------------------------------------------

Launcher::~Launcher()
{
    Shutdown();
}

void Launcher::Initialize(HWND owner, Host* host)
{
    owner_ = owner;
    host_ = host;
}

void Launcher::Shutdown()
{
    if (hwnd_)
    {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    visible_ = false;
    brush_.Reset();
    target_.Reset();
    bitmaps_.clear();
    if (surfaceDc_)
    {
        SelectObject(surfaceDc_, surfaceOld_);
        DeleteDC(surfaceDc_);
        surfaceDc_ = nullptr;
    }
    if (surfaceBitmap_)
    {
        DeleteObject(surfaceBitmap_);
        surfaceBitmap_ = nullptr;
    }
}

const wchar_t* Launcher::T(const wchar_t* chinese) const
{
    return host_ ? host_->LauncherText(chinese) : chinese;
}

bool Launcher::GridMode() const
{
    return mode_ == Mode::Start && query_.empty();
}

void Launcher::Show(Mode mode, const RECT& icon, DockEdge edge)
{
    if (!owner_)
    {
        return;
    }

    static bool registered = false;
    if (!registered)
    {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = &Launcher::WindowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = kLauncherClass;
        RegisterClassExW(&windowClass);
        registered = true;
    }

    if (!fieldFormat_)
    {
        fieldFormat_ = ui::CreateTextFormat(14.0f, DWRITE_FONT_WEIGHT_NORMAL);
        bigFieldFormat_ = ui::CreateTextFormat(21.0f, DWRITE_FONT_WEIGHT_NORMAL);
        itemFormat_ = ui::CreateTextFormat(13.5f, DWRITE_FONT_WEIGHT_NORMAL);
        // Grid labels wrap onto a second line before trimming, like the
        // Windows Start menu, instead of cutting every long name short.
        gridFormat_ = ui::CreateTextFormat(12.0f, DWRITE_FONT_WEIGHT_NORMAL,
                                           true);
        if (gridFormat_ && ui::DirectWrite())
        {
            DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER,
                                     0, 0};
            ComPtr<IDWriteInlineObject> ellipsis;
            ui::DirectWrite()->CreateEllipsisTrimmingSign(gridFormat_.Get(),
                                                          &ellipsis);
            gridFormat_->SetTrimming(&trimming, ellipsis.Get());
        }
        sectionFormat_ = ui::CreateTextFormat(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        smallFormat_ = ui::CreateTextFormat(12.5f, DWRITE_FONT_WEIGHT_NORMAL);
        glyphFormat_ = ui::CreateTextFormat(14.0f, DWRITE_FONT_WEIGHT_NORMAL,
                                            false, ui::IconFont());
        bigGlyphFormat_ = ui::CreateTextFormat(19.0f, DWRITE_FONT_WEIGHT_NORMAL,
                                               false, ui::IconFont());
        pinGlyphFormat_ = ui::CreateTextFormat(10.0f, DWRITE_FONT_WEIGHT_NORMAL,
                                               false, ui::IconFont());
        groupFormat_ = ui::CreateTextFormat(13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        if (gridFormat_)
        {
            gridFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        for (IDWriteTextFormat* format : {glyphFormat_.Get(),
                                          bigGlyphFormat_.Get(),
                                          pinGlyphFormat_.Get()})
        {
            if (format)
            {
                format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            }
        }
    }

    if (!hwnd_)
    {
        hwnd_ = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kLauncherClass,
            L"LightDock Launcher", WS_POPUP, 0, 0, 1, 1, owner_, nullptr,
            GetModuleHandleW(nullptr), this);
        if (!hwnd_)
        {
            return;
        }
    }

    mode_ = mode;
    edge_ = edge;
    icon_ = icon;
    dark_ = ui::SystemUsesDarkTheme();
    palette_ = &ui::GetPalette(dark_);

    // The first open enumerates synchronously; later opens show the cached
    // list immediately and refresh it right after the first frame.
    const bool refreshLater = !catalog_.Apps().empty() && catalog_.Stale();
    if (catalog_.Apps().empty())
    {
        catalog_.Refresh(true);
    }
    pins_ = host_ ? host_->LauncherPins() : std::vector<std::wstring>{};
    hidden_ = host_ ? host_->LauncherHidden() : std::vector<std::wstring>{};
    dragging_ = false;
    dragApp_ = -1;
    dragSlot_ = -1;
    query_.clear();
    UpdateResults();
    if (refreshLater)
    {
        PostMessageW(hwnd_, kRefreshMessage, 0, 0);
    }

    alpha_ = 0;
    shownAt_ = GetTickCount64();
    visible_ = true;
    caretOn_ = true;
    Place();
    Render();

    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);
    SetTimer(hwnd_, kFrameTimer, 15, nullptr);
    SetTimer(hwnd_, kCaretTimer, 530, nullptr);

    if (host_)
    {
        host_->OnLauncherVisibilityChanged(true);
    }
}

void Launcher::Hide()
{
    if (!visible_ || !hwnd_)
    {
        return;
    }
    visible_ = false;
    hiddenAt_ = GetTickCount64();
    KillTimer(hwnd_, kFrameTimer);
    KillTimer(hwnd_, kCaretTimer);
    ShowWindow(hwnd_, SW_HIDE);
    query_.clear();
    hover_ = Hit{};
    pressed_ = Hit{};
    if (host_)
    {
        host_->OnLauncherVisibilityChanged(false);
    }
}

// --- results -------------------------------------------------------------------

void Launcher::UpdateResults()
{
    results_.clear();
    // Search results preselect the best match; the grid starts unselected
    // until the keyboard or pointer picks an app.
    selected_ = GridMode() ? -1 : 0;
    scroll_ = 0.0f;

    std::vector<AppCatalog::App>& apps = catalog_.Apps();
    if (query_.empty())
    {
        if (mode_ == Mode::Start)
        {
            BuildGrid();
        }
        return;
    }

    std::wstring q = Lower(query_);
    while (!q.empty() && q.back() == L' ')
    {
        q.pop_back();
    }
    if (q.empty())
    {
        return;
    }

    auto isSeparator = [](wchar_t c)
    {
        return c == L' ' || c == L'-' || c == L'_' || c == L'.' || c == L'('
            || c == L'（';
    };

    std::vector<std::pair<int, int>> scored;   // score, index
    for (size_t i = 0; i < apps.size(); ++i)
    {
        const AppCatalog::App& app = apps[i];
        const std::wstring& name = app.lowerName;
        int score = -1;

        const size_t at = name.find(q);
        if (name == q)
        {
            score = 0;
        }
        else if (at == 0)
        {
            score = 1;
        }
        else if (at != std::wstring::npos && isSeparator(name[at - 1]))
        {
            score = 2;
        }
        else
        {
            // Initials: "vsc" -> "Visual Studio Code".
            std::wstring initials;
            bool start = true;
            for (wchar_t c : name)
            {
                if (isSeparator(c))
                {
                    start = true;
                    continue;
                }
                if (start)
                {
                    initials.push_back(c);
                    start = false;
                }
            }
            if (q.size() >= 2 && initials.rfind(q, 0) == 0)
            {
                score = 3;
            }
            else if (at != std::wstring::npos)
            {
                score = 4;
            }
            else if (!app.lowerTarget.empty()
                     && app.lowerTarget.find(q) != std::wstring::npos)
            {
                score = 5;
            }
        }

        if (score >= 0)
        {
            scored.emplace_back(score, static_cast<int>(i));
        }
    }

    std::stable_sort(scored.begin(), scored.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& entry : scored)
    {
        results_.push_back(entry.second);
    }
}

// --- layout --------------------------------------------------------------------

void Launcher::ComputeLayout()
{
    if (mode_ == Mode::Start)
    {
        bodyW_ = kStartWidth;
        bodyH_ = kStartHeight;
        return;
    }


    bodyW_ = kSearchWidth;
    const float fieldBlock = 10.0f + kSearchFieldH + 10.0f;
    if (query_.empty())
    {
        bodyH_ = fieldBlock;
        return;
    }
    const int rows = results_.empty()
        ? 1 : (std::min)(kSearchRows, static_cast<int>(results_.size()));
    bodyH_ = 10.0f + kSearchFieldH + 1.0f + 8.0f + rows * kRowH + 8.0f;
}

void Launcher::Place()
{
    HMONITOR monitor = MonitorFromRect(&icon_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    GetMonitorInfoW(monitor, &info);
    const RECT bounds = info.rcMonitor;

    const float newScale = static_cast<float>(ui::MonitorDpi(monitor)) / 96.0f;
    if (std::abs(newScale - scale_) > 0.001f)
    {
        scale_ = newScale;
        target_.Reset();
        brush_.Reset();
        bitmaps_.clear();
    }
    const float s = scale_;

    ComputeLayout();

    // Keep the Start panel on screen on small displays.
    const float edgeRoom = 8.0f * s;
    const bool vertical = edge_ == DockEdge::Left || edge_ == DockEdge::Right;
    const float available = vertical
        ? static_cast<float>(bounds.bottom - bounds.top) - 2.0f * edgeRoom
        : static_cast<float>(bounds.bottom - bounds.top)
            - static_cast<float>(icon_.bottom - icon_.top)
            - (kTipGap + kTailLength) * s - 2.0f * edgeRoom;
    bodyH_ = (std::min)(bodyH_, (std::max)(160.0f, available / s));
    const float availableW = static_cast<float>(bounds.right - bounds.left)
        - 2.0f * edgeRoom;
    bodyW_ = (std::min)(bodyW_, (std::max)(240.0f, availableW / s));

    const float bw = bodyW_ * s;
    const float bh = bodyH_ * s;
    const float cx = (icon_.left + icon_.right) * 0.5f;
    const float cy = (icon_.top + icon_.bottom) * 0.5f;
    const float gap = kTipGap * s;
    const float tail = kTailLength * s;

    float left = 0.0f, top = 0.0f;
    D2D1_POINT_2F tip{};
    switch (edge_)
    {
    case DockEdge::Top:
        tip = D2D1::Point2F(cx, icon_.bottom + gap);
        top = tip.y + tail;
        left = cx - bw * 0.5f;
        break;
    case DockEdge::Left:
        tip = D2D1::Point2F(icon_.right + gap, cy);
        left = tip.x + tail;
        top = cy - bh * 0.5f;
        break;
    case DockEdge::Right:
        tip = D2D1::Point2F(icon_.left - gap, cy);
        left = tip.x - tail - bw;
        top = cy - bh * 0.5f;
        break;
    case DockEdge::Bottom:
    default:
        tip = D2D1::Point2F(cx, icon_.top - gap);
        top = tip.y - tail - bh;
        left = cx - bw * 0.5f;
        break;
    }

    left = ClampF(left, bounds.left + edgeRoom,
                  (std::max)(bounds.left + edgeRoom, bounds.right - edgeRoom - bw));
    top = ClampF(top, bounds.top + edgeRoom,
                 (std::max)(bounds.top + edgeRoom, bounds.bottom - edgeRoom - bh));

    const int margin = static_cast<int>(std::ceil(kMargin * s));
    windowPos_.x = static_cast<LONG>(std::lround(left)) - margin;
    windowPos_.y = static_cast<LONG>(std::lround(top)) - margin;
    windowSize_.cx = static_cast<LONG>(std::ceil(bw)) + 2 * margin;
    windowSize_.cy = static_cast<LONG>(std::ceil(bh)) + 2 * margin;

    const float m = margin / s;
    body_ = D2D1::RectF(m, m, m + bodyW_, m + bodyH_);
    if (GridMode())
    {
        BuildGrid();    // the width may have been clamped above
    }
    LayoutFooter();
    tip_ = D2D1::Point2F((tip.x - windowPos_.x) / s, (tip.y - windowPos_.y) / s);
}

D2D1_RECT_F Launcher::ContentRect() const
{
    if (mode_ == Mode::Start)
    {
        return D2D1::RectF(0.0f, kPad + kStartFieldH + 10.0f, bodyW_,
                           bodyH_ - kFooterH);
    }
    return D2D1::RectF(0.0f, 10.0f + kSearchFieldH + 1.0f, bodyW_, bodyH_);
}

D2D1_RECT_F Launcher::ItemRect(int index) const
{
    if (GridMode())
    {
        return index >= 0 && index < static_cast<int>(cells_.size())
            ? cells_[static_cast<size_t>(index)].rect : D2D1::RectF(0, 0, 0, 0);
    }
    const float y = 8.0f + index * kRowH;
    return D2D1::RectF(10.0f, y, bodyW_ - 10.0f, y + kRowH);
}

float Launcher::ContentHeight() const
{
    if (GridMode())
    {
        return gridHeight_;
    }
    return 8.0f + static_cast<int>(results_.size()) * kRowH + 8.0f;
}

int Launcher::VisibleCount() const
{
    return static_cast<int>(GridMode() ? cells_.size() : results_.size());
}

int Launcher::AppAt(int index) const
{
    if (index < 0 || index >= VisibleCount())
    {
        return -1;
    }
    return GridMode() ? cells_[static_cast<size_t>(index)].app
                      : results_[static_cast<size_t>(index)];
}

// --- pinned row + smart groups ------------------------------------------------

std::vector<int> Launcher::PinnedApps() const
{
    const std::vector<AppCatalog::App>& apps =
        const_cast<AppCatalog&>(catalog_).Apps();
    std::vector<int> result;
    for (const std::wstring& pin : pins_)
    {
        for (size_t i = 0; i < apps.size(); ++i)
        {
            if (apps[i].parsingName == pin)
            {
                result.push_back(static_cast<int>(i));
                break;
            }
        }
    }
    return result;
}

bool Launcher::IsPinned(int app) const
{
    const std::vector<AppCatalog::App>& apps =
        const_cast<AppCatalog&>(catalog_).Apps();
    if (app < 0 || app >= static_cast<int>(apps.size()))
    {
        return false;
    }
    const std::wstring& name = apps[static_cast<size_t>(app)].parsingName;
    return std::find(pins_.begin(), pins_.end(), name) != pins_.end();
}

void Launcher::TogglePin(int app)
{
    std::vector<AppCatalog::App>& apps = catalog_.Apps();
    if (app < 0 || app >= static_cast<int>(apps.size()))
    {
        return;
    }
    const std::wstring& name = apps[static_cast<size_t>(app)].parsingName;
    const auto found = std::find(pins_.begin(), pins_.end(), name);
    if (found != pins_.end())
    {
        pins_.erase(found);
    }
    else
    {
        pins_.push_back(name);
        // Pinning an app brings it back from "Hidden".
        const auto hidden = std::find(hidden_.begin(), hidden_.end(), name);
        if (hidden != hidden_.end())
        {
            hidden_.erase(hidden);
            if (host_)
            {
                host_->SetLauncherHidden(hidden_);
            }
        }
    }
    if (host_)
    {
        host_->SetLauncherPins(pins_);
    }
    if (GridMode())
    {
        const int keep = selected_;
        BuildGrid();
        selected_ = (std::min)(keep, VisibleCount() - 1);
        Scroll(0.0f);
    }
    Render();
}

bool Launcher::IsHidden(int app) const
{
    const std::vector<AppCatalog::App>& apps =
        const_cast<AppCatalog&>(catalog_).Apps();
    if (app < 0 || app >= static_cast<int>(apps.size()))
    {
        return false;
    }
    const std::wstring& name = apps[static_cast<size_t>(app)].parsingName;
    return std::find(hidden_.begin(), hidden_.end(), name) != hidden_.end();
}

void Launcher::ToggleHidden(int app)
{
    std::vector<AppCatalog::App>& apps = catalog_.Apps();
    if (app < 0 || app >= static_cast<int>(apps.size()))
    {
        return;
    }
    const std::wstring name = apps[static_cast<size_t>(app)].parsingName;
    const auto found = std::find(hidden_.begin(), hidden_.end(), name);
    if (found != hidden_.end())
    {
        hidden_.erase(found);
    }
    else
    {
        hidden_.push_back(name);
        // A hidden app does not stay in the pinned row either.
        const auto pin = std::find(pins_.begin(), pins_.end(), name);
        if (pin != pins_.end())
        {
            pins_.erase(pin);
            if (host_)
            {
                host_->SetLauncherPins(pins_);
            }
        }
    }
    if (host_)
    {
        host_->SetLauncherHidden(hidden_);
    }
    if (GridMode())
    {
        BuildGrid();
        selected_ = -1;
        Scroll(0.0f);
    }
    Render();
}

void Launcher::ToggleGroup(int section, bool fromSticky)
{
    if (section < 0 || section >= static_cast<int>(sections_.size())
        || sections_[static_cast<size_t>(section)].group < 0)
    {
        return;
    }
    const Section target = sections_[static_cast<size_t>(section)];
    const size_t group = static_cast<size_t>(target.group);
    expanded_[group] = !expanded_[group];
    BuildGrid();
    selected_ = -1;
    if (fromSticky)
    {
        // Collapsing from the pinned-to-top header: bring that header back
        // into view instead of leaving the list scrolled past it.
        scroll_ = target.top;
    }
    Scroll(0.0f);
    Render();
}

int Launcher::StickySection() const
{
    int current = -1;
    for (size_t i = 0; i < sections_.size(); ++i)
    {
        if (sections_[i].top <= scroll_ + 0.5f)
        {
            current = static_cast<int>(i);
        }
    }
    if (current < 0)
    {
        return -1;
    }
    const Section& section = sections_[static_cast<size_t>(current)];
    const bool pastHeader = section.group < 0
        ? scroll_ > section.top + 0.5f
        : scroll_ > section.top + 0.5f && section.expanded;
    return pastHeader ? current : -1;
}

void Launcher::DrawGroupHeader(const Section& section, const D2D1_RECT_F& rect,
                               bool hot)
{
    const ui::Palette& p = *palette_;
    if (hot)
    {
        brush_->SetColor(p.hover);
        target_->FillRoundedRectangle(D2D1::RoundedRect(rect, 9.0f, 9.0f),
                                      brush_.Get());
    }

    // Disclosure chevron: right when collapsed, down when expanded.
    const float cx = rect.left + 16.0f;
    const float cy = (rect.top + rect.bottom) * 0.5f;
    brush_->SetColor(p.secondaryText);
    ComPtr<ID2D1StrokeStyle> round;
    ui::Direct2D()->CreateStrokeStyle(
        D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                    D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
        nullptr, 0, &round);
    if (section.expanded)
    {
        target_->DrawLine(D2D1::Point2F(cx - 4.0f, cy - 2.0f),
                          D2D1::Point2F(cx, cy + 2.0f), brush_.Get(), 1.6f,
                          round.Get());
        target_->DrawLine(D2D1::Point2F(cx, cy + 2.0f),
                          D2D1::Point2F(cx + 4.0f, cy - 2.0f), brush_.Get(),
                          1.6f, round.Get());
    }
    else
    {
        target_->DrawLine(D2D1::Point2F(cx - 2.0f, cy - 4.0f),
                          D2D1::Point2F(cx + 2.0f, cy), brush_.Get(), 1.6f,
                          round.Get());
        target_->DrawLine(D2D1::Point2F(cx + 2.0f, cy),
                          D2D1::Point2F(cx - 2.0f, cy + 4.0f), brush_.Get(),
                          1.6f, round.Get());
    }

    // Title and count.
    float x = rect.left + 30.0f;
    ComPtr<IDWriteTextLayout> title;
    if (SUCCEEDED(ui::DirectWrite()->CreateTextLayout(
            section.title.c_str(), static_cast<UINT32>(section.title.size()),
            groupFormat_.Get(), 300.0f, rect.bottom - rect.top, &title)))
    {
        DWRITE_TEXT_METRICS metrics{};
        title->GetMetrics(&metrics);
        brush_->SetColor(p.menuText);
        target_->DrawTextLayout(D2D1::Point2F(x, rect.top), title.Get(),
                                brush_.Get());
        x += metrics.widthIncludingTrailingWhitespace + 8.0f;
    }
    const std::wstring count = std::to_wstring(section.count);
    brush_->SetColor(p.tertiaryText);
    target_->DrawText(count.c_str(), static_cast<UINT32>(count.size()),
                      smallFormat_.Get(),
                      D2D1::RectF(x, rect.top, x + 60.0f, rect.bottom),
                      brush_.Get());

    // Collapsed groups preview their first apps on the right.
    if (!section.expanded)
    {
        std::vector<AppCatalog::App>& apps = catalog_.Apps();
        float right = rect.right - 10.0f;
        for (auto it = section.preview.rbegin(); it != section.preview.rend();
             ++it)
        {
            DrawIcon(apps[static_cast<size_t>(*it)],
                     D2D1::RectF(right - 22.0f, cy - 11.0f, right, cy + 11.0f));
            right -= 28.0f;
        }
    }
}

void Launcher::BuildGrid()
{
    cells_.clear();
    sections_.clear();

    const float cellW = (bodyW_ - 2.0f * kPad) / kGridColumns;
    std::vector<AppCatalog::App>& apps = catalog_.Apps();
    float y = 0.0f;

    auto addCells = [&](const std::vector<int>& list, bool pinned)
    {
        for (size_t k = 0; k < list.size(); ++k)
        {
            const int row = static_cast<int>(k) / kGridColumns;
            const int column = static_cast<int>(k) % kGridColumns;
            const float x = kPad + column * cellW;
            const float top = y + row * kCellH;
            cells_.push_back(Cell{list[k],
                                  D2D1::RectF(x, top, x + cellW, top + kCellH),
                                  pinned});
        }
        const int rows = (static_cast<int>(list.size()) + kGridColumns - 1)
            / kGridColumns;
        y += rows * kCellH;
    };

    // --- pinned row ------------------------------------------------------------
    std::vector<int> pinned = PinnedApps();
    if (dragging_ && dragApp_ >= 0)
    {
        pinned.erase(std::remove(pinned.begin(), pinned.end(), dragApp_),
                     pinned.end());
        if (dragSlot_ >= 0)
        {
            const size_t slot = (std::min)(static_cast<size_t>(dragSlot_),
                                           pinned.size());
            pinned.insert(pinned.begin() + static_cast<std::ptrdiff_t>(slot),
                          dragApp_);
        }
        else if (dragFromPinned_)
        {
            pinned = PinnedApps();
        }
    }

    const float pinnedTop = y;
    {
        Section pinnedSection;
        pinnedSection.title = T(L"置顶");
        pinnedSection.top = y;
        sections_.push_back(std::move(pinnedSection));
    }
    y += kGridTitleH;
    if (pinned.empty())
    {
        pinHint_ = D2D1::RectF(kPad, y, bodyW_ - kPad, y + 52.0f);
        y += 52.0f;
    }
    else
    {
        pinHint_ = D2D1::RectF(0, 0, 0, 0);
        addCells(pinned, true);
    }
    pinnedArea_ = D2D1::RectF(0.0f, pinnedTop, bodyW_, y);
    y += 10.0f;

    // --- smart groups, then "Hidden"; all collapsed until clicked -------------
    for (int group = 0; group <= kHiddenGroup; ++group)
    {
        std::vector<int> members;
        for (size_t i = 0; i < apps.size(); ++i)
        {
            const int index = static_cast<int>(i);
            const bool hidden = IsHidden(index);
            const int category = apps[i].category < 0 ? AppCatalog::Other
                                                      : apps[i].category;
            if (group == kHiddenGroup ? hidden : (!hidden && category == group))
            {
                members.push_back(index);
            }
        }
        if (members.empty())
        {
            continue;
        }

        Section section;
        section.title = group == kHiddenGroup
            ? T(L"已隐藏") : T(AppCatalog::CategoryTitle(group));
        section.top = y;
        section.group = group;
        section.expanded = expanded_[static_cast<size_t>(group)];
        section.count = static_cast<int>(members.size());
        section.header = D2D1::RectF(kPad, y, bodyW_ - kPad, y + kGroupHeaderH);
        for (size_t k = 0; k < members.size() && k < 6; ++k)
        {
            section.preview.push_back(members[k]);
        }
        sections_.push_back(std::move(section));

        y += kGroupHeaderH + 4.0f;
        if (expanded_[static_cast<size_t>(group)])
        {
            addCells(members, false);
            y += 8.0f;
        }
    }

    gridHeight_ = y + 8.0f;
}

D2D1_RECT_F Launcher::PinButtonRect(int index) const
{
    const D2D1_RECT_F cell = ItemRect(index);
    const float cx = (cell.left + cell.right) * 0.5f;
    return D2D1::RectF(cx + 14.0f, cell.top + 4.0f, cx + 34.0f,
                       cell.top + 24.0f);
}

void Launcher::UpdateDrag(float contentX, float contentY)
{
    dragPoint_ = D2D1::Point2F(contentX, contentY);

    int slot = -1;
    const D2D1_RECT_F zone = D2D1::RectF(
        pinnedArea_.left, pinnedArea_.top - 10.0f, pinnedArea_.right,
        pinnedArea_.bottom + kCellH * 0.5f);
    if (Contains(zone, contentX, contentY))
    {
        const float cellW = (bodyW_ - 2.0f * kPad) / kGridColumns;
        const int column = std::clamp(
            static_cast<int>((contentX - kPad) / cellW), 0, kGridColumns - 1);
        const int row = (std::max)(0, static_cast<int>(
            (contentY - pinnedArea_.top - kGridTitleH) / kCellH));
        std::vector<int> others = PinnedApps();
        others.erase(std::remove(others.begin(), others.end(), dragApp_),
                     others.end());
        slot = (std::min)(row * kGridColumns + column,
                          static_cast<int>(others.size()));
    }

    // Auto-scroll while dragging near the edges of the list.
    const D2D1_RECT_F content = ContentRect();
    const float viewY = contentY - scroll_;
    if (viewY < 24.0f)
    {
        Scroll(-14.0f);
    }
    else if (viewY > (content.bottom - content.top) - 24.0f)
    {
        Scroll(14.0f);
    }

    if (slot != dragSlot_)
    {
        dragSlot_ = slot;
        BuildGrid();
    }
    Render();
}

void Launcher::FinishDrag()
{
    if (!dragging_)
    {
        return;
    }
    if (dragSlot_ >= 0)
    {
        std::vector<int> order = PinnedApps();
        order.erase(std::remove(order.begin(), order.end(), dragApp_),
                    order.end());
        const size_t slot = (std::min)(static_cast<size_t>(dragSlot_),
                                       order.size());
        order.insert(order.begin() + static_cast<std::ptrdiff_t>(slot),
                     dragApp_);

        std::vector<std::wstring> pins;
        for (int app : order)
        {
            pins.push_back(catalog_.Apps()[static_cast<size_t>(app)].parsingName);
        }
        pins_ = pins;
        if (host_)
        {
            host_->SetLauncherPins(pins_);
        }
        const std::wstring& dropped =
            catalog_.Apps()[static_cast<size_t>(dragApp_)].parsingName;
        const auto hidden = std::find(hidden_.begin(), hidden_.end(), dropped);
        if (hidden != hidden_.end())
        {
            hidden_.erase(hidden);
            if (host_)
            {
                host_->SetLauncherHidden(hidden_);
            }
        }
    }

    dragging_ = false;
    dragApp_ = -1;
    dragSlot_ = -1;
    BuildGrid();
    selected_ = -1;
    Scroll(0.0f);
    Render();
}

void Launcher::Scroll(float delta)
{
    const D2D1_RECT_F content = ContentRect();
    const float maximum = (std::max)(
        0.0f, ContentHeight() - (content.bottom - content.top));
    scroll_ = ClampF(scroll_ + delta, 0.0f, maximum);
    // Whole device pixels, so icons stay pixel-aligned while scrolling.
    scroll_ = std::round(scroll_ * scale_) / scale_;
}

void Launcher::EnsureSelectionVisible()
{
    if (selected_ < 0 || selected_ >= VisibleCount())
    {
        return;
    }
    const D2D1_RECT_F content = ContentRect();
    const float viewH = content.bottom - content.top;
    const D2D1_RECT_F item = ItemRect(selected_);
    if (item.top - 4.0f < scroll_)
    {
        scroll_ = (std::max)(0.0f, item.top - (GridMode() ? kGridTitleH : 8.0f));
    }
    else if (item.bottom + 4.0f > scroll_ + viewH)
    {
        scroll_ = item.bottom + 8.0f - viewH;
    }
    Scroll(0.0f);
}

// --- rendering ---------------------------------------------------------------------

bool Launcher::EnsureSurface()
{
    if (!surfaceDc_ || surfaceSize_.cx != windowSize_.cx
        || surfaceSize_.cy != windowSize_.cy)
    {
        if (surfaceDc_)
        {
            SelectObject(surfaceDc_, surfaceOld_);
            DeleteDC(surfaceDc_);
            surfaceDc_ = nullptr;
        }
        if (surfaceBitmap_)
        {
            DeleteObject(surfaceBitmap_);
            surfaceBitmap_ = nullptr;
        }

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = windowSize_.cx;
        info.bmiHeader.biHeight = -windowSize_.cy;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        surfaceDc_ = CreateCompatibleDC(nullptr);
        surfaceBitmap_ = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS,
                                          &bits, nullptr, 0);
        if (!surfaceDc_ || !surfaceBitmap_)
        {
            return false;
        }
        surfaceOld_ = SelectObject(surfaceDc_, surfaceBitmap_);
        surfaceSize_ = windowSize_;
    }

    if (!target_)
    {
        const D2D1_RENDER_TARGET_PROPERTIES properties =
            D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                  D2D1_ALPHA_MODE_PREMULTIPLIED),
                96.0f * scale_, 96.0f * scale_);
        if (!ui::Direct2D()
            || FAILED(ui::Direct2D()->CreateDCRenderTarget(&properties,
                                                           &target_))
            || FAILED(target_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0),
                                                     &brush_)))
        {
            target_.Reset();
            return false;
        }
        target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        bitmaps_.clear();
    }
    return true;
}

void Launcher::Render()
{
    if (!hwnd_ || !visible_ || !EnsureSurface())
    {
        return;
    }

    RECT bind{0, 0, windowSize_.cx, windowSize_.cy};
    if (FAILED(target_->BindDC(surfaceDc_, &bind)))
    {
        return;
    }

    const ui::Palette& p = *palette_;
    iconsPending_ = false;
    iconBudget_ = 10;

    target_->BeginDraw();
    target_->SetTransform(D2D1::Matrix3x2F::Identity());
    target_->Clear(D2D1::ColorF(0, 0, 0, 0));

    ui::FillSoftShadow(target_.Get(), brush_.Get(), body_, kRadius, 18.0f,
                       6.0f, p.dark ? 0.60f : 0.26f);
    ComPtr<ID2D1PathGeometry> bubble =
        BubbleGeometry(body_, kRadius, edge_, tip_, kTailHalf);
    if (bubble)
    {
        brush_->SetColor(p.menuBg);
        target_->FillGeometry(bubble.Get(), brush_.Get());
        brush_->SetColor(p.menuBorder);
        target_->DrawGeometry(bubble.Get(), brush_.Get(), 1.0f / scale_);
    }

    target_->SetTransform(D2D1::Matrix3x2F::Translation(body_.left, body_.top));
    DrawField();

    const D2D1_RECT_F content = ContentRect();
    if (content.bottom > content.top
        && (mode_ == Mode::Start || !query_.empty()))
    {
        if (mode_ == Mode::Search || scroll_ > 0.5f)
        {
            brush_->SetColor(p.menuSeparator);
            target_->FillRectangle(
                D2D1::RectF(0.0f, content.top - 1.0f / scale_, bodyW_,
                            content.top),
                brush_.Get());
        }

        target_->PushAxisAlignedClip(content, D2D1_ANTIALIAS_MODE_ALIASED);
        target_->SetTransform(D2D1::Matrix3x2F::Translation(
            body_.left, body_.top + content.top - scroll_));
        if (GridMode())
        {
            DrawGrid();
        }
        else
        {
            DrawList();
        }
        target_->SetTransform(
            D2D1::Matrix3x2F::Translation(body_.left, body_.top));
        target_->PopAxisAlignedClip();
    }

    if (mode_ == Mode::Start)
    {
        DrawFooter();
    }

    const HRESULT hr = target_->EndDraw();
    if (hr == static_cast<HRESULT>(D2DERR_RECREATE_TARGET))
    {
        target_.Reset();
        brush_.Reset();
        bitmaps_.clear();
        return;
    }

    Present();
}

void Launcher::Present()
{
    BLENDFUNCTION blend{AC_SRC_OVER, 0, alpha_, AC_SRC_ALPHA};
    POINT source{0, 0};
    HDC screen = GetDC(nullptr);
    UpdateLayeredWindow(hwnd_, screen, &windowPos_, &windowSize_, surfaceDc_,
                        &source, 0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

void Launcher::DrawField()
{
    const ui::Palette& p = *palette_;
    const bool big = mode_ == Mode::Search;

    D2D1_RECT_F field;
    D2D1_RECT_F glyph;
    float textLeft;
    if (big)
    {
        field = D2D1::RectF(14.0f, 10.0f, bodyW_ - 14.0f, 10.0f + kSearchFieldH);
        glyph = D2D1::RectF(field.left + 4.0f, field.top, field.left + 32.0f,
                            field.bottom);
        textLeft = field.left + 42.0f;
    }
    else
    {
        field = D2D1::RectF(kPad, kPad, bodyW_ - kPad, kPad + kStartFieldH);
        brush_->SetColor(p.hover);
        target_->FillRoundedRectangle(D2D1::RoundedRect(field, 9.0f, 9.0f),
                                      brush_.Get());
        glyph = D2D1::RectF(field.left + 6.0f, field.top, field.left + 32.0f,
                            field.bottom);
        textLeft = field.left + 34.0f;
    }

    const wchar_t magnifier[2] = {0xE721, 0};
    brush_->SetColor(p.secondaryText);
    target_->DrawText(magnifier, 1, big ? bigGlyphFormat_.Get()
                                        : glyphFormat_.Get(),
                      glyph, brush_.Get());

    IDWriteTextFormat* format = big ? bigFieldFormat_.Get() : fieldFormat_.Get();
    const D2D1_RECT_F textArea = D2D1::RectF(textLeft, field.top,
                                             field.right - 10.0f, field.bottom);
    if (query_.empty())
    {
        const wchar_t* placeholder = big ? T(L"搜索应用") : T(L"搜索应用");
        brush_->SetColor(p.tertiaryText);
        target_->DrawText(placeholder, static_cast<UINT32>(wcslen(placeholder)),
                          format, textArea, brush_.Get());
    }

    float textWidth = 0.0f;
    ComPtr<IDWriteTextLayout> layout;
    if (!query_.empty() && ui::DirectWrite()
        && SUCCEEDED(ui::DirectWrite()->CreateTextLayout(
            query_.c_str(), static_cast<UINT32>(query_.size()), format,
            10000.0f, textArea.bottom - textArea.top, &layout)))
    {
        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);
        textWidth = metrics.widthIncludingTrailingWhitespace;
    }

    const float available = textArea.right - textArea.left - 4.0f;
    const float shift = (std::max)(0.0f, textWidth - available);
    if (layout)
    {
        target_->PushAxisAlignedClip(textArea, D2D1_ANTIALIAS_MODE_ALIASED);
        brush_->SetColor(p.menuText);
        target_->DrawTextLayout(D2D1::Point2F(textArea.left - shift,
                                              textArea.top),
                                layout.Get(), brush_.Get());
        target_->PopAxisAlignedClip();
    }

    // Caret.
    const float caretX = textArea.left + textWidth - shift + 1.0f;
    const float caretH = big ? 24.0f : 17.0f;
    const float cy = (field.top + field.bottom) * 0.5f;
    if (caretOn_)
    {
        brush_->SetColor(p.accent);
        target_->FillRectangle(D2D1::RectF(caretX, cy - caretH * 0.5f,
                                           caretX + (big ? 2.0f : 1.5f),
                                           cy + caretH * 0.5f),
                               brush_.Get());
    }

    // The system caret only tells the IME where to put its windows.
    const int x = static_cast<int>(std::lround((body_.left + caretX) * scale_));
    const int y = static_cast<int>(std::lround(
        (body_.top + cy - caretH * 0.5f) * scale_));
    SetCaretPos(x, y);
}

D2D1_RECT_F Launcher::SnapToPixels(const D2D1_RECT_F& rect) const
{
    // Icons are pre-scaled to an exact pixel size; drawing them on whole
    // device pixels keeps them 1:1 instead of resampled (the jaggies).
    D2D1_MATRIX_3X2_F transform{};
    target_->GetTransform(&transform);
    const float s = scale_;
    const float x = std::round((rect.left + transform._31) * s) / s - transform._31;
    const float y = std::round((rect.top + transform._32) * s) / s - transform._32;
    const float size = std::round((rect.right - rect.left) * s) / s;
    return D2D1::RectF(x, y, x + size, y + size);
}

ID2D1Bitmap* Launcher::ScaledIcon(AppCatalog::App& app, int pixels)
{
    IWICBitmapSource* source = app.icon.Get();
    if (!source || pixels <= 0)
    {
        return nullptr;
    }
    for (const ScaledBitmap& entry : bitmaps_)
    {
        if (entry.source == source && entry.pixels == pixels)
        {
            return entry.bitmap.Get();
        }
    }
    if (bitmaps_.size() > 900)
    {
        bitmaps_.clear();
    }

    IWICImagingFactory* wic = catalog_.Wic();
    ComPtr<IWICBitmapClipper> clipper;
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> converter;
    ComPtr<ID2D1Bitmap> bitmap;
    IWICBitmapSource* input = source;
    if (wic && app.crop.Width > 0
        && SUCCEEDED(wic->CreateBitmapClipper(&clipper))
        && SUCCEEDED(clipper->Initialize(source, &app.crop)))
    {
        input = clipper.Get();
    }
    if (wic
        && SUCCEEDED(wic->CreateBitmapScaler(&scaler))
        && SUCCEEDED(scaler->Initialize(input, static_cast<UINT>(pixels),
                                        static_cast<UINT>(pixels),
                                        WICBitmapInterpolationModeFant))
        && SUCCEEDED(wic->CreateFormatConverter(&converter))
        && SUCCEEDED(converter->Initialize(
            scaler.Get(), GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
    {
        // DPI = device DPI, so the bitmap's DIP size equals the target rect.
        const D2D1_BITMAP_PROPERTIES properties = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                              D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f * scale_, 96.0f * scale_);
        target_->CreateBitmapFromWicBitmap(converter.Get(), &properties,
                                           &bitmap);
    }

    ScaledBitmap entry;
    entry.source = source;
    entry.pixels = pixels;
    entry.bitmap = bitmap;
    bitmaps_.push_back(entry);
    return bitmaps_.back().bitmap.Get();
}

void Launcher::DrawIcon(AppCatalog::App& app, const D2D1_RECT_F& requested)
{
    if (!app.iconLoaded)
    {
        if (iconBudget_ > 0)
        {
            --iconBudget_;
            catalog_.Icon(app);
        }
        else
        {
            iconsPending_ = true;
        }
    }

    const D2D1_RECT_F rect = SnapToPixels(requested);
    const float size = rect.right - rect.left;
    const float radius = size * 0.225f;            // macOS-like squircle
    const D2D1_ROUNDED_RECT shape = D2D1::RoundedRect(rect, radius, radius);
    const float hairline = 1.0f / scale_;

    // Soft contact shadow under every tile/plate.
    brush_->SetColor(D2D1::ColorF(0, 0, 0, palette_->dark ? 0.35f : 0.14f));
    target_->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(rect.left, rect.top + hairline,
                                      rect.right, rect.bottom + hairline),
                          radius, radius),
        brush_.Get());

    if (app.icon && app.tile)
    {
        // Full-bleed artwork: clip it to the same rounded square.
        const int pixels = static_cast<int>(std::lround(size * scale_));
        if (ID2D1Bitmap* bitmap = ScaledIcon(app, pixels))
        {
            ComPtr<ID2D1BitmapBrush> fill;
            if (SUCCEEDED(target_->CreateBitmapBrush(bitmap, &fill)))
            {
                fill->SetTransform(
                    D2D1::Matrix3x2F::Translation(rect.left, rect.top));
                target_->FillRoundedRectangle(shape, fill.Get());
                brush_->SetColor(D2D1::ColorF(0, 0, 0, 0.10f));
                target_->DrawRoundedRectangle(shape, brush_.Get(), hairline);
                return;
            }
        }
    }

    // Everything else sits on a plate: the app's own tile colour for store
    // logos, otherwise white, or dark when the artwork is mostly white.
    const bool colored = app.icon && app.coloredPlate;
    const bool darkPlate = app.icon && !colored && app.lightGlyph;
    brush_->SetColor(colored ? app.plateColor
                     : darkPlate ? D2D1::ColorF(0.17f, 0.17f, 0.19f)
                                 : D2D1::ColorF(0.985f, 0.985f, 0.99f));
    target_->FillRoundedRectangle(shape, brush_.Get());
    brush_->SetColor(darkPlate || colored ? D2D1::ColorF(1, 1, 1, 0.10f)
                                          : D2D1::ColorF(0, 0, 0, 0.10f));
    target_->DrawRoundedRectangle(shape, brush_.Get(), hairline);

    if (!app.icon)
    {
        return;
    }
    // Store logos are glyphs: macOS-like glyph size on their coloured tile.
    const float fill = colored ? 0.62f : 0.74f;
    const float inner = std::round(size * fill * scale_) / scale_;
    const float offset = std::round((size - inner) * 0.5f * scale_) / scale_;
    const int pixels = static_cast<int>(std::lround(inner * scale_));
    if (ID2D1Bitmap* bitmap = ScaledIcon(app, pixels))
    {
        target_->DrawBitmap(bitmap,
                            D2D1::RectF(rect.left + offset, rect.top + offset,
                                        rect.left + offset + inner,
                                        rect.top + offset + inner),
                            1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    }
}

void Launcher::DrawGrid()
{
    const ui::Palette& p = *palette_;
    const D2D1_RECT_F content = ContentRect();
    const float viewH = content.bottom - content.top;
    std::vector<AppCatalog::App>& apps = catalog_.Apps();

    auto drawTitle = [&](const std::wstring& title, float top)
    {
        brush_->SetColor(p.secondaryText);
        target_->DrawText(title.c_str(), static_cast<UINT32>(title.size()),
                          sectionFormat_.Get(),
                          D2D1::RectF(kPad + 6.0f, top, bodyW_ - kPad,
                                      top + kGridTitleH - 4.0f),
                          brush_.Get());
    };

    for (size_t i = 0; i < sections_.size(); ++i)
    {
        const Section& section = sections_[i];
        if (section.top + kGroupHeaderH < scroll_ || section.top > scroll_ + viewH)
        {
            continue;
        }
        if (section.group < 0)
        {
            drawTitle(section.title, section.top);
        }
        else
        {
            DrawGroupHeader(section, section.header,
                            hover_.target == Target::GroupHeader
                                && hover_.index == static_cast<int>(i));
        }
    }

    // Empty pinned row: explain how to fill it, as a dashed drop target.
    if (pinHint_.right > pinHint_.left)
    {
        const D2D1_RECT_F hint = Inset(pinHint_, 2.0f, 4.0f);
        const bool target = dragging_ && dragSlot_ >= 0;
        if (target)
        {
            brush_->SetColor(WithAlpha(p.accent, 0.12f));
            target_->FillRoundedRectangle(D2D1::RoundedRect(hint, 10.0f, 10.0f),
                                          brush_.Get());
        }
        ComPtr<ID2D1StrokeStyle> dashed;
        const float dashes[] = {4.0f, 3.0f};
        ui::Direct2D()->CreateStrokeStyle(
            D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                        D2D1_CAP_STYLE_ROUND,
                                        D2D1_LINE_JOIN_ROUND, 10.0f,
                                        D2D1_DASH_STYLE_CUSTOM, 0.0f),
            dashes, 2, &dashed);
        brush_->SetColor(target ? p.accent : p.menuSeparator);
        target_->DrawRoundedRectangle(D2D1::RoundedRect(hint, 10.0f, 10.0f),
                                      brush_.Get(), 1.0f, dashed.Get());
        const wchar_t* text = T(L"把应用拖到这里，或点击图标右上角的图钉来置顶");
        brush_->SetColor(p.secondaryText);
        ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(ui::DirectWrite()->CreateTextLayout(
                text, static_cast<UINT32>(wcslen(text)), smallFormat_.Get(),
                hint.right - hint.left, hint.bottom - hint.top, &layout)))
        {
            layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            target_->DrawTextLayout(D2D1::Point2F(hint.left, hint.top),
                                    layout.Get(), brush_.Get());
        }
    }

    for (int i = 0; i < VisibleCount(); ++i)
    {
        const Cell& cell = cells_[static_cast<size_t>(i)];
        if (cell.rect.bottom < scroll_ || cell.rect.top > scroll_ + viewH)
        {
            continue;
        }
        // The dragged app leaves a gap where it will land.
        if (dragging_ && cell.pinned && cell.app == dragApp_)
        {
            continue;
        }
        AppCatalog::App& app = apps[static_cast<size_t>(cell.app)];

        if (i == selected_ && !dragging_)
        {
            brush_->SetColor(p.dark ? D2D1::ColorF(1, 1, 1, 0.10f)
                                    : D2D1::ColorF(0, 0, 0, 0.07f));
            target_->FillRoundedRectangle(
                D2D1::RoundedRect(Inset(cell.rect, 4.0f, 3.0f), 10.0f, 10.0f),
                brush_.Get());
        }

        const float cx = (cell.rect.left + cell.rect.right) * 0.5f;
        DrawIcon(app, D2D1::RectF(cx - 24.0f, cell.rect.top + 10.0f,
                                  cx + 24.0f, cell.rect.top + 58.0f));
        brush_->SetColor(p.menuText);
        target_->DrawText(app.name.c_str(), static_cast<UINT32>(app.name.size()),
                          gridFormat_.Get(),
                          D2D1::RectF(cell.rect.left + 6.0f, cell.rect.top + 64.0f,
                                      cell.rect.right - 6.0f,
                                      cell.rect.top + 97.0f),
                          brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

        // Pin toggle on the hovered cell (always shown in the pinned row
        // while hovered, so unpinning is one click too).
        if (!dragging_ && hover_.index == i
            && (hover_.target == Target::Item
                || hover_.target == Target::PinToggle))
        {
            const D2D1_RECT_F button = PinButtonRect(i);
            const bool pinnedApp = IsPinned(cell.app);
            const bool hot = hover_.target == Target::PinToggle;
            const D2D1_ELLIPSE circle = D2D1::Ellipse(
                D2D1::Point2F((button.left + button.right) * 0.5f,
                              (button.top + button.bottom) * 0.5f),
                10.0f, 10.0f);
            brush_->SetColor(D2D1::ColorF(0, 0, 0, 0.25f));
            target_->FillEllipse(D2D1::Ellipse(
                D2D1::Point2F(circle.point.x, circle.point.y + 0.8f), 10.5f,
                10.5f), brush_.Get());
            brush_->SetColor(pinnedApp || hot ? p.accent
                             : (p.dark ? D2D1::ColorF(0.36f, 0.36f, 0.38f)
                                       : D2D1::ColorF(1, 1, 1)));
            target_->FillEllipse(circle, brush_.Get());
            const wchar_t glyph[2] = {
                static_cast<wchar_t>(pinnedApp ? 0xE77A : 0xE718), 0};
            brush_->SetColor(pinnedApp || hot ? p.onAccent : p.menuText);
            target_->DrawText(glyph, 1, pinGlyphFormat_.Get(), button,
                              brush_.Get());
        }
    }

    // Sticky header for the expanded group currently at the top; clicking
    // it collapses the group.
    const int sticky = StickySection();
    if (sticky >= 0)
    {
        const Section& section = sections_[static_cast<size_t>(sticky)];
        const float height = section.group < 0 ? kGridTitleH : kGroupHeaderH + 4.0f;
        brush_->SetColor(p.menuBg);
        target_->FillRectangle(D2D1::RectF(0.0f, scroll_, bodyW_,
                                           scroll_ + height),
                               brush_.Get());
        brush_->SetColor(p.menuSeparator);
        target_->FillRectangle(D2D1::RectF(0.0f, scroll_ + height - 1.0f / scale_,
                                           bodyW_, scroll_ + height),
                               brush_.Get());
        if (section.group < 0)
        {
            drawTitle(section.title, scroll_);
        }
        else
        {
            DrawGroupHeader(section,
                            D2D1::RectF(kPad, scroll_, bodyW_ - kPad,
                                        scroll_ + kGroupHeaderH),
                            hover_.target == Target::GroupHeader
                                && hover_.index == sticky);
        }
    }

    // The dragged icon follows the pointer.
    if (dragging_ && dragApp_ >= 0)
    {
        AppCatalog::App& app = apps[static_cast<size_t>(dragApp_)];
        const D2D1_RECT_F rect = D2D1::RectF(
            dragPoint_.x - 28.0f, dragPoint_.y - 28.0f, dragPoint_.x + 28.0f,
            dragPoint_.y + 28.0f);
        ui::FillSoftShadow(target_.Get(), brush_.Get(), Inset(rect, 6.0f, 6.0f),
                           12.0f, 8.0f, 4.0f, 0.25f);
        DrawIcon(app, rect);
    }
}

void Launcher::DrawList()
{
    const ui::Palette& p = *palette_;
    const D2D1_RECT_F content = ContentRect();
    const float viewH = content.bottom - content.top;

    if (results_.empty())
    {
        const wchar_t* text = T(L"没有找到应用");
        brush_->SetColor(p.secondaryText);
        target_->DrawText(text, static_cast<UINT32>(wcslen(text)),
                          itemFormat_.Get(),
                          D2D1::RectF(24.0f, 8.0f, bodyW_ - 24.0f, 8.0f + kRowH),
                          brush_.Get());
        return;
    }

    std::vector<AppCatalog::App>& apps = catalog_.Apps();
    for (int i = 0; i < VisibleCount(); ++i)
    {
        const D2D1_RECT_F row = ItemRect(i);
        if (row.bottom < scroll_ || row.top > scroll_ + viewH)
        {
            continue;
        }
        AppCatalog::App& app = apps[static_cast<size_t>(results_[i])];
        const bool selected = i == selected_;
        if (selected)
        {
            brush_->SetColor(p.accent);
            target_->FillRoundedRectangle(
                D2D1::RoundedRect(Inset(row, 0.0f, 2.0f), 8.0f, 8.0f),
                brush_.Get());
        }

        const float cy = (row.top + row.bottom) * 0.5f;
        DrawIcon(app, D2D1::RectF(row.left + 10.0f, cy - 14.0f,
                                  row.left + 38.0f, cy + 14.0f));
        brush_->SetColor(selected ? p.onAccent : p.menuText);
        target_->DrawText(app.name.c_str(), static_cast<UINT32>(app.name.size()),
                          itemFormat_.Get(),
                          D2D1::RectF(row.left + 50.0f, row.top,
                                      row.right - 90.0f, row.bottom),
                          brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

        if (selected)
        {
            const wchar_t* hint = T(L"打开");
            brush_->SetColor(WithAlpha(p.onAccent, 0.8f));
            ComPtr<IDWriteTextLayout> layout;
            if (SUCCEEDED(ui::DirectWrite()->CreateTextLayout(
                    hint, static_cast<UINT32>(wcslen(hint)), smallFormat_.Get(),
                    80.0f, row.bottom - row.top, &layout)))
            {
                layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                target_->DrawTextLayout(
                    D2D1::Point2F(row.right - 92.0f, row.top), layout.Get(),
                    brush_.Get());
            }
        }
    }
}

void Launcher::LayoutFooter()
{
    const float top = bodyH_ - kFooterH;
    const float cy = top + kFooterH * 0.5f;
    auto measure = [&](const wchar_t* text)
    {
        ComPtr<IDWriteTextLayout> layout;
        DWRITE_TEXT_METRICS metrics{};
        if (ui::DirectWrite()
            && SUCCEEDED(ui::DirectWrite()->CreateTextLayout(
                text, static_cast<UINT32>(wcslen(text)), smallFormat_.Get(),
                400.0f, 40.0f, &layout)))
        {
            layout->GetMetrics(&metrics);
        }
        return std::ceil(metrics.widthIncludingTrailingWhitespace);
    };

    // [⏻ 电源 ⌄] on the far right, [⚙ 设置] next to it: labelled buttons, and
    // the chevron says "this opens a menu", not "this shuts down".
    const float powerW = 12.0f + 16.0f + 6.0f + measure(T(L"电源")) + 6.0f + 10.0f + 12.0f;
    const float settingsW = 12.0f + 16.0f + 6.0f + measure(T(L"设置")) + 12.0f;
    powerButton_ = D2D1::RectF(bodyW_ - kPad - powerW, cy - 15.0f,
                               bodyW_ - kPad, cy + 15.0f);
    settingsButton_ = D2D1::RectF(powerButton_.left - 8.0f - settingsW,
                                  cy - 15.0f, powerButton_.left - 8.0f,
                                  cy + 15.0f);
    systemButton_ = D2D1::RectF(kPad, cy - 15.0f,
                                kPad + 20.0f + measure(T(L"打开系统开始菜单")),
                                cy + 15.0f);
}

void Launcher::DrawFooter()
{
    const ui::Palette& p = *palette_;
    const float top = bodyH_ - kFooterH;
    brush_->SetColor(p.menuSeparator);
    target_->FillRectangle(D2D1::RectF(0.0f, top, bodyW_, top + 1.0f / scale_),
                           brush_.Get());
    LayoutFooter();

    // "Open the Windows Start menu" for whatever the system menu still
    // offers (recommendations, account, etc.).
    const wchar_t* system = T(L"打开系统开始菜单");
    if (hover_.target == Target::SystemStart)
    {
        brush_->SetColor(p.hover);
        target_->FillRoundedRectangle(
            D2D1::RoundedRect(systemButton_, 7.0f, 7.0f), brush_.Get());
    }
    brush_->SetColor(p.accent);
    target_->DrawText(system, static_cast<UINT32>(wcslen(system)),
                      smallFormat_.Get(),
                      D2D1::RectF(systemButton_.left + 10.0f, systemButton_.top,
                                  systemButton_.right, systemButton_.bottom),
                      brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

    auto pill = [&](const D2D1_RECT_F& rect, Target target, wchar_t glyph,
                    const wchar_t* label, bool menu)
    {
        const bool hot = hover_.target == target;
        brush_->SetColor(hot ? (p.dark ? D2D1::ColorF(1, 1, 1, 0.16f)
                                       : D2D1::ColorF(0, 0, 0, 0.10f))
                             : p.hover);
        target_->FillRoundedRectangle(D2D1::RoundedRect(rect, 8.0f, 8.0f),
                                      brush_.Get());
        brush_->SetColor(p.menuSeparator);
        target_->DrawRoundedRectangle(D2D1::RoundedRect(rect, 8.0f, 8.0f),
                                      brush_.Get(), 1.0f / scale_);

        const wchar_t text[2] = {glyph, 0};
        brush_->SetColor(p.menuText);
        target_->DrawText(text, 1, glyphFormat_.Get(),
                          D2D1::RectF(rect.left + 10.0f, rect.top,
                                      rect.left + 30.0f, rect.bottom),
                          brush_.Get());
        target_->DrawText(label, static_cast<UINT32>(wcslen(label)),
                          smallFormat_.Get(),
                          D2D1::RectF(rect.left + 34.0f, rect.top,
                                      rect.right - (menu ? 20.0f : 8.0f),
                                      rect.bottom),
                          brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        if (menu)
        {
            const float cx = rect.right - 15.0f;
            const float cy = (rect.top + rect.bottom) * 0.5f;
            brush_->SetColor(p.secondaryText);
            ComPtr<ID2D1StrokeStyle> round;
            ui::Direct2D()->CreateStrokeStyle(
                D2D1::StrokeStyleProperties(
                    D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                    D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
                nullptr, 0, &round);
            target_->DrawLine(D2D1::Point2F(cx - 3.5f, cy - 1.5f),
                              D2D1::Point2F(cx, cy + 2.0f), brush_.Get(), 1.4f,
                              round.Get());
            target_->DrawLine(D2D1::Point2F(cx, cy + 2.0f),
                              D2D1::Point2F(cx + 3.5f, cy - 1.5f), brush_.Get(),
                              1.4f, round.Get());
        }
    };
    pill(settingsButton_, Target::Settings, 0xE713, T(L"设置"), false);
    pill(powerButton_, Target::Power, 0xE7E8, T(L"电源"), true);
}

// --- input -------------------------------------------------------------------------

Launcher::Hit Launcher::HitTest(float x, float y) const
{
    const float bx = x - body_.left;
    const float by = y - body_.top;
    if (bx < 0.0f || by < 0.0f || bx >= bodyW_ || by >= bodyH_)
    {
        return {};
    }

    if (mode_ == Mode::Start)
    {
        const float top = bodyH_ - kFooterH;
        if (by >= top)
        {
            if (Contains(systemButton_, bx, by))
            {
                return Hit{Target::SystemStart, -1};
            }
            if (Contains(powerButton_, bx, by))
            {
                return Hit{Target::Power, -1};
            }
            if (Contains(settingsButton_, bx, by))
            {
                return Hit{Target::Settings, -1};
            }
            return {};
        }
    }

    const D2D1_RECT_F content = ContentRect();
    if (Contains(content, bx, by))
    {
        const float cy = by - content.top + scroll_;
        if (GridMode())
        {
            const int sticky = StickySection();
            if (sticky >= 0 && sections_[static_cast<size_t>(sticky)].group >= 0
                && cy < scroll_ + kGroupHeaderH)
            {
                return Hit{Target::GroupHeader, sticky};
            }
            for (size_t i = 0; i < sections_.size(); ++i)
            {
                if (sections_[i].group >= 0
                    && Contains(sections_[i].header, bx, cy))
                {
                    return Hit{Target::GroupHeader, static_cast<int>(i)};
                }
            }
        }
        for (int i = 0; i < VisibleCount(); ++i)
        {
            if (Contains(ItemRect(i), bx, cy))
            {
                if (GridMode()
                    && Contains(Inset(PinButtonRect(i), -2.0f, -2.0f), bx, cy))
                {
                    return Hit{Target::PinToggle, i};
                }
                return Hit{Target::Item, i};
            }
        }
        return {};
    }
    return Hit{Target::Field, -1};
}

void Launcher::MoveSelection(int dx, int dy)
{
    const int count = VisibleCount();
    if (count == 0)
    {
        return;
    }
    if (selected_ < 0)
    {
        selected_ = 0;
        EnsureSelectionVisible();
        Render();
        return;
    }

    int next = selected_;
    if (!GridMode())
    {
        next += dy;
    }
    else if (dx != 0)
    {
        next += dx;
    }
    else
    {
        // Rows of different groups are not aligned to one big grid: move to
        // the cell in the next/previous row closest horizontally.
        for (int step = 0; step < std::abs(dy); ++step)
        {
            const D2D1_RECT_F from = cells_[static_cast<size_t>(next)].rect;
            const float fromX = (from.left + from.right) * 0.5f;
            int best = -1;
            float bestRow = 0.0f;
            float bestDistance = 0.0f;
            for (int i = 0; i < count; ++i)
            {
                const D2D1_RECT_F r = cells_[static_cast<size_t>(i)].rect;
                const bool candidate = dy > 0 ? r.top > from.top + 1.0f
                                              : r.top < from.top - 1.0f;
                if (!candidate)
                {
                    continue;
                }
                const float distance = std::abs((r.left + r.right) * 0.5f - fromX);
                const bool closerRow = best < 0
                    || (dy > 0 ? r.top < bestRow - 1.0f : r.top > bestRow + 1.0f);
                const bool sameRow = best >= 0 && std::abs(r.top - bestRow) < 1.0f;
                if (closerRow || (sameRow && distance < bestDistance))
                {
                    best = i;
                    bestRow = r.top;
                    bestDistance = distance;
                }
            }
            if (best < 0)
            {
                break;
            }
            next = best;
        }
    }
    selected_ = std::clamp(next, 0, count - 1);
    EnsureSelectionVisible();
    Render();
}

void Launcher::LaunchSelected(bool elevated)
{
    if (selected_ < 0 || selected_ >= VisibleCount())
    {
        return;
    }
    const int index = AppAt(selected_);
    if (index < 0)
    {
        return;
    }
    const AppCatalog::App app = catalog_.Apps()[static_cast<size_t>(index)];
    Hide();
    AppCatalog::Launch(app, elevated);
}

void Launcher::ShowItemMenu(int index, POINT screen)
{
    if (index < 0 || index >= VisibleCount())
    {
        return;
    }
    selected_ = index;
    Render();

    const int appIndex = AppAt(index);
    if (appIndex < 0)
    {
        return;
    }
    const AppCatalog::App app = catalog_.Apps()[static_cast<size_t>(appIndex)];
    const bool desktop = !app.targetPath.empty();

    std::vector<ui::MenuItem> items;
    items.push_back(ui::MenuItem::Command(1, T(L"打开")));
    if (desktop)
    {
        items.push_back(ui::MenuItem::Command(2, T(L"以管理员身份运行")));
    }
    items.push_back(ui::MenuItem::Separator());
    items.push_back(ui::MenuItem::Command(
        5, IsPinned(appIndex) ? T(L"取消置顶") : T(L"置顶")));
    items.push_back(ui::MenuItem::Command(
        6, IsHidden(appIndex) ? T(L"取消隐藏") : T(L"隐藏")));
    if (desktop)
    {
        items.push_back(ui::MenuItem::Command(3, T(L"固定到 Dock")));
        items.push_back(ui::MenuItem::Command(4, T(L"打开所在文件夹")));
    }

    menuOpen_ = true;
    const UINT command = ui::TrackMenu(hwnd_, items, screen);
    menuOpen_ = false;

    switch (command)
    {
    case 1:
        LaunchSelected();
        break;
    case 2:
        LaunchSelected(true);
        break;
    case 3:
        Hide();
        if (host_)
        {
            host_->OnLauncherPin(app.targetPath);
        }
        break;
    case 4:
        Hide();
        AppLauncher::RevealInExplorer(app.targetPath);
        break;
    case 5:
        TogglePin(appIndex);
        break;
    case 6:
        ToggleHidden(appIndex);
        break;
    default:
        break;
    }
}

void Launcher::ShowPowerMenu()
{
    std::vector<ui::MenuItem> items;
    items.push_back(ui::MenuItem::Command(1, T(L"锁定")));
    items.push_back(ui::MenuItem::Command(2, T(L"注销")));
    items.push_back(ui::MenuItem::Separator());
    items.push_back(ui::MenuItem::Command(3, T(L"重启")));
    items.push_back(ui::MenuItem::Command(4, T(L"关机")));

    POINT anchor{
        windowPos_.x + static_cast<LONG>(std::lround(
            (body_.left + powerButton_.right) * scale_)),
        windowPos_.y + static_cast<LONG>(std::lround(
            (body_.top + powerButton_.top - 4.0f) * scale_))};
    ui::MenuOptions options;
    options.alignRight = true;
    options.alignBottom = true;

    menuOpen_ = true;
    const UINT command = ui::TrackMenu(hwnd_, items, anchor, options);
    menuOpen_ = false;
    if (command == 0)
    {
        return;
    }

    Hide();
    switch (command)
    {
    case 1:
        LockWorkStation();
        break;
    case 2:
        ExitWindowsEx(EWX_LOGOFF, SHTDN_REASON_FLAG_PLANNED);
        break;
    case 3:
        ShellOpen(L"shutdown.exe", L"/r /t 0", SW_HIDE);
        break;
    case 4:
        ShellOpen(L"shutdown.exe", L"/s /hybrid /t 0", SW_HIDE);
        break;
    default:
        break;
    }
}

void Launcher::InsertText(const std::wstring& text)
{
    for (wchar_t c : text)
    {
        if (c >= 32 && c != 127)
        {
            query_.push_back(c);
        }
    }
    UpdateResults();
    Place();
    caretOn_ = true;
    SetTimer(hwnd_, kCaretTimer, 530, nullptr);
    Render();
}

void Launcher::Backspace()
{
    if (query_.empty())
    {
        return;
    }
    query_.pop_back();
    if (!query_.empty() && IS_HIGH_SURROGATE(query_.back()))
    {
        query_.pop_back();
    }
    UpdateResults();
    Place();
    caretOn_ = true;
    SetTimer(hwnd_, kCaretTimer, 530, nullptr);
    Render();
}

// --- window procedure ------------------------------------------------------------

LRESULT CALLBACK Launcher::WindowProc(HWND hwnd, UINT message, WPARAM wParam,
                                      LPARAM lParam)
{
    Launcher* self = nullptr;
    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<Launcher*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self)
        {
            self->hwnd_ = hwnd;
        }
    }
    else
    {
        self = reinterpret_cast<Launcher*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self || self->hwnd_ != hwnd)
    {
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return self->HandleMessage(message, wParam, lParam);
}

LRESULT Launcher::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    auto dipX = [this](LPARAM l) { return GET_X_LPARAM(l) / scale_; };
    auto dipY = [this](LPARAM l) { return GET_Y_LPARAM(l) / scale_; };

    switch (message)
    {
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE && !menuOpen_)
        {
            Hide();
        }
        break;

    case WM_SETFOCUS:
        CreateCaret(hwnd_, nullptr, 2,
                    static_cast<int>(std::lround(22.0f * scale_)));
        Render();
        return 0;

    case WM_KILLFOCUS:
        DestroyCaret();
        return 0;

    case WM_TIMER:
        if (wParam == kCaretTimer)
        {
            caretOn_ = !caretOn_;
            Render();
        }
        else if (wParam == kFrameTimer)
        {
            const float t = (std::min)(
                1.0f, static_cast<float>(GetTickCount64() - shownAt_) / 120.0f);
            const BYTE alpha = static_cast<BYTE>(std::lround(
                (1.0f - (1.0f - t) * (1.0f - t)) * 255.0f));
            const bool fading = alpha != alpha_;
            alpha_ = alpha;
            if (fading || iconsPending_)
            {
                Render();
            }
            if (alpha_ == 255 && !iconsPending_)
            {
                KillTimer(hwnd_, kFrameTimer);
            }
        }
        return 0;

    case WM_MOUSEMOVE:
    {
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd_, 0};
        TrackMouseEvent(&track);
        const float x = dipX(lParam);
        const float y = dipY(lParam);
        const D2D1_RECT_F content = ContentRect();
        const float contentX = x - body_.left;
        const float contentY = y - body_.top - content.top + scroll_;

        if (dragging_)
        {
            UpdateDrag(contentX, contentY);
            return 0;
        }

        // Press and move on a grid cell starts a drag into the pinned row.
        if ((wParam & MK_LBUTTON) && pressed_.target == Target::Item
            && GridMode())
        {
            const float dx = contentX - pressPoint_.x;
            const float dy = contentY - pressPoint_.y;
            if (dx * dx + dy * dy > 36.0f)
            {
                const Cell& cell = cells_[static_cast<size_t>(pressed_.index)];
                dragging_ = true;
                dragApp_ = cell.app;
                dragFromPinned_ = cell.pinned;
                dragSlot_ = -1;
                hover_ = Hit{};
                SetCapture(hwnd_);
                UpdateDrag(contentX, contentY);
                return 0;
            }
        }

        const Hit hit = HitTest(x, y);
        bool changed = !(hit == hover_);
        hover_ = hit;
        if ((hit.target == Target::Item || hit.target == Target::PinToggle)
            && hit.index != selected_)
        {
            selected_ = hit.index;
            changed = true;
        }
        if (changed)
        {
            Render();
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        if (hover_.target != Target::None)
        {
            hover_ = Hit{};
            Render();
        }
        return 0;

    case WM_SETCURSOR:
    {
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(hwnd_, &point);
        const Hit hit = HitTest(point.x / scale_, point.y / scale_);
        SetCursor(LoadCursorW(nullptr, hit.target == Target::Field
                                           ? IDC_IBEAM : IDC_ARROW));
        return TRUE;
    }

    case WM_LBUTTONDOWN:
    {
        pressed_ = HitTest(dipX(lParam), dipY(lParam));
        const D2D1_RECT_F content = ContentRect();
        pressPoint_ = D2D1::Point2F(
            dipX(lParam) - body_.left,
            dipY(lParam) - body_.top - content.top + scroll_);
        return 0;
    }

    case WM_CAPTURECHANGED:
        if (dragging_ && reinterpret_cast<HWND>(lParam) != hwnd_)
        {
            dragSlot_ = -1;
            FinishDrag();
        }
        return 0;

    case WM_LBUTTONUP:
    {
        if (dragging_)
        {
            pressed_ = Hit{};
            FinishDrag();
            if (GetCapture() == hwnd_)
            {
                ReleaseCapture();
            }
            return 0;
        }

        const Hit hit = HitTest(dipX(lParam), dipY(lParam));
        const Hit pressed = pressed_;
        pressed_ = Hit{};
        if (!(hit == pressed))
        {
            return 0;
        }
        switch (hit.target)
        {
        case Target::PinToggle:
            TogglePin(AppAt(hit.index));
            break;
        case Target::GroupHeader:
        {
            const int sticky = StickySection();
            ToggleGroup(hit.index, hit.index == sticky
                && sections_[static_cast<size_t>(sticky)].expanded);
            break;
        }
        case Target::Item:
            selected_ = hit.index;
            LaunchSelected();
            break;
        case Target::SystemStart:
            Hide();
            if (host_)
            {
                host_->OnLauncherSystemStart();
            }
            break;
        case Target::Settings:
            Hide();
            ShellOpen(L"ms-settings:");
            break;
        case Target::Power:
            ShowPowerMenu();
            break;
        default:
            break;
        }
        return 0;
    }

    case WM_RBUTTONUP:
    {
        const Hit hit = HitTest(dipX(lParam), dipY(lParam));
        if (hit.target == Target::Item || hit.target == Target::PinToggle)
        {
            POINT screen{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ClientToScreen(hwnd_, &screen);
            ShowItemMenu(hit.index, screen);
        }
        return 0;
    }

    case WM_MOUSEWHEEL:
        Scroll(-static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam))
               / WHEEL_DELTA * 60.0f);
        Render();
        return 0;

    case WM_KEYDOWN:
    {
        const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        switch (wParam)
        {
        case VK_ESCAPE:
            if (!query_.empty())
            {
                query_.clear();
                UpdateResults();
                Place();
                Render();
            }
            else
            {
                Hide();
            }
            return 0;
        case VK_RETURN:
            LaunchSelected(control && shift);
            return 0;
        case VK_UP:
            MoveSelection(0, -1);
            return 0;
        case VK_DOWN:
            MoveSelection(0, 1);
            return 0;
        case VK_LEFT:
            if (GridMode())
            {
                MoveSelection(-1, 0);
            }
            return 0;
        case VK_RIGHT:
            if (GridMode())
            {
                MoveSelection(1, 0);
            }
            return 0;
        case VK_PRIOR:
            MoveSelection(0, GridMode() ? -4 : -kSearchRows);
            return 0;
        case VK_NEXT:
            MoveSelection(0, GridMode() ? 4 : kSearchRows);
            return 0;
        case 'V':
            if (control && OpenClipboard(hwnd_))
            {
                HANDLE data = GetClipboardData(CF_UNICODETEXT);
                const auto* text = data
                    ? static_cast<const wchar_t*>(GlobalLock(data)) : nullptr;
                std::wstring pasted;
                if (text)
                {
                    pasted = text;
                    GlobalUnlock(data);
                }
                CloseClipboard();
                for (wchar_t& c : pasted)
                {
                    if (c == L'\r' || c == L'\n' || c == L'\t')
                    {
                        c = L' ';
                    }
                }
                InsertText(pasted);
            }
            return 0;
        default:
            break;
        }
        break;
    }

    case WM_CHAR:
    {
        const wchar_t c = static_cast<wchar_t>(wParam);
        if (c == 8)
        {
            Backspace();
        }
        else if (c == 127)
        {
            // Ctrl+Backspace clears the query.
            query_.clear();
            UpdateResults();
            Place();
            Render();
        }
        else if (c >= 32)
        {
            InsertText(std::wstring(1, c));
        }
        return 0;
    }

    case kRefreshMessage:
        if (visible_)
        {
            const float scroll = scroll_;
            catalog_.Refresh(true);
            UpdateResults();
            scroll_ = scroll;
            Scroll(0.0f);
            Place();
            Render();
        }
        return 0;

    case WM_SETTINGCHANGE:
        if (lParam && wcscmp(reinterpret_cast<const wchar_t*>(lParam),
                             L"ImmersiveColorSet") == 0)
        {
            dark_ = ui::SystemUsesDarkTheme();
            palette_ = &ui::GetPalette(dark_);
            Render();
        }
        break;

    case WM_NCDESTROY:
    {
        HWND hwnd = hwnd_;
        hwnd_ = nullptr;
        visible_ = false;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    default:
        break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

} // namespace ld

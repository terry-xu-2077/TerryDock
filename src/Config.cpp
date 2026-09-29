#include "Config.h"

#include "Json.h"
#include "Utils.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ld
{

std::wstring Config::Directory()
{
    return GetAppDataDir();
}

std::wstring Config::FilePath()
{
    return GetAppDataDir() + L"\\config.json";
}

DockConfig Config::Load()
{
    DockConfig config;

    const std::wstring path = FilePath();

    FILE* file = _wfopen(path.c_str(), L"rb");
    if (!file)
    {
        return config;
    }

    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fseek(file, 0, SEEK_SET);

    std::string text;
    if (size > 0)
    {
        text.resize(static_cast<size_t>(size));
        const size_t read = fread(text.data(), 1, text.size(), file);
        text.resize(read);
    }

    fclose(file);

    if (text.empty())
    {
        return config;
    }

    const json::ParseResult result = json::Parse(text);
    if (!result.ok || !result.value.IsObject())
    {
        return config;
    }

    const json::Value& root = result.value;

    if (const json::Value* v = root.Find("iconSize"))
    {
        config.settings.iconSize = v->AsInt(config.settings.iconSize);
    }

    if (const json::Value* v = root.Find("iconSpacing"))
    {
        config.settings.iconSpacing = v->AsInt(config.settings.iconSpacing);
    }

    if (const json::Value* v = root.Find("overallScale"))
    {
        config.settings.overallScale = ClampF(
            static_cast<float>(v->AsDouble(config.settings.overallScale)),
            0.5f, 1.5f);
    }

    if (const json::Value* v = root.Find("autoHide"))
    {
        config.settings.autoHide = v->AsBool(config.settings.autoHide);
    }

    if (const json::Value* v = root.Find("autoHideDelayMs"))
    {
        config.settings.autoHideDelayMs = (std::clamp)(
            v->AsInt(config.settings.autoHideDelayMs), 0, 5000);
    }

    if (const json::Value* v = root.Find("autoHideAnimationMs"))
    {
        config.settings.autoHideAnimationMs = std::clamp(
            v->AsInt(config.settings.autoHideAnimationMs), 0, 1000);
    }
    else if (const json::Value* v = root.Find("autoHideSpeed"))
    {
        // Migrate the old spring-frequency multiplier to an intuitive
        // duration while retaining the closest approximate feel.
        const float legacy = ClampF(
            static_cast<float>(v->AsDouble(0.64)), 0.1f, 2.0f);
        config.settings.autoHideAnimationMs = std::clamp(
            static_cast<int>(std::lround(60.0f / legacy)), 0, 1000);
    }

    if (const json::Value* v = root.Find("magnification"))
    {
        config.settings.magnification =
            static_cast<float>(v->AsDouble(config.settings.magnification));
    }

    if (const json::Value* v = root.Find("tooltipOpacity"))
    {
        config.settings.tooltipOpacity = ClampF(
            static_cast<float>(v->AsDouble(config.settings.tooltipOpacity)),
            0.0f, 1.0f);
    }

    if (const json::Value* v = root.Find("tooltipFadeSeconds"))
    {
        config.settings.tooltipFadeSeconds = ClampF(
            static_cast<float>(v->AsDouble(config.settings.tooltipFadeSeconds)),
            0.05f, 1.0f);
    }

    if (const json::Value* v = root.Find("tooltipScaleV2"))
    {
        config.settings.tooltipScale = ClampF(
            static_cast<float>(v->AsDouble(config.settings.tooltipScale)),
            0.5f, 2.0f);
    }
    else if (const json::Value* v = root.Find("tooltipScale"))
    {
        // Normalize the user's previous 80% setting to the new 100% baseline.
        config.settings.tooltipScale = ClampF(
            static_cast<float>(v->AsDouble(0.8)) / 0.8f,
            0.5f, 2.0f);
    }

    if (const json::Value* v = root.Find("englishLanguage"))
    {
        config.settings.englishLanguage = v->AsBool(false);
    }

    if (const json::Value* v = root.Find("tooltipCornerRadius"))
    {
        config.settings.tooltipCornerRadius = ClampF(
            static_cast<float>(v->AsDouble(
                config.settings.tooltipCornerRadius)), 0.0f, 40.0f);
    }

    if (const json::Value* v = root.Find("windowMenuHoverDelayMs"))
    {
        config.settings.windowMenuHoverDelayMs = std::clamp(
            v->AsInt(config.settings.windowMenuHoverDelayMs), 0, 1500);
    }

    if (const json::Value* background = root.Find("background"))
    {
        if (const json::Value* v = background->Find("opacity"))
        {
            config.settings.backgroundOpacity =
                static_cast<float>(v->AsDouble(config.settings.backgroundOpacity));
        }

        if (const json::Value* v = background->Find("cornerRadius"))
        {
            config.settings.cornerRadius =
                static_cast<float>(v->AsDouble(config.settings.cornerRadius));
        }

        if (const json::Value* v = background->Find("borderOpacity"))
        {
            config.settings.borderOpacity =
                static_cast<float>(v->AsDouble(config.settings.borderOpacity));
        }

        if (const json::Value* v = background->Find("shadowOpacity"))
        {
            config.settings.shadowOpacity =
                static_cast<float>(v->AsDouble(config.settings.shadowOpacity));
        }

        if (const json::Value* v = background->Find("top"))
        {
            config.settings.backgroundTop = Utf8ToWide(v->AsString());
        }

        if (const json::Value* v = background->Find("bottom"))
        {
            config.settings.backgroundBottom = Utf8ToWide(v->AsString());
        }
    }

    // Migration: the old format had a global "enabled" switch for the plate.
    // That switch now lives on every icon, so remember the old value and use
    // it as the default for entries that carry no explicit one.
    bool plateEnabledDefault = true;

    if (const json::Value* backdrop = root.Find("iconBackdrop"))
    {
        if (const json::Value* v = backdrop->Find("enabled"))
        {
            plateEnabledDefault = v->AsBool(plateEnabledDefault);
        }

        if (const json::Value* v = backdrop->Find("cornerRadius"))
        {
            config.settings.backdrop.cornerRadius =
                static_cast<float>(v->AsDouble(config.settings.backdrop.cornerRadius));
        }

        if (const json::Value* v = backdrop->Find("opacity"))
        {
            config.settings.backdrop.opacity =
                static_cast<float>(v->AsDouble(config.settings.backdrop.opacity));
        }

        if (const json::Value* v = backdrop->Find("strokeWidth"))
        {
            config.settings.backdrop.strokeWidth = ClampF(
                static_cast<float>(v->AsDouble(config.settings.backdrop.strokeWidth)),
                0.0f, 4.0f);
        }

        if (const json::Value* v = backdrop->Find("strokeOpacity"))
        {
            config.settings.backdrop.strokeOpacity = ClampF(
                static_cast<float>(v->AsDouble(config.settings.backdrop.strokeOpacity)),
                0.0f, 1.0f);
        }

        if (const json::Value* v = backdrop->Find("iconScale"))
        {
            config.settings.backdrop.iconScale =
                static_cast<float>(v->AsDouble(config.settings.backdrop.iconScale));
        }

        // Legacy "gradient" / "top" / "bottom" keys are intentionally not
        // read: the global layer no longer owns plate colours.
    }

    if (const json::Value* v = root.Find("panelMode"))
    {
        const std::wstring mode = Utf8ToWide(v->AsString("fixed"));

        if (EqualsIgnoreCase(mode, L"elastic"))
        {
            config.settings.panelMode = PanelMode::Elastic;
        }
        else if (EqualsIgnoreCase(mode, L"static")
                 || EqualsIgnoreCase(mode, L"none"))
        {
            config.settings.panelMode = PanelMode::Static;
        }
        else
        {
            config.settings.panelMode = PanelMode::Fixed;
        }
    }

    if (const json::Value* v = root.Find("dockEdge"))
    {
        const std::wstring edge = Utf8ToWide(v->AsString("bottom"));
        if (EqualsIgnoreCase(edge, L"top"))
            config.settings.dockEdge = DockEdge::Top;
        else if (EqualsIgnoreCase(edge, L"left"))
            config.settings.dockEdge = DockEdge::Left;
        else if (EqualsIgnoreCase(edge, L"right"))
            config.settings.dockEdge = DockEdge::Right;
        else
            config.settings.dockEdge = DockEdge::Bottom;
    }

    if (const json::Value* v = root.Find("monitor"))
    {
        config.settings.monitor = Utf8ToWide(v->AsString());
    }

    if (const json::Value* apps = root.Find("apps"))
    {
        for (const json::Value& entry : apps->Items())
        {
            if (!entry.IsObject())
            {
                continue;
            }

            AppEntry item;
            item.id = Utf8ToWide(entry.Find("id")
                ? entry.Find("id")->AsString() : std::string());
            item.name = Utf8ToWide(entry.Find("name")
                ? entry.Find("name")->AsString() : std::string());
            item.targetPath = Utf8ToWide(entry.Find("targetPath")
                ? entry.Find("targetPath")->AsString() : std::string());
            item.resolvedPath = Utf8ToWide(entry.Find("resolvedPath")
                ? entry.Find("resolvedPath")->AsString() : std::string());
            item.arguments = Utf8ToWide(entry.Find("arguments")
                ? entry.Find("arguments")->AsString() : std::string());
            item.processName = Utf8ToWide(entry.Find("processName")
                ? entry.Find("processName")->AsString() : std::string());
            item.iconFile = Utf8ToWide(entry.Find("icon")
                ? entry.Find("icon")->AsString() : std::string());

            // Per entry plate look. The corner radius is global and is not
            // read here; a missing "enabled" inherits the old global switch.
            if (const json::Value* plate = entry.Find("plate"))
            {
                auto readFloat = [plate](const char* key, float fallback)
                {
                    return plate->Find(key)
                        ? static_cast<float>(plate->Find(key)->AsDouble(fallback))
                        : fallback;
                };

                item.plate.enabled = plate->Find("enabled")
                    ? plate->Find("enabled")->AsBool(item.plate.enabled)
                    : plateEnabledDefault;

                item.plate.iconScale = readFloat("iconScale",
                                                 item.plate.iconScale);

                if (plate->Find("opacity"))
                {
                    item.plate.opacity = ClampF(
                        static_cast<float>(plate->Find("opacity")->AsDouble(
                            item.plate.opacity)), 0.0f, 1.0f);
                }

                // Legacy entries only carried "bottom" when the user had
                // actually set a second colour, so its presence implies the
                // custom switch.
                item.plate.customBottom = plate->Find("customBottom")
                    ? plate->Find("customBottom")->AsBool(item.plate.customBottom)
                    : plate->Find("bottom") != nullptr;

                if (plate->Find("top"))
                {
                    item.plate.top = Utf8ToWide(plate->Find("top")->AsString());
                }

                if (plate->Find("bottom"))
                {
                    item.plate.bottom =
                        Utf8ToWide(plate->Find("bottom")->AsString());
                }

                if (plate->Find("strokeColor"))
                {
                    item.plate.strokeColor =
                        Utf8ToWide(plate->Find("strokeColor")->AsString());
                }

                if (plate->Find("strokeOpacity"))
                {
                    item.plate.strokeOpacity = ClampF(
                        static_cast<float>(plate->Find("strokeOpacity")->AsDouble(
                            item.plate.strokeOpacity)), 0.0f, 1.0f);
                }
            }

            if (item.targetPath.empty())
            {
                continue;
            }

            if (item.id.empty())
            {
                item.id = MakeStableId(item.targetPath);
            }

            if (item.resolvedPath.empty())
            {
                item.resolvedPath = item.targetPath;
            }

            if (item.processName.empty())
            {
                item.processName = GetFileName(item.resolvedPath);
            }

            if (item.name.empty())
            {
                item.name = GetFileStem(item.resolvedPath);
            }

            if (item.iconFile.empty())
            {
                item.iconFile = item.id + L".png";
            }

            config.apps.push_back(std::move(item));
        }
    }

    return config;
}

bool Config::Save(const DockConfig& config)
{
    EnsureDirectoryExists(Directory());
    EnsureDirectoryExists(GetIconCacheDir());

    json::Value root(json::Value::Type::Object);

    root.Set("iconSize", json::Value(config.settings.iconSize));
    root.Set("iconSpacing", json::Value(config.settings.iconSpacing));
    root.Set("overallScale",
             json::Value(static_cast<double>(config.settings.overallScale)));
    root.Set("autoHide", json::Value(config.settings.autoHide));
    root.Set("autoHideDelayMs", json::Value(config.settings.autoHideDelayMs));
    root.Set("autoHideAnimationMs",
             json::Value(config.settings.autoHideAnimationMs));
    root.Set("magnification",
             json::Value(static_cast<double>(config.settings.magnification)));
    root.Set("tooltipOpacity",
             json::Value(static_cast<double>(config.settings.tooltipOpacity)));
    root.Set("tooltipFadeSeconds",
             json::Value(static_cast<double>(config.settings.tooltipFadeSeconds)));
    root.Set("tooltipScaleV2",
             json::Value(static_cast<double>(config.settings.tooltipScale)));
    root.Set("englishLanguage",
             json::Value(config.settings.englishLanguage));
    root.Set("tooltipCornerRadius",
             json::Value(static_cast<double>(
                 config.settings.tooltipCornerRadius)));
    root.Set("windowMenuHoverDelayMs",
             json::Value(config.settings.windowMenuHoverDelayMs));

    json::Value background(json::Value::Type::Object);
    background.Set("opacity",
                   json::Value(static_cast<double>(config.settings.backgroundOpacity)));
    background.Set("cornerRadius",
                   json::Value(static_cast<double>(config.settings.cornerRadius)));
    background.Set("borderOpacity",
                   json::Value(static_cast<double>(config.settings.borderOpacity)));
    background.Set("shadowOpacity",
                   json::Value(static_cast<double>(config.settings.shadowOpacity)));

    // Panel background gradient. Written only when customised so a fresh
    // install keeps a minimal config file.
    if (!config.settings.backgroundTop.empty()
        || !config.settings.backgroundBottom.empty())
    {
        background.Set("top",
                       json::Value(WideToUtf8(config.settings.backgroundTop)));
        background.Set("bottom",
                       json::Value(WideToUtf8(config.settings.backgroundBottom)));
    }
    root.Set("background", std::move(background));

    json::Value backdrop(json::Value::Type::Object);
    backdrop.Set("cornerRadius",
                 json::Value(static_cast<double>(config.settings.backdrop.cornerRadius)));
    backdrop.Set("opacity",
                 json::Value(static_cast<double>(config.settings.backdrop.opacity)));
    backdrop.Set("strokeWidth",
                 json::Value(static_cast<double>(config.settings.backdrop.strokeWidth)));
    backdrop.Set("strokeOpacity",
                 json::Value(static_cast<double>(config.settings.backdrop.strokeOpacity)));
    backdrop.Set("iconScale",
                 json::Value(static_cast<double>(config.settings.backdrop.iconScale)));
    root.Set("iconBackdrop", std::move(backdrop));

    const char* panelMode = "fixed";
    if (config.settings.panelMode == PanelMode::Elastic)
    {
        panelMode = "elastic";
    }
    else if (config.settings.panelMode == PanelMode::Static)
    {
        panelMode = "static";
    }

    root.Set("panelMode", json::Value(panelMode));

    const char* dockEdge = "bottom";
    switch (config.settings.dockEdge)
    {
    case DockEdge::Top: dockEdge = "top"; break;
    case DockEdge::Left: dockEdge = "left"; break;
    case DockEdge::Right: dockEdge = "right"; break;
    case DockEdge::Bottom: default: break;
    }
    root.Set("dockEdge", json::Value(dockEdge));

    if (!config.settings.monitor.empty())
    {
        root.Set("monitor", json::Value(WideToUtf8(config.settings.monitor)));
    }

    json::Value apps(json::Value::Type::Array);
    for (const AppEntry& item : config.apps)
    {
        json::Value entry(json::Value::Type::Object);
        entry.Set("id", json::Value(WideToUtf8(item.id)));
        entry.Set("name", json::Value(WideToUtf8(item.name)));
        entry.Set("targetPath", json::Value(WideToUtf8(item.targetPath)));
        entry.Set("resolvedPath", json::Value(WideToUtf8(item.resolvedPath)));
        entry.Set("arguments", json::Value(WideToUtf8(item.arguments)));
        entry.Set("processName", json::Value(WideToUtf8(item.processName)));
        entry.Set("icon", json::Value(WideToUtf8(item.iconFile)));

        // Every entry carries its plate block: the state is small and this
        // keeps the on-disk format lossless for the editor.
        {
            json::Value plate(json::Value::Type::Object);
            plate.Set("enabled", json::Value(item.plate.enabled));
            plate.Set("iconScale",
                      json::Value(static_cast<double>(item.plate.iconScale)));
            if (item.plate.opacity >= 0.0f)
            {
                plate.Set("opacity",
                          json::Value(static_cast<double>(item.plate.opacity)));
            }
            plate.Set("customBottom", json::Value(item.plate.customBottom));
            plate.Set("top", json::Value(WideToUtf8(item.plate.top)));
            plate.Set("bottom", json::Value(WideToUtf8(item.plate.bottom)));
            if (!item.plate.strokeColor.empty())
            {
                plate.Set("strokeColor",
                          json::Value(WideToUtf8(item.plate.strokeColor)));
            }
            if (item.plate.strokeOpacity >= 0.0f)
            {
                plate.Set("strokeOpacity",
                          json::Value(static_cast<double>(item.plate.strokeOpacity)));
            }
            entry.Set("plate", std::move(plate));
        }

        apps.PushBack(std::move(entry));
    }

    root.Set("apps", std::move(apps));

    const std::string text = json::Serialize(root, true);
    const std::wstring path = FilePath();

    FILE* file = _wfopen(path.c_str(), L"wb");
    if (!file)
    {
        return false;
    }

    const size_t written = fwrite(text.data(), 1, text.size(), file);
    fclose(file);

    return written == text.size();
}

} // namespace ld

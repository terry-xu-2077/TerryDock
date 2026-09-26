#include "Utils.h"

#include <shlobj.h>

#include <cctype>
#include <cstdio>
#include <vector>

namespace ld
{

std::wstring Utf8ToWide(std::string_view text)
{
    if (text.empty())
    {
        return {};
    }

    const int needed = MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);

    if (needed <= 0)
    {
        return {};
    }

    std::wstring result(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        result.data(), needed);

    return result;
}

std::string WideToUtf8(std::wstring_view text)
{
    if (text.empty())
    {
        return {};
    }

    const int needed = WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);

    if (needed <= 0)
    {
        return {};
    }

    std::string result(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        result.data(), needed, nullptr, nullptr);

    return result;
}

std::wstring GetAppDataDir()
{
    wchar_t buffer[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(
            nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, buffer)))
    {
        return std::wstring(buffer) + L"\\LightDock";
    }

    if (const wchar_t* env = _wgetenv(L"APPDATA"))
    {
        return std::wstring(env) + L"\\LightDock";
    }

    return L"LightDock";
}

std::wstring GetIconCacheDir()
{
    return GetAppDataDir() + L"\\icons";
}

bool EnsureDirectoryExists(const std::wstring& directory)
{
    if (directory.empty())
    {
        return false;
    }

    if (GetFileAttributesW(directory.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        return true;
    }

    return CreateDirectoryW(directory.c_str(), nullptr) != FALSE
        || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool FileExists(const std::wstring& path)
{
    if (path.empty())
    {
        return false;
    }

    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES
        && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool DeleteFileIfExists(const std::wstring& path)
{
    if (!FileExists(path))
    {
        return false;
    }

    return DeleteFileW(path.c_str()) != FALSE;
}

std::wstring GetFileExtension(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    const size_t dot = path.find_last_of(L'.');

    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash))
    {
        return {};
    }

    std::wstring extension = path.substr(dot);
    for (wchar_t& ch : extension)
    {
        ch = static_cast<wchar_t>(towlower(ch));
    }

    return extension;
}

std::wstring GetFileName(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
    {
        return path;
    }

    return path.substr(slash + 1);
}

std::wstring GetFileStem(const std::wstring& path)
{
    std::wstring name = GetFileName(path);
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos)
    {
        return name;
    }

    return name.substr(0, dot);
}

std::wstring GetFileDescription(const std::wstring& path)
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

    std::vector<unsigned char> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()))
    {
        return {};
    }

    struct LanguageCodePage
    {
        WORD language;
        WORD codePage;
    };

    auto* translation = static_cast<LanguageCodePage*>(nullptr);
    UINT translationSize = 0;

    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation",
                        reinterpret_cast<void**>(&translation),
                        &translationSize)
        || translation == nullptr
        || translationSize < sizeof(LanguageCodePage))
    {
        return {};
    }

    // The first translation block is the one the executable was built with;
    // that is the localized (e.g. Simplified Chinese) string table.
    auto query = [&](const wchar_t* key) -> std::wstring
    {
        wchar_t subPath[128];
        swprintf(subPath, 128, L"\\StringFileInfo\\%04x%04x\\%s",
                 translation[0].language, translation[0].codePage, key);

        wchar_t* value = nullptr;
        UINT valueSize = 0;
        if (VerQueryValueW(data.data(), subPath,
                           reinterpret_cast<void**>(&value), &valueSize)
            && value != nullptr && value[0] != L'\0')
        {
            return std::wstring(value);
        }

        return {};
    };

    std::wstring description = query(L"FileDescription");
    if (!description.empty())
    {
        return description;
    }

    return query(L"ProductName");
}

std::wstring GetParentDirectory(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
    {
        return {};
    }

    if (slash == 0)
    {
        return path.substr(0, 1);
    }

    return path.substr(0, slash);
}

std::wstring ToLower(std::wstring text)
{
    for (wchar_t& ch : text)
    {
        ch = static_cast<wchar_t>(towlower(ch));
    }

    return text;
}

bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b)
{
    if (a.size() != b.size())
    {
        return false;
    }

    for (size_t i = 0; i < a.size(); ++i)
    {
        if (towlower(a[i]) != towlower(b[i]))
        {
            return false;
        }
    }

    return true;
}

bool ParseHexColor(std::wstring_view text,
                   float& r, float& g, float& b, float& a)
{
    std::wstring value(text);

    if (!value.empty() && value[0] == L'#')
    {
        value.erase(0, 1);
    }

    if (value.size() != 3 && value.size() != 6 && value.size() != 8)
    {
        return false;
    }

    for (wchar_t c : value)
    {
        if (!iswxdigit(c))
        {
            return false;
        }
    }

    auto channel = [](wchar_t hi, wchar_t lo) -> float
    {
        auto nibble = [](wchar_t c) -> int
        {
            if (c >= L'0' && c <= L'9') { return c - L'0'; }
            if (c >= L'a' && c <= L'f') { return c - L'a' + 10; }
            if (c >= L'A' && c <= L'F') { return c - L'A' + 10; }
            return 0;
        };

        return static_cast<float>(nibble(hi) * 16 + nibble(lo)) / 255.0f;
    };

    if (value.size() == 3)
    {
        r = channel(value[0], value[0]);
        g = channel(value[1], value[1]);
        b = channel(value[2], value[2]);
        a = 1.0f;
        return true;
    }

    r = channel(value[0], value[1]);
    g = channel(value[2], value[3]);
    b = channel(value[4], value[5]);
    a = (value.size() == 8) ? channel(value[6], value[7]) : 1.0f;

    return true;
}

std::wstring MakeStableId(std::wstring_view seed)
{
    // FNV-1a 64 bit. Cheap, stable across runs, no dependency on locale.
    uint64_t hash = 14695981039346656037ull;

    std::string bytes = WideToUtf8(seed);
    for (char c : bytes)
    {
        hash ^= static_cast<uint64_t>(static_cast<unsigned char>(c));
        hash *= 1099511628211ull;
    }

    wchar_t buffer[32];
    swprintf(buffer, 32, L"%016llx", static_cast<unsigned long long>(hash));

    return std::wstring(buffer);
}

} // namespace ld

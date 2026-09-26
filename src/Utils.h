#pragma once

#include "Common.h"

#include <string>
#include <string_view>

namespace ld
{

std::wstring Utf8ToWide(std::string_view text);
std::string  WideToUtf8(std::wstring_view text);

/// %APPDATA%\LightDock
std::wstring GetAppDataDir();

/// %APPDATA%\LightDock\icons
std::wstring GetIconCacheDir();

bool EnsureDirectoryExists(const std::wstring& directory);
bool FileExists(const std::wstring& path);
bool DeleteFileIfExists(const std::wstring& path);

/// Lower-cased extension including the dot, e.g. L".lnk". Empty when none.
std::wstring GetFileExtension(const std::wstring& path);

/// File name including extension.
std::wstring GetFileName(const std::wstring& path);

/// File name without extension.
std::wstring GetFileStem(const std::wstring& path);

/// The executable's localized description from its version resource
/// (e.g. explorer.exe -> "文件资源管理器" on a Chinese system).
/// Falls back to ProductName; empty when the file has no version info.
std::wstring GetFileDescription(const std::wstring& path);

/// Parent directory including a trailing separator when one exists.
std::wstring GetParentDirectory(const std::wstring& path);

std::wstring ToLower(std::wstring text);

bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b);

/// Stable, filesystem safe identifier derived from a string.
std::wstring MakeStableId(std::wstring_view seed);

/// Parses "#RGB", "#RRGGBB" or "#RRGGBBAA" into 0..1 components.
/// Returns false for anything malformed.
bool ParseHexColor(std::wstring_view text,
                   float& r, float& g, float& b, float& a);

} // namespace ld

#pragma once

#include "Common.h"

#include <string>

namespace ld
{

class AppLauncher
{
public:
    /// Launches an .exe or a .lnk with optional arguments.
    /// Works for both because the shell resolves shortcuts itself.
    static bool Launch(const std::wstring& path, const std::wstring& arguments);

    /// Same as Launch but used by the "open" context menu entry.
    static bool Open(const std::wstring& path);
};

} // namespace ld

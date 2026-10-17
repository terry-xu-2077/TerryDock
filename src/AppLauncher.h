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

    /// Activate apps installed as MSIX/AppX packages using their application
    /// identity, not the access-restricted, versioned WindowsApps EXE path.
    static bool IsPackagedApplicationPath(const std::wstring& path);
    static bool MatchesPackagedAppId(const std::wstring& path,
                                     const std::wstring& appUserModelId);
    static std::wstring ResolvePackagedAppId(const std::wstring& path);
    static bool LaunchPackagedApp(const std::wstring& appUserModelId,
                                  const std::wstring& arguments);

    /// Returns a suitable user-facing top-level window for a process, or
    /// nullptr when that running process has no previewable window.
    static HWND FindRunningWindow(const std::wstring& processName);

    /// Restores and foregrounds an existing user-facing window for a process.
    /// Returns false when no suitable window is currently open.
    static bool ActivateRunningWindow(const std::wstring& processName);

    /// Restores and foregrounds this exact top-level window.
    static bool ActivateWindow(HWND hwnd);

    /// Same as Launch but used by the "open" context menu entry.
    static bool Open(const std::wstring& path);

    /// Opens Explorer with the file selected.
    static bool RevealInExplorer(const std::wstring& path);
};

} // namespace ld

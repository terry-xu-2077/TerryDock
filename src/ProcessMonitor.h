#pragma once

#include "Common.h"

#include <string>
#include <vector>

namespace ld
{

/// Polls the running state of the dock's applications.
///
/// Deliberately dumb: a single toolhelp snapshot, filtered by the handful of
/// process names the dock actually cares about. No process list is cached.
class ProcessMonitor
{
public:
    /// Fills `running` for every name in `names`, in the same order.
    static void Query(const std::vector<std::wstring>& names,
                      std::vector<bool>& running);

    /// Explorer's desktop and taskbar are also explorer.exe processes. For
    /// that entry, report running only while a user-facing folder window is
    /// open; all other apps continue to use process presence.
    static bool HasExplorerFolderWindow();

    static bool IsRunning(const std::wstring& processName);
};

} // namespace ld

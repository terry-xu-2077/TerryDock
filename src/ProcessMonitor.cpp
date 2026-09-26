#include "ProcessMonitor.h"

#include "Utils.h"

#include <tlhelp32.h>

namespace ld
{

namespace
{

struct ExplorerWindowSearch
{
    bool found = false;
};

BOOL CALLBACK FindExplorerFolderWindow(HWND hwnd, LPARAM parameter)
{
    auto* search = reinterpret_cast<ExplorerWindowSearch*>(parameter);
    if (!search || !IsWindowVisible(hwnd))
    {
        return TRUE;
    }

    wchar_t className[128]{};
    GetClassNameW(hwnd, className, ARRAYSIZE(className));
    if (wcscmp(className, L"CabinetWClass") != 0
        && wcscmp(className, L"ExploreWClass") != 0)
    {
        return TRUE;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId == 0)
    {
        return TRUE;
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                 processId);
    if (!process)
    {
        return TRUE;
    }

    wchar_t imagePath[32768]{};
    DWORD pathLength = ARRAYSIZE(imagePath);
    const bool isExplorer = QueryFullProcessImageNameW(
        process, 0, imagePath, &pathLength)
        && EqualsIgnoreCase(GetFileName(imagePath), L"explorer.exe");
    CloseHandle(process);

    if (!isExplorer)
    {
        return TRUE;
    }

    // Do not exclude iconic windows: a minimized folder is still open.
    search->found = true;
    return FALSE;
}

/// Snapshot of the process names the dock cares about. The full system list
/// is never materialised.
class ProcessScanner
{
public:
    ProcessScanner()
        : snapshot_(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0))
    {
    }

    ~ProcessScanner()
    {
        if (snapshot_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(snapshot_);
        }
    }

    bool Valid() const { return snapshot_ != INVALID_HANDLE_VALUE; }

    bool Contains(std::wstring_view name) const
    {
        if (!Valid() || name.empty())
        {
            return false;
        }

        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);

        if (!Process32FirstW(snapshot_, &entry))
        {
            return false;
        }

        do
        {
            if (EqualsIgnoreCase(entry.szExeFile, name))
            {
                return true;
            }
        } while (Process32NextW(snapshot_, &entry));

        return false;
    }

private:
    HANDLE snapshot_;
};

} // namespace

void ProcessMonitor::Query(const std::vector<std::wstring>& names,
                           std::vector<bool>& running)
{
    running.assign(names.size(), false);

    if (names.empty())
    {
        return;
    }

    ProcessScanner scanner;
    if (!scanner.Valid())
    {
        return;
    }

    for (size_t i = 0; i < names.size(); ++i)
    {
        running[i] = EqualsIgnoreCase(names[i], L"explorer.exe")
            ? HasExplorerFolderWindow()
            : scanner.Contains(names[i]);
    }
}

bool ProcessMonitor::HasExplorerFolderWindow()
{
    ExplorerWindowSearch search;
    EnumWindows(FindExplorerFolderWindow,
                reinterpret_cast<LPARAM>(&search));
    return search.found;
}

bool ProcessMonitor::IsRunning(const std::wstring& processName)
{
    ProcessScanner scanner;
    return scanner.Contains(processName);
}

} // namespace ld

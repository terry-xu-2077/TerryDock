#include "AppLauncher.h"

#include "Utils.h"

namespace ld
{

bool AppLauncher::Launch(const std::wstring& path, const std::wstring& arguments)
{
    if (path.empty())
    {
        return false;
    }

    const std::wstring workingDirectory = GetParentDirectory(path);

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    // Keep the shell UI enabled so Windows can explain missing file
    // associations or an unavailable script runtime to the user.
    info.fMask = SEE_MASK_NOASYNC | SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"open";
    info.lpFile = path.c_str();
    info.lpParameters = arguments.empty() ? nullptr : arguments.c_str();
    info.lpDirectory = workingDirectory.empty() ? nullptr : workingDirectory.c_str();
    info.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&info))
    {
        return false;
    }

    if (info.hProcess)
    {
        CloseHandle(info.hProcess);
    }

    return true;
}

bool AppLauncher::Open(const std::wstring& path)
{
    return Launch(path, std::wstring());
}

} // namespace ld

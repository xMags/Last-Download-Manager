#include "platform/win/WinSystemActions.h"

#include <windows.h>
#include <mmsystem.h>
#include <powrprof.h>

namespace ldm::platform {

namespace {

// Shutting down needs the shutdown privilege enabled on the process token.
bool EnableShutdownPrivilege()
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    bool ok = ::LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &privileges.Privileges[0].Luid) &&
              ::AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr) && ::GetLastError() == ERROR_SUCCESS;
    ::CloseHandle(token);
    return ok;
}

}  // namespace

void WinSystemActions::PlaySoundFile(const std::filesystem::path& file)
{
    ::PlaySoundW(file.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
}

bool WinSystemActions::StartProgram(const std::filesystem::path& program, const std::wstring& arguments)
{
    std::wstring commandLine = L"\"" + program.wstring() + L"\" " + arguments;
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    // The program path is passed separately, so a path with spaces can never be reinterpreted.
    if (!::CreateProcessW(program.c_str(), commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                          nullptr, &startup, &process)) {
        return false;
    }
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return true;
}

bool WinSystemActions::ShutDown(bool forceTerminate)
{
    if (!EnableShutdownPrivilege()) {
        return false;
    }
    // SHUTDOWN_FORCE_SELF ends this session's programs instead of asking them first.
    const DWORD flags = SHUTDOWN_POWEROFF | SHUTDOWN_FORCE_OTHERS | (forceTerminate ? SHUTDOWN_FORCE_SELF : 0);
    return ::InitiateShutdownW(nullptr, nullptr, 0, flags,
                               SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_FLAG_PLANNED) == ERROR_SUCCESS;
}

bool WinSystemActions::Sleep()
{
    return ::SetSuspendState(FALSE, FALSE, FALSE) != FALSE;
}

bool WinSystemActions::Hibernate()
{
    return ::SetSuspendState(TRUE, FALSE, FALSE) != FALSE;
}

std::optional<bool> WinSystemActions::WakeTimersAllowed()
{
    GUID* scheme = nullptr;
    if (::PowerGetActiveScheme(nullptr, &scheme) != ERROR_SUCCESS || scheme == nullptr) {
        return std::nullopt;
    }
    DWORD value = 0;
    const DWORD read = ::PowerReadACValueIndex(nullptr, scheme, &GUID_SLEEP_SUBGROUP, &GUID_ALLOW_RTC_WAKE, &value);
    ::LocalFree(scheme);
    if (read != ERROR_SUCCESS) {
        return std::nullopt;
    }
    // 0 disables wake timers, 1 enables them, 2 allows only "important" ones, which LDM's are not.
    return value == 1;
}

}  // namespace ldm::platform

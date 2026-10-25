// ldm-host.exe: the native messaging host (M3 design section 3). The browser starts it with stdio
// connected to the extension and ends it when the extension disconnects. It relays messages
// between the extension and the running LDM and validates what it relays. The extension's first
// message is a hello; only a hello that asks for it starts LDM when it is not running. The host
// holds no state of its own.
#include "browser/Protocol.h"
#include "platform/win/PipeIo.h"

#include <windows.h>
#include <shellapi.h>

#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr DWORD kStartupWaitMs = 10'000;
constexpr DWORD kRetryIntervalMs = 200;

// Exit codes, so a failing host can be told apart in tests and logs.
enum class ExitCode : int {
    Done = 0,
    NotStartedByBrowser = 2,
    NoLdm = 3,
    ProtocolError = 4,
};

std::vector<std::wstring> Arguments()
{
    int count = 0;
    LPWSTR* raw = ::CommandLineToArgvW(::GetCommandLineW(), &count);
    if (raw == nullptr) {
        return {};
    }
    std::vector<std::wstring> arguments(raw + 1, raw + count);  // skip the program name
    ::LocalFree(raw);
    return arguments;
}

std::filesystem::path OwnFolder()
{
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) {
            return {};
        }
        if (length < path.size()) {
            path.resize(length);
            return std::filesystem::path(path).parent_path();
        }
        path.resize(path.size() * 2);
    }
}

// Starts ldm.exe from this host's folder, hidden in the tray.
bool StartLdm()
{
    const std::filesystem::path ldm = OwnFolder() / L"ldm.exe";
    std::wstring commandLine = L"\"" + ldm.wstring() + L"\" --tray";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    // No inherited handles: LDM must not hold the browser's stdio pipes open.
    if (!::CreateProcessW(ldm.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup,
                          &process)) {
        return false;
    }
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return true;
}

// `refusedError` is the Windows error of a connection LDM's pipe refused, such as access denied
// when LDM runs elevated; 0 when LDM was simply not there.
ldm::platform::OwnedHandle ConnectToLdm(bool startIfMissing, DWORD& refusedError)
{
    refusedError = 0;
    const std::wstring name = ldm::platform::BrowserPipeName();
    bool started = false;
    const ULONGLONG deadline = ::GetTickCount64() + kStartupWaitMs;
    for (;;) {
        ldm::platform::OwnedHandle pipe(::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                                      OPEN_EXISTING,
                                                      FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                                                      nullptr));
        if (pipe.Valid()) {
            return pipe;
        }
        const DWORD error = ::GetLastError();
        if (error == ERROR_FILE_NOT_FOUND && !startIfMissing) {
            return {};
        }
        if (error == ERROR_FILE_NOT_FOUND && !started) {
            started = true;
            if (!StartLdm()) {
                return {};
            }
        } else if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PIPE_BUSY) {
            refusedError = error;  // for example, access denied when LDM runs elevated
            return {};
        }
        if (::GetTickCount64() >= deadline) {
            return {};
        }
        if (error == ERROR_PIPE_BUSY) {
            ::WaitNamedPipeW(name.c_str(), kRetryIntervalMs);
        } else {
            ::Sleep(kRetryIntervalMs);
        }
    }
}

bool ReadExact(HANDLE file, void* data, DWORD size)
{
    auto* bytes = static_cast<char*>(data);
    DWORD done = 0;
    while (done < size) {
        DWORD chunk = 0;
        if (!::ReadFile(file, bytes + done, size - done, &chunk, nullptr) || chunk == 0) {
            return false;
        }
        done += chunk;
    }
    return true;
}

bool WriteExact(HANDLE file, const void* data, DWORD size)
{
    const auto* bytes = static_cast<const char*>(data);
    DWORD done = 0;
    while (done < size) {
        DWORD chunk = 0;
        if (!::WriteFile(file, bytes + done, size - done, &chunk, nullptr) || chunk == 0) {
            return false;
        }
        done += chunk;
    }
    return true;
}

// Native messaging framing: a 32-bit length in native byte order (little-endian on x64), then JSON.
std::optional<std::string> ReadNativeMessage(HANDLE input)
{
    unsigned char header[4] = {};
    if (!ReadExact(input, header, 4)) {
        return std::nullopt;
    }
    const std::uint32_t length = ldm::platform::DecodeFrameLength(header);
    if (length == 0 || length > ldm::browser::kMaxInboundBytes) {
        return std::nullopt;
    }
    std::string message(length, '\0');
    if (!ReadExact(input, message.data(), length)) {
        return std::nullopt;
    }
    return message;
}

bool WriteNativeMessage(HANDLE output, const std::string& message)
{
    if (message.empty() || message.size() > ldm::browser::kMaxOutboundBytes) {
        return false;
    }
    unsigned char header[4] = {};
    ldm::platform::EncodeFrameLength(static_cast<std::uint32_t>(message.size()), header);
    return WriteExact(output, header, 4) && WriteExact(output, message.data(), static_cast<DWORD>(message.size()));
}

// Windows lets only the program the user is working in bring a window to the front. That is the
// browser, which started this host, so the host may pass the right on: LDM can then show the
// dialog for a captured download in front of the browser instead of behind it.
void LetLdmComeToFront(HANDLE pipe)
{
    ULONG ldmProcess = 0;
    if (::GetNamedPipeServerProcessId(pipe, &ldmProcess)) {
        ::AllowSetForegroundWindow(ldmProcess);
    }
}

// Browser to LDM. Returns when the browser closes stdin or sends something invalid.
ExitCode RelayFromBrowser(HANDLE input, HANDLE pipe)
{
    for (;;) {
        const auto message = ReadNativeMessage(input);
        if (!message) {
            return ExitCode::Done;  // the extension disconnected; Chrome closes stdin
        }
        if (!ldm::browser::IsValidEnvelope(*message)) {
            return ExitCode::ProtocolError;
        }
        LetLdmComeToFront(pipe);
        if (!ldm::platform::WriteFrame(pipe, *message, ldm::browser::kMaxInboundBytes, nullptr)) {
            return ExitCode::NoLdm;
        }
    }
}

// LDM to browser. Returns when LDM closes the pipe (for example, on exit).
ExitCode RelayFromLdm(HANDLE pipe, HANDLE output)
{
    for (;;) {
        const auto message = ldm::platform::ReadFrame(pipe, ldm::browser::kMaxOutboundBytes, nullptr, INFINITE);
        if (!message) {
            return ExitCode::NoLdm;
        }
        if (!WriteNativeMessage(output, *message)) {
            return ExitCode::Done;  // the browser went away
        }
    }
}

}  // namespace

int WINAPI wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int)
{
    ::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);

    const auto host = ldm::browser::HostInfoFromArguments(Arguments());
    const HANDLE input = ::GetStdHandle(STD_INPUT_HANDLE);
    const HANDLE output = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (!host || input == nullptr || input == INVALID_HANDLE_VALUE || output == nullptr || output == INVALID_HANDLE_VALUE) {
        return static_cast<int>(ExitCode::NotStartedByBrowser);
    }
    const auto first = ReadNativeMessage(input);
    if (!first) {
        return static_cast<int>(ExitCode::Done);
    }
    if (!ldm::browser::IsValidEnvelope(*first)) {
        return static_cast<int>(ExitCode::ProtocolError);
    }
    DWORD refusedError = 0;
    const ldm::platform::OwnedHandle pipe = ConnectToLdm(ldm::browser::HelloAsksToStartLdm(*first), refusedError);
    if (!pipe.Valid() && refusedError == ERROR_ACCESS_DENIED) {
        // A disconnect alone would read as "LDM is not running"; the extension says what is wrong.
        (void)WriteNativeMessage(output, ldm::browser::EncodeHostError("ldm-elevated"));
        return static_cast<int>(ExitCode::NoLdm);
    }
    if (!pipe.Valid() ||
        !ldm::platform::WriteFrame(pipe.Get(), ldm::browser::EncodeHostInfo(*host), ldm::browser::kMaxInboundBytes, nullptr) ||
        !ldm::platform::WriteFrame(pipe.Get(), *first, ldm::browser::kMaxInboundBytes, nullptr)) {
        // The extension sees the disconnect and shows that LDM is not running or cannot be reached.
        return static_cast<int>(ExitCode::NoLdm);
    }

    // Two directions, two threads. Whichever ends first ends the process: the other one is
    // blocked in a read that only the end of the process interrupts cleanly.
    std::thread fromLdm([&pipe, output] { ::ExitProcess(static_cast<UINT>(RelayFromLdm(pipe.Get(), output))); });
    const ExitCode code = RelayFromBrowser(input, pipe.Get());
    ::ExitProcess(static_cast<UINT>(code));
}

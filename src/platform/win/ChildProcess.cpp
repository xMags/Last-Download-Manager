#include "platform/win/ChildProcess.h"

#include "base/Utf.h"
#include "platform/win/PipeIo.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace ldm::platform {

namespace {

base::Error SystemError(const std::string& what)
{
    return base::MakeError(base::ErrorCode::Io, what + " failed (error " + std::to_string(::GetLastError()) + ")");
}

// Reads the program's output until every writer has closed the pipe. Keeps at most `limit` bytes
// and drops the rest, so the program never blocks on a full pipe.
void ReadAll(HANDLE pipe, std::size_t limit, std::string& output, bool& truncated)
{
    std::array<char, 4096> buffer{};
    for (;;) {
        DWORD read = 0;
        if (!::ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) || read == 0) {
            return;  // ERROR_BROKEN_PIPE: the last writer is gone
        }
        const std::size_t room = limit - std::min(limit, output.size());
        const std::size_t kept = std::min<std::size_t>(room, read);
        output.append(buffer.data(), kept);
        if (kept < read) {
            truncated = true;
        }
    }
}

// The job every run gets: ends all its processes when the handle closes (also when LDM dies),
// caps each process's memory, and suppresses crash dialogs, which would otherwise wait for a
// click nobody sees.
OwnedHandle CreateRunJob(std::size_t maxMemoryBytes)
{
    OwnedHandle job(::CreateJobObjectW(nullptr, nullptr));
    if (!job.Valid()) {
        return {};
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION | JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    limits.ProcessMemoryLimit = maxMemoryBytes;
    if (!::SetInformationJobObject(job.Get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        return {};
    }
    return job;
}

// A pipe for one of the program's outputs: the write end is inheritable, the read end is not.
bool CreateOutputPipe(SECURITY_ATTRIBUTES& inherit, OwnedHandle& read, OwnedHandle& write)
{
    HANDLE rawRead = nullptr;
    HANDLE rawWrite = nullptr;
    if (!::CreatePipe(&rawRead, &rawWrite, &inherit, 0)) {
        return false;
    }
    read = OwnedHandle(rawRead);
    write = OwnedHandle(rawWrite);
    return ::SetHandleInformation(read.Get(), HANDLE_FLAG_INHERIT, 0) != FALSE;
}

}  // namespace

std::wstring QuoteArgument(std::wstring_view argument)
{
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        return std::wstring(argument);
    }
    std::wstring quoted = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t c : argument) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            // Backslashes before a quote are doubled, and the quote itself is escaped.
            quoted.append(backslashes * 2 + 1, L'\\');
        } else {
            quoted.append(backslashes, L'\\');
        }
        backslashes = 0;
        quoted.push_back(c);
    }
    // Backslashes before the closing quote are doubled so they do not escape it.
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

base::Result<ChildProcessResult> RunChildProcess(const ChildProcessRequest& request, HANDLE stopEvent)
{
    if (!request.program.is_absolute()) {
        return base::MakeError(base::ErrorCode::InvalidArgument, "the program path must be absolute");
    }

    // Output: the read end stays here, the write end goes to the program. Standard error gets a
    // pipe of its own only when the caller asked for it.
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    OwnedHandle outputRead;
    OwnedHandle outputWrite;
    if (!CreateOutputPipe(inherit, outputRead, outputWrite)) {
        return SystemError("CreatePipe");
    }
    OwnedHandle errorsRead;
    OwnedHandle errorsWrite;
    if (request.separateErrors && !CreateOutputPipe(inherit, errorsRead, errorsWrite)) {
        return SystemError("CreatePipe");
    }
    OwnedHandle input(::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr));
    if (!input.Valid()) {
        return SystemError("Opening NUL");
    }

    // Only these handles are inherited, not every inheritable handle LDM happens to hold.
    std::vector<HANDLE> inherited{input.Get(), outputWrite.Get()};
    if (errorsWrite.Valid()) {
        inherited.push_back(errorsWrite.Get());
    }
    SIZE_T attributeSize = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
    std::vector<std::byte> attributeBuffer(attributeSize);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeBuffer.data());  // sized by Windows above
    if (!::InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize)) {
        return SystemError("InitializeProcThreadAttributeList");
    }
    const std::unique_ptr<std::remove_pointer_t<LPPROC_THREAD_ATTRIBUTE_LIST>, decltype(&::DeleteProcThreadAttributeList)>
        attributeGuard(attributes, &::DeleteProcThreadAttributeList);
    if (!::UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(),
                                     sizeof(HANDLE) * inherited.size(), nullptr, nullptr)) {
        return SystemError("UpdateProcThreadAttribute");
    }

    OwnedHandle job = CreateRunJob(request.limits.maxMemoryBytes);
    if (!job.Valid()) {
        return SystemError("Creating the job object");
    }

    std::wstring commandLine = QuoteArgument(request.program.native());
    for (const std::wstring& argument : request.arguments) {
        commandLine += L' ';
        commandLine += QuoteArgument(argument);
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.Get();
    startup.StartupInfo.hStdOutput = outputWrite.Get();
    startup.StartupInfo.hStdError = errorsWrite.Valid() ? errorsWrite.Get() : outputWrite.Get();
    startup.lpAttributeList = attributes;
    const wchar_t* directory = request.workingDirectory ? request.workingDirectory->c_str() : nullptr;
    PROCESS_INFORMATION started{};
    // Suspended until it is in the job, so it cannot start anything outside the job first.
    if (!::CreateProcessW(request.program.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
                          CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                          nullptr, directory, &startup.StartupInfo, &started)) {
        return SystemError("Starting " + base::ToUtf8(request.program.filename().native()));
    }
    OwnedHandle process(started.hProcess);
    OwnedHandle thread(started.hThread);
    if (!::AssignProcessToJobObject(job.Get(), process.Get())) {
        ::TerminateProcess(process.Get(), 1);
        return SystemError("AssignProcessToJobObject");
    }
    // The program has its copies now; with ours closed, reading ends when the program's do.
    outputWrite.Reset();
    errorsWrite.Reset();
    input.Reset();

    ChildProcessResult result;
    std::thread reader([pipe = outputRead.Get(), limit = request.limits.maxOutputBytes, &result] {
        ReadAll(pipe, limit, result.output, result.outputTruncated);
    });
    std::thread errorReader;
    if (errorsRead.Valid()) {
        errorReader = std::thread([pipe = errorsRead.Get(), limit = request.limits.maxErrorBytes, &result] {
            ReadAll(pipe, limit, result.errors, result.errorsTruncated);
        });
    }
    const auto joinReaders = [&reader, &errorReader] {
        reader.join();
        if (errorReader.joinable()) {
            errorReader.join();
        }
    };
    ::ResumeThread(thread.Get());

    const std::array<HANDLE, 2> waits{process.Get(), stopEvent};
    const DWORD count = stopEvent != nullptr ? 2 : 1;
    const auto timeout = static_cast<DWORD>(std::min<long long>(request.limits.timeout.count(), INFINITE - 1));
    const DWORD waited = ::WaitForMultipleObjects(count, waits.data(), FALSE, timeout);
    const DWORD waitError = ::GetLastError();
    if (waited == WAIT_FAILED) {
        ::TerminateJobObject(job.Get(), 1);
        ::WaitForSingleObject(process.Get(), INFINITE);
        joinReaders();
        return base::MakeError(base::ErrorCode::Io, "Waiting for the program failed (error " + std::to_string(waitError) + ")");
    }
    if (waited == WAIT_OBJECT_0 + 1) {
        result.end = ChildProcessEnd::Stopped;
    } else if (waited == WAIT_TIMEOUT) {
        result.end = ChildProcessEnd::TimedOut;
    }
    // Ends the program if it is still running, and in every case whatever it left running, which
    // would otherwise keep the output pipe open.
    ::TerminateJobObject(job.Get(), 1);
    ::WaitForSingleObject(process.Get(), INFINITE);
    joinReaders();
    if (result.end == ChildProcessEnd::Exited && !::GetExitCodeProcess(process.Get(), &result.exitCode)) {
        return SystemError("GetExitCodeProcess");
    }
    return result;
}

}  // namespace ldm::platform

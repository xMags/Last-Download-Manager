#pragma once

#include "base/Result.h"

#include <windows.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Runs helper programs (ffmpeg.exe and ffprobe.exe, M3 design section 6) as separate processes,
// so a crash or a hostile media file stays out of LDM. Each run gets its own job object: the
// program and everything it starts end when the run ends, when the time limit passes, when the
// caller stops it, or when LDM itself ends, even by a crash.
namespace ldm::platform {

struct ChildProcessLimits {
    std::chrono::milliseconds timeout{std::chrono::minutes(10)};
    // Standard output and standard error together; beyond this the output is read and dropped,
    // so the program never stalls on a full pipe.
    std::size_t maxOutputBytes = 1024 * 1024;
    // Standard error, when it is kept apart (ChildProcessRequest::separateErrors).
    std::size_t maxErrorBytes = 64 * 1024;
    // Per process in the job; Windows ends a process that commits more.
    std::size_t maxMemoryBytes = std::size_t{1024} * 1024 * 1024;
};

struct ChildProcessRequest {
    std::filesystem::path program;  // absolute, so Windows never searches for it
    std::vector<std::wstring> arguments;
    std::optional<std::filesystem::path> workingDirectory;
    ChildProcessLimits limits;
    // Standard error goes to ChildProcessResult::errors instead of into `output`: for programs
    // whose output is data (JSON) that warnings must not mix into.
    bool separateErrors = false;
};

enum class ChildProcessEnd {
    Exited,    // the program ended by itself; exitCode is its code
    TimedOut,  // the time limit passed and the program was ended
    Stopped,   // the caller's stop event was set and the program was ended
};

struct ChildProcessResult {
    ChildProcessEnd end = ChildProcessEnd::Exited;
    DWORD exitCode = 0;
    // As the program wrote it: standard output and error interleaved, or standard output alone
    // with separateErrors.
    std::string output;
    bool outputTruncated = false;
    std::string errors;  // standard error, with separateErrors only
    bool errorsTruncated = false;
};

// Runs the program and waits for it on the calling thread, so it must not be called on the UI
// thread. `stopEvent` (optional) ends the run early. Safe to call from several threads at once.
// Fails without starting anything for a relative program path.
[[nodiscard]] base::Result<ChildProcessResult> RunChildProcess(const ChildProcessRequest& request,
                                                               HANDLE stopEvent = nullptr);

// Quotes one argument so the program's C runtime (and CommandLineToArgvW) reads it back unchanged.
[[nodiscard]] std::wstring QuoteArgument(std::wstring_view argument);

}  // namespace ldm::platform

#include "engine/DiskWriter.h"

#include "base/Paths.h"
#include "base/Utf.h"

#include <windows.h>
#include <winhttp.h>
#include <winioctl.h>

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace ldm::engine {

namespace {

using core::FailureClass;
using core::FailureReason;
using core::TransferFailure;

// Largest file FAT12/16/32 can hold.
constexpr std::int64_t kFatMaxFileSize = 0xFFFFFFFFLL;

class FileHandle {
public:
    FileHandle() = default;
    explicit FileHandle(HANDLE handle) noexcept : handle_(handle) {}
    FileHandle(FileHandle&& other) noexcept : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}
    FileHandle& operator=(FileHandle&& other) noexcept
    {
        if (this != &other) {
            Reset();
            handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    ~FileHandle() { Reset(); }

    [[nodiscard]] HANDLE Get() const noexcept { return handle_; }
    [[nodiscard]] bool Valid() const noexcept { return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr; }

    // Closes now and reports whether CloseHandle succeeded.
    bool Close() noexcept
    {
        if (!Valid()) {
            return true;
        }
        const BOOL closed = ::CloseHandle(std::exchange(handle_, INVALID_HANDLE_VALUE));
        return closed != FALSE;
    }

private:
    void Reset() noexcept { Close(); }

    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

std::string Win32Message(DWORD error)
{
    wchar_t* buffer = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error,
        0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);  // FORMAT_MESSAGE_ALLOCATE_BUFFER takes a pointer to the pointer
    std::wstring text = length > 0 && buffer != nullptr ? std::wstring(buffer, length) : std::wstring();
    if (buffer != nullptr) {
        ::LocalFree(buffer);
    }
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) {
        text.pop_back();
    }
    return "Windows error " + std::to_string(error) + (text.empty() ? "" : ": " + base::ToUtf8(text));
}

TransferFailure FailureFromWin32(DWORD error, std::string_view operation)
{
    std::string message = std::string(operation) + ": " + Win32Message(error);
    switch (error) {
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        return {FailureReason::DiskFull, FailureClass::NeedsUser, 0, std::move(message)};
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
    case ERROR_WRITE_PROTECT:
        return {FailureReason::PermissionDenied, FailureClass::NeedsUser, 0, std::move(message)};
    case ERROR_FILE_TOO_LARGE:
        return {FailureReason::FileTooLarge, FailureClass::NeedsUser, 0, std::move(message)};
    default:
        return {FailureReason::DiskError, FailureClass::NeedsUser, 0, std::move(message)};
    }
}

bool IsFatVolume(HANDLE file)
{
    wchar_t fileSystem[MAX_PATH + 1] = {};
    if (!::GetVolumeInformationByHandleW(file, nullptr, 0, nullptr, nullptr, nullptr, fileSystem,
                                         static_cast<DWORD>(std::size(fileSystem)))) {
        return false;
    }
    const std::wstring_view name(fileSystem);
    return name == L"FAT" || name == L"FAT32" || name == L"FAT12" || name == L"FAT16";
}

std::optional<std::int64_t> FileLength(HANDLE file)
{
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file, &size)) {
        return std::nullopt;
    }
    return size.QuadPart;
}

// Applies an HTTP date as the file's last-write time. Best effort: failures leave the time as is.
void SetFileTimeFromHttpDate(const std::filesystem::path& path, const std::string& httpDate)
{
    SYSTEMTIME systemTime{};
    if (!::WinHttpTimeToSystemTime(base::ToUtf16(httpDate).c_str(), &systemTime)) {
        return;
    }
    FILETIME fileTime{};
    if (!::SystemTimeToFileTime(&systemTime, &fileTime)) {
        return;
    }
    FileHandle file(::CreateFileW(base::ToExtendedLengthPath(path).c_str(), FILE_WRITE_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.Valid()) {
        ::SetFileTime(file.Get(), nullptr, nullptr, &fileTime);
    }
}

// Moves the finished file to its final name. Without `replace` a file already there is never
// overwritten. With it that file is replaced in one step, so a move that fails leaves it as it
// was: a rename over it on the same volume, or from another volume a copy beside it, made durable
// and then renamed over it.
std::optional<TransferFailure> MoveToFinalName(const std::wstring& from, const std::wstring& to, bool replace)
{
    if (!replace) {
        if (!::MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) {
            return FailureFromWin32(::GetLastError(), "move to final name");
        }
        return std::nullopt;
    }
    if (::MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return std::nullopt;
    }
    if (const DWORD error = ::GetLastError(); error != ERROR_NOT_SAME_DEVICE) {
        return FailureFromWin32(error, "replace the existing file");
    }
    const std::wstring copy = to + L".ldmtmp";
    const auto failWith = [&copy](std::string_view operation) {
        const DWORD error = ::GetLastError();
        ::DeleteFileW(copy.c_str());
        return FailureFromWin32(error, operation);
    };
    if (!::CopyFileW(from.c_str(), copy.c_str(), FALSE)) {
        return failWith("copy to the final folder");
    }
    {
        FileHandle written(::CreateFileW(copy.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                         nullptr));
        if (!written.Valid() || !::FlushFileBuffers(written.Get())) {
            return failWith("copy to the final folder");
        }
    }
    if (!::MoveFileExW(copy.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return failWith("replace the existing file");
    }
    ::DeleteFileW(from.c_str());  // best effort: the file is complete at its final name either way
    return std::nullopt;
}

}  // namespace

struct DiskWriter::OpenFile {
    FileHandle handle;
    Watermarks written;
    Watermarks durable;
    bool dirty = false;
    bool failed = false;
    std::chrono::steady_clock::time_point lastFlush;
};

DiskWriter::DiskWriter(BufferPool& pool, ReportSink reportSink, FinalizeSink finalizeSink,
                       std::chrono::milliseconds flushInterval)
    : pool_(pool),
      reportSink_(std::move(reportSink)),
      finalizeSink_(std::move(finalizeSink)),
      flushInterval_(flushInterval),
      thread_([this](std::stop_token stopToken) { Run(std::move(stopToken)); })
{
}

DiskWriter::~DiskWriter()
{
    thread_.request_stop();
    wake_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void DiskWriter::Open(RunKey key, std::filesystem::path path, OpenMode mode, std::int64_t requiredLength)
{
    Enqueue(OpenJob{key, std::move(path), mode, requiredLength});
}

void DiskWriter::SetLength(RunKey key, std::int64_t length)
{
    Enqueue(SetLengthJob{key, length});
}

void DiskWriter::Write(RunKey key, std::uint32_t segment, std::int64_t offset, std::vector<std::byte> data)
{
    Enqueue(WriteJob{key, segment, offset, std::move(data)});
}

void DiskWriter::Close(RunKey key)
{
    Enqueue(CloseJob{key});
}

void DiskWriter::Finalize(core::FinalizeRequest request)
{
    Enqueue(FinalizeJob{std::move(request)});
}

void DiskWriter::Enqueue(Job job)
{
    {
        std::lock_guard lock(mutex_);
        pending_.push_back(std::move(job));
    }
    wake_.notify_one();
}

void DiskWriter::Run(std::stop_token stopToken)
{
    std::vector<Job> batch;
    while (true) {
        {
            std::unique_lock lock(mutex_);
            const bool anyDirty =
                std::any_of(files_.begin(), files_.end(), [](const auto& entry) { return entry.second->dirty; });
            if (anyDirty) {
                wake_.wait_for(lock, stopToken, flushInterval_, [this] { return !pending_.empty(); });
            } else {
                wake_.wait(lock, stopToken, [this] { return !pending_.empty(); });
            }
            batch.swap(pending_);
        }
        for (Job& job : batch) {
            std::visit([this](auto& typed) { Handle(typed); }, job);
        }
        batch.clear();

        if (stopToken.stop_requested()) {
            std::lock_guard lock(mutex_);
            if (pending_.empty()) {
                break;
            }
            continue;  // drain what arrived during shutdown before closing files
        }
        FlushDue(std::chrono::steady_clock::now());
    }
    // Shutdown: every file still open is flushed and closed. Its owner receives Closed as usual.
    while (!files_.empty()) {
        CloseJob job{files_.begin()->first};
        Handle(job);
    }
}

void DiskWriter::Fail(RunKey key, OpenFile& file, TransferFailure failure)
{
    file.failed = true;
    reportSink_(DiskReport{key, disk::Failed{std::move(failure)}});
}

void DiskWriter::Handle(OpenJob& job)
{
    auto file = std::make_unique<OpenFile>();
    file->lastFlush = std::chrono::steady_clock::now();
    const auto fail = [&](TransferFailure failure) {
        OpenFile& stored = *file;
        files_[job.key] = std::move(file);
        Fail(job.key, stored, std::move(failure));
    };

    std::error_code ignored;
    std::filesystem::create_directories(job.path.parent_path(), ignored);
    const DWORD disposition = job.mode == OpenMode::CreateNew ? CREATE_ALWAYS : OPEN_EXISTING;
    file->handle = FileHandle(::CreateFileW(base::ToExtendedLengthPath(job.path).c_str(), GENERIC_READ | GENERIC_WRITE,
                                            FILE_SHARE_READ, nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file->handle.Valid()) {
        const DWORD error = ::GetLastError();
        if (job.mode == OpenMode::Resume && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)) {
            fail({FailureReason::PartialFileMissing, FailureClass::NeedsUser, 0, "partial file is missing"});
            return;
        }
        fail(FailureFromWin32(error, "open partial file"));
        return;
    }
    if (job.mode == OpenMode::Resume) {
        const auto length = FileLength(file->handle.Get());
        if (!length || *length < job.requiredLength) {
            fail({FailureReason::PartialFileMissing, FailureClass::NeedsUser, 0,
                  "partial file is shorter than the saved progress"});
            return;
        }
    }
    files_[job.key] = std::move(file);
}

void DiskWriter::Handle(SetLengthJob& job)
{
    const auto it = files_.find(job.key);
    if (it == files_.end() || it->second->failed) {
        return;
    }
    OpenFile& file = *it->second;
    const HANDLE handle = file.handle.Get();
    if (job.length > kFatMaxFileSize && IsFatVolume(handle)) {
        Fail(job.key, file, {FailureReason::FileTooLarge, FailureClass::NeedsUser, 0,
                             "the file is larger than the volume allows (FAT32 limit 4 GB)"});
        return;
    }
    const auto current = FileLength(handle);
    if (current && *current == job.length) {
        return;
    }
    // Sparse first, so extending does not make Windows zero-fill the whole range. Volumes without
    // sparse support (FAT, exFAT) reject this, and the file is then extended normally.
    DWORD returned = 0;
    ::DeviceIoControl(handle, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr);
    FILE_END_OF_FILE_INFO endOfFile{};
    endOfFile.EndOfFile.QuadPart = job.length;
    if (!::SetFileInformationByHandle(handle, FileEndOfFileInfo, &endOfFile, sizeof(endOfFile))) {
        Fail(job.key, file, FailureFromWin32(::GetLastError(), "set file length"));
    }
}

void DiskWriter::Handle(WriteJob& job)
{
    const auto size = static_cast<std::int64_t>(job.data.size());
    const auto release = [&] { pool_.Release(size); };
    const auto it = files_.find(job.key);
    if (it == files_.end() || it->second->failed) {
        release();
        return;
    }
    OpenFile& file = *it->second;
    std::int64_t written = 0;
    while (written < size) {
        const std::int64_t offset = job.offset + written;
        OVERLAPPED overlapped{};
        overlapped.Offset = static_cast<DWORD>(static_cast<std::uint64_t>(offset) & 0xFFFFFFFFULL);
        overlapped.OffsetHigh = static_cast<DWORD>(static_cast<std::uint64_t>(offset) >> 32);
        const auto chunk = static_cast<DWORD>(std::min<std::int64_t>(size - written, std::numeric_limits<DWORD>::max()));
        DWORD done = 0;
        if (!::WriteFile(file.handle.Get(), job.data.data() + written, chunk, &done, &overlapped) || done == 0) {
            const DWORD error = done == 0 && ::GetLastError() == ERROR_SUCCESS ? ERROR_WRITE_FAULT : ::GetLastError();
            release();
            Fail(job.key, file, FailureFromWin32(error, "write partial file"));
            return;
        }
        written += done;
    }
    release();
    std::int64_t& mark = file.written[job.segment];
    mark = std::max(mark, job.offset + size);
    file.dirty = true;
}

void DiskWriter::Handle(CloseJob& job)
{
    const auto it = files_.find(job.key);
    if (it == files_.end()) {
        // Never opened (or already closed): still acknowledge, so the owner never waits forever.
        reportSink_(DiskReport{job.key, disk::Closed{}});
        return;
    }
    std::unique_ptr<OpenFile> file = std::move(it->second);
    files_.erase(it);

    std::optional<TransferFailure> failure;
    if (file->handle.Valid() && !file->failed) {
        if (file->dirty && !::FlushFileBuffers(file->handle.Get())) {
            failure = FailureFromWin32(::GetLastError(), "flush partial file");
        } else {
            file->durable = file->written;
        }
    }
    if (!file->handle.Close() && !failure) {
        failure = FailureFromWin32(::GetLastError(), "close partial file");
    }
    reportSink_(DiskReport{job.key, disk::Closed{std::move(file->durable), std::move(failure)}});
}

void DiskWriter::Handle(FinalizeJob& job)
{
    const core::FinalizeRequest& request = job.request;
    core::FinalizeFinished result;
    const std::wstring from = base::ToExtendedLengthPath(request.partialPath);
    const std::wstring to = base::ToExtendedLengthPath(request.finalPath);
    if (from != to) {
        std::error_code ignored;
        std::filesystem::create_directories(request.finalPath.parent_path(), ignored);
        result.failure = MoveToFinalName(from, to, request.replaceExisting);
    }
    if (!result.failure && request.lastModified) {
        SetFileTimeFromHttpDate(request.finalPath, *request.lastModified);
    }
    if (!result.failure && request.removeFolder) {
        // Best effort: the download is complete either way; a folder left behind holds only
        // pieces that were already joined into the final file.
        std::error_code ignored;
        std::filesystem::remove_all(std::filesystem::path(base::ToExtendedLengthPath(*request.removeFolder)), ignored);
    }
    finalizeSink_(core::EngineEvent{request.id, request.epoch, std::move(result)});
}

void DiskWriter::FlushDue(std::chrono::steady_clock::time_point now)
{
    for (auto& [key, file] : files_) {
        if (!file->dirty || file->failed) {
            continue;
        }
        if (now - file->lastFlush < flushInterval_) {
            continue;
        }
        file->lastFlush = now;
        if (!::FlushFileBuffers(file->handle.Get())) {
            Fail(key, *file, FailureFromWin32(::GetLastError(), "flush partial file"));
            continue;
        }
        file->dirty = false;
        file->durable = file->written;
        reportSink_(DiskReport{key, disk::Durable{file->durable}});
    }
}

}  // namespace ldm::engine

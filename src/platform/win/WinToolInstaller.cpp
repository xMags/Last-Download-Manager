#include "platform/win/WinToolInstaller.h"

#include "base/Utf.h"
#include "platform/win/ChildProcess.h"

#include <windows.h>

#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>
#include <vector>

namespace ldm::platform {

namespace {

std::string Name(core::SiteTool tool)
{
    return std::string(core::SiteToolName(tool));
}

std::filesystem::path SystemProgram(const wchar_t* name)
{
    std::array<wchar_t, MAX_PATH> system{};
    const UINT length = ::GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
    if (length == 0 || length >= system.size()) {
        return {};
    }
    return std::filesystem::path(std::wstring(system.data(), length)) / name;
}

bool WriteAll(const std::filesystem::path& path, std::string_view bytes)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    return static_cast<bool>(out);
}

std::string FirstLine(const std::string& text)
{
    std::string line = text.substr(0, text.find_first_of("\r\n"));
    if (line.size() > 200) {
        line.resize(200);
    }
    return line;
}

bool IsSafeVersion(std::string_view version)
{
    return !version.empty() && version.size() <= 32 && std::all_of(version.begin(), version.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-' ||
                      c == '_';
           });
}

// What the program says about its version must name the release: yt-dlp prints "2026.10.21",
// Deno "deno 2.9.8 (stable, release, ...)".
bool AnswersWithVersion(core::SiteTool tool, const std::string& output, const std::string& version)
{
    const std::string line = FirstLine(output);
    if (tool == core::SiteTool::YtDlp) {
        return line == version;
    }
    return line.starts_with("deno " + version + " ") || line == "deno " + version;
}

void RemoveQuietly(const std::filesystem::path& path)
{
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
}

}  // namespace

std::string Sha256Hex(std::string_view bytes)
{
    std::array<unsigned char, 32> digest{};
    // ULONG lengths: hashed in pieces, so an input of any size fits.
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (!BCRYPT_SUCCESS(::BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0))) {
        return {};
    }
    bool ok = true;
    constexpr size_t kPiece = 1 << 30;
    for (size_t offset = 0; ok && offset < bytes.size(); offset += kPiece) {
        const size_t length = std::min(kPiece, bytes.size() - offset);
        // BCryptHashData takes a non-const pointer but does not write through it.
        auto* data = reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data() + offset));
        ok = BCRYPT_SUCCESS(::BCryptHashData(hash, data, static_cast<ULONG>(length), 0));
    }
    ok = ok && BCRYPT_SUCCESS(::BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0));
    ::BCryptDestroyHash(hash);
    if (!ok) {
        return {};
    }
    constexpr char kHex[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(64);
    for (const unsigned char byte : digest) {
        hex.push_back(kHex[byte >> 4]);
        hex.push_back(kHex[byte & 0x0F]);
    }
    return hex;
}

WinToolInstaller::WinToolInstaller(SiteToolsFolder& folder, core::EngineListener& listener)
    : folder_(folder), listener_(listener)
{
    worker_ = std::thread([this] { RunWorker(); });
}

WinToolInstaller::~WinToolInstaller()
{
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        jobs_.clear();
    }
    wake_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void WinToolInstaller::Install(core::ToolInstallRequest request)
{
    {
        std::lock_guard lock(mutex_);
        if (stopping_) {
            return;
        }
        jobs_.emplace_back(std::move(request));
    }
    wake_.notify_one();
}

void WinToolInstaller::Remove()
{
    {
        std::lock_guard lock(mutex_);
        if (stopping_) {
            return;
        }
        jobs_.emplace_back(RemoveAll{});
    }
    wake_.notify_one();
}

std::optional<std::string> WinToolInstaller::InstalledVersion(core::SiteTool tool) const
{
    std::shared_lock lock(folder_.Lock());
    std::error_code error;
    if (!std::filesystem::is_regular_file(folder_.ProgramOf(tool), error)) {
        return std::nullopt;
    }
    std::ifstream in(folder_.VersionFileOf(tool), std::ios::binary);
    std::string version((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    while (!version.empty() && (version.back() == '\n' || version.back() == '\r' || version.back() == ' ')) {
        version.pop_back();
    }
    return IsSafeVersion(version) ? std::optional<std::string>(version) : std::nullopt;
}

void WinToolInstaller::RunWorker()
{
    while (true) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if (stopping_) {
                return;
            }
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        core::ToolInstalled answer;
        if (auto* install = std::get_if<core::ToolInstallRequest>(&job)) {
            answer.tool = install->tool;
            answer.version = install->version;
            answer.failure = InstallNow(*install);
            install->archive.clear();  // tens of megabytes, no longer needed
        } else {
            answer.action = core::ToolInstalled::Action::Remove;
            answer.failure = RemoveNow();
        }
        {
            std::lock_guard lock(mutex_);
            if (stopping_) {
                return;
            }
        }
        listener_.OnEngineEvent(core::EngineEvent{model::DownloadId{core::kToolEventId}, 0, std::move(answer)});
    }
}

std::optional<std::string> WinToolInstaller::InstallNow(const core::ToolInstallRequest& request)
{
    const std::string name = Name(request.tool);
    if (!IsSafeVersion(request.version)) {
        return "The version of " + name + " is not one LDM can install.";
    }
    if (Sha256Hex(request.archive) != request.sha256) {
        return "The download of " + name + " does not match its published checksum.";
    }
    const std::filesystem::path current = folder_.FolderOf(request.tool);
    const std::filesystem::path fresh = current.wstring() + L".new";
    const std::filesystem::path old = current.wstring() + L".old";
    const std::filesystem::path archive = current.wstring() + L".download.zip";
    std::error_code error;
    std::filesystem::create_directories(folder_.Root(), error);
    RemoveQuietly(fresh);
    std::filesystem::create_directories(fresh, error);
    if (error || !WriteAll(archive, request.archive)) {
        RemoveQuietly(fresh);
        RemoveQuietly(archive);
        return "LDM could not write " + name + " into its tools folder.";
    }
    const auto cleanUp = [&] {
        RemoveQuietly(fresh);
        RemoveQuietly(archive);
    };

    // Windows' own tar (bsdtar) reads zip archives and refuses entries with absolute paths or
    // "..", so nothing lands outside the new folder.
    const std::filesystem::path tar = SystemProgram(L"tar.exe");
    if (tar.empty() || !std::filesystem::is_regular_file(tar, error)) {
        cleanUp();
        return "Windows' tar.exe was not found; installing " + name + " needs Windows 10 version 1803 or later.";
    }
    ChildProcessRequest unpack;
    unpack.program = tar;
    unpack.arguments = {L"-x", L"-f", archive.native(), L"-C", fresh.native()};
    unpack.limits.timeout = std::chrono::minutes(5);
    const auto unpacked = RunChildProcess(unpack);
    if (!unpacked || unpacked.Value().end != ChildProcessEnd::Exited || unpacked.Value().exitCode != 0) {
        const std::string why = unpacked ? FirstLine(unpacked.Value().output) : unpacked.GetError().message;
        cleanUp();
        return "Unpacking " + name + " failed" + (why.empty() ? std::string(".") : ": " + why);
    }
    const std::filesystem::path program = fresh / folder_.ProgramOf(request.tool).filename();
    if (!std::filesystem::is_regular_file(program, error)) {
        cleanUp();
        return "The download of " + name + " does not contain its program.";
    }

    ChildProcessRequest check;
    check.program = program;
    check.arguments = {L"--version"};
    check.workingDirectory = fresh;
    check.limits.timeout = std::chrono::seconds(60);
    const auto checked = RunChildProcess(check);
    if (!checked || checked.Value().end != ChildProcessEnd::Exited || checked.Value().exitCode != 0 ||
        !AnswersWithVersion(request.tool, checked.Value().output, request.version)) {
        cleanUp();
        return "The new " + name + " did not start properly, so the installed one was kept.";
    }
    // Written into the new folder, so it moves into place with the program it describes.
    if (!WriteAll(fresh / folder_.VersionFileOf(request.tool).filename(), request.version + "\n")) {
        cleanUp();
        return "LDM could not write " + name + " into its tools folder.";
    }

    {
        // No lookup runs while the folders change places.
        std::unique_lock lock(folder_.Lock());
        RemoveQuietly(old);
        if (std::filesystem::exists(current, error)) {
            std::filesystem::rename(current, old, error);
            if (error) {
                cleanUp();
                return "The installed " + name + " is in use and could not be replaced.";
            }
        }
        std::filesystem::rename(fresh, current, error);
        if (error) {
            std::error_code ignored;
            std::filesystem::rename(old, current, ignored);  // put the previous one back
            cleanUp();
            return "LDM could not put the new " + name + " in place.";
        }
    }
    RemoveQuietly(old);
    RemoveQuietly(archive);
    return std::nullopt;
}

std::optional<std::string> WinToolInstaller::RemoveNow()
{
    std::unique_lock lock(folder_.Lock());
    std::vector<std::filesystem::path> targets;
    for (const core::SiteTool tool : {core::SiteTool::YtDlp, core::SiteTool::Deno}) {
        const std::wstring folder = folder_.FolderOf(tool).wstring();
        targets.insert(targets.end(), {folder, folder + L".new", folder + L".old", folder + L".download.zip"});
    }
    targets.push_back(folder_.Root() / L"cache");
    bool left = false;
    for (const std::filesystem::path& target : targets) {
        std::error_code error;
        std::filesystem::remove_all(target, error);
        left = left || static_cast<bool>(error);
    }
    std::error_code ignored;
    std::filesystem::remove(folder_.Root(), ignored);  // only when empty
    if (left) {
        return std::string("Some components could not be removed; close any program using them and try again.");
    }
    return std::nullopt;
}

}  // namespace ldm::platform

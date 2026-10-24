#include "platform/win/FfmpegMediaTools.h"

#include "base/Utf.h"
#include "platform/win/ChildProcess.h"
#include "platform/win/FfmpegCommands.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

namespace ldm::platform {

namespace {

using core::FailureClass;
using core::FailureReason;
using core::TransferFailure;

TransferFailure JoinFailure(std::string message)
{
    return TransferFailure{FailureReason::Protocol, FailureClass::NeedsUser, 0, std::move(message)};
}

// The first line FFmpeg printed, as the reason a join failed.
std::string FirstLine(const std::string& output)
{
    std::string line = output.substr(0, output.find_first_of("\r\n"));
    if (line.size() > 300) {
        line.resize(300);
    }
    return line.empty() ? std::string("no reason given") : line;
}

bool WriteFile(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    out.close();
    return static_cast<bool>(out);
}

std::optional<std::int64_t> SizeOf(const std::filesystem::path& path)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(size);
}

bool StartsWithWebVtt(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::array<char, 9> head{};
    in.read(head.data(), static_cast<std::streamsize>(head.size()));
    const std::string text(head.data(), static_cast<size_t>(in.gcount()));
    const std::string_view body = text.starts_with("\xEF\xBB\xBF") ? std::string_view(text).substr(3) : std::string_view(text);
    return body.starts_with("WEBVTT");
}

// FFmpeg's HLS reader runs WebVTT pieces together as one text, so a piece that does not end
// with a blank line takes the next piece's WEBVTT header into its last cue, and players then
// show "WEBVTT" as subtitle text. Copies of the pieces that each end with a blank line go into
// `folder`\subtitles; the returned input names them. Nothing when a piece cannot be read or
// written.
std::optional<core::MuxInput> WithSeparatedCues(const core::MuxInput& input, const std::filesystem::path& folder)
{
    core::MuxInput copy = input;
    const std::filesystem::path copies = folder / L"subtitles";
    std::error_code error;
    std::filesystem::create_directories(copies, error);
    if (error) {
        return std::nullopt;
    }
    for (core::MuxPiece& piece : copy.pieces) {
        if (piece.init) {
            continue;
        }
        std::ifstream in(piece.file, std::ios::binary);
        if (!in) {
            return std::nullopt;
        }
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!text.ends_with("\n\n") && !text.ends_with("\r\n\r\n")) {
            text += text.ends_with("\n") ? "\n" : "\n\n";
        }
        const std::filesystem::path separated = copies / piece.file.filename();
        if (!WriteFile(separated, text)) {
            return std::nullopt;
        }
        piece.file = separated;
    }
    return copy;
}

class EventHandle {
public:
    EventHandle() : handle_(::CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~EventHandle()
    {
        if (handle_ != nullptr) {
            ::CloseHandle(handle_);
        }
    }
    EventHandle(const EventHandle&) = delete;
    EventHandle& operator=(const EventHandle&) = delete;
    [[nodiscard]] HANDLE Get() const noexcept { return handle_; }

private:
    HANDLE handle_;
};

}  // namespace

FfmpegMediaTools::FfmpegMediaTools(std::filesystem::path toolsFolder, core::EngineListener& listener)
    : ffmpeg_(toolsFolder / L"ffmpeg.exe"), ffprobe_(toolsFolder / L"ffprobe.exe"), listener_(listener)
{
    worker_ = std::thread([this] { RunWorker(); });
}

FfmpegMediaTools::~FfmpegMediaTools()
{
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        jobs_.clear();
        if (currentStop_ != nullptr) {
            ::SetEvent(currentStop_);
        }
    }
    wake_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

FfmpegMediaTools::JobKey FfmpegMediaTools::KeyOf(const Job& job)
{
    return std::visit([](const auto& request) { return JobKey{request.id, request.epoch}; }, job);
}

core::EnginePayload FfmpegMediaTools::StoppedAnswer(const Job& job)
{
    if (std::holds_alternative<core::DescribeRequest>(job)) {
        core::DescribeFinished stopped;
        stopped.stopped = true;
        return stopped;
    }
    core::MuxFinished stopped;
    stopped.stopped = true;
    return stopped;
}

void FfmpegMediaTools::Enqueue(Job job)
{
    {
        std::lock_guard lock(mutex_);
        if (stopping_) {
            return;
        }
        jobs_.push_back(std::move(job));
    }
    wake_.notify_one();
}

void FfmpegMediaTools::Mux(core::MuxRequest request)
{
    Enqueue(std::move(request));
}

void FfmpegMediaTools::Describe(core::DescribeRequest request)
{
    Enqueue(std::move(request));
}

void FfmpegMediaTools::Cancel(model::DownloadId id, std::int64_t epoch)
{
    std::optional<Job> dropped;
    {
        std::lock_guard lock(mutex_);
        if (current_ && *current_ == JobKey{id, epoch}) {
            ::SetEvent(currentStop_);
            return;
        }
        const auto queued =
            std::find_if(jobs_.begin(), jobs_.end(), [&](const Job& job) { return KeyOf(job) == JobKey{id, epoch}; });
        if (queued == jobs_.end()) {
            return;
        }
        dropped = std::move(*queued);
        jobs_.erase(queued);
    }
    listener_.OnEngineEvent(core::EngineEvent{id, epoch, StoppedAnswer(*dropped)});
}

void FfmpegMediaTools::RunWorker()
{
    while (true) {
        std::optional<Job> job;
        EventHandle stop;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if (stopping_) {
                return;
            }
            job = std::move(jobs_.front());
            jobs_.pop_front();
            current_ = KeyOf(*job);
            currentStop_ = stop.Get();
        }
        core::EnginePayload answer;
        if (const auto* mux = std::get_if<core::MuxRequest>(&*job)) {
            answer = Process(*mux, stop.Get());
        } else {
            core::DescribeFinished described;
            described.mediaInfo = DescribeFile(std::get<core::DescribeRequest>(*job).file, stop.Get());
            answer = std::move(described);
        }
        {
            std::lock_guard lock(mutex_);
            current_.reset();
            currentStop_ = nullptr;
            if (stopping_) {
                return;  // the core no longer waits for answers
            }
        }
        if (::WaitForSingleObject(stop.Get(), 0) == WAIT_OBJECT_0) {
            answer = StoppedAnswer(*job);
        }
        const JobKey key = KeyOf(*job);
        listener_.OnEngineEvent(core::EngineEvent{key.first, key.second, std::move(answer)});
    }
}

core::MuxFinished FfmpegMediaTools::Process(const core::MuxRequest& request, HANDLE stop) const
{
    if (request.inputs.empty()) {
        core::MuxFinished failed;
        failed.failure = JoinFailure("the stream has no tracks to join");
        return failed;
    }
    return request.preferredExtension == "vtt" ? ConvertSubtitles(request, stop) : JoinStreams(request, stop);
}

core::MuxFinished FfmpegMediaTools::JoinStreams(const core::MuxRequest& request, HANDLE stop) const
{
    core::MuxFinished result;
    std::vector<MuxSource> sources;
    for (size_t i = 0; i < request.inputs.size(); ++i) {
        const core::MuxInput& input = request.inputs[i];
        if (input.role == model::TrackRole::Subtitles) {
            continue;  // subtitles are downloads of their own
        }
        if (input.wholeFile) {
            sources.push_back(MuxSource{*input.wholeFile, false, input.role});
            continue;
        }
        const auto playlist = LocalPlaylist(input, request.folder);
        const std::filesystem::path file = request.folder / (L"track" + std::to_wstring(i) + L".m3u8");
        if (!playlist) {
            result.failure = JoinFailure("the stream's pieces are not in its parts folder");
            return result;
        }
        if (!WriteFile(file, *playlist)) {
            result.failure = TransferFailure{FailureReason::DiskError, FailureClass::NeedsUser, 0,
                                             "the list of the stream's pieces could not be written"};
            return result;
        }
        sources.push_back(MuxSource{file, true, input.role});
    }
    if (sources.empty()) {
        result.failure = JoinFailure("the stream has no video or audio to join");
        return result;
    }

    std::vector<MediaContainer> containers{ContainerFor(request.inputs)};
    if (containers.front() == MediaContainer::Mp4) {
        containers.push_back(MediaContainer::Matroska);  // what MP4 refuses, Matroska takes
    }
    std::string lastError;
    for (const MediaContainer container : containers) {
        const std::string extension = ExtensionFor(container, request.inputs);
        const std::filesystem::path output = request.folder / base::ToUtf16("output." + extension);
        ChildProcessRequest run;
        run.program = ffmpeg_;
        run.arguments = JoinArguments(sources, container, output);
        run.workingDirectory = request.folder;
        run.limits.timeout = kJoinTimeout;
        const auto ran = RunChildProcess(run, stop);
        if (!ran) {
            result.failure = TransferFailure{FailureReason::Internal, FailureClass::Fatal, 0,
                                             "FFmpeg could not be started: " + ran.GetError().message};
            return result;
        }
        if (ran.Value().end == ChildProcessEnd::Stopped) {
            result.stopped = true;
            return result;
        }
        if (ran.Value().end == ChildProcessEnd::TimedOut) {
            result.failure = JoinFailure("FFmpeg took too long to join the stream");
            return result;
        }
        const auto size = SizeOf(output);
        if (ran.Value().exitCode == 0 && size && *size > 0) {
            result.output = output;
            result.extension = extension;
            result.bytes = *size;
            result.mediaInfo = DescribeFile(output, stop);
            return result;
        }
        lastError = FirstLine(ran.Value().output);
        std::error_code ignored;
        std::filesystem::remove(output, ignored);
    }
    result.failure = JoinFailure("FFmpeg could not join the stream: " + lastError);
    return result;
}

core::MuxFinished FfmpegMediaTools::ConvertSubtitles(const core::MuxRequest& request, HANDLE stop) const
{
    core::MuxFinished result;
    const core::MuxInput& input = request.inputs.front();
    std::optional<std::filesystem::path> single = input.wholeFile;
    const auto mediaPieces = std::count_if(input.pieces.begin(), input.pieces.end(), [](const core::MuxPiece& p) { return !p.init; });
    if (!single && mediaPieces == 1 && input.pieces.size() == 1) {
        single = input.pieces.front().file;
    }
    const std::filesystem::path output = request.folder / L"output.vtt";
    MuxSource source;
    if (single) {
        if (StartsWithWebVtt(*single)) {
            // Already WebVTT: kept exactly as the site made it.
            std::error_code error;
            std::filesystem::copy_file(*single, output, std::filesystem::copy_options::overwrite_existing, error);
            if (error) {
                result.failure = TransferFailure{FailureReason::DiskError, FailureClass::NeedsUser, 0,
                                                 "the subtitles could not be copied: " + error.message()};
                return result;
            }
        }
        source = MuxSource{*single, false, model::TrackRole::Subtitles};
    } else {
        const auto separated = WithSeparatedCues(input, request.folder);
        const auto playlist = separated ? LocalPlaylist(*separated, request.folder) : std::nullopt;
        const std::filesystem::path file = request.folder / L"subtitles.m3u8";
        if (!playlist || !WriteFile(file, *playlist)) {
            result.failure = JoinFailure("the subtitles' pieces could not be listed");
            return result;
        }
        source = MuxSource{file, true, model::TrackRole::Subtitles};
    }
    if (!SizeOf(output)) {
        ChildProcessRequest run;
        run.program = ffmpeg_;
        run.arguments = SubtitleArguments(source, output);
        run.workingDirectory = request.folder;
        const auto ran = RunChildProcess(run, stop);
        if (!ran) {
            result.failure = TransferFailure{FailureReason::Internal, FailureClass::Fatal, 0,
                                             "FFmpeg could not be started: " + ran.GetError().message};
            return result;
        }
        if (ran.Value().end == ChildProcessEnd::Stopped) {
            result.stopped = true;
            return result;
        }
        if (ran.Value().end != ChildProcessEnd::Exited || ran.Value().exitCode != 0 || !SizeOf(output)) {
            result.failure = JoinFailure("FFmpeg could not turn the subtitles into WebVTT: " + FirstLine(ran.Value().output));
            return result;
        }
    }
    result.output = output;
    result.extension = "vtt";
    result.bytes = SizeOf(output).value_or(0);
    result.mediaInfo = input.language.empty() ? "WebVTT subtitles" : "WebVTT subtitles, " + input.language;
    return result;
}

std::string FfmpegMediaTools::DescribeFile(const std::filesystem::path& file, HANDLE stop) const
{
    ChildProcessRequest run;
    run.program = ffprobe_;
    run.arguments = ProbeArguments(file);
    run.limits.timeout = std::chrono::minutes(1);
    const auto ran = RunChildProcess(run, stop);
    if (!ran || ran.Value().end != ChildProcessEnd::Exited || ran.Value().exitCode != 0) {
        return {};  // the file is fine; only its description is missing
    }
    return DescribeProbe(ran.Value().output);
}

}  // namespace ldm::platform

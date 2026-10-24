#include "core/Coordinator.h"

#include "base/Paths.h"
#include "base/Utf.h"
#include "core/FailureText.h"
#include "core/FileNames.h"
#include "core/Matching.h"
#include "core/MediaDownloads.h"
#include "core/MediaPieces.h"
#include "core/NetworkAddress.h"
#include "model/Cookie.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <iterator>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ldm::core {

namespace {

using model::DownloadStatus;

constexpr int kMaxNumberedNames = 9999;
// How long the core thread sleeps at most while a schedule is enabled (M4 design section 4.1).
constexpr std::chrono::seconds kScheduleRecheck{60};
constexpr int kMaxFinalizeAttempts = 3;
constexpr int kMaxConnections = 32;
constexpr int kMaxSpeedLimiterKilobytes = 10'000'000;
// A live recording whose playlist cannot be read this many times in a row fails.
constexpr int kMaxPlaylistReloadFailures = 6;

struct BuiltinCategory {
    std::string_view name;
    std::string_view extensions;
};

// The built-in categories (checklist W-02) with their extension lists.
constexpr std::array<BuiltinCategory, 6> kBuiltinCategories{{
    {"General", ""},
    {"Compressed", "zip rar 7z arj gz sit sitx sea ace bz2 tar lzh lha z cab xz zst tgz"},
    {"Documents", "doc docx pdf txt rtf odt ods odp xls xlsx ppt pptx epub csv md"},
    {"Music", "mp3 wav wma mpa ram ra aac aif aiff m4a flac ogg opus mid midi"},
    {"Programs", "exe msi msix msixbundle appx appxbundle msu iso img dmg apk bin"},
    {"Video", "avi mpg mpe mpeg asf wmv mov qt rm rmvb mp4 flv m4v webm ogv ts mkv 3gp"},
}};

std::filesystem::path PathFromUtf8(std::string_view utf8)
{
    return std::filesystem::path(base::ToUtf16(utf8));
}

std::string PathToUtf8(const std::filesystem::path& path)
{
    return base::ToUtf8(path.wstring());
}

bool CanStart(DownloadStatus status)
{
    return status == DownloadStatus::Queued || status == DownloadStatus::Paused || status == DownloadStatus::Failed;
}

// A type a browser reports for a file it downloads. Web page types are left out, so a page the
// browser itself downloaded never makes LDM refuse a page.
bool IsFileContentType(std::string_view type)
{
    if (type.empty() || type.size() > 256 ||
        std::any_of(type.begin(), type.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7F; })) {
        return false;
    }
    std::string lower(type.substr(0, type.find(';')));
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    lower.erase(std::remove(lower.begin(), lower.end(), ' '), lower.end());
    return !lower.empty() && lower != "text/html" && lower != "application/xhtml+xml";
}

// A stream's pieces while it downloads: the download's partial path names a folder.
std::filesystem::path PartsFolder(const model::Download& download)
{
    return PathFromUtf8(download.partialPath.value_or(std::string()));
}

// Two downloads of the same stream: the same manifest and the same tracks of it.
bool SameStream(const model::Download& download, const AddDownloadCommand& command)
{
    if (!command.media || !download.IsStream() || download.tracks.size() != command.media->tracks.size()) {
        return false;
    }
    for (size_t i = 0; i < download.tracks.size(); ++i) {
        if (download.tracks[i].sourceUrl != command.media->tracks[i].sourceUrl) {
            return false;
        }
    }
    return true;
}

// How long until a live recording's playlists are read again (RFC 8216 section 6.3.4): the target
// duration after a reload that brought something, half of it after one that did not. The target
// is the longest recent piece of a live track, kept between 1 and 60 seconds.
std::chrono::milliseconds ReloadInterval(const model::Download& download, bool changed)
{
    constexpr size_t kRecentPieces = 16;
    constexpr std::int64_t kUnknownUs = 2'000'000;  // no segment yet: a common target duration
    std::int64_t longestUs = 0;
    for (const model::MediaTrack& track : download.tracks) {
        if (!track.live) {
            continue;
        }
        size_t seen = 0;
        for (auto piece = track.pieces.rbegin(); piece != track.pieces.rend() && seen < kRecentPieces; ++piece) {
            if (piece->kind == model::MediaPiece::Kind::Media) {
                longestUs = std::max(longestUs, piece->durationUs);
                ++seen;
            }
        }
    }
    const std::int64_t targetUs =
        std::clamp(longestUs > 0 ? longestUs : kUnknownUs, std::int64_t{1'000'000}, std::int64_t{60'000'000});
    return std::chrono::milliseconds((changed ? targetUs : targetUs / 2) / 1000);
}

// A video or audio file that ffprobe can describe for the list (checklist C-04): the media types
// LDM knows, apart from subtitles, which have no picture or sound to describe.
bool IsDescribedMediaFile(const std::string& fileName)
{
    static constexpr std::array<std::string_view, 24> kTypes{
        "3gp", "aac", "asf", "avi", "f4v", "flac", "flv", "m4a", "m4v", "mkv",  "mov", "mp3",
        "mp4", "mpeg", "mpg", "ogg", "ogv", "opus", "qt", "ts", "wav", "webm", "wma", "wmv"};
    std::string extension = FileExtension(fileName);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return std::find(kTypes.begin(), kTypes.end(), extension) != kTypes.end();
}

bool HasRecordedPieces(const model::Download& download)
{
    return std::any_of(download.tracks.begin(), download.tracks.end(), [](const model::MediaTrack& track) {
        return std::any_of(track.pieces.begin(), track.pieces.end(), [](const model::MediaPiece& piece) {
            return piece.kind == model::MediaPiece::Kind::Media && piece.done;
        });
    });
}

std::int64_t ProgressEnd(const model::Download& download)
{
    std::int64_t end = 0;
    for (const model::Segment& segment : download.segments) {
        end = std::max(end, segment.start + segment.done);
    }
    return end;
}

}  // namespace

Coordinator::Coordinator(const CoordinatorPorts& ports, CoordinatorConfig config)
    : ports_(ports), config_(std::move(config))
{
    if (ports_.engine != nullptr && ports_.proxy != nullptr && ports_.observer != nullptr) {
        manifests_ = std::make_unique<ManifestFetches>(*ports_.engine, *ports_.proxy, *ports_.observer);
    }
}

// ---------------------------------------------------------------------------------------------
// Startup

base::Status Coordinator::Initialize()
{
    auto storedSettings = ports_.store->LoadSettings();
    if (!storedSettings) {
        return storedSettings.GetError();
    }
    model::SettingsLoadResult loaded = model::SettingsFromMap(storedSettings.Value());
    settings_ = std::move(loaded.settings);
    for (std::string& warning : loaded.warnings) {
        ReportError("Setting reset to its default: " + warning);
    }

    auto categories = ports_.store->LoadCategories();
    if (!categories) {
        return categories.GetError();
    }
    categories_ = std::move(categories).Value();
    if (categories_.empty()) {
        if (base::Status seeded = SeedCategories(); !seeded) {
            return seeded;
        }
    }

    auto queues = ports_.store->LoadQueues();
    if (!queues) {
        return queues.GetError();
    }
    queues_ = std::move(queues).Value();

    auto logins = ports_.store->LoadSiteLogins();
    if (!logins) {
        return logins.GetError();
    }
    siteLogins_ = std::move(logins).Value();
    auto limits = ports_.store->LoadServerLimits();
    if (!limits) {
        return limits.GetError();
    }
    serverLimits_ = std::move(limits).Value();

    auto downloads = ports_.store->LoadDownloads();
    if (!downloads) {
        return downloads.GetError();
    }
    const auto now = ports_.clock->Steady();
    for (model::Download& download : downloads.Value()) {
        nextSortOrder_ = std::max(nextSortOrder_, download.sortOrder + 1);
        const model::DownloadId id = download.id;
        Entry entry;
        entry.download = std::move(download);
        entry.lastCheckpoint = now;
        entries_.emplace(id, std::move(entry));
    }
    std::vector<model::DownloadId> ids;
    for (const auto& [id, entry] : entries_) {
        ids.push_back(id);
    }
    for (const model::DownloadId id : ids) {
        RecoverAfterRestart(entries_.at(id));
    }

    speedLimiterOn_ = settings_.speedLimiterOnStartup;
    ApplySpeedLimiter();
    ports_.proxy->Configure(settings_);
    // Starts that passed while LDM was not running are missed (M4 design decision 4).
    lastScheduleCheckUnixMs_ = ports_.clock->NowUnixMs();
    PlanSchedules();
    StartQueuesOnAppStart();
    PublishSnapshot();
    return {};
}

base::Status Coordinator::SeedCategories()
{
    std::int64_t order = 0;
    for (const BuiltinCategory& builtin : kBuiltinCategories) {
        model::Category category;
        category.name = std::string(builtin.name);
        category.extensions = std::string(builtin.extensions);
        category.folder = PathToUtf8(builtin.name == "General" ? config_.downloadsFolder
                                                               : config_.downloadsFolder / PathFromUtf8(builtin.name));
        category.builtin = true;
        category.sortOrder = order++;
        auto id = ports_.store->InsertCategory(category);
        if (!id) {
            return id.GetError();
        }
        category.id = id.Value();
        categories_.push_back(std::move(category));
    }
    return {};
}

void Coordinator::RecoverAfterRestart(Entry& entry)
{
    model::Download& d = entry.download;
    if (d.awaitingConfirmation) {
        // LDM ended while the info dialog was open: the user never accepted this download (A-03).
        // Its partial file goes with it; a final file cannot exist, completion waits for the answer.
        RemoveEntry(d.id, false);
        return;
    }
    bool changed = false;
    switch (d.status) {
    case DownloadStatus::Downloading:
    case DownloadStatus::Stopping:
        d.status = DownloadStatus::Paused;
        changed = true;
        break;
    case DownloadStatus::Finalizing: {
        // A crash between the move and its commit leaves a complete final file and no partial file.
        // A stream joined again from its pieces otherwise: its run finds them all on disk.
        const bool partialGone = !d.partialPath || !ports_.fileSystem->Exists(PathFromUtf8(*d.partialPath));
        const auto finalSize = ports_.fileSystem->FileSize(FinalPath(d));
        if (partialGone && finalSize && d.totalSize && *finalSize == *d.totalSize) {
            d.status = DownloadStatus::Completed;
            d.partialPath.reset();
            d.cookieSecret.clear();  // design decision 3: browser cookies and form data are kept only until completion
            d.postSecret.clear();
            d.postContentType.reset();
            if (!d.completedAtMs) {
                d.completedAtMs = ports_.clock->NowUnixMs();
            }
            LeaveQueueOnCompletion(entry);
        } else {
            d.status = DownloadStatus::Paused;
        }
        changed = true;
        break;
    }
    case DownloadStatus::Deleting:
        RemoveEntry(d.id, false);
        return;
    default:
        break;
    }
    // The database may lag behind the partial file, never the other way round; a partial file that
    // is gone or shorter than the stored progress means the progress cannot be trusted.
    if (d.IsStream()) {
        if (d.status != DownloadStatus::Completed && d.partialPath) {
            StoreTracks(entry, VerifyMediaProgress(d, PartsFolder(d), *ports_.fileSystem));
        }
    } else if (d.status != DownloadStatus::Completed && ProgressEnd(d) > 0) {
        const auto size = d.partialPath ? ports_.fileSystem->FileSize(PathFromUtf8(*d.partialPath)) : std::nullopt;
        if (!size || *size < ProgressEnd(d)) {
            ResetProgress(entry);
            changed = true;
        }
    }
    if (changed) {
        Save(entry);
    }
}

// ---------------------------------------------------------------------------------------------
// Commands

void Coordinator::Handle(Command command)
{
    if (shuttingDown_) {
        if (const auto* add = std::get_if<AddDownloadCommand>(&command)) {
            AddResult result;
            result.token = add->token;
            result.error = "LDM is shutting down";
            Notify(std::move(result));
        }
        return;
    }
    std::visit(
        [this](auto& typed) {
            using T = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<T, AddDownloadCommand>) {
                Add(typed);
            } else if constexpr (std::is_same_v<T, ConfirmDownloadCommand>) {
                Confirm(typed);
            } else if constexpr (std::is_same_v<T, ResumeCommand>) {
                Resume(typed.ids);
            } else if constexpr (std::is_same_v<T, StopCommand>) {
                Stop(typed.ids);
            } else if constexpr (std::is_same_v<T, StopAllCommand>) {
                StopAll();
            } else if constexpr (std::is_same_v<T, FinishRecordingCommand>) {
                FinishRecording(typed.ids);
            } else if constexpr (std::is_same_v<T, DeleteCommand>) {
                Delete(typed.ids, typed.deleteFiles);
            } else if constexpr (std::is_same_v<T, DeleteCompletedCommand>) {
                DeleteCompleted();
            } else if constexpr (std::is_same_v<T, RedownloadCommand>) {
                Redownload(typed.id);
            } else if constexpr (std::is_same_v<T, SetDownloadSpeedLimitCommand>) {
                SetDownloadSpeedLimit(typed.id, typed.bytesPerSecond);
            } else if constexpr (std::is_same_v<T, SetSpeedLimiterCommand>) {
                SetSpeedLimiter(typed.enabled, typed.kilobytesPerSecond);
            } else if constexpr (std::is_same_v<T, UpdateDownloadCommand>) {
                Update(typed);
            } else if constexpr (std::is_same_v<T, MoveDownloadCommand>) {
                Move(typed);
            } else if constexpr (std::is_same_v<T, SaveCategoryCommand>) {
                SaveCategory(std::move(typed.category));
            } else if constexpr (std::is_same_v<T, DeleteCategoryCommand>) {
                DeleteCategory(typed.id);
            } else if constexpr (std::is_same_v<T, ReassignCategoryCommand>) {
                ReassignCategory(typed.id);
            } else if constexpr (std::is_same_v<T, UpdateSettingsCommand>) {
                UpdateSettings(std::move(typed.settings));
            } else if constexpr (std::is_same_v<T, ModifySettingsCommand>) {
                if (typed.modify) {
                    model::Settings changed = settings_;
                    typed.modify(changed);
                    UpdateSettings(std::move(changed));
                }
            } else if constexpr (std::is_same_v<T, ReplaceSiteLoginsCommand>) {
                ReplaceSiteLogins(typed.logins);
            } else if constexpr (std::is_same_v<T, ReplaceServerLimitsCommand>) {
                ReplaceServerLimits(std::move(typed.limits));
            } else if constexpr (std::is_same_v<T, QuotaDecisionCommand>) {
                DecideQuota(typed.keepDownloading);
            } else if constexpr (std::is_same_v<T, WaitForAddressCommand>) {
                WaitForAddress(typed.id);
            } else if constexpr (std::is_same_v<T, ReplaceAddressCommand>) {
                ReplaceAddress(typed);
            } else if constexpr (std::is_same_v<T, FetchManifestCommand>) {
                if (manifests_) {
                    manifests_->Start(std::move(typed), settings_.userAgent);
                }
            } else if constexpr (std::is_same_v<T, SaveQueueCommand>) {
                SaveQueue(std::move(typed.queue), typed.members);
            } else if constexpr (std::is_same_v<T, DeleteQueueCommand>) {
                DeleteQueue(typed.id);
            } else if constexpr (std::is_same_v<T, SetQueueCommand>) {
                SetQueue(typed.ids, typed.queue);
            } else if constexpr (std::is_same_v<T, ReorderQueueCommand>) {
                ReorderQueue(typed.queue, typed.order);
            } else if constexpr (std::is_same_v<T, StartQueueCommand>) {
                StartQueue(typed.id);
            } else if constexpr (std::is_same_v<T, StopQueueCommand>) {
                StopQueue(typed.id);
            } else if constexpr (std::is_same_v<T, ClockChangedCommand>) {
                CheckSchedules();
            } else if constexpr (std::is_same_v<T, SetListedCommand>) {
                SetListed(typed.ids);
            } else {
                static_assert(sizeof(T) == 0, "unhandled command");
            }
        },
        command);
}

void Coordinator::Add(const AddDownloadCommand& command)
{
    AddResult result;
    result.token = command.token;
    const auto fail = [&](std::string error) {
        result.error = std::move(error);
        Notify(std::move(result));
    };

    const auto url = ParseUrl(command.url);
    if (!url) {
        fail("The address is not a valid http, https or ftp address.");
        return;
    }
    // Keep the address as the user gave it, minus surrounding whitespace.
    std::string address = command.url;
    address.erase(0, address.find_first_not_of(" \t\r\n"));
    address.erase(address.find_last_not_of(" \t\r\n") + 1);

    std::optional<std::string> overwriteName;
    std::optional<std::string> overwriteDir;
    if (command.media && url->scheme != "http" && url->scheme != "https") {
        fail("A stream's address must be an http or https address.");
        return;
    }
    const auto duplicate = std::find_if(entries_.begin(), entries_.end(), [&](const auto& item) {
        const model::Download& existing = item.second.download;
        const bool same = command.media ? SameStream(existing, command) : !existing.IsStream();
        return existing.url == address && same && !item.second.deleteWhenStopped;
    });
    if (duplicate != entries_.end()) {
        Entry& existing = duplicate->second;
        switch (command.duplicatePolicy.value_or(settings_.duplicatePolicy)) {
        case model::DuplicatePolicy::Ask:
            result.duplicateOf = existing.download.id;
            Notify(std::move(result));
            return;
        case model::DuplicatePolicy::AddNumbered:
            break;
        case model::DuplicatePolicy::Overwrite:
            overwriteName = existing.download.fileName;
            overwriteDir = existing.download.saveDir;
            Delete({existing.download.id}, false);
            break;
        case model::DuplicatePolicy::ResumeOrShowComplete:
            result.id = existing.download.id;
            if (existing.download.status == DownloadStatus::Completed) {
                result.showExistingComplete = true;
            } else {
                Resume({existing.download.id});
            }
            Notify(std::move(result));
            return;
        }
    }

    std::string fileName = SanitizeFileName(command.fileName ? *command.fileName
                                            : overwriteName ? *overwriteName
                                                            : FileNameFromUrlPath(url->path));
    // Subtitles of a stream go where its video goes, so a player finds them next to it.
    const bool onlySubtitles = command.media && !command.media->tracks.empty() &&
                               std::all_of(command.media->tracks.begin(), command.media->tracks.end(),
                                           [](const MediaTrackChoice& t) { return t.role == model::TrackRole::Subtitles; });
    model::CategoryId categoryId = CategoryFor(onlySubtitles ? WithExtension(fileName, "mp4") : fileName, *url);
    if (command.categoryId) {
        if (FindCategory(*command.categoryId) == nullptr) {
            fail("The selected category no longer exists.");
            return;
        }
        categoryId = *command.categoryId;
    }
    std::string saveDir = command.saveDir ? *command.saveDir : overwriteDir ? *overwriteDir : FolderFor(categoryId);
    if (!PathFromUtf8(saveDir).is_absolute()) {
        fail("The save folder must be a full path.");
        return;
    }

    // Another download's name can never be taken over; a file that is only on disk may be
    // replaced when the user or the duplicate policy says so (checklist A-05, A-06).
    bool replaceFinal = false;
    const std::filesystem::path target = PathFromUtf8(saveDir) / PathFromUtf8(fileName);
    const bool reserved = IsReserved(target, std::nullopt);
    if (reserved || ports_.fileSystem->Exists(target)) {
        bool rename = true;
        if (overwriteName) {
            rename = reserved;
        } else if (command.existingFilePolicy == ExistingFilePolicy::Cancel) {
            fail("A file with this name already exists.");
            return;
        } else if (command.existingFilePolicy == ExistingFilePolicy::Replace) {
            rename = reserved;
        }
        if (rename) {
            auto unique = UniqueFileName(PathFromUtf8(saveDir), fileName, std::nullopt);
            if (!unique) {
                fail("No free file name could be found in the save folder.");
                return;
            }
            fileName = std::move(*unique);
        } else {
            replaceFinal = true;
        }
    }

    model::Download download;
    download.publicId = ports_.ids->NewPublicId();
    download.url = address;
    download.referer = command.referer;
    download.parentPage = command.parentPage;
    download.userAgent = command.userAgent;
    download.fileName = fileName;
    download.saveDir = saveDir;
    download.categoryId = categoryId;
    download.description = command.description;
    if (command.contentType && IsFileContentType(*command.contentType)) {
        download.contentType = command.contentType;
    }
    download.status = DownloadStatus::Queued;
    download.awaitingConfirmation = command.mode == AddMode::Speculative;
    download.createdAtMs = ports_.clock->NowUnixMs();
    download.sortOrder = nextSortOrder_++;
    if (command.loginUser && !command.loginUser->empty()) {
        download.loginUser = command.loginUser;
        if (command.loginPassword && !command.loginPassword->empty()) {
            auto secret = ports_.secrets->Protect(*command.loginPassword);
            if (!secret) {
                fail("The password could not be protected: " + secret.GetError().message);
                return;
            }
            download.loginSecret = std::move(secret).Value();
        }
    }
    std::vector<model::Cookie> cookies;
    std::copy_if(command.cookies.begin(), command.cookies.end(), std::back_inserter(cookies), model::IsValidCookie);
    if (!cookies.empty()) {
        auto secret = ports_.secrets->Protect(model::CookiesToText(cookies));
        if (!secret) {
            fail("The browser cookies could not be protected: " + secret.GetError().message);
            return;
        }
        download.cookieSecret = std::move(secret).Value();
    }
    if (command.post) {
        auto secret = ports_.secrets->Protect(command.post->body);
        if (!secret) {
            fail("The form data could not be protected: " + secret.GetError().message);
            return;
        }
        download.postSecret = std::move(secret).Value();
        download.postContentType = command.post->contentType;
    }
    if (command.media) {
        if (auto error = ValidateMedia(*command.media, download)) {
            fail(std::move(*error));
            return;
        }
    }
    // Download Later goes into the main queue unless another was chosen (M4 design decision 2).
    const std::optional<model::QueueId> queue = command.queue ? command.queue
                                                : command.mode == AddMode::Later ? MainQueue()
                                                                                 : std::nullopt;
    if (queue) {
        if (FindQueue(*queue) == nullptr) {
            fail("The selected queue no longer exists.");
            return;
        }
        download.queueId = queue;
        download.queuePosition = NextQueuePosition(*queue);
    }
    download.projectId = command.projectId;
    download.listed = command.listed || !command.projectId;  // only a Grabber download starts hidden

    auto inserted = ports_.store->InsertDownload(download);
    if (!inserted) {
        fail("The download could not be saved: " + inserted.GetError().message);
        return;
    }
    download.id = inserted.Value();
    Entry entry;
    entry.download = std::move(download);
    entry.userNamed = command.fileName.has_value() || overwriteName.has_value();
    entry.userChoseCategory = command.categoryId.has_value();
    entry.userChoseFolder = command.saveDir.has_value();
    entry.replaceFinalFile = replaceFinal;
    entry.lastCheckpoint = ports_.clock->Steady();
    if (command.media && command.media->approximateBytes && *command.media->approximateBytes > 0) {
        entry.approximateBytes = command.media->approximateBytes;
    }
    const model::DownloadId id = entry.download.id;
    Entry& stored = entries_.emplace(id, std::move(entry)).first->second;
    MarkChanged(stored);
    if (command.mode != AddMode::Later) {
        Start(stored);
    }
    result.id = id;
    Notify(std::move(result));
}

void Coordinator::Confirm(const ConfirmDownloadCommand& command)
{
    Entry* entry = Find(command.id);
    if (entry == nullptr || !entry->download.awaitingConfirmation) {
        return;
    }
    model::Download& d = entry->download;
    if (!PathFromUtf8(command.saveDir).is_absolute()) {
        ReportError("The save folder must be a full path.");
        return;
    }
    std::optional<model::CategoryId> category = d.categoryId;
    if (command.categoryId && FindCategory(*command.categoryId) != nullptr) {
        category = command.categoryId;
        entry->userChoseCategory = true;
    }
    const std::string name = SanitizeFileName(command.fileName);
    if (name != d.fileName || command.saveDir != d.saveDir) {
        auto unique = UniqueFileName(PathFromUtf8(command.saveDir), name, d.id);
        if (!unique) {
            ReportError("No free file name could be found in the save folder.");
            return;
        }
        d.fileName = std::move(*unique);
        d.saveDir = command.saveDir;
    }
    d.categoryId = category;
    d.description = command.description;
    entry->userNamed = true;
    d.awaitingConfirmation = false;
    // Download Later goes into the main queue unless another was chosen (M4 design decision 2).
    const std::optional<model::QueueId> queue = command.queue ? command.queue
                                                : !command.start  ? MainQueue()
                                                                  : std::nullopt;
    if (queue && FindQueue(*queue) != nullptr && d.queueId != queue) {
        d.queuePosition = NextQueuePosition(*queue);
        d.queueId = queue;
        d.queueFailures = 0;
        MarkMembersChanged(*queue);
    }

    if (command.rememberFolderForCategory && category) {
        const auto it = std::find_if(categories_.begin(), categories_.end(),
                                     [&](const model::Category& c) { return c.id == *category; });
        if (it != categories_.end() && it->folder != command.saveDir) {
            it->folder = command.saveDir;
            if (base::Status saved = ports_.store->SaveCategory(*it); !saved) {
                ReportError("The category folder could not be saved: " + saved.GetError().message);
            }
            snapshotDirty_ = true;
        }
    }

    if (entry->holdFinalize) {
        entry->holdFinalize = false;
        CompleteRun(*entry);
    } else if (command.start) {
        if (!entry->running && CanStart(d.status)) {
            Start(*entry);
        }
    } else if (entry->running) {
        RequestStop(*entry);  // "Download Later": keep what arrived (checklist A-03)
    }
    Save(*entry);
    MarkChanged(*entry);
}

void Coordinator::Resume(const std::vector<model::DownloadId>& ids)
{
    for (const model::DownloadId id : ids) {
        StopWaitingForAddress(id);  // resumed by hand: the old address is tried again
        for (auto& [queueId, run] : queueRuns_) {
            run.stoppedByUser.erase(id);
        }
        Entry* entry = Find(id);
        if (entry != nullptr && !entry->running && !entry->finalizing && CanStart(entry->download.status)) {
            Start(*entry);
        }
    }
}

void Coordinator::Stop(const std::vector<model::DownloadId>& ids)
{
    for (const model::DownloadId id : ids) {
        Entry* entry = Find(id);
        if (entry == nullptr) {
            continue;
        }
        if (entry->download.queueId) {
            if (const auto run = queueRuns_.find(*entry->download.queueId); run != queueRuns_.end()) {
                run->second.stoppedByUser.insert(id);  // its queue does not start it again in this run
                MarkChanged(*entry);
            }
        }
        if (entry->quotaHeld) {
            // Stopped by the user: it no longer starts by itself when the quota allows.
            entry->quotaHeld = false;
            entry->download.lastResult.reset();
            Save(*entry);
            MarkChanged(*entry);
        }
        if (entry->running) {
            RequestStop(*entry);
        } else if (entry->muxing || entry->describing) {
            CancelMediaTools(*entry);  // the pieces or the file stay; Resume joins or describes them again
        }
    }
}

void Coordinator::StopAll()
{
    std::vector<model::QueueId> running;
    for (const auto& [id, run] : queueRuns_) {
        running.push_back(id);
    }
    for (const model::QueueId id : running) {
        StopQueue(id);
    }
    for (auto& [id, entry] : entries_) {
        if (entry.running) {
            RequestStop(entry);
        }
    }
}

void Coordinator::FinishRecording(const std::vector<model::DownloadId>& ids)
{
    for (const model::DownloadId id : ids) {
        Entry* entry = Find(id);
        if (entry == nullptr || entry->deleteWhenStopped || entry->finalizing || !IsRecording(entry->download)) {
            continue;
        }
        if (entry->running) {
            // Set first: a run the engine never started ends inside RequestStop.
            entry->finishRecording = true;
            RequestStop(*entry);
            continue;
        }
        if (CanStart(entry->download.status)) {
            if (entry->quotaHeld) {
                entry->quotaHeld = false;
                entry->download.lastResult.reset();
            }
            FinishRecordingNow(*entry);
        }
    }
}

void Coordinator::Delete(const std::vector<model::DownloadId>& ids, bool deleteFiles)
{
    for (const model::DownloadId id : ids) {
        Entry* entry = Find(id);
        if (entry == nullptr || entry->deleteWhenStopped) {
            continue;
        }
        if (entry->running || entry->finalizing) {
            // Files are deleted only after the engine closed them (design section 3).
            entry->deleteWhenStopped = true;
            entry->deleteFilesWhenStopped = deleteFiles;
            entry->download.status = DownloadStatus::Deleting;
            Save(*entry);
            MarkChanged(*entry);
            if (entry->running) {
                RequestStop(*entry);  // last: a run that never reached the engine is removed here
            } else if (entry->muxing || entry->describing) {
                CancelMediaTools(*entry);
            }
            continue;
        }
        RemoveEntry(id, deleteFiles && entry->download.status == DownloadStatus::Completed);
    }
}

void Coordinator::DeleteCompleted()
{
    std::vector<model::DownloadId> completed;
    for (const auto& [id, entry] : entries_) {
        if (entry.download.status == DownloadStatus::Completed) {
            completed.push_back(id);
        }
    }
    Delete(completed, false);
}

void Coordinator::SetListed(const std::vector<model::DownloadId>& ids)
{
    for (const model::DownloadId id : ids) {
        Entry* entry = Find(id);
        if (entry == nullptr || entry->download.listed) {
            continue;
        }
        entry->download.listed = true;
        Save(*entry);
        MarkChanged(*entry);
    }
}

void Coordinator::Redownload(model::DownloadId id)
{
    for (auto& [queueId, run] : queueRuns_) {
        run.stoppedByUser.erase(id);
    }
    Entry* entry = Find(id);
    if (entry == nullptr || entry->deleteWhenStopped || entry->finalizing) {
        return;
    }
    if (entry->running) {
        entry->restartWhenStopped = true;
        RequestStop(*entry);
        return;
    }
    entry->syncCheck.reset();  // the user wants the file again, whether it changed or not
    if (entry->download.status == DownloadStatus::Completed) {
        entry->replaceFinalFile = true;
        entry->download.completedAtMs.reset();
    }
    ResetProgress(*entry);
    entry->automaticRestarts = 0;
    entry->forcedMaxConnections.reset();
    entry->download.status = DownloadStatus::Queued;
    Start(*entry);
}

void Coordinator::SetDownloadSpeedLimit(model::DownloadId id, std::optional<std::int64_t> bytesPerSecond)
{
    Entry* entry = Find(id);
    if (entry == nullptr) {
        return;
    }
    if (bytesPerSecond && *bytesPerSecond <= 0) {
        bytesPerSecond.reset();
    }
    entry->download.speedLimitBytesPerSecond = bytesPerSecond;
    if (entry->running) {
        ports_.engine->SetSpeedLimit(id, entry->download.epoch, bytesPerSecond);
    }
    Save(*entry);
    MarkChanged(*entry);
}

void Coordinator::SetSpeedLimiter(bool enabled, std::optional<int> kilobytesPerSecond)
{
    if (kilobytesPerSecond) {
        if (*kilobytesPerSecond < 1 || *kilobytesPerSecond > kMaxSpeedLimiterKilobytes) {
            ReportError("The speed limit must be between 1 and 10000000 KB/s.");
            return;
        }
        settings_.speedLimiterKilobytesPerSecond = *kilobytesPerSecond;
    }
    settings_.speedLimiterEnabled = enabled;
    speedLimiterOn_ = enabled;
    if (base::Status saved = ports_.store->SaveSettings(model::SettingsToMap(settings_)); !saved) {
        ReportError("Settings could not be saved: " + saved.GetError().message);
    }
    ApplySpeedLimiter();
    snapshotDirty_ = true;
}

void Coordinator::Update(const UpdateDownloadCommand& command)
{
    Entry* entry = Find(command.id);
    if (entry == nullptr || entry->deleteWhenStopped) {
        return;
    }
    model::Download& d = entry->download;
    if (command.url && *command.url != d.url) {
        if (d.IsStream()) {
            ReportError("The address of a stream cannot be changed: its tracks come from that address.");
            return;
        }
        if (!ParseUrl(*command.url)) {
            ReportError("The address is not a valid http, https or ftp address.");
            return;
        }
        d.url = *command.url;  // used from the next start; a running transfer keeps its address
    }
    if (command.description) {
        d.description = *command.description;
    }
    if (command.referer) {
        d.referer = command.referer->empty() ? std::nullopt : command.referer;
    }
    if (command.loginUser) {
        d.loginUser = command.loginUser->empty() ? std::nullopt : command.loginUser;
        if (!d.loginUser) {
            d.loginSecret.clear();
        }
    }
    if (command.loginPassword) {
        if (command.loginPassword->empty()) {
            d.loginSecret.clear();
        } else {
            auto secret = ports_.secrets->Protect(*command.loginPassword);
            if (!secret) {
                ReportError("The password could not be protected: " + secret.GetError().message);
                return;
            }
            d.loginSecret = std::move(secret).Value();
        }
    }
    if (command.completionAction) {
        d.completionAction = *command.completionAction;
    }
    if (command.maxConnections) {
        d.maxConnections = std::clamp(*command.maxConnections, 1, kMaxConnections);
    }
    Save(*entry);
    MarkChanged(*entry);
}

void Coordinator::WaitForAddress(model::DownloadId id)
{
    Entry* entry = Find(id);
    if (entry == nullptr || entry->deleteWhenStopped || entry->finalizing ||
        entry->download.status == DownloadStatus::Completed || entry->download.IsStream()) {
        return;  // a stream's addresses come from its manifest, not from one page link
    }
    if (waitingForAddress_ && *waitingForAddress_ != id) {
        StopWaitingForAddress(*waitingForAddress_);  // one at a time: the newer request takes over
    }
    waitingForAddress_ = id;
    waitingSince_ = ports_.clock->Steady();
    entry->resumeWhenStopped = false;
    if (entry->quotaHeld) {
        entry->quotaHeld = false;
        entry->download.lastResult.reset();
        Save(*entry);
    }
    if (entry->running) {
        RequestStop(*entry);
    }
    MarkChanged(*entry);
}

void Coordinator::ReplaceAddress(const ReplaceAddressCommand& command)
{
    Entry* entry = Find(command.id);
    if (entry == nullptr || waitingForAddress_ != command.id || entry->deleteWhenStopped || entry->finalizing) {
        return;
    }
    if (!ParseUrl(command.url)) {
        ReportError("The new address is not a valid http, https or ftp address.");
        return;
    }
    std::vector<std::uint8_t> cookieSecret;
    std::vector<model::Cookie> cookies;
    std::copy_if(command.cookies.begin(), command.cookies.end(), std::back_inserter(cookies), model::IsValidCookie);
    if (!cookies.empty()) {
        auto secret = ports_.secrets->Protect(model::CookiesToText(cookies));
        if (!secret) {
            ReportError("The browser cookies could not be protected: " + secret.GetError().message);
            return;
        }
        cookieSecret = std::move(secret).Value();
    }
    std::vector<std::uint8_t> postSecret;
    if (command.post) {
        auto secret = ports_.secrets->Protect(command.post->body);
        if (!secret) {
            ReportError("The form data could not be protected: " + secret.GetError().message);
            return;
        }
        postSecret = std::move(secret).Value();
    }

    model::Download& d = entry->download;
    d.url = command.url;
    d.finalUrl.reset();
    if (command.referer) {
        d.referer = command.referer;
    }
    if (command.userAgent) {
        d.userAgent = command.userAgent;
    }
    d.cookieSecret = std::move(cookieSecret);
    d.postSecret = std::move(postSecret);
    d.postContentType = command.post ? std::optional<std::string>(command.post->contentType) : std::nullopt;
    // The size and validators stay: the next run checks with them that the file is the same one.
    d.lastResult.reset();
    waitingForAddress_.reset();
    Save(*entry);
    MarkChanged(*entry);
    if (entry->running) {
        entry->resumeWhenStopped = true;
    } else if (CanStart(d.status)) {
        Start(*entry);
    }
}

void Coordinator::StopWaitingForAddress(model::DownloadId id)
{
    if (waitingForAddress_ != id) {
        return;
    }
    waitingForAddress_.reset();
    if (Entry* entry = Find(id)) {
        MarkChanged(*entry);
    }
}

void Coordinator::Move(const MoveDownloadCommand& command)
{
    Entry* entry = Find(command.id);
    if (entry == nullptr || entry->deleteWhenStopped || entry->finalizing) {
        return;
    }
    const std::filesystem::path directory = PathFromUtf8(command.saveDir);
    if (!directory.is_absolute()) {
        ReportError("The save folder must be a full path.");
        return;
    }
    const std::string name = SanitizeFileName(command.fileName);
    const std::filesystem::path target = directory / PathFromUtf8(name);
    model::Download& d = entry->download;
    const bool unchanged = base::PathComparisonKey(target) == base::PathComparisonKey(FinalPath(d));
    if (!unchanged && IsReserved(target, d.id)) {
        ReportError("Another download already uses this file name.");
        return;
    }
    // A completed file was already moved on disk by the caller, so its new path exists.
    if (!unchanged && d.status != DownloadStatus::Completed && ports_.fileSystem->Exists(target)) {
        ReportError("A file with this name already exists.");
        return;
    }
    d.saveDir = command.saveDir;
    d.fileName = name;
    entry->userNamed = true;
    Save(*entry);
    MarkChanged(*entry);
}

void Coordinator::SaveCategory(model::Category category)
{
    if (category.name.empty() || !PathFromUtf8(category.folder).is_absolute()) {
        ReportError("A category needs a name and a full folder path.");
        return;
    }
    if (category.id.value == 0) {
        std::int64_t order = 0;
        for (const model::Category& existing : categories_) {
            order = std::max(order, existing.sortOrder + 1);
        }
        category.sortOrder = order;
        category.builtin = false;
        auto id = ports_.store->InsertCategory(category);
        if (!id) {
            ReportError("The category could not be saved: " + id.GetError().message);
            return;
        }
        category.id = id.Value();
        categories_.push_back(std::move(category));
    } else {
        const auto it = std::find_if(categories_.begin(), categories_.end(),
                                     [&](const model::Category& c) { return c.id == category.id; });
        if (it == categories_.end()) {
            return;
        }
        category.builtin = it->builtin;
        category.sortOrder = it->sortOrder;
        if (base::Status saved = ports_.store->SaveCategory(category); !saved) {
            ReportError("The category could not be saved: " + saved.GetError().message);
            return;
        }
        *it = std::move(category);
    }
    snapshotDirty_ = true;
}

void Coordinator::DeleteCategory(model::CategoryId id)
{
    const auto it = std::find_if(categories_.begin(), categories_.end(), [&](const model::Category& c) { return c.id == id; });
    if (it == categories_.end() || it->builtin) {
        return;
    }
    if (base::Status deleted = ports_.store->DeleteCategory(id); !deleted) {
        ReportError("The category could not be deleted: " + deleted.GetError().message);
        return;
    }
    categories_.erase(it);
    for (auto& [downloadId, entry] : entries_) {
        if (entry.download.categoryId == id) {
            entry.download.categoryId.reset();  // the database did the same with ON DELETE SET NULL
            MarkChanged(entry);
        }
    }
    snapshotDirty_ = true;
}

void Coordinator::ReassignCategory(model::CategoryId id)
{
    const model::Category* category = FindCategory(id);
    if (category == nullptr) {
        return;
    }
    std::vector<std::string> extensions;
    for (size_t pos = 0; pos < category->extensions.size();) {
        const size_t end = category->extensions.find(' ', pos);
        const std::string word = category->extensions.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        if (!word.empty()) {
            extensions.push_back(word);
        }
        pos = end == std::string::npos ? category->extensions.size() : end + 1;
    }
    for (auto& [downloadId, entry] : entries_) {
        const std::string extension = FileExtension(entry.download.fileName);
        if (entry.download.categoryId != id && !extension.empty() &&
            std::find(extensions.begin(), extensions.end(), extension) != extensions.end()) {
            entry.download.categoryId = id;
            Save(entry);
            MarkChanged(entry);
        }
    }
}

void Coordinator::UpdateSettings(model::Settings settings)
{
    // A round trip through the stored form applies the same validation as loading does.
    const model::SettingsMap map = model::SettingsToMap(settings);
    model::SettingsLoadResult validated = model::SettingsFromMap(map);
    for (std::string& warning : validated.warnings) {
        ReportError("Setting reset to its default: " + warning);
    }
    settings_ = std::move(validated.settings);
    if (base::Status saved = ports_.store->SaveSettings(model::SettingsToMap(settings_)); !saved) {
        ReportError("Settings could not be saved: " + saved.GetError().message);
    }
    ports_.proxy->Configure(settings_);
    ApplySpeedLimiter();
    snapshotDirty_ = true;
    EnforceQuota();  // a new limit may stop downloads, or release held ones
}

void Coordinator::ReplaceSiteLogins(const std::vector<SiteLoginInput>& logins)
{
    std::vector<model::SiteLogin> stored;
    for (const SiteLoginInput& input : logins) {
        if (input.pattern.empty() || input.username.empty()) {
            ReportError("Every site login needs a server and a user name.");
            return;
        }
        model::SiteLogin login;
        login.pattern = input.pattern;
        login.username = input.username;
        // An empty password keeps the one already stored for the same pattern and user.
        const auto previous = std::find_if(siteLogins_.begin(), siteLogins_.end(), [&](const model::SiteLogin& l) {
            return l.pattern == input.pattern && l.username == input.username;
        });
        if (input.password.empty() && previous != siteLogins_.end()) {
            login.secret = previous->secret;
        } else {
            auto secret = ports_.secrets->Protect(input.password);
            if (!secret) {
                ReportError("A password could not be protected: " + secret.GetError().message);
                return;
            }
            login.secret = std::move(secret).Value();
        }
        stored.push_back(std::move(login));
    }
    if (base::Status saved = ports_.store->ReplaceSiteLogins(stored); !saved) {
        ReportError("Site logins could not be saved: " + saved.GetError().message);
        return;
    }
    auto reloaded = ports_.store->LoadSiteLogins();
    siteLogins_ = reloaded ? std::move(reloaded).Value() : std::move(stored);
    snapshotDirty_ = true;
}

void Coordinator::ReplaceServerLimits(std::vector<model::ServerLimit> limits)
{
    for (const model::ServerLimit& limit : limits) {
        if (limit.pattern.empty() || limit.maxConnections < 1 || limit.maxConnections > kMaxConnections) {
            ReportError("Each server exception needs a server and between 1 and 32 connections.");
            return;
        }
    }
    if (base::Status saved = ports_.store->ReplaceServerLimits(limits); !saved) {
        ReportError("Server exceptions could not be saved: " + saved.GetError().message);
        return;
    }
    serverLimits_ = std::move(limits);
    snapshotDirty_ = true;
}

// ---------------------------------------------------------------------------------------------
// Engine events

void Coordinator::OnEngineEvent(EngineEvent event)
{
    // Manifest fetches are not downloads: their answers and proxy lookups go to their own owner.
    if (auto* fetched = std::get_if<FetchFinished>(&event.payload)) {
        if (fetched->fetchId < 0) {
            OnPlaylistFetched(*fetched);
        } else if (manifests_) {
            manifests_->OnFinished(std::move(*fetched));
        }
        return;
    }
    if (ManifestFetches::IsFetchKey(event.id)) {
        if (const auto* resolved = std::get_if<ProxyResolved>(&event.payload); resolved && manifests_) {
            manifests_->OnProxyResolved(event.id, resolved->proxy);
        }
        return;
    }
    Entry* entry = Find(event.id);
    if (entry == nullptr || event.epoch != entry->download.epoch) {
        return;  // a late event from an older run (design section 3, epochs)
    }
    std::visit(
        [this, entry](const auto& payload) {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, ResponseInfo>) {
                OnResponseInfo(*entry, payload);
            } else if constexpr (std::is_same_v<T, ProgressInfo>) {
                OnProgress(*entry, payload);
            } else if constexpr (std::is_same_v<T, TransferFinished>) {
                OnFinished(*entry, payload);
            } else if constexpr (std::is_same_v<T, FinalizeFinished>) {
                OnFinalized(*entry, payload);
            } else if constexpr (std::is_same_v<T, ProxyResolved>) {
                OnProxyResolved(*entry, payload);
            } else if constexpr (std::is_same_v<T, FetchFinished>) {
                // handled above
            } else if constexpr (std::is_same_v<T, MediaProgress>) {
                OnMediaProgress(*entry, payload);
            } else if constexpr (std::is_same_v<T, MediaFinished>) {
                OnMediaFinished(*entry, payload);
            } else if constexpr (std::is_same_v<T, MuxFinished>) {
                OnMuxFinished(*entry, payload);
            } else if constexpr (std::is_same_v<T, DescribeFinished>) {
                OnDescribeFinished(*entry, payload);
            } else {
                static_assert(sizeof(T) == 0, "unhandled engine event");
            }
        },
        event.payload);
}

void Coordinator::OnResponseInfo(Entry& entry, const ResponseInfo& info)
{
    if (!entry.running) {
        return;
    }
    model::Download& d = entry.download;
    d.finalUrl = info.finalUrl != d.url ? std::optional<std::string>(info.finalUrl) : std::nullopt;
    d.totalSize = info.totalSize;
    d.resumable = info.resumable;
    d.validators = info.validators;
    if (info.contentType) {
        d.contentType = info.contentType;
    }
    if (!entry.userNamed && info.fileName) {
        // The server's name wins over the address's when the user did not choose one (ENG-06).
        const std::string name = SanitizeFileName(*info.fileName);
        if (name != d.fileName) {
            if (d.awaitingConfirmation && !entry.userChoseCategory) {
                if (const auto url = ParseUrl(d.url)) {
                    const model::CategoryId category = CategoryFor(name, *url);
                    d.categoryId = category;
                    if (!entry.userChoseFolder) {
                        d.saveDir = FolderFor(category);
                    }
                }
            }
            if (auto unique = UniqueFileName(PathFromUtf8(d.saveDir), name, d.id)) {
                d.fileName = std::move(*unique);
            }
        }
    }
    Save(entry);
    MarkChanged(entry);
}

void Coordinator::OnProgress(Entry& entry, const ProgressInfo& progress)
{
    if (!entry.running) {
        return;
    }
    model::Download& d = entry.download;
    d.segments = progress.segments;
    if (progress.totalSize) {
        d.totalSize = progress.totalSize;
    }
    d.resumable = progress.resumable;
    entry.bytesPerSecond = progress.bytesPerSecond;
    entry.receivedBytes = progress.receivedBytes;
    entry.connections = progress.connections;
    entry.retrying = progress.lastRetry;
    entry.unsaved = true;
    const auto now = ports_.clock->Steady();
    // receivedBytes is the run's running total; only the growth since the last report is new.
    quota_.Record(now, progress.receivedBytes - entry.quotaCountedBytes, QuotaLength());
    entry.quotaCountedBytes = progress.receivedBytes;
    if (now - entry.lastCheckpoint >= config_.checkpointInterval) {
        Save(entry);
    }
    MarkChanged(entry);
    EnforceQuota();
}

void Coordinator::OnFinished(Entry& entry, const TransferFinished& finished)
{
    if (!entry.running) {
        return;
    }
    model::Download& d = entry.download;
    entry.running = false;
    entry.stopRequested = false;
    entry.bytesPerSecond = 0;
    entry.connections.clear();
    entry.retrying.reset();
    d.segments = finished.segments;
    entry.receivedBytes = d.DownloadedBytes();
    if (finished.totalSize) {
        d.totalSize = finished.totalSize;
    }
    MarkChanged(entry);
    OnRunEnded(entry, finished.outcome, finished.failure);
}

void Coordinator::OnRunEnded(Entry& entry, TransferOutcome outcome, const std::optional<TransferFailure>& reportedFailure)
{
    model::Download& d = entry.download;
    if (entry.deleteWhenStopped) {
        RemoveEntry(d.id, false);
        UpdatePowerRequest();
        return;
    }
    if (entry.restartWhenStopped) {
        entry.restartWhenStopped = false;
        Redownload(d.id);
        UpdatePowerRequest();
        return;
    }
    if (entry.syncCheck && EndSyncCheck(entry, outcome, reportedFailure)) {
        UpdatePowerRequest();
        return;
    }
    if (entry.resumeWhenStopped && outcome != TransferOutcome::Completed) {
        // The old run ended after a new address arrived: continue from the new one.
        entry.resumeWhenStopped = false;
        d.status = DownloadStatus::Paused;
        Save(entry);
        Start(entry);
        UpdatePowerRequest();
        return;
    }
    entry.resumeWhenStopped = false;
    if (entry.finishRecording && outcome != TransferOutcome::Completed && !shuttingDown_) {
        entry.finishRecording = false;
        FinishRecordingNow(entry);
        UpdatePowerRequest();
        return;
    }
    entry.finishRecording = false;
    if (entry.recordingFailure && outcome != TransferOutcome::Completed && !shuttingDown_) {
        const TransferFailure failure = *std::exchange(entry.recordingFailure, std::nullopt);
        Fail(entry, failure);
        UpdatePowerRequest();
        return;
    }
    entry.recordingFailure.reset();

    switch (outcome) {
    case TransferOutcome::Completed:
        StopWaitingForAddress(d.id);
        if (d.awaitingConfirmation) {
            entry.holdFinalize = true;  // the user may still rename it in the open info dialog
            d.status = DownloadStatus::Finalizing;
            Save(entry);
        } else {
            CompleteRun(entry);
        }
        break;
    case TransferOutcome::Stopped:
    case TransferOutcome::Unchanged:  // only for a synchronization check, which ended above
        d.status = DownloadStatus::Paused;
        Save(entry);
        break;
    case TransferOutcome::Failed: {
        const TransferFailure failure = reportedFailure.value_or(
            TransferFailure{FailureReason::Internal, FailureClass::Fatal, 0, "the transfer failed without a reason"});
        if (shuttingDown_ || d.status == DownloadStatus::Stopping) {
            d.status = DownloadStatus::Paused;
            d.lastResult = DescribeFailure(failure);
            Save(entry);
            break;
        }
        const bool restartable =
            failure.reason == FailureReason::InconsistentContent || failure.reason == FailureReason::PartialFileMissing;
        if (restartable && entry.automaticRestarts < config_.maxAutomaticRestarts) {
            // Never mix bytes from two different answers: start over, with one connection when
            // the server served different data per connection (ENG-03).
            entry.automaticRestarts += 1;
            if (failure.reason == FailureReason::InconsistentContent) {
                entry.forcedMaxConnections = 1;
            }
            ResetProgress(entry);
            Start(entry);
            break;
        }
        Fail(entry, failure);
        break;
    }
    }
    UpdatePowerRequest();
}

void Coordinator::OnFinalized(Entry& entry, const FinalizeFinished& finalized)
{
    if (!entry.finalizing) {
        return;
    }
    entry.finalizing = false;
    model::Download& d = entry.download;
    if (finalized.failure) {
        // Another program may have created the final name meanwhile: pick a free one and retry.
        if (!entry.deleteWhenStopped && ports_.fileSystem->Exists(FinalPath(d)) &&
            entry.finalizeAttempts < kMaxFinalizeAttempts) {
            if (auto unique = UniqueFileName(PathFromUtf8(d.saveDir), d.fileName, d.id)) {
                d.fileName = std::move(*unique);
                BeginFinalize(entry);
                return;
            }
        }
        if (entry.deleteWhenStopped) {
            RemoveEntry(d.id, false);
            UpdatePowerRequest();
            return;
        }
        Fail(entry, *finalized.failure);
        UpdatePowerRequest();
        return;
    }
    d.status = DownloadStatus::Completed;
    d.completedAtMs = ports_.clock->NowUnixMs();
    d.partialPath.reset();
    entry.muxOutput.reset();
    d.cookieSecret.clear();  // design decision 3: browser cookies and form data are kept only until completion
    d.postSecret.clear();
    d.postContentType.reset();
    entry.replaceFinalFile = false;
    entry.finalizeAttempts = 0;
    entry.automaticRestarts = 0;
    entry.forcedMaxConnections.reset();
    LeaveQueueOnCompletion(entry);
    Save(entry);
    MarkChanged(entry);
    UpdatePowerRequest();
    if (entry.deleteWhenStopped) {
        RemoveEntry(d.id, entry.deleteFilesWhenStopped);
        return;
    }
    DownloadCompleted completed;
    completed.id = d.id;
    completed.fileName = d.fileName;
    completed.finalPath = PathToUtf8(FinalPath(d));
    completed.url = d.url;
    completed.size = d.totalSize;
    completed.completionAction = d.completionAction;
    Notify(std::move(completed));
}

// ---------------------------------------------------------------------------------------------
// Lifecycle helpers

void Coordinator::Start(Entry& entry)
{
    model::Download& d = entry.download;
    if (QuotaBlocks(ports_.clock->Steady())) {
        HoldForQuota(entry);
        d.status = DownloadStatus::Paused;
        Save(entry);
        return;
    }
    entry.quotaHeld = false;
    entry.listedThisRun = false;
    entry.nextPlaylistReload.reset();
    entry.pendingReloads = 0;
    entry.reloadChanged = false;
    entry.reloadFailure.reset();
    entry.reloadFailures = 0;
    entry.recordingFailure.reset();
    entry.finishRecording = false;
    d.epoch += 1;
    d.status = DownloadStatus::Downloading;
    d.lastTryAtMs = ports_.clock->NowUnixMs();
    d.lastResult.reset();
    if (!d.partialPath) {
        // Next to the destination by default, so completion is an instant rename (design section 4).
        const bool useTemporary = !settings_.temporaryDirectory.empty() &&
                                  PathFromUtf8(settings_.temporaryDirectory).is_absolute();
        const std::filesystem::path directory =
            useTemporary ? PathFromUtf8(settings_.temporaryDirectory) : PathFromUtf8(d.saveDir);
        const std::string stem = useTemporary ? d.publicId : d.fileName;
        // A stream's pieces go into a folder of their own.
        const std::string extension = d.IsStream() ? std::string(kPartsFolderExtension) : std::string(".ldmpart");
        std::filesystem::path partial = directory / PathFromUtf8(stem + extension);
        for (int n = 2; (IsReserved(partial, d.id) || ports_.fileSystem->Exists(partial)) && n <= kMaxNumberedNames; ++n) {
            partial = directory / PathFromUtf8(stem + "." + std::to_string(n) + extension);
        }
        d.partialPath = PathToUtf8(partial);
    }
    if (base::Status created = ports_.fileSystem->CreateDirectories(PathFromUtf8(*d.partialPath).parent_path()); !created) {
        Fail(entry, TransferFailure{FailureReason::PermissionDenied, FailureClass::NeedsUser, 0,
                                    "the folder could not be created: " + created.GetError().message});
        return;
    }
    Save(entry);  // the epoch is stored before the engine can report anything for it
    entry.quotaCountedBytes = d.DownloadedBytes();  // bytes from earlier runs were counted then
    entry.runStartBytes = d.DownloadedBytes();
    entry.running = true;
    entry.stopRequested = false;
    MarkChanged(entry);
    UpdatePowerRequest();
    const ProxyLookup lookup = ports_.proxy->Resolve(d.id, d.epoch, d.url);
    if (lookup.pending) {
        entry.awaitingProxy = true;  // shows as connecting; OnProxyResolved starts the engine
        return;
    }
    StartEngine(entry, lookup.proxy);
}

void Coordinator::StartEngine(Entry& entry, std::optional<std::string> proxy)
{
    if (entry.download.IsStream()) {
        entry.runProxy = std::move(proxy);
        StartStream(entry);
        return;
    }
    TransferRequest request = BuildRequest(entry);
    request.proxy = std::move(proxy);
    ports_.engine->Start(std::move(request));
}

void Coordinator::OnProxyResolved(Entry& entry, const ProxyResolved& resolved)
{
    if (!entry.awaitingProxy || !entry.running) {
        return;
    }
    entry.awaitingProxy = false;
    StartEngine(entry, resolved.proxy);
}

TransferRequest Coordinator::BuildRequest(Entry& entry)
{
    const model::Download& d = entry.download;
    TransferRequest request;
    request.id = d.id;
    request.epoch = d.epoch;
    request.url = d.url;
    const auto url = ParseUrl(d.url);
    request.referer = d.referer ? d.referer : url ? std::optional<std::string>(url->SiteRoot()) : std::nullopt;
    request.userAgent = d.userAgent.value_or(settings_.userAgent);

    if (d.loginUser) {
        TransferCredentials credentials{*d.loginUser, {}};
        if (!d.loginSecret.empty()) {
            if (auto password = ports_.secrets->Unprotect(d.loginSecret)) {
                credentials.password = std::move(password).Value();
            }
        }
        request.credentials = std::move(credentials);
    } else if (url) {
        if (const model::SiteLogin* login = MatchSiteLogin(siteLogins_, *url)) {
            if (auto password = ports_.secrets->Unprotect(login->secret)) {
                request.credentials = TransferCredentials{login->username, std::move(password).Value()};
            }
        }
    }
    if (!d.cookieSecret.empty()) {
        if (auto cookies = ports_.secrets->Unprotect(d.cookieSecret)) {
            request.cookieLines = model::CookieLines(cookies.Value());
        }
    }
    if (!d.postSecret.empty()) {
        if (auto body = ports_.secrets->Unprotect(d.postSecret)) {
            request.post = PostData{d.postContentType.value_or("application/x-www-form-urlencoded"), std::move(body).Value()};
        }
    }
    request.ftpActiveMode = !settings_.ftpPassive;
    request.partialPath = PathFromUtf8(d.partialPath.value_or(std::string()));
    request.knownSize = d.totalSize;
    request.resumable = d.resumable;
    request.validators = d.validators;
    if (entry.syncCheck) {
        // A fresh run that asks whether the completed file changed.
        request.skipIfUnchanged = true;
        request.knownSize = entry.syncCheck->totalSize;
        request.validators = entry.syncCheck->validators;
    }
    request.knownContentType = d.contentType;
    request.ignoreLastModified = settings_.ignoreModificationTimeChanges;
    request.segments = d.segments;
    request.maxConnections = ConnectionsFor(entry, url, request.post.has_value());
    request.speedLimitBytesPerSecond = d.speedLimitBytesPerSecond;
    request.maxAttemptsPerSegment = config_.maxAttemptsPerSegment;
    return request;
}

int Coordinator::ConnectionsFor(const Entry& entry, const std::optional<Url>& url, bool post) const
{
    int connections = entry.download.maxConnections.value_or(settings_.defaultMaxConnections);
    if (url) {
        if (const auto serverLimit = MatchServerLimit(serverLimits_, url->host)) {
            connections = std::min(connections, *serverLimit);
        }
    }
    if (entry.forcedMaxConnections) {
        connections = std::min(connections, *entry.forcedMaxConnections);
    }
    if (post) {
        connections = 1;
    }
    return std::clamp(connections, 1, kMaxConnections);
}

void Coordinator::RequestStop(Entry& entry)
{
    if (!entry.running || entry.stopRequested) {
        return;
    }
    if (entry.awaitingProxy || entry.pendingPlaylists > 0) {
        // The engine never started this run: finish it here, as the engine would have, so delete,
        // restart and shutdown follow their usual path. `entry` may be removed by this call.
        // Answers to playlist fetches still under way are dropped when they come.
        entry.awaitingProxy = false;
        if (entry.download.IsStream()) {
            std::erase_if(playlistFetches_, [&entry](const auto& fetch) { return fetch.second.id == entry.download.id; });
            entry.pendingPlaylists = 0;
            MediaFinished stopped;
            stopped.outcome = TransferOutcome::Stopped;
            OnMediaFinished(entry, stopped);
            return;
        }
        TransferFinished stopped;
        stopped.outcome = TransferOutcome::Stopped;
        stopped.segments = entry.download.segments;
        stopped.totalSize = entry.download.totalSize;
        OnFinished(entry, stopped);
        return;
    }
    entry.stopRequested = true;
    if (entry.download.status == DownloadStatus::Downloading) {
        entry.download.status = DownloadStatus::Stopping;
    }
    ports_.engine->Stop(entry.download.id, entry.download.epoch);
    MarkChanged(entry);
}

void Coordinator::ResetProgress(Entry& entry)
{
    model::Download& d = entry.download;
    if (d.IsStream()) {
        if (d.partialPath) {
            if (base::Status removed = ports_.fileSystem->RemoveFolder(PartsFolder(d)); !removed) {
                ReportError("The downloaded parts could not be deleted: " + removed.GetError().message);
            }
        }
        StoreTracks(entry, ResetMediaProgress(d));
        entry.unsavedPieces.clear();
        d.totalSize.reset();
        entry.receivedBytes = 0;
        MarkChanged(entry);
        return;
    }
    if (d.partialPath) {
        if (base::Status removed = ports_.fileSystem->Remove(PathFromUtf8(*d.partialPath)); !removed) {
            ReportError("A partial file could not be deleted: " + removed.GetError().message);
        }
    }
    d.segments.clear();
    d.totalSize.reset();
    d.resumable.reset();
    d.validators = {};
    d.finalUrl.reset();
    entry.receivedBytes = 0;
    MarkChanged(entry);
}

void Coordinator::RemoveEntry(model::DownloadId id, bool deleteFinalFile)
{
    Entry* entry = Find(id);
    if (entry == nullptr) {
        return;
    }
    if (waitingForAddress_ == id) {
        waitingForAddress_.reset();
    }
    for (auto& [queueId, run] : queueRuns_) {
        run.stoppedByUser.erase(id);  // row ids can be used again by a later download
    }
    const model::Download& d = entry->download;
    if (d.partialPath) {
        const std::filesystem::path partial = PathFromUtf8(*d.partialPath);
        const base::Status removed =
            d.IsStream() ? ports_.fileSystem->RemoveFolder(partial) : ports_.fileSystem->Remove(partial);
        if (!removed) {
            ReportError("A partial file could not be deleted: " + removed.GetError().message);
        }
    }
    if (deleteFinalFile) {
        // Completed files go to the Recycle Bin, so a mistaken delete can be undone.
        if (base::Status recycled = ports_.fileSystem->Recycle(FinalPath(d)); !recycled) {
            ReportError("The file was kept: " + recycled.GetError().message);
        }
    }
    if (base::Status deleted = ports_.store->DeleteDownload(id); !deleted) {
        ReportError("The download could not be removed from the list: " + deleted.GetError().message);
    }
    entries_.erase(id);
    snapshotDirty_ = true;
}

void Coordinator::BeginFinalize(Entry& entry)
{
    model::Download& d = entry.download;
    d.status = DownloadStatus::Finalizing;
    const std::filesystem::path target = FinalPath(d);
    if (entry.replaceFinalFile && ports_.fileSystem->Exists(target)) {
        if (base::Status removed = ports_.fileSystem->Remove(target); !removed) {
            Fail(entry, TransferFailure{FailureReason::PermissionDenied, FailureClass::NeedsUser, 0,
                                        "the existing file could not be replaced: " + removed.GetError().message});
            return;
        }
    }
    entry.finalizeAttempts += 1;
    entry.finalizing = true;
    Save(entry);
    MarkChanged(entry);
    FinalizeRequest request;
    request.id = d.id;
    request.epoch = d.epoch;
    request.partialPath = PathFromUtf8(d.partialPath.value_or(std::string()));
    request.finalPath = target;
    if (d.IsStream()) {
        // The joined file is moved; the folder of pieces goes once it is in place.
        request.partialPath = entry.muxOutput.value_or(std::filesystem::path());
        request.removeFolder = PartsFolder(d);
    } else if (settings_.setFileDateFromServer) {
        request.lastModified = d.validators.lastModified;
    }
    ports_.engine->Finalize(std::move(request));
}

void Coordinator::Fail(Entry& entry, const TransferFailure& failure)
{
    model::Download& d = entry.download;
    d.status = DownloadStatus::Failed;
    d.lastResult = DescribeFailure(failure);
    CountQueueFailure(entry);
    Save(entry);
    MarkChanged(entry);
    Notify(DownloadFailed{d.id, d.fileName, failure});
}

// ---------------------------------------------------------------------------------------------
// Streams (M3 design section 6)

std::optional<std::string> Coordinator::ValidateMedia(const MediaChoice& media, model::Download& download)
{
    if (media.kind != model::DownloadKind::Hls && media.kind != model::DownloadKind::Dash) {
        return "Only HLS and DASH streams can be downloaded.";
    }
    if (media.tracks.empty() || media.tracks.size() > kMaxMediaTracks) {
        return "A stream download needs between 1 and " + std::to_string(kMaxMediaTracks) + " tracks.";
    }
    const auto clip = [](const std::string& text) { return text.substr(0, 200); };
    download.kind = media.kind;
    download.mediaFromPrivateNetwork = media.fromPrivateNetwork;
    download.tracks.clear();
    for (const MediaTrackChoice& choice : media.tracks) {
        const auto source = ParseUrl(choice.sourceUrl);
        if (!source || (source->scheme != "http" && source->scheme != "https")) {
            return "A track of the stream has no valid http or https address.";
        }
        if (media.kind == model::DownloadKind::Dash && !choice.segments) {
            return "A DASH track needs its segment list.";
        }
        model::MediaTrack track;
        track.role = choice.role;
        track.sourceUrl = choice.sourceUrl;
        track.codecs = clip(choice.codecs);
        track.language = clip(choice.language);
        track.name = clip(choice.name);
        if (choice.segments) {
            if (choice.segments->drm) {
                return "The stream is protected (DRM), which LDM does not download.";
            }
            if (choice.segments->live) {
                return "LDM records live HLS streams only, and this is a live DASH stream.";
            }
            if (choice.segments->segments.empty() || choice.segments->segments.size() > media::kMaxSegmentsPerTrack) {
                return "A track of the stream lists no segments, or too many.";
            }
            FillTrack(track, *choice.segments);
        }
        download.tracks.push_back(std::move(track));
    }
    const auto manifest = ParseUrl(download.url);
    if (!media.fromPrivateNetwork && manifest && IsPrivateHost(manifest->host)) {
        download.mediaFromPrivateNetwork = true;  // the manifest's own host is private: so may its pieces be
    }
    if (const auto refused = CheckPrivateAddresses(download)) {
        return "The stream comes from the internet but points into your own network, which LDM refuses.";
    }
    return std::nullopt;
}

void Coordinator::StartStream(Entry& entry)
{
    model::Download& d = entry.download;
    const std::vector<std::uint32_t> unlisted = UnlistedTracks(d);
    if (!unlisted.empty()) {
        entry.playlistFailure.reset();
        entry.pendingPlaylists = static_cast<int>(unlisted.size());
        entry.listedThisRun = true;
        for (const std::uint32_t track : unlisted) {
            FetchPlaylist(entry, track, PlaylistPurpose::Initial);
        }
        return;
    }
    if (!entry.listedThisRun && IsRecording(d)) {
        // A recording that continues after a pause: what it listed then has left the stream's
        // window by now. The first reload, at once, picks it up at the live edge.
        std::vector<std::uint32_t> changed;
        for (std::uint32_t t = 0; t < d.tracks.size(); ++t) {
            if (d.tracks[t].live && SkipUnfetchedPieces(d.tracks[t])) {
                changed.push_back(t);
            }
        }
        StoreTracks(entry, changed);
    }
    if (const auto refused = CheckPrivateAddresses(d)) {
        MediaFinished failed;
        failed.failure = refused;
        OnMediaFinished(entry, failed);
        return;
    }
    MediaRequest request = BuildMediaRequest(d, PartsFolder(d));
    request.referer = d.referer;
    request.userAgent = d.userAgent.value_or(settings_.userAgent);
    request.cookieLines = StreamCookieLines(d);
    request.proxy = entry.runProxy;
    request.maxConnections = ConnectionsFor(entry, ParseUrl(d.url), false);
    request.speedLimitBytesPerSecond = d.speedLimitBytesPerSecond;
    request.maxAttemptsPerSegment = config_.maxAttemptsPerSegment;
    ports_.engine->StartMedia(std::move(request));
    if (IsRecording(d)) {
        const auto now = ports_.clock->Steady();
        entry.nextPlaylistReload = entry.listedThisRun ? now + ReloadInterval(d, true) : now;
    }
}

std::vector<std::string> Coordinator::StreamCookieLines(const model::Download& download)
{
    if (download.cookieSecret.empty()) {
        return {};
    }
    auto cookies = ports_.secrets->Unprotect(download.cookieSecret);
    return cookies ? model::CookieLines(cookies.Value()) : std::vector<std::string>{};
}

void Coordinator::FetchPlaylist(Entry& entry, std::uint32_t track, PlaylistPurpose purpose)
{
    const model::Download& d = entry.download;
    FetchRequest fetch;
    fetch.fetchId = --lastPlaylistFetchId_;
    fetch.url = d.tracks[track].sourceUrl;
    fetch.referer = d.referer;
    fetch.userAgent = d.userAgent.value_or(settings_.userAgent);
    fetch.cookieLines = StreamCookieLines(d);
    fetch.proxy = entry.runProxy;
    fetch.maxBytes = media::kMaxManifestBytes;
    fetch.refusePrivateAddresses = !d.mediaFromPrivateNetwork;
    playlistFetches_.emplace(fetch.fetchId, PlaylistFetch{d.id, d.epoch, track, purpose});
    ports_.engine->Fetch(std::move(fetch));
}

void Coordinator::ReloadPlaylists(Entry& entry)
{
    entry.nextPlaylistReload.reset();
    const model::Download& d = entry.download;
    for (std::uint32_t t = 0; t < d.tracks.size(); ++t) {
        if (d.tracks[t].live) {
            entry.pendingReloads += 1;
            FetchPlaylist(entry, t, PlaylistPurpose::Reload);
        }
    }
}

void Coordinator::OnPlaylistReloaded(Entry& entry, std::uint32_t track, const FetchFinished& fetched)
{
    if (entry.pendingReloads <= 0) {
        return;
    }
    entry.pendingReloads -= 1;
    model::Download& d = entry.download;
    // A track finished meanwhile takes nothing more.
    if (!entry.stopRequested && track < d.tracks.size() && d.tracks[track].live) {
        if (fetched.failure) {
            entry.reloadFailure = fetched.failure;
        } else if (TrackReload reload = ReloadTrack(d.tracks[track], fetched); reload.failure) {
            entry.reloadFailure = std::move(reload.failure);
        } else if (reload.append.changed) {
            entry.reloadChanged = true;
            if (base::Status stored = ports_.store->AppendMediaTrack(d.id, track, d.tracks[track],
                                                                     reload.append.firstNewKey,
                                                                     reload.append.firstNewPiece);
                !stored) {
                ReportError("Download state could not be saved: " + stored.GetError().message);
            }
            MarkChanged(entry);
            if (auto refused = CheckPrivateAddresses(d)) {
                entry.recordingFailure = std::move(refused);
            } else {
                MediaExtension extension;
                extension.tracks.push_back(BuildTrackExtension(d, PartsFolder(d), track, reload.append));
                ports_.engine->ExtendMedia(d.id, d.epoch, std::move(extension));
            }
        }
    }
    if (entry.pendingReloads > 0) {
        return;
    }
    // Every playlist of this reload answered.
    const bool changed = std::exchange(entry.reloadChanged, false);
    const std::optional<TransferFailure> failure = std::exchange(entry.reloadFailure, std::nullopt);
    if (entry.stopRequested) {
        return;
    }
    if (failure && !entry.recordingFailure) {
        entry.reloadFailures += 1;
        if (entry.reloadFailures >= kMaxPlaylistReloadFailures) {
            entry.recordingFailure = failure;
        }
    } else if (!failure) {
        entry.reloadFailures = 0;
    }
    if (entry.recordingFailure) {
        RequestStop(entry);
        return;
    }
    if (IsRecording(d)) {
        entry.nextPlaylistReload = ports_.clock->Steady() + ReloadInterval(d, changed);
    }
}

void Coordinator::FinishRecordingNow(Entry& entry)
{
    model::Download& d = entry.download;
    if (!HasRecordedPieces(d)) {
        // Nothing to join: it stays a recording that Resume continues.
        d.status = DownloadStatus::Paused;
        d.lastResult = "Nothing was recorded yet";
        Save(entry);
        MarkChanged(entry);
        return;
    }
    std::vector<std::uint32_t> changed;
    for (std::uint32_t t = 0; t < d.tracks.size(); ++t) {
        model::MediaTrack& track = d.tracks[t];
        bool trackChanged = !track.wholeFile && SkipUnfetchedPieces(track);
        if (track.live) {
            track.live = false;
            trackChanged = true;
        }
        if (trackChanged) {
            changed.push_back(t);
        }
    }
    StoreTracks(entry, changed);
    d.lastResult.reset();
    MarkChanged(entry);
    if (d.awaitingConfirmation) {
        entry.holdFinalize = true;  // the user may still rename it in the open info dialog
        d.status = DownloadStatus::Finalizing;
        Save(entry);
        return;
    }
    CompleteRun(entry);
}

void Coordinator::OnPlaylistFetched(const FetchFinished& fetched)
{
    const auto it = playlistFetches_.find(fetched.fetchId);
    if (it == playlistFetches_.end()) {
        return;  // its run was stopped meanwhile
    }
    const PlaylistFetch fetch = it->second;
    playlistFetches_.erase(it);
    Entry* entry = Find(fetch.id);
    if (entry == nullptr || entry->download.epoch != fetch.epoch || !entry->running) {
        return;
    }
    if (fetch.purpose == PlaylistPurpose::Reload) {
        OnPlaylistReloaded(*entry, fetch.track, fetched);
        return;
    }
    if (entry->pendingPlaylists <= 0) {
        return;
    }
    entry->pendingPlaylists -= 1;
    if (fetched.failure) {
        if (!entry->playlistFailure) {
            entry->playlistFailure = fetched.failure;
        }
    } else if (!entry->playlistFailure && fetch.track < entry->download.tracks.size()) {
        if (auto failure = ListTrack(entry->download.tracks[fetch.track], fetched)) {
            entry->playlistFailure = std::move(failure);
        } else {
            StoreTracks(*entry, {fetch.track});
            MarkChanged(*entry);
        }
    }
    if (entry->pendingPlaylists > 0) {
        return;
    }
    if (entry->playlistFailure) {
        MediaFinished failed;
        failed.failure = entry->playlistFailure;
        entry->playlistFailure.reset();
        OnMediaFinished(*entry, failed);
        return;
    }
    StartStream(*entry);
}

void Coordinator::OnMediaProgress(Entry& entry, const MediaProgress& progress)
{
    if (!entry.running) {
        return;
    }
    model::Download& d = entry.download;
    const std::vector<MediaPieceDone> applied = ApplyPiecesDone(d, progress.done);
    entry.unsavedPieces.insert(entry.unsavedPieces.end(), applied.begin(), applied.end());
    ApplyRanges(d, progress.ranges);
    entry.bytesPerSecond = progress.bytesPerSecond;
    entry.receivedBytes = entry.runStartBytes + progress.receivedBytes;
    // A stream's connections have no byte ranges to show; the table lists how many there are.
    entry.connections.assign(static_cast<size_t>(std::max(progress.connections, 0)), ConnectionView{});
    entry.retrying = progress.lastRetry;
    entry.unsaved = true;
    const auto now = ports_.clock->Steady();
    quota_.Record(now, entry.receivedBytes - entry.quotaCountedBytes, QuotaLength());
    entry.quotaCountedBytes = entry.receivedBytes;
    if (now - entry.lastCheckpoint >= config_.checkpointInterval) {
        Save(entry);
    }
    MarkChanged(entry);
    EnforceQuota();
}

void Coordinator::OnMediaFinished(Entry& entry, const MediaFinished& finished)
{
    if (!entry.running) {
        return;
    }
    model::Download& d = entry.download;
    entry.running = false;
    entry.stopRequested = false;
    entry.bytesPerSecond = 0;
    entry.connections.clear();
    entry.retrying.reset();
    entry.nextPlaylistReload.reset();
    entry.pendingReloads = 0;  // answers still under way find the run gone
    const std::vector<MediaPieceDone> applied = ApplyPiecesDone(d, finished.done);
    entry.unsavedPieces.insert(entry.unsavedPieces.end(), applied.begin(), applied.end());
    ApplyRanges(d, finished.ranges);
    entry.receivedBytes = d.DownloadedBytes();
    entry.unsaved = true;
    MarkChanged(entry);
    OnRunEnded(entry, finished.outcome, finished.failure);
}

void Coordinator::CompleteRun(Entry& entry)
{
    if (entry.download.IsStream()) {
        BeginMux(entry);
    } else if (ports_.media != nullptr && IsDescribedMediaFile(entry.download.fileName)) {
        BeginDescribe(entry);
    } else {
        BeginFinalize(entry);
    }
}

void Coordinator::BeginDescribe(Entry& entry)
{
    model::Download& d = entry.download;
    d.status = DownloadStatus::Finalizing;
    entry.finalizing = true;
    entry.describing = true;
    entry.stopRequested = false;
    Save(entry);
    MarkChanged(entry);
    UpdatePowerRequest();
    ports_.media->Describe(DescribeRequest{d.id, d.epoch, PathFromUtf8(d.partialPath.value_or(std::string()))});
}

void Coordinator::OnDescribeFinished(Entry& entry, const DescribeFinished& finished)
{
    if (!entry.describing) {
        return;
    }
    model::Download& d = entry.download;
    const bool stopRequested = entry.stopRequested;
    entry.describing = false;
    entry.finalizing = false;
    entry.stopRequested = false;
    if (entry.deleteWhenStopped) {
        RemoveEntry(d.id, false);
        UpdatePowerRequest();
        return;
    }
    if (finished.stopped || stopRequested) {
        d.status = DownloadStatus::Paused;  // the file is complete; Resume describes and moves it
        Save(entry);
        MarkChanged(entry);
        UpdatePowerRequest();
        return;
    }
    if (!finished.mediaInfo.empty()) {
        d.description = d.description.empty() ? finished.mediaInfo : d.description + "; " + finished.mediaInfo;
    }
    BeginFinalize(entry);
}

void Coordinator::BeginMux(Entry& entry)
{
    model::Download& d = entry.download;
    if (ports_.media == nullptr) {
        Fail(entry, TransferFailure{FailureReason::Internal, FailureClass::Fatal, 0, "LDM's media tools are not available"});
        return;
    }
    d.status = DownloadStatus::Finalizing;
    entry.finalizing = true;
    entry.muxing = true;
    entry.stopRequested = false;
    Save(entry);
    MarkChanged(entry);
    UpdatePowerRequest();
    ports_.media->Mux(BuildMuxRequest(d, PartsFolder(d)));
}

void Coordinator::CancelMediaTools(Entry& entry)
{
    if ((!entry.muxing && !entry.describing) || ports_.media == nullptr) {
        return;
    }
    entry.stopRequested = true;
    ports_.media->Cancel(entry.download.id, entry.download.epoch);
}

void Coordinator::OnMuxFinished(Entry& entry, const MuxFinished& finished)
{
    if (!entry.muxing) {
        return;
    }
    model::Download& d = entry.download;
    const bool stopRequested = entry.stopRequested;
    entry.muxing = false;
    entry.finalizing = false;
    entry.stopRequested = false;
    if (entry.deleteWhenStopped) {
        RemoveEntry(d.id, false);
        UpdatePowerRequest();
        return;
    }
    if (finished.stopped || stopRequested) {
        d.status = DownloadStatus::Paused;  // every piece is still there; Resume joins them again
        Save(entry);
        MarkChanged(entry);
        UpdatePowerRequest();
        return;
    }
    if (finished.failure) {
        Fail(entry, *finished.failure);
        UpdatePowerRequest();
        return;
    }
    entry.muxOutput = finished.output;
    d.totalSize = finished.bytes;
    std::string extension = FileExtension(d.fileName);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (!finished.extension.empty() && extension != finished.extension) {
        // MP4 could not hold the streams, so the file is Matroska: its name says so.
        if (auto unique = UniqueFileName(PathFromUtf8(d.saveDir), WithExtension(d.fileName, finished.extension), d.id)) {
            d.fileName = std::move(*unique);
        }
    }
    if (!finished.mediaInfo.empty()) {
        d.description = d.description.empty() ? finished.mediaInfo : d.description + "; " + finished.mediaInfo;
    }
    BeginFinalize(entry);
}

void Coordinator::StoreTracks(Entry& entry, const std::vector<std::uint32_t>& tracks)
{
    const model::Download& d = entry.download;
    for (const std::uint32_t track : tracks) {
        if (track >= d.tracks.size()) {
            continue;
        }
        if (base::Status stored = ports_.store->ReplaceMediaTrack(d.id, track, d.tracks[track]); !stored) {
            ReportError("Download state could not be saved: " + stored.GetError().message);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Names and paths

std::filesystem::path Coordinator::FinalPath(const model::Download& download) const
{
    return PathFromUtf8(download.saveDir) / PathFromUtf8(download.fileName);
}

bool Coordinator::IsReserved(const std::filesystem::path& path, std::optional<model::DownloadId> except) const
{
    const std::wstring key = base::PathComparisonKey(path);
    for (const auto& [id, entry] : entries_) {
        if (except && id == *except) {
            continue;
        }
        const model::Download& d = entry.download;
        // A download being deleted still owns its partial file until the engine lets go of it,
        // but its final name is free again.
        if (!entry.deleteWhenStopped && base::PathComparisonKey(FinalPath(d)) == key) {
            return true;
        }
        if (d.partialPath && base::PathComparisonKey(PathFromUtf8(*d.partialPath)) == key) {
            return true;
        }
    }
    return false;
}

std::optional<std::string> Coordinator::UniqueFileName(const std::filesystem::path& directory, const std::string& name,
                                                       std::optional<model::DownloadId> except) const
{
    const auto isFree = [this, &directory, &except](const std::string& candidate) {
        const std::filesystem::path path = directory / PathFromUtf8(candidate);
        if (IsReserved(path, except)) {
            return false;
        }
        // The download's own existing file (its earlier final name) does not block it.
        bool ownFile = false;
        if (except) {
            const auto it = entries_.find(*except);
            ownFile = it != entries_.end() &&
                      base::PathComparisonKey(FinalPath(it->second.download)) == base::PathComparisonKey(path);
        }
        return ownFile || !ports_.fileSystem->Exists(path);
    };
    if (isFree(name)) {
        return name;
    }
    // "a (2).zip" taken: continue with "a (3).zip" rather than "a (2) (2).zip".
    const NumberedName numbered = SplitNumberedFileName(name);
    for (int n = numbered.number + 1; n <= numbered.number + kMaxNumberedNames; ++n) {
        std::string candidate = NumberedFileName(numbered.base, n);
        if (isFree(candidate)) {
            return candidate;
        }
    }
    return std::nullopt;
}

model::CategoryId Coordinator::CategoryFor(const std::string& fileName, const Url& url) const
{
    if (const auto matched = MatchCategory(categories_, fileName, url)) {
        return *matched;
    }
    const auto general = std::find_if(categories_.begin(), categories_.end(),
                                      [](const model::Category& c) { return c.builtin && c.extensions.empty(); });
    return general != categories_.end() ? general->id : categories_.front().id;
}

const model::Category* Coordinator::FindCategory(model::CategoryId id) const
{
    const auto it = std::find_if(categories_.begin(), categories_.end(), [&](const model::Category& c) { return c.id == id; });
    return it == categories_.end() ? nullptr : &*it;
}

std::string Coordinator::FolderFor(model::CategoryId id) const
{
    const model::Category* category = FindCategory(id);
    return category != nullptr ? category->folder : PathToUtf8(config_.downloadsFolder);
}

// ---------------------------------------------------------------------------------------------
// Persistence and publication

void Coordinator::Save(Entry& entry)
{
    entry.lastCheckpoint = ports_.clock->Steady();
    entry.unsaved = false;
    if (!entry.unsavedPieces.empty()) {
        // Pieces first: one stored as done is on disk whatever the row says, never the reverse.
        if (base::Status saved = ports_.store->MarkMediaPiecesDone(entry.download.id, entry.unsavedPieces); !saved) {
            ReportError("Download state could not be saved: " + saved.GetError().message);
        }
        entry.unsavedPieces.clear();
    }
    if (base::Status saved = ports_.store->SaveDownload(entry.download); !saved) {
        ReportError("Download state could not be saved: " + saved.GetError().message);
    }
}

void Coordinator::ReportError(std::string message)
{
    Notify(CoreError{std::move(message)});
}

void Coordinator::Notify(Notification notification)
{
    ports_.observer->OnNotification(std::move(notification));
}

void Coordinator::MarkChanged(Entry& entry)
{
    entry.viewDirty = true;
    snapshotDirty_ = true;
}

void Coordinator::Tick()
{
    const auto now = ports_.clock->Steady();
    for (auto& [id, entry] : entries_) {
        if (entry.unsaved && now - entry.lastCheckpoint >= config_.checkpointInterval) {
            Save(entry);
        }
        if (entry.running && !entry.stopRequested && entry.pendingReloads == 0 && entry.nextPlaylistReload &&
            now >= *entry.nextPlaylistReload) {
            ReloadPlaylists(entry);
        }
    }
    EnforceQuota();
    if (waitingForAddress_ && now - waitingSince_ >= kAddressWaitLimit) {
        StopWaitingForAddress(*waitingForAddress_);
    }
    if (ports_.timeZone != nullptr) {
        const std::int64_t nowUnixMs = ports_.clock->NowUnixMs();
        if (nowUnixMs < lastScheduleCheckUnixMs_ ||
            (nextScheduleEventUnixMs_ && nowUnixMs >= *nextScheduleEventUnixMs_)) {
            CheckSchedules();
        } else {
            // Nothing is planned before now, so nothing up to now can be due later.
            lastScheduleCheckUnixMs_ = nowUnixMs;
        }
    }
    RunQueues();
    if (snapshotDirty_ && now - lastSnapshot_ >= config_.snapshotInterval) {
        PublishSnapshot();
    }
}

std::chrono::steady_clock::time_point Coordinator::NextDeadline() const
{
    auto deadline = std::chrono::steady_clock::time_point::max();
    if (snapshotDirty_) {
        deadline = lastSnapshot_ + config_.snapshotInterval;
    }
    for (const auto& [id, entry] : entries_) {
        if (entry.unsaved) {
            deadline = std::min(deadline, entry.lastCheckpoint + config_.checkpointInterval);
        }
        if (entry.running && entry.pendingReloads == 0 && entry.nextPlaylistReload) {
            deadline = std::min(deadline, *entry.nextPlaylistReload);
        }
    }
    // Wake when the quota period ends, to restart held downloads, or when an override runs out.
    const auto now = ports_.clock->Steady();
    if (const auto end = quota_.End(now, QuotaLength()); end && settings_.quota.enabled) {
        deadline = std::min(deadline, *end);
    }
    if (quotaIgnoredUntil_) {
        deadline = std::min(deadline, *quotaIgnoredUntil_);
    }
    if (waitingForAddress_) {
        deadline = std::min(deadline, waitingSince_ + kAddressWaitLimit);
    }
    if (nextScheduleEventUnixMs_) {
        const std::int64_t wait = std::max<std::int64_t>(0, *nextScheduleEventUnixMs_ - ports_.clock->NowUnixMs());
        deadline = std::min(deadline, now + std::chrono::milliseconds(wait));
    }
    if (anyScheduleEnabled_) {
        // The steady clock may not count while the computer sleeps; look at the time again soon.
        deadline = std::min(deadline, now + kScheduleRecheck);
    }
    return deadline;
}

std::chrono::minutes Coordinator::QuotaLength() const
{
    return std::chrono::hours(std::max(1, settings_.quota.hours));
}

bool Coordinator::QuotaBlocks(std::chrono::steady_clock::time_point now) const
{
    if (!settings_.quota.enabled || shuttingDown_) {
        return false;
    }
    if (quotaIgnoredUntil_ && now < *quotaIgnoredUntil_) {
        return false;
    }
    return quota_.Used(now, QuotaLength()) >= std::int64_t{settings_.quota.megabytes} * 1024 * 1024;
}

void Coordinator::HoldForQuota(Entry& entry)
{
    entry.quotaHeld = true;
    entry.download.lastResult = "Waiting for the download quota; continues by itself when the quota period ends";
    MarkChanged(entry);
}

void Coordinator::StopForQuota()
{
    for (auto& [id, entry] : entries_) {
        if (entry.running && !entry.stopRequested) {
            HoldForQuota(entry);
            RequestStop(entry);
        }
    }
}

void Coordinator::EnforceQuota()
{
    if (shuttingDown_) {
        return;
    }
    const auto now = ports_.clock->Steady();
    if (quotaIgnoredUntil_ && now >= *quotaIgnoredUntil_) {
        quotaIgnoredUntil_.reset();
    }
    if (!QuotaBlocks(now)) {
        quotaQuestionOpen_ = false;
        for (auto& [id, entry] : entries_) {
            if (entry.quotaHeld && !entry.running && !entry.finalizing && !entry.deleteWhenStopped) {
                entry.quotaHeld = false;
                entry.download.lastResult.reset();
                if (CanStart(entry.download.status)) {
                    Start(entry);
                } else {
                    MarkChanged(entry);
                }
            }
        }
        return;
    }
    const bool anyRunning =
        std::any_of(entries_.begin(), entries_.end(), [](const auto& item) { return item.second.running && !item.second.stopRequested; });
    if (!anyRunning) {
        return;
    }
    if (!settings_.quota.warnBeforeStopping) {
        StopForQuota();
        return;
    }
    if (quotaQuestionOpen_) {
        return;  // the downloads run on until the user answers
    }
    quotaQuestionOpen_ = true;
    QuotaReached reached;
    reached.megabytes = settings_.quota.megabytes;
    reached.hours = settings_.quota.hours;
    if (const auto end = quota_.End(now, QuotaLength())) {
        reached.periodEndsAtUnixMs =
            ports_.clock->NowUnixMs() + std::chrono::duration_cast<std::chrono::milliseconds>(*end - now).count();
    }
    Notify(reached);
}

void Coordinator::DecideQuota(bool keepDownloading)
{
    if (!quotaQuestionOpen_) {
        return;  // the period ended, or the limit changed, before the answer came
    }
    quotaQuestionOpen_ = false;
    const auto now = ports_.clock->Steady();
    if (keepDownloading) {
        quotaIgnoredUntil_ = quota_.End(now, QuotaLength());
        EnforceQuota();
        return;
    }
    StopForQuota();
}

void Coordinator::PublishSnapshot()
{
    auto snapshot = std::make_shared<Snapshot>();
    snapshot->version = ++snapshotVersion_;
    std::vector<Entry*> ordered;
    ordered.reserve(entries_.size());
    for (auto& [id, entry] : entries_) {
        ordered.push_back(&entry);
    }
    std::sort(ordered.begin(), ordered.end(), [](const Entry* a, const Entry* b) {
        return std::pair(a->download.sortOrder, a->download.id.value) < std::pair(b->download.sortOrder, b->download.id.value);
    });
    snapshot->downloads.reserve(ordered.size());
    for (Entry* entry : ordered) {
        if (entry->viewDirty || !entry->view) {
            auto view = std::make_shared<DownloadView>();
            view->download = entry->download;
            view->download.loginSecret.clear();
            view->download.cookieSecret.clear();
            view->download.postSecret.clear();
            view->hasPassword = !entry->download.loginSecret.empty();
            view->awaitingConfirmation = entry->download.awaitingConfirmation;
            view->bytesPerSecond = entry->bytesPerSecond;
            view->receivedBytes = entry->running ? entry->receivedBytes : entry->download.DownloadedBytes();
            view->connections = entry->connections;
            view->retrying = entry->retrying;
            view->waitingForQuota = entry->quotaHeld;
            view->waitingForAddress = waitingForAddress_ == entry->download.id;
            view->waitingInQueue = IsWaitingInQueue(*entry);
            std::optional<std::int64_t> expectedSize = entry->download.totalSize;
            if (entry->download.IsStream()) {
                for (model::MediaTrack& track : view->download.tracks) {
                    track.pieces.clear();
                    track.keys.clear();
                }
                const StreamSummary summary = SummarizeStream(entry->download);
                DownloadView::Stream stream;
                stream.piecesDone = summary.piecesDone;
                stream.piecesTotal = summary.piecesTotal;
                stream.fraction = entry->download.status == DownloadStatus::Completed ? 1.0 : summary.fraction;
                stream.live = summary.live;
                stream.recordedUs = summary.recordedUs;
                // From what arrived once there is enough of it; the panel's estimate before that.
                // A recording has no end to estimate towards: it is as large as what it recorded.
                if (summary.live) {
                    stream.estimatedBytes = summary.bytesDone;
                } else if (summary.estimatedBytes) {
                    stream.estimatedBytes = summary.estimatedBytes;
                } else {
                    stream.estimatedBytes = entry->approximateBytes;
                }
                if (!expectedSize && !summary.live) {
                    expectedSize = stream.estimatedBytes;
                }
                view->stream = stream;
            }
            if (entry->bytesPerSecond > 0 && expectedSize) {
                view->secondsLeft = std::max<std::int64_t>(*expectedSize - view->receivedBytes, 0) / entry->bytesPerSecond;
            }
            entry->view = std::move(view);
            entry->viewDirty = false;
        }
        snapshot->downloads.push_back(entry->view);
        snapshot->totalBytesPerSecond += entry->bytesPerSecond;
        if (entry->running) {
            snapshot->activeDownloads += 1;
        }
    }
    snapshot->categories = categories_;
    std::sort(snapshot->categories.begin(), snapshot->categories.end(),
              [](const model::Category& a, const model::Category& b) { return a.sortOrder < b.sortOrder; });
    std::map<model::QueueId, QueueView> queueViews;
    for (const model::Queue& queue : queues_) {
        queueViews.emplace(queue.id, QueueView{queue, queueRuns_.contains(queue.id), 0, 0});
    }
    for (const auto& [id, entry] : entries_) {
        if (!entry.download.queueId) {
            continue;
        }
        if (const auto view = queueViews.find(*entry.download.queueId); view != queueViews.end()) {
            view->second.members += 1;
            view->second.runningMembers += DescribeMember(entry).busy ? 1 : 0;
        }
    }
    for (auto& [id, view] : queueViews) {
        snapshot->queues.push_back(std::move(view));
    }
    std::sort(snapshot->queues.begin(), snapshot->queues.end(), [](const QueueView& a, const QueueView& b) {
        return std::pair(a.queue.sortOrder, a.queue.id.value) < std::pair(b.queue.sortOrder, b.queue.id.value);
    });
    snapshot->settings = settings_;
    snapshot->serverLimits = serverLimits_;
    for (const model::SiteLogin& login : siteLogins_) {
        snapshot->siteLogins.push_back(SiteLoginView{login.pattern, login.username});
    }
    snapshot->speedLimiterOn = speedLimiterOn_;
    const auto now = ports_.clock->Steady();
    if (settings_.quota.enabled) {
        snapshot->quotaLimitBytes = std::int64_t{settings_.quota.megabytes} * 1024 * 1024;
        snapshot->quotaUsedBytes = quota_.Used(now, QuotaLength());
        const bool anyHeld =
            std::any_of(entries_.begin(), entries_.end(), [](const auto& item) { return item.second.quotaHeld; });
        if (const auto end = quota_.End(now, QuotaLength()); end && anyHeld) {
            snapshot->quotaEndsAtUnixMs =
                ports_.clock->NowUnixMs() + std::chrono::duration_cast<std::chrono::milliseconds>(*end - now).count();
        }
    }
    snapshotDirty_ = false;
    lastSnapshot_ = ports_.clock->Steady();
    ports_.observer->OnSnapshot(std::move(snapshot));
}

void Coordinator::UpdatePowerRequest()
{
    const bool active = std::any_of(entries_.begin(), entries_.end(),
                                    [](const auto& item) { return item.second.running || item.second.finalizing; });
    if (active != powerActive_) {
        powerActive_ = active;
        ports_.power->SetTransfersActive(active);
    }
}

void Coordinator::ApplySpeedLimiter()
{
    if (speedLimiterOn_) {
        ports_.engine->SetGlobalSpeedLimit(std::int64_t{settings_.speedLimiterKilobytesPerSecond} * 1024);
    } else {
        ports_.engine->SetGlobalSpeedLimit(std::nullopt);
    }
}

Coordinator::Entry* Coordinator::Find(model::DownloadId id)
{
    const auto it = entries_.find(id);
    return it == entries_.end() ? nullptr : &it->second;
}

// ---------------------------------------------------------------------------------------------
// Shutdown

void Coordinator::BeginShutdown()
{
    shuttingDown_ = true;
    queueRuns_.clear();  // every queue is stopped at the next start anyway; nothing to tell
    for (auto& [id, entry] : entries_) {
        if (entry.running) {
            RequestStop(entry);
        } else if (entry.muxing || entry.describing) {
            CancelMediaTools(entry);  // the pieces or the file stay; the next start finishes them again
        }
    }
}

bool Coordinator::ShutdownComplete() const
{
    return std::none_of(entries_.begin(), entries_.end(),
                        [](const auto& item) { return item.second.running || item.second.finalizing; });
}

void Coordinator::FinalCheckpoint()
{
    for (auto& [id, entry] : entries_) {
        model::Download& d = entry.download;
        if (d.status == DownloadStatus::Downloading || d.status == DownloadStatus::Stopping) {
            d.status = DownloadStatus::Paused;
            entry.unsaved = true;
        }
        if (entry.unsaved) {
            Save(entry);
        }
    }
    if (powerActive_) {
        powerActive_ = false;
        ports_.power->SetTransfersActive(false);
    }
}

}  // namespace ldm::core

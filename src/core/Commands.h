#pragma once

#include "media/MediaModel.h"
#include "model/Category.h"
#include "model/ConnectionRules.h"
#include "model/Cookie.h"
#include "model/Download.h"
#include "model/Ids.h"
#include "model/Media.h"
#include "model/Queue.h"
#include "model/Settings.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

// Requests to the coordinator. The UI, the command line and IPC all speak these; every field is
// validated by the coordinator, never trusted.
namespace ldm::core {

enum class AddMode {
    StartNow,
    Later,        // keep in the list without starting ("Download Later")
    Speculative,  // start at once while the user still edits the info dialog (checklist A-03)
};

// What to do when the target file already exists on disk (checklist A-06).
enum class ExistingFilePolicy {
    AutoRename,
    Replace,
    Cancel,
};

// The form submission a download came from (checklist ENG-11).
struct PostData {
    std::string contentType;
    std::string body;  // bytes, sent as is
};

// One track of a stream, as the user chose it in the media panel.
struct MediaTrackChoice {
    model::TrackRole role = model::TrackRole::Video;
    std::string sourceUrl;  // HLS: the media playlist; DASH: the MPD
    std::string codecs;
    std::string language;
    std::string name;
    // DASH: the track's list, which the MPD already gave. HLS: nothing; LDM reads the media
    // playlist when the download first starts.
    std::optional<media::TrackSegments> segments;
    // A site video's track: the resolver's format id and the exact length the site stated.
    std::string siteFormat;
    std::optional<std::int64_t> expectedBytes;
    // An HLS site format's parameters for its playlist's addresses (media::SiteFormat).
    std::string segmentQuery;
    std::string keyQuery;
};

// A stream to download instead of a file (M3 design section 6). The command's `url` is then the
// manifest's address and `fileName` the name of the file LDM makes from the tracks.
struct MediaChoice {
    model::DownloadKind kind = model::DownloadKind::Hls;  // Hls or Dash
    // The manifest came from a loopback or private address (FetchFinished::fromPrivateNetwork);
    // otherwise the stream's pieces may not be on such addresses.
    bool fromPrivateNetwork = false;
    // A video of a supported site (model::Download::siteVideo): a Dash stream of whole-file or
    // piece-list tracks, or an Hls stream of playlists, each track with its format; the command's
    // `url` is the video's page.
    bool siteVideo = false;
    // A site video added from a list (model::Download::siteQuality): no tracks yet; it picks them
    // this way when it first starts.
    std::optional<model::SiteQuality> siteQuality;
    std::vector<MediaTrackChoice> tracks;
    std::optional<std::int64_t> approximateBytes;  // shown until the stream's size is known
    // A site video's other headers for its formats (QualityTracks::headers), sent with every request.
    std::vector<std::string> siteHeaders;
};

// At most this many tracks in one stream download: video, audio and some to spare.
inline constexpr size_t kMaxMediaTracks = 8;

struct AddDownloadCommand {
    std::uint64_t token = 0;  // echoed in AddResult so the caller can match the answer
    std::string url;
    std::optional<std::string> fileName;  // a name the user chose; otherwise the server's or the URL's
    std::optional<std::string> saveDir;   // otherwise the category folder
    std::optional<model::CategoryId> categoryId;
    std::string description;
    std::optional<std::string> referer;
    std::optional<std::string> parentPage;
    std::optional<std::string> loginUser;
    std::optional<std::string> loginPassword;
    // From a browser capture: the browser's own User-Agent and the cookies it would send.
    std::optional<std::string> userAgent;
    std::vector<model::Cookie> cookies;
    std::optional<PostData> post;
    // The type the browser received (a capture), so a web page in the file's place is recognised
    // (checklist ENG-15). Ignored when it is itself a web page type.
    std::optional<std::string> contentType;
    AddMode mode = AddMode::StartNow;
    // Set after the user answered the duplicate prompt; otherwise Options > Downloads applies.
    std::optional<model::DuplicatePolicy> duplicatePolicy;
    ExistingFilePolicy existingFilePolicy = ExistingFilePolicy::AutoRename;
    std::optional<MediaChoice> media;
    // The download queue to join. AddMode::Later without one joins the main queue.
    std::optional<model::QueueId> queue;
    // A Site Grabber download (M4 design section 6.3): the project it belongs to, and whether it
    // shows in the main list; one that does not joins it with SetListedCommand.
    std::optional<model::GrabberProjectId> projectId;
    bool listed = true;
};

// The user pressed Start Download or Download Later in the info dialog of a speculative add.
struct ConfirmDownloadCommand {
    model::DownloadId id;
    std::string fileName;
    std::string saveDir;
    std::optional<model::CategoryId> categoryId;
    std::string description;
    bool start = true;
    bool rememberFolderForCategory = false;
    // The download queue to join. Download Later (`start` false) without one joins the main queue.
    std::optional<model::QueueId> queue;
};

struct ResumeCommand {
    std::vector<model::DownloadId> ids;
};
struct StopCommand {
    std::vector<model::DownloadId> ids;
};
struct StopAllCommand {};
// Ends live recordings (checklist P-07): what is on disk is joined into the file, and what the
// stream lists beyond it is given up. A running recording stops first. A recording with nothing
// on disk yet only stops. Ignored for downloads that are not live recordings.
struct FinishRecordingCommand {
    std::vector<model::DownloadId> ids;
};
struct DeleteCommand {
    std::vector<model::DownloadId> ids;
    bool deleteFiles = false;  // also delete completed files from disk (checklist L-07)
};
struct DeleteCompletedCommand {};
struct RedownloadCommand {
    model::DownloadId id;
};
struct SetDownloadSpeedLimitCommand {
    model::DownloadId id;
    std::optional<std::int64_t> bytesPerSecond;
};
// Downloads > Speed Limiter (checklist Q-09). `kilobytesPerSecond` replaces the stored value when set.
struct SetSpeedLimiterCommand {
    bool enabled = false;
    std::optional<int> kilobytesPerSecond;
};

// Properties dialog (checklist C-02). Unset fields stay as they are.
struct UpdateDownloadCommand {
    model::DownloadId id;
    std::optional<std::string> url;
    std::optional<std::string> description;
    std::optional<std::string> referer;
    std::optional<std::string> loginUser;
    std::optional<std::string> loginPassword;  // empty string clears the stored password
    std::optional<model::CompletionAction> completionAction;
    std::optional<int> maxConnections;
};

// Move/Rename (checklist L-05). For completed downloads the file on disk is moved by the caller's
// platform layer first; this records the new location.
struct MoveDownloadCommand {
    model::DownloadId id;
    std::string saveDir;
    std::string fileName;
};

struct SaveCategoryCommand {
    model::Category category;  // id 0 adds a new category
};
struct DeleteCategoryCommand {
    model::CategoryId id;
};
// Puts every download whose file type is in the category's list into that category (checklist
// W-03, offered after the list changed). Folders of existing downloads stay as they are.
struct ReassignCategoryCommand {
    model::CategoryId id;
};
struct UpdateSettingsCommand {
    model::Settings settings;
};
// Changes some settings starting from the current values on the core thread, so two front ends
// that change different settings never overwrite each other's change. In-process callers only.
struct ModifySettingsCommand {
    std::function<void(model::Settings&)> modify;
};

struct SiteLoginInput {
    std::string pattern;
    std::string username;
    std::string password;  // plain text from the dialog; protected before it is stored
};
struct ReplaceSiteLoginsCommand {
    std::vector<SiteLoginInput> logins;
};
struct ReplaceServerLimitsCommand {
    std::vector<model::ServerLimit> limits;
};

// The answer to QuotaReached when Options ask to warn before stopping (checklist O-05).
struct QuotaDecisionCommand {
    bool keepDownloading = false;  // true: ignore the quota until the current period ends
};

// How long a download waits for a new address (checklist ENG-08).
inline constexpr std::chrono::minutes kAddressWaitLimit{10};

// Refresh download address (checklist ENG-08): the download stops and waits for the browser to
// hand over a new address for the same file. One download waits at a time; a newer request takes
// over, and waiting ends after kAddressWaitLimit, on Resume, or when the download is deleted.
struct WaitForAddressCommand {
    model::DownloadId id;
};
// The new address for the waiting download, with what the browser sent along; the download
// resumes from it, with the usual checks that the file did not change. Ignored when that
// download no longer waits.
struct ReplaceAddressCommand {
    model::DownloadId id;
    std::string url;
    std::optional<std::string> referer;
    std::optional<std::string> userAgent;
    std::vector<model::Cookie> cookies;
    std::optional<PostData> post;
};

// Download queues (M4 design section 3). A queue with id 0 is added as a custom queue; the
// built-in queues keep their names.
struct SaveQueueCommand {
    model::Queue queue;
    // A new queue only: downloads that join it at once (Add to queue > Create new queue).
    std::vector<model::DownloadId> members;
};
// Deletes a custom queue, stopping it first; its members stay as downloads outside any queue.
struct DeleteQueueCommand {
    model::QueueId id;
};
// Add to queue (a queue) and Delete from queue (none), checklist L-05. New members go to the end;
// a running download keeps running. Adding a completed download to an ordinary queue downloads it
// again when its turn comes, so front ends ask first (M4 design decision 3).
struct SetQueueCommand {
    std::vector<model::DownloadId> ids;
    std::optional<model::QueueId> queue;
};
// Files in the queue (checklist Q-03): the members in their new order. Members left out keep their
// order after those listed; other downloads are ignored.
struct ReorderQueueCommand {
    model::QueueId queue;
    std::vector<model::DownloadId> order;
};
// Shows Site Grabber downloads in the main list (Add to main list).
struct SetListedCommand {
    std::vector<model::DownloadId> ids;
};
struct StartQueueCommand {
    model::QueueId id;
};
// Stops the queue and its running members, without its completion action (checklist Q-08).
struct StopQueueCommand {
    model::QueueId id;
};
// The computer woke up, or the clock or the time zone changed: schedules are looked at again at
// once rather than at their next planned time.
struct ClockChangedCommand {};
// Help > Check for updates (checklist X-05): asks GitHub now, whatever the settings say. Answered
// by an UpdateCheckFinished notification.
struct CheckForUpdatesCommand {};

// Fetch a streaming manifest into memory for the media panel (M3 design section 6), with the
// proxy downloads use. Answered by a FetchFinished notification with the same id.
struct FetchManifestCommand {
    std::int64_t fetchId = 0;  // chosen by the caller, at least 1
    std::string url;  // http or https
    std::optional<std::string> referer;
    std::optional<std::string> userAgent;  // the browser's; Options' when absent
    std::vector<model::Cookie> cookies;
    std::size_t maxBytes = 0;
};

// Asks what a page offers that the site resolver reads (video-sites design, section 3). Answered by a
// SiteLookupFinished notification with this lookupId; a recent answer for the same video may be
// given again without asking the site.
struct LookUpSiteVideoCommand {
    std::int64_t lookupId = 0;  // chosen by the caller, at least 1
    std::string pageUrl;        // any address core::CheckSitePage accepts
    // The browser's cookies for the page's site, when Options let it sign in there (decision E1).
    std::vector<model::Cookie> cookies;
};

// The caller no longer wants the answer (the tab went to another page). It is not sent.
struct CancelSiteLookupCommand {
    std::int64_t lookupId = 0;
};

// Options: check for newer site video components now, or delete them.
struct CheckSiteToolsCommand {};
struct RemoveSiteToolsCommand {};

using Command = std::variant<AddDownloadCommand, ConfirmDownloadCommand, ResumeCommand, StopCommand, StopAllCommand,
                             FinishRecordingCommand, DeleteCommand, DeleteCompletedCommand, RedownloadCommand, SetDownloadSpeedLimitCommand,
                             SetSpeedLimiterCommand, UpdateDownloadCommand, MoveDownloadCommand, SaveCategoryCommand,
                             DeleteCategoryCommand, ReassignCategoryCommand, UpdateSettingsCommand,
                             ModifySettingsCommand, ReplaceSiteLoginsCommand, ReplaceServerLimitsCommand,
                             QuotaDecisionCommand, WaitForAddressCommand, ReplaceAddressCommand, FetchManifestCommand,
                             SaveQueueCommand, DeleteQueueCommand, SetQueueCommand, ReorderQueueCommand,
                             StartQueueCommand, StopQueueCommand, ClockChangedCommand, SetListedCommand,
                             CheckForUpdatesCommand, LookUpSiteVideoCommand, CancelSiteLookupCommand,
                             CheckSiteToolsCommand, RemoveSiteToolsCommand>;

// Where front ends send commands. Thread-safe; returns at once.
class CommandSink {
public:
    virtual ~CommandSink() = default;
    virtual void Post(Command command) = 0;
};

}  // namespace ldm::core

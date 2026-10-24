#pragma once

#include "model/Ids.h"
#include "model/Schedule.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Site Grabber projects (M4 design section 6, checklist G-01 to G-08).
namespace ldm::model {

// The wizard's templates (checklist G-01). Each presets the filters; Custom leaves them as they are.
enum class GrabberTemplate {
    Custom,
    Pictures,          // all pictures on the site
    Audio,             // audio files on the start page and one level below it
    Video,             // all video on the site
    AllFiles,          // every file that is not a web page
    WholeSite,         // the whole site, pages included, for offline browsing
    StartPageOffline,  // the start page complete, with what it shows, for offline browsing
};

// Where checked files are saved (checklist G-02).
enum class GrabberSaveMode {
    ByCategory,   // each file in its category's folder, as the main list does
    OneCategory,  // every file in one category
    OneFolder,    // every file in one folder
};

// What a project's schedule does when its time comes (checklist G-08).
enum class GrabberScheduleMode {
    Explore,          // explore only
    ExploreDownload,  // explore, then download what the filters check
    DownloadChecked,  // download what is checked, without exploring
};

[[nodiscard]] std::string_view ToString(GrabberTemplate value) noexcept;
[[nodiscard]] std::optional<GrabberTemplate> ParseGrabberTemplate(std::string_view text) noexcept;
[[nodiscard]] std::string_view ToString(GrabberSaveMode value) noexcept;
[[nodiscard]] std::optional<GrabberSaveMode> ParseGrabberSaveMode(std::string_view text) noexcept;
[[nodiscard]] std::string_view ToString(GrabberScheduleMode value) noexcept;
[[nodiscard]] std::optional<GrabberScheduleMode> ParseGrabberScheduleMode(std::string_view text) noexcept;

inline constexpr int kMaxGrabberDepth = 99;
inline constexpr int kMaxGrabberConcurrency = 10;
inline constexpr std::size_t kMaxGrabberMasks = 64;
inline constexpr std::size_t kMaxGrabberMaskLength = 256;

// Which pages the explorer follows (checklist G-03). Depths count links from the start page,
// which is level 0 and always read.
struct GrabberExploreFilters {
    std::optional<int> baseSiteDepth = 1;  // pages of the start page's site; nothing: the whole site
    int otherSitesDepth = 0;               // pages on other sites, counted from the first one; 0: none
    bool noParentDirectories = true;  // base-site pages only below the start page's folder
    bool mainDomainSites = false;     // every subdomain of the start page's domain counts as the base site
    // Wildcard masks on the address path ("/docs/*"). An empty include list includes everything.
    std::vector<std::string> includePaths;
    std::vector<std::string> excludePaths;

    bool operator==(const GrabberExploreFilters&) const = default;
};

// Which files the Grabber lists and checks (checklist G-04).
struct GrabberFileFilters {
    // Masks on the file name ("*.jpg"). An empty include list includes every file.
    std::vector<std::string> includeTypes;
    std::vector<std::string> excludeTypes;
    std::optional<std::int64_t> minBytes;
    std::optional<std::int64_t> maxBytes;
    std::vector<std::string> includePaths;  // masks on the address path, as in the explore filters
    std::vector<std::string> excludePaths;
    bool thisSiteOnly = false;    // only files on the base site
    bool hideDuplicates = true;   // the same address found again is not listed again
    bool downloadImmediately = false;  // matched files download while exploring goes on

    bool operator==(const GrabberFileFilters&) const = default;
};

// A project as the wizard edits it, stored as one JSON document.
struct GrabberProjectDefinition {
    std::string startPage;
    GrabberTemplate templateKind = GrabberTemplate::Custom;

    GrabberSaveMode saveMode = GrabberSaveMode::ByCategory;
    std::optional<CategoryId> category;  // OneCategory
    std::string folder;                  // OneFolder: an absolute path
    bool keepSiteFolders = false;        // keep the site's relative folders below the save folder
    bool savePages = false;              // the pages explored are saved too (the offline templates)
    bool convertLinks = false;           // saved pages link to the saved files (offline browsing)
    bool overwrite = false;              // replace existing files instead of numbering new ones
    bool addToMainListOnClose = false;   // checked files join the main list when the window closes

    GrabberExploreFilters explore;
    GrabberFileFilters files;

    int exploreConcurrency = 4;   // pages read at the same time, 1 to kMaxGrabberConcurrency
    int downloadConcurrency = 4;  // files downloaded at the same time, 1 to kMaxGrabberConcurrency
    bool linkTextAsDescription = true;
    bool useBrowserCookies = false;  // ask a connected browser for the start page's cookies

    bool operator==(const GrabberProjectDefinition&) const = default;
};

// A stored project or template (G-07). Its login password is kept protected, like site logins.
struct GrabberProject {
    GrabberProjectId id;
    std::string name;
    bool isTemplate = false;
    GrabberProjectDefinition definition;
    std::string loginUser;
    std::vector<std::uint8_t> loginSecret;  // protected password; empty for none
    Schedule schedule;
    GrabberScheduleMode scheduleMode = GrabberScheduleMode::Explore;
    std::int64_t createdAtMs = 0;
    std::optional<std::int64_t> lastRunAtMs;

    bool operator==(const GrabberProject&) const = default;
};

enum class GrabberItemKind {
    Page,
    File,
};

enum class GrabberItemState {
    Found,        // listed, not downloaded
    Filtered,     // found but left out by the file filters; shown only under the filter nodes
    Downloading,  // a download of the main list (hidden there) runs for it
    Done,
    Failed,
};

[[nodiscard]] std::string_view ToString(GrabberItemKind value) noexcept;
[[nodiscard]] std::optional<GrabberItemKind> ParseGrabberItemKind(std::string_view text) noexcept;
[[nodiscard]] std::string_view ToString(GrabberItemState value) noexcept;
[[nodiscard]] std::optional<GrabberItemState> ParseGrabberItemState(std::string_view text) noexcept;

// One page or file a project found.
struct GrabberItem {
    std::int64_t id = 0;  // within its project, in the order found
    std::string url;
    GrabberItemKind kind = GrabberItemKind::File;
    std::optional<std::int64_t> parentId;  // the page that linked here; nothing for the start page
    int depth = 0;
    std::string linkText;
    std::optional<std::int64_t> size;
    std::string contentType;
    bool checked = false;
    GrabberItemState state = GrabberItemState::Found;
    std::optional<DownloadId> downloadId;
    std::string localPath;  // where the file or page was saved; empty until it was

    bool operator==(const GrabberItem&) const = default;
};

}  // namespace ldm::model

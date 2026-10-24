#pragma once

#include "core/TransferEngine.h"
#include "model/Cookie.h"
#include "model/GrabberProject.h"
#include "model/Ids.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

// Requests to the Site Grabber's explorer (M4 design section 6), which runs on the core thread
// beside the coordinator. Defined here so the core thread can carry them; the explorer itself is
// in the grabber library. Every field is validated by the explorer, never trusted.
namespace ldm::core {

// Saves a project or template; id 0 adds a new one. Answered by GrabberProjectSaved.
struct GrabberSaveProjectCommand {
    std::uint64_t token = 0;
    model::GrabberProject project;  // loginSecret is ignored; the password comes in plain text below
    // The login password: nothing keeps the stored one, an empty string clears it.
    std::optional<std::string> loginPassword;
};
// Deletes a project with its items. Its downloads that never joined the main list leave the list
// too; files already on disk stay.
struct GrabberDeleteProjectCommand {
    model::GrabberProjectId id;
};
// The project's window opened or closed: while open, its items are published. Closing adds the
// checked files to the main list when the project asks for it.
struct GrabberOpenCommand {
    model::GrabberProjectId id;
    bool open = true;
};
// The project's wizard opened or closed: a schedule does not start a project being edited.
struct GrabberEditingCommand {
    model::GrabberProjectId id;
    bool open = true;
};
// Explores from the start page. `keepChecks` (Update all) keeps the items found before and their
// checks; otherwise the list starts empty. `cookies` are the browser's for the start page, kept in
// memory for this run only.
struct GrabberExploreCommand {
    model::GrabberProjectId id;
    bool keepChecks = false;
    std::vector<model::Cookie> cookies;
    bool thenDownload = false;  // download the checked files once exploring ends
};
// Stops exploring and the project's downloads.
struct GrabberStopCommand {
    model::GrabberProjectId id;
};
// Checks or unchecks files, by item id, or every file with `all`.
struct GrabberCheckCommand {
    model::GrabberProjectId id;
    std::vector<std::int64_t> items;
    bool all = false;
    bool checked = true;
};
// Downloads the checked files that are not downloaded yet.
struct GrabberDownloadCommand {
    model::GrabberProjectId id;
};
struct GrabberStopDownloadsCommand {
    model::GrabberProjectId id;
};
// Shows the downloads of these items in the main list; with no items, of every checked item.
struct GrabberAddToMainListCommand {
    model::GrabberProjectId id;
    std::vector<std::int64_t> items;
};

using GrabberCommand =
    std::variant<GrabberSaveProjectCommand, GrabberDeleteProjectCommand, GrabberOpenCommand, GrabberEditingCommand,
                 GrabberExploreCommand, GrabberStopCommand, GrabberCheckCommand, GrabberDownloadCommand,
                 GrabberStopDownloadsCommand, GrabberAddToMainListCommand>;

// Where front ends send Grabber commands. Thread-safe; returns at once.
class GrabberSink {
public:
    virtual ~GrabberSink() = default;
    virtual void Post(GrabberCommand command) = 0;
};

// Fetches and proxy lookups of the explorer use ids at or below this, so the core thread can tell
// their engine events from the coordinator's (playlists use small negative ids, the media panel
// positive ones).
inline constexpr std::int64_t kGrabberFetchIdBase = -(std::int64_t{1} << 40);

[[nodiscard]] inline bool IsGrabberFetch(std::int64_t id) noexcept
{
    return id <= kGrabberFetchIdBase;
}

// What the core thread drives beside the coordinator. Every method is called on the core thread.
class GrabberComponent {
public:
    virtual ~GrabberComponent() = default;
    // After the coordinator initialized.
    virtual void Initialize() = 0;
    virtual void Handle(GrabberCommand command) = 0;
    // A fetch answer or a proxy lookup with a Grabber id (IsGrabberFetch).
    virtual void OnEngineEvent(EngineEvent event) = 0;
    virtual void Tick() = 0;
    [[nodiscard]] virtual std::chrono::steady_clock::time_point NextDeadline() const = 0;
    // Stops starting anything and saves what is not saved yet.
    virtual void Shutdown() = 0;
};

}  // namespace ldm::core

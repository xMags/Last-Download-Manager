#pragma once

#include "model/GrabberProject.h"
#include "model/Ids.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

// What the Grabber's explorer publishes (M4 design section 6.5). Snapshots are immutable and
// shared, as the coordinator's are: item rows that did not change are shared between snapshots.
namespace ldm::grabber {

struct GrabberStats {
    int pagesExplored = 0;
    int pagesWaiting = 0;  // found and to be read, or being read
    int pagesFailed = 0;
    int filesFound = 0;    // files listed, filtered ones not counted
    int filesChecked = 0;
    int filesDone = 0;
    int filesFailed = 0;
    std::int64_t bytesDone = 0;  // of the files downloaded
    bool limitReached = false;  // a page or item cap stopped the exploration early
};

struct GrabberProjectView {
    model::GrabberProject project;  // loginSecret is always empty here
    bool hasPassword = false;
    std::optional<std::string> damaged;  // the stored definition could not be read
    bool open = false;      // its window is open
    bool exploring = false;
    bool downloading = false;
    GrabberStats stats;
    // Every item, in the order found, while the project's window is open; empty otherwise.
    std::vector<std::shared_ptr<const model::GrabberItem>> items;
};

struct GrabberSnapshot {
    std::uint64_t version = 0;
    std::vector<GrabberProjectView> projects;  // templates too, by name
};

// The answer to GrabberSaveProjectCommand.
struct GrabberProjectSaved {
    std::uint64_t token = 0;
    std::optional<model::GrabberProjectId> id;
    std::optional<std::string> error;
};

// An exploration or a project's downloads ended by themselves (not by Stop).
struct GrabberRunFinished {
    model::GrabberProjectId id;
    std::string name;
    bool explored = false;    // false: the downloads ended
    bool scheduled = false;   // its schedule started it
};

// Something went wrong that is not tied to an item (storage, a damaged project).
struct GrabberError {
    std::string message;
};

using GrabberNotification = std::variant<GrabberProjectSaved, GrabberRunFinished, GrabberError>;

// Receives the explorer's output. Called on the core thread; implementations only post.
class GrabberObserver {
public:
    virtual ~GrabberObserver() = default;
    virtual void OnGrabberSnapshot(std::shared_ptr<const GrabberSnapshot> snapshot) = 0;
    virtual void OnGrabberNotification(GrabberNotification notification) = 0;
};

}  // namespace ldm::grabber

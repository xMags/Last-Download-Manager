#pragma once

#include "base/Result.h"
#include "model/GrabberProject.h"
#include "model/Ids.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ldm::core {

// A stored project with its definition still as written: the explorer reads the definition as
// untrusted input (grabber::ParseDefinition), so a damaged row cannot stop LDM from starting.
struct StoredGrabberProject {
    model::GrabberProject project;  // `definition` is left at its defaults
    std::string definitionJson;
};

// Persistence of Site Grabber projects and their items (M4 design section 6.1). Used from the
// core thread only. Every write is atomic.
class GrabberStore {
public:
    virtual ~GrabberStore() = default;

    [[nodiscard]] virtual base::Result<std::vector<StoredGrabberProject>> LoadGrabberProjects() = 0;
    // `project.id` and `project.definition` are ignored; the definition is `definitionJson`.
    [[nodiscard]] virtual base::Result<model::GrabberProjectId> InsertGrabberProject(
        const model::GrabberProject& project, std::string_view definitionJson) = 0;
    [[nodiscard]] virtual base::Status SaveGrabberProject(const model::GrabberProject& project,
                                                          std::string_view definitionJson) = 0;
    // Deletes the project, its items and its schedule. Its downloads stay, without a project.
    [[nodiscard]] virtual base::Status DeleteGrabberProject(model::GrabberProjectId id) = 0;

    [[nodiscard]] virtual base::Result<std::vector<model::GrabberItem>> LoadGrabberItems(model::GrabberProjectId id) = 0;
    // Inserts or replaces these items.
    [[nodiscard]] virtual base::Status SaveGrabberItems(model::GrabberProjectId id,
                                                        std::span<const model::GrabberItem> items) = 0;
    // Deletes every item of the project (a new exploration).
    [[nodiscard]] virtual base::Status ClearGrabberItems(model::GrabberProjectId id) = 0;
};

}  // namespace ldm::core

#pragma once

#include "base/Result.h"
#include "storage/Sqlite.h"

namespace ldm::storage {

// Highest schema version this build understands.
[[nodiscard]] int LatestSchemaVersion() noexcept;

// Brings the database up to LatestSchemaVersion(), one transaction per step. Refuses a database
// written by a newer build instead of guessing at its layout.
[[nodiscard]] base::Status Migrate(Connection& connection);

}  // namespace ldm::storage

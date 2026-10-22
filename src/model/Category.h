#pragma once

#include "model/Ids.h"

#include <cstdint>
#include <string>

namespace ldm::model {

// A download category (checklist W-02, W-03). Extensions and sites are space-separated lists;
// sites may contain '*' wildcards.
struct Category {
    CategoryId id;
    std::string name;
    std::string extensions;
    std::string sites;
    std::string folder;  // UTF-8 absolute path
    bool rememberLastFolder = false;
    bool builtin = false;
    std::int64_t sortOrder = 0;
};

}  // namespace ldm::model

// The extension decides which downloads to capture with its own copy of LDM's matching rules
// (extension/src/capture-rules.js). Both test suites check the same cases, so the copies cannot
// drift apart (M3 design section 5).
#include "core/FileNames.h"
#include "core/Matching.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <string>

namespace {

TEST(CaptureCases, LdmMatchesTheCasesTheExtensionIsTestedWith)
{
    std::ifstream file(LDM_CAPTURE_CASES);
    ASSERT_TRUE(file) << "missing " << LDM_CAPTURE_CASES;
    const nlohmann::json cases = nlohmann::json::parse(file);
    ASSERT_FALSE(cases.empty());
    for (const auto& entry : cases) {
        std::string list;
        for (const auto& pattern : entry["patterns"]) {
            list += pattern.get<std::string>() + " ";
        }
        const std::string name = entry["name"].get<std::string>();
        SCOPED_TRACE(name + " with [" + list + "]");
        EXPECT_EQ(ldm::core::MatchesExtensionList(list, ldm::core::FileExtension(name)), entry["capture"].get<bool>());
    }
}

}  // namespace

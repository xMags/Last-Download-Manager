#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ldm::ui {

// Add batch download (checklist A-07): an address with one '*' replaced by a run of numbers or
// letters.
struct BatchPattern {
    std::string address;  // must contain exactly one '*'
    bool letters = false;
    int fromNumber = 1;
    int toNumber = 10;
    int width = 1;  // zero-padding width for numbers, 1 for none
    char fromLetter = 'a';
    char toLetter = 'z';
};

inline constexpr size_t kMaxBatchSize = 1000;

struct BatchExpansion {
    std::vector<std::string> addresses;
    std::optional<std::string> error;  // why nothing was produced
};

// Expands the pattern in order. Ranges may run backwards ("from 12 to 1"). Rejects patterns
// without exactly one '*', invalid letters or more than kMaxBatchSize addresses.
[[nodiscard]] BatchExpansion ExpandBatch(const BatchPattern& pattern);

}  // namespace ldm::ui

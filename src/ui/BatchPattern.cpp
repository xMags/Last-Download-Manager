#include "ui/BatchPattern.h"

#include <algorithm>
#include <cstdlib>

namespace ldm::ui {

namespace {

bool IsLetter(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool SameCase(char a, char b)
{
    return (a >= 'a' && a <= 'z') == (b >= 'a' && b <= 'z');
}

}  // namespace

BatchExpansion ExpandBatch(const BatchPattern& pattern)
{
    BatchExpansion result;
    const size_t star = pattern.address.find('*');
    if (star == std::string::npos || pattern.address.find('*', star + 1) != std::string::npos) {
        result.error = "The address must contain exactly one * where the number or letter goes.";
        return result;
    }
    const std::string prefix = pattern.address.substr(0, star);
    const std::string suffix = pattern.address.substr(star + 1);

    if (pattern.letters) {
        if (!IsLetter(pattern.fromLetter) || !IsLetter(pattern.toLetter) || !SameCase(pattern.fromLetter, pattern.toLetter)) {
            result.error = "Use two letters of the same case, such as a to z.";
            return result;
        }
        const int step = pattern.fromLetter <= pattern.toLetter ? 1 : -1;
        for (int c = pattern.fromLetter;; c += step) {
            result.addresses.push_back(prefix + static_cast<char>(c) + suffix);
            if (c == pattern.toLetter) {
                break;
            }
        }
        return result;
    }

    if (pattern.fromNumber < 0 || pattern.toNumber < 0 || pattern.width < 1 || pattern.width > 10) {
        result.error = "Numbers must not be negative, and the width must be between 1 and 10.";
        return result;
    }
    const long long count = std::llabs(static_cast<long long>(pattern.toNumber) - pattern.fromNumber) + 1;
    if (count > static_cast<long long>(kMaxBatchSize)) {
        result.error = "A batch can hold at most 1000 files.";
        return result;
    }
    const int step = pattern.fromNumber <= pattern.toNumber ? 1 : -1;
    for (int n = pattern.fromNumber;; n += step) {
        std::string number = std::to_string(n);
        if (static_cast<int>(number.size()) < pattern.width) {
            number.insert(0, static_cast<size_t>(pattern.width) - number.size(), '0');
        }
        result.addresses.push_back(prefix + number + suffix);
        if (n == pattern.toNumber) {
            break;
        }
    }
    return result;
}

}  // namespace ldm::ui

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ldm::core {

// What a launch of ldm.exe asks for (checklist A-10, W-05). A second launch forwards this to the
// running instance instead of starting another one (design section 7).
struct LaunchRequest {
    std::optional<std::string> url;       // UTF-8
    std::optional<std::string> saveDir;   // UTF-8, absolute
    std::optional<std::string> fileName;  // UTF-8
    bool silent = false;     // add without any dialog
    bool queueOnly = false;  // add without starting
    bool startQueue = false;  // start the main download queue
    bool tray = false;        // start hidden in the tray (used by Launch on startup)
    bool help = false;
};

struct CommandLineParse {
    LaunchRequest request;
    std::optional<std::string> error;  // shown to the user; nothing is forwarded or added
};

// Parses arguments after the program name. Switches use LDM's own names, documented in
// CommandLineHelp(). Validates every value: the address must be http(s) or ftp(s), the folder
// absolute, the name free of path separators.
[[nodiscard]] CommandLineParse ParseCommandLine(const std::vector<std::wstring>& arguments);

[[nodiscard]] std::wstring CommandLineHelp();

}  // namespace ldm::core

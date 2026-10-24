#pragma once

#include "core/Snapshot.h"
#include "grabber/GrabberSnapshot.h"
#include "model/GrabberProject.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

// What the Grabber window shows of a project's items (checklist G-05). Pure, so the views and
// texts are tested without the window.
namespace ldm::ui {

// A node of the Grabber window's tree.
struct GrabberNode {
    enum class Kind {
        AllFiles,     // every file listed, left-out ones not counted
        Checked,
        Downloaded,
        Failed,
        Type,         // files of one extension (`type`, upper case; empty for none)
        OtherSites,   // files not on the start page's host
        LeftOut,      // files the filters left out
        Pages,        // the pages found, read or not
    };
    Kind kind = Kind::AllFiles;
    std::wstring type;

    bool operator==(const GrabberNode&) const = default;
};

// Whether the node shows the item. `startHost` is the start page's host, lower case.
[[nodiscard]] bool InGrabberNode(const model::GrabberItem& item, const GrabberNode& node, const std::string& startHost);

// The extensions of the listed files, upper case, sorted, without repeats; "" for files without one.
[[nodiscard]] std::vector<std::wstring> GrabberTypes(const std::vector<std::shared_ptr<const model::GrabberItem>>& items);

// The list's columns.
[[nodiscard]] std::wstring GrabberItemName(const model::GrabberItem& item);
[[nodiscard]] std::wstring GrabberItemType(const model::GrabberItem& item);
// `download`: the main list's row of the item's download, while it is being downloaded.
[[nodiscard]] std::wstring GrabberItemStatus(const model::GrabberItem& item, const core::DownloadView* download);

// The status line: what the project is doing and its counts.
[[nodiscard]] std::wstring GrabberStatusText(const grabber::GrabberProjectView& view);
// Statistics (checklist G-05), one fact per line.
[[nodiscard]] std::wstring GrabberStatisticsText(const grabber::GrabberProjectView& view);

}  // namespace ldm::ui

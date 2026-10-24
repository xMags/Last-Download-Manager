#pragma once

#include "core/GrabberCommands.h"
#include "core/Snapshot.h"
#include "grabber/GrabberSnapshot.h"
#include "ui/Dialog.h"
#include "ui/GrabberView.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ldm::ui {

// The Site Grabber's window for one project (checklist G-05, M4 design section 6.5): a tree of
// views on the left (all files, by state, by type, pages, other sites, left out), the items of
// the chosen view in a list on the right with check boxes, the project's commands along the top
// and its statistics at the bottom. Modeless, one per project; it reads the explorer's snapshots
// and posts commands.
class GrabberWindow final : public Dialog {
public:
    struct Callbacks {
        std::function<void(core::GrabberCommand)> post;
        // Explore, or Update all with `keepChecks`; the owner adds the browser's cookies.
        std::function<void(model::GrabberProjectId, bool keepChecks)> explore;
        std::function<void(model::GrabberProjectId)> edit;
        std::function<void(model::GrabberProjectId)> saveAsTemplate;
        std::function<bool(const std::filesystem::path&)> openFile;
        std::function<bool(const std::filesystem::path&)> showInFolder;
        // Called after the window was destroyed; the owner deletes the object.
        std::function<void(model::GrabberProjectId)> closed;
    };

    GrabberWindow(const Theme& theme, model::GrabberProjectId project, Callbacks callbacks);

    void Open(HWND owner, std::shared_ptr<const grabber::GrabberSnapshot> snapshot);
    void Activate();
    void Update(std::shared_ptr<const grabber::GrabberSnapshot> snapshot);
    // The main list's snapshot, for the progress of the files being downloaded.
    void UpdateDownloads(const core::Snapshot& snapshot);
    [[nodiscard]] model::GrabberProjectId Project() const noexcept { return project_; }

private:
    bool OnInit() override;
    bool OnCommand(int id, int code, HWND control) override;
    bool OnNotify(const NMHDR& header, LRESULT& result) override;
    bool OnMessage(UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result) override;
    void OnDestroyed() override;

    void Layout();
    void RefreshTree();
    void RefreshRows();
    void RefreshButtons();
    void RefreshStatus();
    void ShowStatistics();
    void ShowListMenu(POINT screen);
    void CheckRows(const std::vector<int>& rows, bool checked);
    void Post(core::GrabberCommand command);
    [[nodiscard]] const grabber::GrabberProjectView* View() const;
    [[nodiscard]] const model::GrabberItem* RowItem(int row) const;
    [[nodiscard]] std::vector<int> SelectedRows() const;
    [[nodiscard]] std::wstring Cell(const model::GrabberItem& item, int column) const;

    model::GrabberProjectId project_;
    Callbacks callbacks_;
    std::shared_ptr<const grabber::GrabberSnapshot> snapshot_;
    // The download list's rows of the files being downloaded, by download id.
    std::unordered_map<std::int64_t, std::shared_ptr<const core::DownloadView>> downloads_;
    GrabberNode node_;
    std::vector<GrabberNode> treeNodes_;  // tree item lParam -> node
    std::vector<std::wstring> typeNodes_;  // the "By type" children as last built
    std::vector<std::size_t> rows_;  // list row -> index into the project's items
    std::wstring cellText_;          // kept alive for LVN_GETDISPINFO
    RECT buttonsBottom_{};           // the buttons' row, from the template
    bool refreshingTree_ = false;
    bool seen_ = false;  // the project was in a snapshot since the window opened
};

}  // namespace ldm::ui

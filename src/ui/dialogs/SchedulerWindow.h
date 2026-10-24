#pragma once

#include "core/Commands.h"
#include "core/Snapshot.h"
#include "model/Queue.h"
#include "ui/Dialog.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ldm::ui {

// Downloads > Scheduler (checklist Q-01 to Q-04, M4 design section 7): the queues on the left;
// on the right their schedule (Q-02), their files (Q-03) and the download quota (Q-04). Modeless
// and single. Apply saves the chosen queue's settings and the quota; the file list's buttons act
// at once, since it shows the queue as it is.
class SchedulerWindow final : public Dialog {
public:
    struct Callbacks {
        std::function<void(core::Command)> post;
        // Called after the window was destroyed; the owner deletes the object.
        std::function<void()> closed;
        // Whether Windows lets programs wake the computer (SystemActions::WakeTimersAllowed).
        std::function<std::optional<bool>()> wakeTimersAllowed;
    };

    SchedulerWindow(const Theme& theme, Callbacks callbacks);

    void Open(HWND owner, std::shared_ptr<const core::Snapshot> snapshot, std::optional<model::QueueId> select);
    // Shows the window in front, with `select` chosen when given.
    void Activate(std::optional<model::QueueId> select);
    void Update(std::shared_ptr<const core::Snapshot> snapshot);

private:
    bool OnInit() override;
    bool OnCommand(int id, int code, HWND control) override;
    bool OnNotify(const NMHDR& header, LRESULT& result) override;
    void OnDestroyed() override;

    void ShowPage(int page);
    void RefreshQueueList();
    void RefreshFiles();
    void RefreshButtons();
    // Puts a queue's settings and the quota into the fields; they stay as edited until Apply or
    // another queue is chosen.
    void LoadQueue(const core::QueueView& view);
    void LoadQuota();
    void EnableScheduleFields();
    // Changes the chosen queue, asking first about unapplied changes. False when the user stayed.
    bool Choose(model::QueueId queue);
    // Applies the fields; false when one is not valid (the user was told) and nothing was sent.
    bool Apply();
    // Unapplied changes: true to go on (applied or dropped), false to stay.
    bool SettleChanges();
    void MoveSelectedFiles(int step);
    void RemoveSelectedFiles();
    void CreateQueue();
    void DeleteQueue();
    void MarkEdited();

    [[nodiscard]] const core::QueueView* Chosen() const;
    [[nodiscard]] std::vector<model::DownloadId> Members() const;  // of the chosen queue, in queue order
    [[nodiscard]] std::vector<model::DownloadId> SelectedFiles() const;

    Callbacks callbacks_;
    std::shared_ptr<const core::Snapshot> snapshot_;
    std::optional<model::QueueId> chosen_;
    std::vector<model::QueueId> listed_;      // list box row -> queue
    std::vector<model::DownloadId> files_;    // files list row -> download
    std::optional<std::string> selectWhenAdded_;  // a queue created here, chosen once it appears
    std::vector<std::wstring> listTexts_;     // what the list box shows, to rebuild it only on changes
    std::optional<model::Queue> loaded_;      // the queue as its fields were loaded
    std::optional<model::DownloadQuota> loadedQuota_;
    bool edited_ = false;
    bool loading_ = false;
};

}  // namespace ldm::ui

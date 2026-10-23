#pragma once

#include "browser/Protocol.h"
#include "core/Commands.h"
#include "core/Snapshot.h"
#include "model/Ids.h"
#include "model/Settings.h"
#include "ui/Theme.h"

#include <windows.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>

// What happens around browser captures after the capture itself: the exception offer after two
// cancelled captures from one site (checklist B-06), the explanation when LDM could not take a
// download over (B-06, ENG-15), and Refresh download address (ENG-08).
namespace ldm::ui {

// Counts the captured downloads whose Download File Info dialog was cancelled, per site and per
// address, while LDM runs.
class CancelCounter {
public:
    struct Offer {
        std::string host;
        std::string url;
        bool sameAddress = false;  // the same address was cancelled twice, not only the same site
    };

    // Records a cancel; returns an offer on the second cancel for the same site. Counts for that
    // site start over after an offer, so the next one comes after two more cancels.
    [[nodiscard]] std::optional<Offer> Cancelled(const std::string& url);

private:
    std::map<std::string, int> perHost_;
    std::map<std::string, int> perAddress_;
};

// An address exception that covers `url` and its neighbours: the folder of the address with '*'
// for the rest ("https://a.test/dl/x.zip?t=1" becomes "https://a.test/dl/*"). Nothing for an
// address that does not parse.
[[nodiscard]] std::optional<std::string> SuggestAddressPattern(const std::string& url);

// Appends `word` to a space or line separated list unless it is already there (ignoring case).
[[nodiscard]] std::string AddToList(const std::string& list, const std::string& word, char separator);

// True when a captured download looks like the file `waiting` waits for a new address for: the
// same name (ignoring case), and the same size when both sizes are known.
[[nodiscard]] bool IsNewAddressFor(const core::DownloadView& waiting, const std::optional<std::string>& fileName,
                                   std::optional<std::int64_t> fileSize);

// The keys that keep a clicked link in the browser, as the user would press them ("Alt",
// "Shift+Ctrl"); empty when that set is off or has no keys.
[[nodiscard]] std::wstring PreventKeysText(const model::CaptureKeys& keys);

class CaptureFollowUp {
public:
    struct Context {
        HWND owner = nullptr;
        core::CommandSink* commands = nullptr;
        const Theme* theme = nullptr;
        std::function<std::shared_ptr<const core::Snapshot>()> snapshot;
        // Opens `url` in a tab of a connected browser; false when no browser is connected.
        std::function<bool(const std::string&)> openPage;
    };

    explicit CaptureFollowUp(Context context);

    // A captured download was added (its id is known).
    void OnCapturedAdded(model::DownloadId id);
    // The Download File Info dialog of a captured download was cancelled.
    void OnCaptureCancelled(const std::string& url);
    // A failed download: explains when LDM got a web page or a refusal instead of the file.
    void OnFailed(const core::DownloadFailed& failed);

    // Row menu > Refresh download address.
    void RefreshAddress(model::DownloadId id);
    // A new capture: when a download waits for a new address and this looks like its file, asks
    // and hands the address to it. Returns true when the capture was used that way.
    [[nodiscard]] bool OfferAsNewAddress(const browser::Capture& capture);

private:
    [[nodiscard]] const core::DownloadView* FindDownload(const core::Snapshot& snapshot, model::DownloadId id) const;
    void OfferException(const CancelCounter::Offer& offer);
    void Tell(const std::wstring& message, bool warning) const;
    [[nodiscard]] bool Ask(const std::wstring& question) const;
    void Post(core::Command command) const;

    Context context_;
    CancelCounter cancels_;
    std::set<model::DownloadId> captured_;  // captured this session; their first failure means take-over failed
};

}  // namespace ldm::ui

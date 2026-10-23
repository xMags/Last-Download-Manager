#include "ui/CaptureFollowUp.h"

#include "base/Utf.h"
#include "core/FileNames.h"
#include "core/Url.h"
#include "ui/dialogs/CaptureDialogs.h"
#include "ui/dialogs/MessageDialog.h"

#include <algorithm>
#include <cctype>
#include <string_view>
#include <utility>

namespace ldm::ui {

namespace {

// Downloads remembered as captured this session; the oldest are forgotten beyond this.
constexpr size_t kMaxRemembered = 500;

bool SameText(std::string_view a, std::string_view b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// The name a capture would get: the browser's, else the address's.
std::optional<std::string> CaptureName(const browser::Capture& capture)
{
    if (capture.fileName && !capture.fileName->empty()) {
        return core::SanitizeFileName(*capture.fileName);
    }
    const auto parsed = core::ParseUrl(capture.url);
    if (!parsed) {
        return std::nullopt;
    }
    const std::string name = core::FileNameFromUrlPath(parsed->path);
    return name.empty() ? std::nullopt : std::optional<std::string>(core::SanitizeFileName(name));
}

}  // namespace

std::optional<CancelCounter::Offer> CancelCounter::Cancelled(const std::string& url)
{
    const auto parsed = core::ParseUrl(url);
    if (!parsed) {
        return std::nullopt;
    }
    const int forHost = ++perHost_[parsed->host];
    const int forAddress = ++perAddress_[url];
    if (forHost < 2) {
        return std::nullopt;
    }
    perHost_.erase(parsed->host);
    perAddress_.erase(url);
    return Offer{parsed->host, url, forAddress >= 2};
}

std::optional<std::string> SuggestAddressPattern(const std::string& url)
{
    if (!core::ParseUrl(url)) {
        return std::nullopt;
    }
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) {
        return std::nullopt;
    }
    std::string address = url.substr(0, url.find_first_of("?#"));
    const size_t pathStart = address.find('/', scheme + 3);
    if (pathStart == std::string::npos) {
        return address + "/*";
    }
    return address.substr(0, address.rfind('/') + 1) + "*";
}

std::string AddToList(const std::string& list, const std::string& word, char separator)
{
    size_t start = 0;
    while (start < list.size()) {
        const size_t end = list.find_first_of(" \t\r\n", start);
        const std::string_view entry(list.data() + start, (end == std::string::npos ? list.size() : end) - start);
        if (SameText(entry, word)) {
            return list;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    const bool needsSeparator = !list.empty() && list.back() != ' ' && list.back() != '\n';
    return list + (needsSeparator ? std::string(1, separator) : std::string()) + word;
}

bool IsNewAddressFor(const core::DownloadView& waiting, const std::optional<std::string>& fileName,
                     std::optional<std::int64_t> fileSize)
{
    if (!fileName || !SameText(*fileName, waiting.download.fileName)) {
        return false;
    }
    const std::optional<std::int64_t>& known = waiting.download.totalSize;
    return !known || !fileSize || *known == *fileSize;
}

std::wstring PreventKeysText(const model::CaptureKeys& keys)
{
    if (!keys.preventEnabled) {
        return {};
    }
    std::wstring text;
    const auto add = [&text](bool on, const wchar_t* name) {
        if (on) {
            text += (text.empty() ? L"" : L"+") + std::wstring(name);
        }
    };
    add(keys.preventAlt, L"Alt");
    add(keys.preventShift, L"Shift");
    add(keys.preventCtrl, L"Ctrl");
    return text;
}

CaptureFollowUp::CaptureFollowUp(Context context) : context_(std::move(context)) {}

void CaptureFollowUp::OnCapturedAdded(model::DownloadId id)
{
    captured_.insert(id);
    while (captured_.size() > kMaxRemembered) {
        captured_.erase(captured_.begin());  // ids grow, so the oldest go first
    }
}

void CaptureFollowUp::OnCaptureCancelled(const std::string& url)
{
    const auto offer = cancels_.Cancelled(url);
    if (!offer) {
        return;
    }
    const auto snapshot = context_.snapshot();
    if (!snapshot || !snapshot->settings.offerCaptureExceptions) {
        return;
    }
    OfferException(*offer);
}

void CaptureFollowUp::OfferException(const CancelCounter::Offer& offer)
{
    const std::string pattern = SuggestAddressPattern(offer.url).value_or(offer.url);
    ExceptionOfferDialog dialog(*context_.theme, base::ToUtf16(offer.host), base::ToUtf16(pattern),
                                offer.sameAddress ? ExceptionOfferDialog::Kind::Address : ExceptionOfferDialog::Kind::Site);
    const ExceptionOfferDialog::Choice choice = dialog.Run(context_.owner);
    if (!choice.kind && !choice.dontAskAgain) {
        return;
    }
    const std::optional<ExceptionOfferDialog::Kind> kind = choice.kind;
    const std::string value = base::ToUtf8(choice.value);
    const bool dontAskAgain = choice.dontAskAgain;
    core::ModifySettingsCommand command;
    command.modify = [kind, value, dontAskAgain](model::Settings& s) {
        if (kind == ExceptionOfferDialog::Kind::Site) {
            s.captureSiteExceptions = AddToList(s.captureSiteExceptions, Lower(value), ' ');
        } else if (kind == ExceptionOfferDialog::Kind::Address) {
            s.captureAddressExceptions = AddToList(s.captureAddressExceptions, value, '\n');
        }
        if (dontAskAgain) {
            s.offerCaptureExceptions = false;
        }
    };
    Post(std::move(command));
}

void CaptureFollowUp::OnFailed(const core::DownloadFailed& failed)
{
    const core::TransferFailure& failure = failed.failure;
    const bool page = failure.reason == core::FailureReason::HtmlInsteadOfFile;
    const bool refused = failure.reason == core::FailureReason::NotFound ||
                         (failure.reason == core::FailureReason::HttpError &&
                          (failure.httpStatus == 403 || failure.httpStatus == 410));
    const bool justCaptured = captured_.erase(failed.id) > 0;
    if (!page && !refused) {
        return;
    }
    const auto snapshot = context_.snapshot();
    if (!snapshot) {
        return;
    }
    // A fast failure can come before the snapshot that lists the download.
    const core::DownloadView* view = FindDownload(*snapshot, failed.id);
    const std::int64_t received = view != nullptr ? view->download.DownloadedBytes() : 0;
    const std::wstring name = base::ToUtf16(failed.fileName);
    const std::wstring answer = page ? L"a web page instead of the file" : L"a refusal instead of the file";
    if (justCaptured && received == 0) {
        const std::wstring keys = PreventKeysText(snapshot->settings.captureKeys);
        std::wstring text = L"LDM could not take over this download from the browser:\n" + name + L"\n\nThe site gave LDM " +
                            answer + L". It may use links that work only once, or only in the browser that opened them.\n\n";
        text += keys.empty() ? L"To download such a file in the browser, add the site to the exceptions in Options > "
                               L"File types."
                             : L"To download such a file in the browser, hold " + keys +
                                   L" while you click its link, or add the site to the exceptions in Options > File types.";
        Tell(text, true);
        return;
    }
    if (received == 0) {
        return;  // nothing to resume; the status column says what happened
    }
    Tell(L"When LDM asked for " + name + L" again, the server sent " + answer +
             L". The site probably uses temporary links.\n\nRight-click the download and choose Refresh download "
             L"address to get a new link from the browser, or delete it and download the file again without "
             L"stopping it.",
         true);
}

void CaptureFollowUp::RefreshAddress(model::DownloadId id)
{
    const auto snapshot = context_.snapshot();
    const core::DownloadView* view = snapshot ? FindDownload(*snapshot, id) : nullptr;
    if (view == nullptr || view->download.status == model::DownloadStatus::Completed) {
        return;
    }
    const std::wstring name = base::ToUtf16(view->download.fileName);
    for (const auto& other : snapshot->downloads) {
        if (other->waitingForAddress && other->download.id != id &&
            !Ask(L"LDM is already waiting for a new address for " + base::ToUtf16(other->download.fileName) +
                 L". It waits for one download at a time.\n\nWait for " + name + L" instead?")) {
            return;
        }
    }
    Post(core::WaitForAddressCommand{id});

    std::optional<std::string> page = view->download.parentPage ? view->download.parentPage : view->download.referer;
    if (page && !core::ParseUrl(*page)) {
        page.reset();
    }
    const std::wstring then = L"Start downloading the same file again there, and LDM will offer to continue " + name +
                              L" from the new address. LDM waits for " +
                              std::to_wstring(core::kAddressWaitLimit.count()) + L" minutes.";
    if (!page) {
        Tell(L"LDM is waiting for a new address for " + name + L".\n\nOpen the page you downloaded it from in your "
                 L"browser. " + then,
             false);
        return;
    }
    Tell(L"LDM will open the web page of " + name + L" in your browser.\n\n" + then, false);
    if (!context_.openPage || !context_.openPage(*page)) {
        Tell(L"No browser is connected to LDM, so LDM could not open the page. Open it yourself:\n" +
                 base::ToUtf16(*page),
             false);
    }
}

bool CaptureFollowUp::OfferAsNewAddress(const browser::Capture& capture)
{
    const auto snapshot = context_.snapshot();
    if (!snapshot) {
        return false;
    }
    const auto waiting = std::find_if(snapshot->downloads.begin(), snapshot->downloads.end(),
                                      [](const auto& view) { return view->waitingForAddress; });
    if (waiting == snapshot->downloads.end()) {
        return false;
    }
    const core::DownloadView& view = **waiting;
    if (!IsNewAddressFor(view, CaptureName(capture), capture.fileSize)) {
        return false;
    }
    if (!Ask(L"Is this the new address for " + base::ToUtf16(view.download.fileName) +
             L", which LDM is waiting for?\n\n" + base::ToUtf16(capture.url))) {
        return false;
    }
    core::ReplaceAddressCommand command;
    command.id = view.download.id;
    command.url = capture.url;
    command.referer = capture.referrer;
    command.userAgent = capture.userAgent;
    command.cookies = capture.cookies;
    if (capture.post) {
        command.post = core::PostData{capture.post->contentType, capture.post->body};
    }
    Post(std::move(command));
    return true;
}

const core::DownloadView* CaptureFollowUp::FindDownload(const core::Snapshot& snapshot, model::DownloadId id) const
{
    for (const auto& view : snapshot.downloads) {
        if (view->download.id == id) {
            return view.get();
        }
    }
    return nullptr;
}

void CaptureFollowUp::Tell(const std::wstring& message, bool warning) const
{
    // Over whatever LDM window is in front, such as an open Download File Info dialog.
    const HWND owner = ::GetLastActivePopup(context_.owner);
    (void)ShowMessage(owner, *context_.theme,
                      MessageRequest{message, warning ? MessageKind::Warning : MessageKind::Information});
}

bool CaptureFollowUp::Ask(const std::wstring& question) const
{
    const HWND owner = ::GetLastActivePopup(context_.owner);
    return ShowMessage(owner, *context_.theme,
                       MessageRequest{question, MessageKind::Question, MessageButtons::YesNo, true}) == IDYES;
}

void CaptureFollowUp::Post(core::Command command) const
{
    if (context_.commands != nullptr) {
        context_.commands->Post(std::move(command));
    }
}

}  // namespace ldm::ui

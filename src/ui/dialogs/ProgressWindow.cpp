#include "ui/dialogs/ProgressWindow.h"

#include "base/Utf.h"
#include "resource.h"
#include "ui/Format.h"
#include "ui/Handles.h"

#include <commctrl.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace ldm::ui {

namespace {

constexpr int kStatusPage = 0;
constexpr int kSpeedPage = 1;
constexpr int kCompletionPage = 2;

struct ActionChoice {
    model::CompletionAction action;
    const wchar_t* text;
};

constexpr std::array<ActionChoice, 5> kActions{{
    {model::CompletionAction::None, L"Do nothing"},
    {model::CompletionAction::ExitApp, L"Exit Last Download Manager"},
    {model::CompletionAction::Shutdown, L"Shut down the computer"},
    {model::CompletionAction::Sleep, L"Put the computer to sleep"},
    {model::CompletionAction::Hibernate, L"Hibernate the computer"},
}};

bool IsRunning(model::DownloadStatus status)
{
    return status == model::DownloadStatus::Downloading || status == model::DownloadStatus::Stopping;
}

}  // namespace

ProgressWindow::ProgressWindow(const Theme& theme, model::DownloadId id, Callbacks callbacks)
    : Dialog(IDD_PROGRESS, theme), id_(id), callbacks_(std::move(callbacks))
{
}

void ProgressWindow::Open(HWND owner, const core::DownloadView& view, const model::Settings& settings)
{
    // Owned by nothing, so each progress window has its own taskbar button and can stay open
    // while the main window is hidden in the tray.
    (void)owner;
    CreateModeless(nullptr);
    if (Handle() == nullptr) {
        return;
    }
    // Tabs depend on the settings, so they are built here rather than in OnInit.
    const HWND tab = Item(IDC_PROG_TAB);
    const auto addTab = [&](const wchar_t* title, int page) {
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = const_cast<wchar_t*>(title);  // the tab control copies the text
        TabCtrl_InsertItem(tab, static_cast<int>(pageForTab_.size()), &item);
        pageForTab_.push_back(page);
    };
    addTab(L"Download status", kStatusPage);
    if (settings.progressShowSpeedTab) {
        addTab(L"Speed Limiter", kSpeedPage);
    }
    if (settings.progressShowCompletionTab) {
        addTab(L"Options on completion", kCompletionPage);
    }
    ShowPage(kStatusPage);
    SetChecked(IDC_PROG_SHOW_COMPLETE, settings.showCompleteDialog);
    const auto limit = view.download.speedLimitBytesPerSecond;
    SetChecked(IDC_PROG_LIMIT_ENABLE, limit.has_value());
    SetNumber(IDC_PROG_LIMIT_VALUE, limit ? std::max<std::int64_t>(*limit / 1024, 1) : 500);
    Enable(IDC_PROG_LIMIT_VALUE, limit.has_value());
    SetChecked(IDC_PROG_LIMIT_REMEMBER, true);
    for (size_t i = 0; i < kActions.size(); ++i) {
        if (kActions[i].action == view.download.completionAction) {
            ComboBox_SetCurSel(Item(IDC_PROG_ACTION), static_cast<int>(i));
        }
    }
    Update(view, settings);
    const int show = settings.progressStartView == model::ProgressStartView::Minimized ? SW_SHOWMINNOACTIVE : SW_SHOW;
    ::ShowWindow(Handle(), show);
}

bool ProgressWindow::OnInit()
{
    // In light mode the whole window takes the tab body's look, so labels on the tab pages blend
    // in. Dark mode paints its own colors through WM_CTLCOLOR*.
    if (!CurrentTheme().IsDark()) {
        ::EnableThemeDialogTexture(Handle(), ETDT_ENABLETAB);
    }
    RECT window{};
    ::GetWindowRect(Handle(), &window);
    fullHeight_ = window.bottom - window.top;
    RECT segments{};
    ::GetWindowRect(Item(IDC_PROG_SEGMENTS), &segments);
    collapsedHeight_ = segments.top - window.top;

    const HWND list = Item(IDC_PROG_CONNECTIONS);
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    const struct {
        const wchar_t* title;
        int width;
    } columns[] = {{L"N.", 40}, {L"Downloaded", 110}, {L"Info", 300}};
    for (int i = 0; i < 3; ++i) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.pszText = const_cast<wchar_t*>(columns[i].title);  // copied by the list view
        column.cx = ::MulDiv(columns[i].width, static_cast<int>(::GetDpiForWindow(Handle())), 96);
        ListView_InsertColumn(list, i, &column);
    }
    if (CurrentTheme().IsDark()) {
        ListView_SetBkColor(list, CurrentTheme().Colors().window);
        ListView_SetTextBkColor(list, CurrentTheme().Colors().window);
        ListView_SetTextColor(list, CurrentTheme().Colors().text);
    }
    ::SendMessageW(Item(IDC_PROG_BAR), PBM_SETRANGE32, 0, 1000);
    for (const ActionChoice& choice : kActions) {
        ComboBox_AddString(Item(IDC_PROG_ACTION), choice.text);
    }
    ComboBox_SetCurSel(Item(IDC_PROG_ACTION), 0);
    return false;
}

void ProgressWindow::ShowPage(int page)
{
    const auto showRange = [this](int first, int last, bool visible) {
        for (int id = first; id <= last; ++id) {
            Show(id, visible);
        }
    };
    showRange(IDC_PROG_STATUS_FIRST, IDC_PROG_STATUS_LAST, page == kStatusPage);
    Show(IDC_PROG_BAR, true);
    showRange(IDC_PROG_SPEED_FIRST, IDC_PROG_SPEED_LAST, page == kSpeedPage);
    showRange(IDC_PROG_DONE_FIRST, IDC_PROG_DONE_LAST, page == kCompletionPage);
}

void ProgressWindow::SetDetailsVisible(bool visible)
{
    detailsVisible_ = visible;
    RECT window{};
    ::GetWindowRect(Handle(), &window);
    ::SetWindowPos(Handle(), nullptr, 0, 0, window.right - window.left, visible ? fullHeight_ : collapsedHeight_,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    SetText(IDC_PROG_DETAILS, visible ? L"<< &Hide details" : L"Show &details >>");
}

void ProgressWindow::Update(const core::DownloadView& view, const model::Settings& settings)
{
    (void)settings;
    if (Handle() == nullptr) {
        return;
    }
    const bool wasRunning = latest_ && IsRunning(latest_->download.status);
    latest_ = std::make_shared<core::DownloadView>(view);
    const model::Download& d = view.download;
    updating_ = true;
    const auto fraction = ProgressFraction(view);
    std::wstring title = base::ToUtf16(d.fileName);
    if (fraction) {
        title = std::to_wstring(static_cast<int>(std::floor(*fraction * 100.0))) + L"% " + title;
    }
    ::SetWindowTextW(Handle(), title.c_str());
    SetText(IDC_PROG_URL, base::ToUtf16(d.url));
    std::wstring status = FormatStatus(view);
    if (view.retrying && d.status == model::DownloadStatus::Downloading) {
        status = L"Retrying: " + base::ToUtf16(view.retrying->message);
    } else if (d.status == model::DownloadStatus::Failed && d.lastResult) {
        status = base::ToUtf16(*d.lastResult);
    } else if (d.status == model::DownloadStatus::Downloading) {
        status = view.receivedBytes > 0 ? L"Receiving data..." : L"Connecting...";
    }
    SetText(IDC_PROG_STATUS, status);
    if (d.totalSize) {
        SetText(IDC_PROG_SIZE, FormatSize(*d.totalSize));
    } else if (view.stream && view.stream->estimatedBytes) {
        SetText(IDC_PROG_SIZE, L"about " + FormatSize(*view.stream->estimatedBytes));
    } else {
        SetText(IDC_PROG_SIZE, L"Unknown");
    }
    std::wstring downloaded = FormatSize(view.receivedBytes);
    if (fraction) {
        wchar_t percent[32] = {};
        swprintf_s(percent, L" (%.2f%%)", std::floor(*fraction * 10000.0) / 100.0);
        downloaded += percent;
    }
    SetText(IDC_PROG_DOWNLOADED, downloaded);
    SetText(IDC_PROG_RATE, FormatSpeed(view.bytesPerSecond));
    SetText(IDC_PROG_LEFT, FormatTimeLeft(view.secondsLeft));
    SetText(IDC_PROG_RESUME, FormatResumable(d.resumable));
    ::SendMessageW(Item(IDC_PROG_BAR), PBM_SETPOS, fraction ? static_cast<WPARAM>(*fraction * 1000.0) : 0, 0);
    SetText(IDC_PROG_PAUSE, IsRunning(d.status) ? L"&Pause" : L"&Start");
    Enable(IDC_PROG_PAUSE, d.status != model::DownloadStatus::Stopping && d.status != model::DownloadStatus::Finalizing);
    UpdateConnections(view);
    ::InvalidateRect(Item(IDC_PROG_SEGMENTS), nullptr, FALSE);
    updating_ = false;

    // "Remember this limit" off: the limit belongs to this run only (checklist P-04).
    if (wasRunning && !IsRunning(d.status) && !rememberLimit_ && d.speedLimitBytesPerSecond) {
        callbacks_.post(core::SetDownloadSpeedLimitCommand{id_, std::nullopt});
    }
}

void ProgressWindow::UpdateConnections(const core::DownloadView& view)
{
    const HWND list = Item(IDC_PROG_CONNECTIONS);
    const int wanted = static_cast<int>(view.connections.size());
    while (ListView_GetItemCount(list) > wanted) {
        ListView_DeleteItem(list, ListView_GetItemCount(list) - 1);
    }
    while (ListView_GetItemCount(list) < wanted) {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = ListView_GetItemCount(list);
        item.pszText = const_cast<wchar_t*>(L"");  // copied by the list view
        ListView_InsertItem(list, &item);
    }
    for (int i = 0; i < wanted; ++i) {
        const core::ConnectionView& connection = view.connections[static_cast<size_t>(i)];
        std::wstring number = std::to_wstring(i + 1);
        std::wstring received = FormatSize(connection.received);
        std::wstring info = connection.waitingToRetry ? L"Waiting to retry" : L"Receiving data...";
        ListView_SetItemText(list, i, 0, number.data());
        ListView_SetItemText(list, i, 1, received.data());
        ListView_SetItemText(list, i, 2, info.data());
    }
}

void ProgressWindow::DrawSegments(const DRAWITEMSTRUCT& item) const
{
    const Palette& colors = CurrentTheme().Colors();
    RECT area = item.rcItem;
    UniqueBrush background(::CreateSolidBrush(colors.window));
    UniqueBrush border(::CreateSolidBrush(colors.border));
    UniqueBrush durable(::CreateSolidBrush(colors.accent));
    UniqueBrush live(::CreateSolidBrush(RGB((GetRValue(colors.accent) + 255) / 2, (GetGValue(colors.accent) + 255) / 2,
                                            (GetBValue(colors.accent) + 255) / 2)));
    ::FillRect(item.hDC, &area, background.get());
    ::FrameRect(item.hDC, &area, border.get());
    if (!latest_ || !latest_->download.totalSize || *latest_->download.totalSize <= 0) {
        return;
    }
    const double total = static_cast<double>(*latest_->download.totalSize);
    const int left = area.left + 1;
    const int width = area.right - area.left - 2;
    const auto x = [&](std::int64_t offset) {
        return left + static_cast<int>(std::lround(static_cast<double>(offset) / total * width));
    };
    // Lighter: received but not yet flushed; solid: on disk. Thin lines mark segment starts, so the
    // bar shows where each connection began (checklist P-03).
    for (const core::ConnectionView& connection : latest_->connections) {
        RECT fill{x(connection.segmentStart), area.top + 1, x(connection.segmentStart + connection.received), area.bottom - 1};
        ::FillRect(item.hDC, &fill, live.get());
    }
    for (const model::Segment& segment : latest_->download.segments) {
        RECT fill{x(segment.start), area.top + 1, x(segment.start + segment.done), area.bottom - 1};
        ::FillRect(item.hDC, &fill, durable.get());
        if (segment.start > 0) {
            RECT mark{x(segment.start), area.top + 1, x(segment.start) + 1, area.bottom - 1};
            ::FillRect(item.hDC, &mark, border.get());
        }
    }
}

void ProgressWindow::ApplySpeedLimit()
{
    if (updating_) {
        return;
    }
    const bool enabled = Checked(IDC_PROG_LIMIT_ENABLE);
    Enable(IDC_PROG_LIMIT_VALUE, enabled);
    const auto kilobytes = Number(IDC_PROG_LIMIT_VALUE);
    if (enabled && (!kilobytes || *kilobytes <= 0)) {
        return;  // wait for a usable number
    }
    callbacks_.post(core::SetDownloadSpeedLimitCommand{
        id_, enabled ? std::optional<std::int64_t>(std::int64_t{*kilobytes} * 1024) : std::nullopt});
}

bool ProgressWindow::OnCommand(int id, int code, HWND control)
{
    switch (id) {
    case IDC_PROG_DETAILS:
        SetDetailsVisible(!detailsVisible_);
        return true;
    case IDC_PROG_PAUSE:
        if (latest_ && IsRunning(latest_->download.status)) {
            callbacks_.post(core::StopCommand{{id_}});
        } else {
            callbacks_.post(core::ResumeCommand{{id_}});
        }
        return true;
    case IDCANCEL:
        // the reference's Cancel stops the transfer and closes the window; the download stays in the list.
        if (latest_ && IsRunning(latest_->download.status)) {
            callbacks_.post(core::StopCommand{{id_}});
        }
        Close(IDCANCEL);
        return true;
    case IDC_PROG_LIMIT_ENABLE:
        ApplySpeedLimit();
        return true;
    case IDC_PROG_LIMIT_VALUE:
        if (code == EN_CHANGE) {
            ApplySpeedLimit();
        }
        return true;
    case IDC_PROG_LIMIT_REMEMBER:
        rememberLimit_ = Checked(IDC_PROG_LIMIT_REMEMBER);
        return true;
    case IDC_PROG_SHOW_COMPLETE: {
        const bool show = Checked(IDC_PROG_SHOW_COMPLETE);
        callbacks_.post(core::ModifySettingsCommand{[show](model::Settings& s) { s.showCompleteDialog = show; }});
        return true;
    }
    case IDC_PROG_ACTION:
        if (code == CBN_SELCHANGE) {
            const int index = ComboBox_GetCurSel(Item(IDC_PROG_ACTION));
            if (index >= 0 && static_cast<size_t>(index) < kActions.size()) {
                core::UpdateDownloadCommand update;
                update.id = id_;
                update.completionAction = kActions[static_cast<size_t>(index)].action;
                callbacks_.post(update);
            }
        }
        return true;
    default:
        return Dialog::OnCommand(id, code, control);
    }
}

bool ProgressWindow::OnNotify(const NMHDR& header, LRESULT& result)
{
    if (header.idFrom == IDC_PROG_TAB && header.code == TCN_SELCHANGE) {
        const int index = TabCtrl_GetCurSel(Item(IDC_PROG_TAB));
        if (index >= 0 && static_cast<size_t>(index) < pageForTab_.size()) {
            ShowPage(pageForTab_[static_cast<size_t>(index)]);
        }
        result = 0;
        return true;
    }
    return false;
}

bool ProgressWindow::OnMessage(UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result)
{
    if (message == WM_DRAWITEM && wParam == IDC_PROG_SEGMENTS) {
        DrawSegments(*reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
        result = TRUE;
        return true;
    }
    if (message == WM_CLOSE) {
        Close(IDCANCEL);  // closing the window keeps the download running
        result = 0;
        return true;
    }
    return false;
}

void ProgressWindow::OnDestroyed()
{
    if (!rememberLimit_ && latest_ && latest_->download.speedLimitBytesPerSecond) {
        callbacks_.post(core::SetDownloadSpeedLimitCommand{id_, std::nullopt});
    }
    // The callback deletes this object, so it runs from a copy that outlives it.
    const auto closed = callbacks_.closed;
    const model::DownloadId id = id_;
    if (closed) {
        closed(id);
    }
}

}  // namespace ldm::ui

#include "ui/dialogs/SchedulerWindow.h"

#include "base/Utf.h"
#include "resource.h"
#include "ui/FileDialogs.h"
#include "ui/Format.h"
#include "ui/ScheduleFields.h"
#include "ui/dialogs/MessageDialog.h"
#include "ui/dialogs/QueueDialogs.h"

#include <commctrl.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <filesystem>

namespace ldm::ui {

namespace {

enum Page {
    kSchedulePage,
    kFilesPage,
    kLimitsPage,
};

struct ActionChoice {
    model::CompletionAction action;
    const wchar_t* text;
};

constexpr std::array<ActionChoice, 5> kActions{{
    {model::CompletionAction::None, L"Do nothing"},
    {model::CompletionAction::ExitApp, L"Exit LDM"},
    {model::CompletionAction::Shutdown, L"Turn off the computer"},
    {model::CompletionAction::Sleep, L"Put the computer to sleep"},
    {model::CompletionAction::Hibernate, L"Hibernate the computer"},
}};

struct ProjectModeChoice {
    model::GrabberScheduleMode mode;
    const wchar_t* text;
};

constexpr std::array<ProjectModeChoice, 3> kProjectModes{{
    {model::GrabberScheduleMode::Explore, L"Explore the site"},
    {model::GrabberScheduleMode::ExploreDownload, L"Explore, then download the checked files"},
    {model::GrabberScheduleMode::DownloadChecked, L"Download the checked files"},
}};

constexpr int kDefaultRepeatMinutes = 60;
constexpr int kMaxQuotaMegabytes = 10'000'000;
constexpr int kMaxQuotaHours = 23;

SYSTEMTIME LocalNow()
{
    SYSTEMTIME now{};
    ::GetLocalTime(&now);
    return now;
}

std::int64_t Today()
{
    const SYSTEMTIME now = LocalNow();
    return DaysFromDate(CalendarDate{now.wYear, now.wMonth, now.wDay});
}

void SetTimeOfDay(HWND picker, int minutes)
{
    SYSTEMTIME time = LocalNow();
    time.wHour = static_cast<WORD>(minutes / 60);
    time.wMinute = static_cast<WORD>(minutes % 60);
    time.wSecond = 0;
    time.wMilliseconds = 0;
    DateTime_SetSystemtime(picker, GDT_VALID, &time);
}

int TimeOfDay(HWND picker)
{
    SYSTEMTIME time{};
    if (DateTime_GetSystemtime(picker, &time) != GDT_VALID) {
        return 0;
    }
    return time.wHour * 60 + time.wMinute;
}

void SetDay(HWND picker, std::int64_t days)
{
    const CalendarDate date = DateFromDays(days);
    SYSTEMTIME time{};
    time.wYear = static_cast<WORD>(date.year);
    time.wMonth = static_cast<WORD>(date.month);
    time.wDay = static_cast<WORD>(date.day);
    DateTime_SetSystemtime(picker, GDT_VALID, &time);
}

std::int64_t Day(HWND picker)
{
    SYSTEMTIME time{};
    if (DateTime_GetSystemtime(picker, &time) != GDT_VALID) {
        return Today();
    }
    return DaysFromDate(CalendarDate{time.wYear, time.wMonth, time.wDay});
}

std::wstring Trimmed(const std::wstring& text)
{
    const auto first = text.find_first_not_of(L" \t");
    if (first == std::wstring::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(L" \t") - first + 1);
}

bool SameQuota(const model::DownloadQuota& a, const model::DownloadQuota& b)
{
    return a.enabled == b.enabled && a.megabytes == b.megabytes && a.hours == b.hours &&
           a.warnBeforeStopping == b.warnBeforeStopping;
}

}  // namespace

SchedulerWindow::SchedulerWindow(const Theme& theme, Callbacks callbacks)
    : Dialog(IDD_SCHEDULER, theme), callbacks_(std::move(callbacks))
{
}

void SchedulerWindow::Open(HWND owner, std::shared_ptr<const core::Snapshot> snapshot, std::optional<model::QueueId> select)
{
    snapshot_ = std::move(snapshot);
    if (!snapshot_ || snapshot_->queues.empty()) {
        return;
    }
    chosen_ = select.value_or(snapshot_->queues.front().queue.id);
    CreateModeless(owner);
    if (Handle() == nullptr) {
        return;
    }
    const HWND tab = Item(IDC_SCHED_TAB);
    for (const wchar_t* title : {L"Schedule", L"Files in the queue", L"Download limits"}) {
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = const_cast<wchar_t*>(title);  // the tab control copies the text
        TabCtrl_InsertItem(tab, TabCtrl_GetItemCount(tab), &item);
    }
    RefreshQueueList();
    if (const core::QueueView* view = Chosen()) {
        LoadQueue(*view);
    }
    LoadQuota();
    ShowPage(kSchedulePage);
    ::ShowWindow(Handle(), SW_SHOW);
}

void SchedulerWindow::Activate(std::optional<model::QueueId> select)
{
    if (Handle() == nullptr) {
        return;
    }
    if (::IsIconic(Handle())) {
        ::ShowWindow(Handle(), SW_RESTORE);
    }
    ::SetForegroundWindow(Handle());
    if (select) {
        Choose(*select);
    }
}

bool SchedulerWindow::OnInit()
{
    // In light mode the whole window takes the tab body's look, so labels on the pages blend in.
    if (!CurrentTheme().IsDark()) {
        ::EnableThemeDialogTexture(Handle(), ETDT_ENABLETAB);
    }
    for (const ActionChoice& choice : kActions) {
        ComboBox_AddString(Item(IDC_SCHED_ACTION), choice.text);
    }
    for (const ProjectModeChoice& choice : kProjectModes) {
        ComboBox_AddString(Item(IDC_SCHED_PROJECT_MODE), choice.text);
    }
    // Schedules count in minutes, so the times show no seconds.
    DateTime_SetFormat(Item(IDC_SCHED_START_TIME), L"HH:mm");
    DateTime_SetFormat(Item(IDC_SCHED_STOP_TIME), L"HH:mm");
    for (const int id : {IDC_SCHED_REPEAT_MINUTES, IDC_SCHED_CONCURRENT, IDC_SCHED_RETRIES, IDC_SCHED_QUOTA_HOURS}) {
        ::SendMessageW(Item(id), EM_LIMITTEXT, 4, 0);
    }
    ::SendMessageW(Item(IDC_SCHED_QUOTA_MB), EM_LIMITTEXT, 8, 0);

    const HWND files = Item(IDC_SCHED_FILES);
    ListView_SetExtendedListViewStyle(files, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    const struct {
        const wchar_t* title;
        int width;
    } columns[] = {{L"File name", 190}, {L"Size", 75}, {L"Status", 110}};
    const int dpi = static_cast<int>(::GetDpiForWindow(Handle()));
    for (int i = 0; i < 3; ++i) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.pszText = const_cast<wchar_t*>(columns[i].title);  // copied by the list view
        column.cx = ::MulDiv(columns[i].width, dpi, 96);
        ListView_InsertColumn(files, i, &column);
    }
    if (CurrentTheme().IsDark()) {
        ListView_SetBkColor(files, CurrentTheme().Colors().window);
        ListView_SetTextBkColor(files, CurrentTheme().Colors().window);
        ListView_SetTextColor(files, CurrentTheme().Colors().text);
    }

    const std::optional<bool> wake = callbacks_.wakeTimersAllowed ? callbacks_.wakeTimersAllowed() : std::nullopt;
    SetText(IDC_SCHED_WAKE_NOTE,
            wake == false ? L"Windows does not let programs wake the computer from sleep, so a sleeping computer stays "
                            L"asleep at these times. Power Options > Sleep > Allow wake timers changes that."
                          : L"");
    return false;
}

void SchedulerWindow::ShowPage(int page)
{
    const auto showRange = [this](int first, int last, bool visible) {
        for (int id = first; id <= last; ++id) {
            Show(id, visible);
        }
    };
    showRange(IDC_SCHED_PAGE_FIRST, IDC_SCHED_PAGE_LAST, page == kSchedulePage);
    showRange(IDC_SCHED_FILES_FIRST, IDC_SCHED_FILES_LAST, page == kFilesPage);
    showRange(IDC_SCHED_LIMITS_FIRST, IDC_SCHED_LIMITS_LAST, page == kLimitsPage);
    // A project has its schedule only: no start with LDM and no completion action, but what it
    // does when its time comes.
    const bool project = chosenProject_.has_value();
    if (project && page == kSchedulePage) {
        for (const int id : {IDC_SCHED_ON_STARTUP, IDC_SCHED_ACTION_LABEL, IDC_SCHED_ACTION, IDC_SCHED_FORCE,
                             IDC_SCHED_OPEN_ON, IDC_SCHED_OPEN_FILE, IDC_SCHED_OPEN_BROWSE}) {
            Show(id, false);
        }
    }
    Show(IDC_SCHED_PROJECT_MODE_LABEL, project && page == kSchedulePage);
    Show(IDC_SCHED_PROJECT_MODE, project && page == kSchedulePage);
    TabCtrl_SetCurSel(Item(IDC_SCHED_TAB), page);
}

const core::QueueView* SchedulerWindow::Chosen() const
{
    if (!snapshot_ || !chosen_) {
        return nullptr;
    }
    const auto it = std::find_if(snapshot_->queues.begin(), snapshot_->queues.end(),
                                 [this](const core::QueueView& view) { return view.queue.id == *chosen_; });
    return it == snapshot_->queues.end() ? nullptr : &*it;
}

std::vector<model::DownloadId> SchedulerWindow::Members() const
{
    std::vector<std::pair<std::int64_t, model::DownloadId>> members;
    if (snapshot_ && chosen_) {
        for (const auto& view : snapshot_->downloads) {
            if (view->download.queueId == chosen_) {
                members.emplace_back(view->download.queuePosition, view->download.id);
            }
        }
    }
    std::stable_sort(members.begin(), members.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<model::DownloadId> ids;
    for (const auto& [position, id] : members) {
        ids.push_back(id);
    }
    return ids;
}

std::vector<model::DownloadId> SchedulerWindow::SelectedFiles() const
{
    std::vector<model::DownloadId> ids;
    const HWND files = Item(IDC_SCHED_FILES);
    if (files == nullptr) {
        return ids;  // the window is gone; a missing list would answer row 0 forever
    }
    for (int row = ListView_GetNextItem(files, -1, LVNI_SELECTED); row >= 0; row = ListView_GetNextItem(files, row, LVNI_SELECTED)) {
        if (static_cast<size_t>(row) < files_.size()) {
            ids.push_back(files_[static_cast<size_t>(row)]);
        }
    }
    return ids;
}

void SchedulerWindow::RefreshQueueList()
{
    if (!snapshot_) {
        return;
    }
    std::vector<std::wstring> texts;
    std::vector<Row> rows;
    for (const core::QueueView& view : snapshot_->queues) {
        texts.push_back(base::ToUtf16(view.queue.name) + (view.running ? L"  (running)" : L""));
        rows.push_back(Row{view.queue.id, std::nullopt});
    }
    if (projects_ && callbacks_.postGrabber) {
        for (const grabber::GrabberProjectView& view : projects_->projects) {
            if (view.project.isTemplate || view.damaged) {
                continue;
            }
            texts.push_back(L"Grabber: " + base::ToUtf16(view.project.name) +
                            (view.exploring || view.downloading ? L"  (running)" : L""));
            rows.push_back(Row{std::nullopt, view.project.id});
        }
    }
    const HWND list = Item(IDC_SCHED_QUEUES);
    if (texts != listTexts_ || rows != listed_) {
        ListBox_ResetContent(list);
        for (const std::wstring& text : texts) {
            ListBox_AddString(list, text.c_str());
        }
        listTexts_ = std::move(texts);
        listed_ = std::move(rows);
    }
    const Row current = chosenProject_ ? Row{std::nullopt, chosenProject_} : Row{chosen_, std::nullopt};
    const auto row = std::find(listed_.begin(), listed_.end(), current);
    ListBox_SetCurSel(list, row == listed_.end() ? -1 : static_cast<int>(row - listed_.begin()));
}

void SchedulerWindow::RefreshFiles()
{
    const HWND list = Item(IDC_SCHED_FILES);
    const std::vector<model::DownloadId> members = Members();
    if (members != files_) {
        const std::vector<model::DownloadId> selected = SelectedFiles();
        ListView_DeleteAllItems(list);
        files_ = members;
        for (size_t row = 0; row < files_.size(); ++row) {
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = static_cast<int>(row);
            item.pszText = const_cast<wchar_t*>(L"");  // set below with the other columns
            ListView_InsertItem(list, &item);
            if (std::find(selected.begin(), selected.end(), files_[row]) != selected.end()) {
                ListView_SetItemState(list, static_cast<int>(row), LVIS_SELECTED, LVIS_SELECTED);
            }
        }
    }
    if (!snapshot_) {
        return;
    }
    for (size_t row = 0; row < files_.size(); ++row) {
        const auto view = std::find_if(snapshot_->downloads.begin(), snapshot_->downloads.end(),
                                       [&](const auto& v) { return v->download.id == files_[row]; });
        if (view == snapshot_->downloads.end()) {
            continue;
        }
        const std::wstring texts[] = {base::ToUtf16((*view)->download.fileName), FormatSizeColumn(**view),
                                      FormatStatus(**view)};
        for (int column = 0; column < 3; ++column) {
            wchar_t current[512] = {};
            ListView_GetItemText(list, static_cast<int>(row), column, current, static_cast<int>(std::size(current)));
            if (texts[column] != current) {
                ListView_SetItemText(list, static_cast<int>(row), column, const_cast<wchar_t*>(texts[column].c_str()));
            }
        }
    }
}

void SchedulerWindow::RefreshButtons()
{
    if (chosenProject_) {
        const grabber::GrabberProjectView* project = ChosenProject();
        const bool running = project != nullptr && (project->exploring || project->downloading);
        Enable(IDC_SCHED_START_NOW, project != nullptr && !running);
        Enable(IDC_SCHED_STOP, running);
        Enable(IDC_SCHED_DELETE, false);
        Enable(IDC_SCHED_APPLY, edited_);
        return;
    }
    const core::QueueView* view = Chosen();
    Enable(IDC_SCHED_START_NOW, view != nullptr && !view->running);
    Enable(IDC_SCHED_STOP, view != nullptr && view->running);
    Enable(IDC_SCHED_DELETE, view != nullptr && !view->queue.IsBuiltin());
    Enable(IDC_SCHED_APPLY, edited_);
    const bool anySelected = !SelectedFiles().empty();
    Enable(IDC_SCHED_UP, anySelected);
    Enable(IDC_SCHED_DOWN, anySelected);
    Enable(IDC_SCHED_REMOVE, anySelected);
}

void SchedulerWindow::ShowKind(bool project)
{
    SetText(IDC_SCHED_START_AT, project ? L"Start the project &at" : L"Start the queue &at");
    SetText(IDC_SCHED_STOP_AT, project ? L"&Stop the project at" : L"&Stop the queue at");
    // A project has only the schedule page; a queue keeps the page it was on.
    ShowPage(project ? kSchedulePage : TabCtrl_GetCurSel(Item(IDC_SCHED_TAB)));
}

void SchedulerWindow::LoadSchedule(const model::Schedule& schedule)
{
    SetChecked(IDC_SCHED_ON_STARTUP, schedule.startOnAppStart);
    SetChecked(IDC_SCHED_START_AT, schedule.enabled);
    SetTimeOfDay(Item(IDC_SCHED_START_TIME), schedule.startMinutes);
    const bool once = schedule.mode == model::ScheduleMode::Once;
    SetChecked(IDC_SCHED_ONCE, once);
    SetChecked(IDC_SCHED_DAILY, !once);
    SetDay(Item(IDC_SCHED_DATE), schedule.startDay.value_or(Today()));
    for (int day = 0; day < 7; ++day) {
        SetChecked(IDC_SCHED_DAY_FIRST + day, (schedule.weekdays & (1u << day)) != 0);
    }
    SetChecked(IDC_SCHED_REPEAT, schedule.repeatMinutes > 0);
    SetNumber(IDC_SCHED_REPEAT_MINUTES, schedule.repeatMinutes > 0 ? schedule.repeatMinutes : kDefaultRepeatMinutes);
    SetChecked(IDC_SCHED_STOP_AT, schedule.stopEnabled);
    SetTimeOfDay(Item(IDC_SCHED_STOP_TIME), schedule.stopMinutes);
}

void SchedulerWindow::LoadProject(const grabber::GrabberProjectView& view)
{
    loading_ = true;
    ShowKind(true);
    LoadSchedule(view.project.schedule);
    const auto mode = std::find_if(kProjectModes.begin(), kProjectModes.end(),
                                   [&view](const ProjectModeChoice& c) { return c.mode == view.project.scheduleMode; });
    ComboBox_SetCurSel(Item(IDC_SCHED_PROJECT_MODE), mode == kProjectModes.end() ? 0 : static_cast<int>(mode - kProjectModes.begin()));
    loadedProject_ = view.project;
    loaded_.reset();
    loading_ = false;
    edited_ = false;
    EnableScheduleFields();
    RefreshButtons();
}

const grabber::GrabberProjectView* SchedulerWindow::ChosenProject() const
{
    if (!projects_ || !chosenProject_) {
        return nullptr;
    }
    for (const grabber::GrabberProjectView& view : projects_->projects) {
        if (view.project.id == *chosenProject_) {
            return &view;
        }
    }
    return nullptr;
}

void SchedulerWindow::LoadQueue(const core::QueueView& view)
{
    loading_ = true;
    ShowKind(false);
    const model::Queue& queue = view.queue;
    LoadSchedule(queue.schedule);
    const auto action = std::find_if(kActions.begin(), kActions.end(),
                                     [&queue](const ActionChoice& c) { return c.action == queue.completionAction; });
    ComboBox_SetCurSel(Item(IDC_SCHED_ACTION), action == kActions.end() ? 0 : static_cast<int>(action - kActions.begin()));
    SetChecked(IDC_SCHED_FORCE, queue.forceTerminate);
    SetChecked(IDC_SCHED_OPEN_ON, !queue.openFileWhenDone.empty());
    SetText(IDC_SCHED_OPEN_FILE, base::ToUtf16(queue.openFileWhenDone));
    SetNumber(IDC_SCHED_CONCURRENT, queue.concurrent);
    SetNumber(IDC_SCHED_RETRIES, queue.retriesPerFile);
    loaded_ = queue;
    loadedProject_.reset();
    loading_ = false;
    edited_ = false;
    EnableScheduleFields();
    RefreshFiles();
    RefreshButtons();
}

void SchedulerWindow::LoadQuota()
{
    if (!snapshot_) {
        return;
    }
    loading_ = true;
    const model::DownloadQuota& quota = snapshot_->settings.quota;
    SetChecked(IDC_SCHED_QUOTA_ON, quota.enabled);
    SetNumber(IDC_SCHED_QUOTA_MB, quota.megabytes);
    SetNumber(IDC_SCHED_QUOTA_HOURS, quota.hours);
    SetChecked(IDC_SCHED_QUOTA_WARN, quota.warnBeforeStopping);
    loadedQuota_ = quota;
    loading_ = false;
    EnableScheduleFields();
}

void SchedulerWindow::EnableScheduleFields()
{
    const bool startAt = Checked(IDC_SCHED_START_AT);
    const bool once = Checked(IDC_SCHED_ONCE);
    Enable(IDC_SCHED_START_TIME, startAt);
    Enable(IDC_SCHED_ONCE, startAt);
    Enable(IDC_SCHED_DAILY, startAt);
    Enable(IDC_SCHED_DATE, startAt && once);
    for (int id = IDC_SCHED_DAY_FIRST; id <= IDC_SCHED_DAY_LAST; ++id) {
        Enable(id, startAt && !once);
    }
    Enable(IDC_SCHED_REPEAT, startAt && !once);
    Enable(IDC_SCHED_REPEAT_MINUTES, startAt && !once && Checked(IDC_SCHED_REPEAT));
    Enable(IDC_SCHED_STOP_TIME, Checked(IDC_SCHED_STOP_AT));
    const int action = ComboBox_GetCurSel(Item(IDC_SCHED_ACTION));
    Enable(IDC_SCHED_FORCE, action >= 0 && kActions[static_cast<size_t>(action)].action == model::CompletionAction::Shutdown);
    Enable(IDC_SCHED_OPEN_FILE, Checked(IDC_SCHED_OPEN_ON));
    Enable(IDC_SCHED_OPEN_BROWSE, Checked(IDC_SCHED_OPEN_ON));
    const bool quota = Checked(IDC_SCHED_QUOTA_ON);
    for (const int id : {IDC_SCHED_QUOTA_MB, IDC_SCHED_QUOTA_HOURS, IDC_SCHED_QUOTA_WARN}) {
        Enable(id, quota);
    }
}

void SchedulerWindow::MarkEdited()
{
    if (loading_ || edited_) {
        return;
    }
    edited_ = true;
    RefreshButtons();
}

bool SchedulerWindow::ReadSchedule(model::Schedule& schedule)
{
    const model::Schedule saved = schedule;  // the caller passes the saved one in
    const auto fail = [this](int control, const wchar_t* message) {
        ShowPage(kSchedulePage);
        Warn(message);
        ::SetFocus(Item(control));
        return false;
    };
    schedule.startOnAppStart = Checked(IDC_SCHED_ON_STARTUP);
    schedule.enabled = Checked(IDC_SCHED_START_AT);
    schedule.mode = Checked(IDC_SCHED_ONCE) ? model::ScheduleMode::Once : model::ScheduleMode::Daily;
    schedule.startMinutes = TimeOfDay(Item(IDC_SCHED_START_TIME));
    schedule.startDay = Day(Item(IDC_SCHED_DATE));
    std::uint8_t weekdays = 0;
    for (int day = 0; day < 7; ++day) {
        if (Checked(IDC_SCHED_DAY_FIRST + day)) {
            weekdays = static_cast<std::uint8_t>(weekdays | (1u << day));
        }
    }
    schedule.weekdays = weekdays;
    const bool daily = schedule.mode == model::ScheduleMode::Daily;
    if (schedule.enabled && daily && weekdays == 0) {
        return fail(IDC_SCHED_DAY_FIRST, L"Choose at least one day of the week.");
    }
    // A single start that already happened stays as it is when other settings change; only a new or
    // changed one has to lie ahead.
    const bool sameSingleStart = saved.enabled && saved.mode == model::ScheduleMode::Once &&
                                 saved.startDay == schedule.startDay && saved.startMinutes == schedule.startMinutes;
    if (schedule.enabled && !daily && !sameSingleStart) {
        const SYSTEMTIME now = LocalNow();
        const std::int64_t today = Today();
        const int minutes = now.wHour * 60 + now.wMinute;
        if (*schedule.startDay < today || (*schedule.startDay == today && schedule.startMinutes <= minutes)) {
            return fail(IDC_SCHED_DATE, L"That day and time have passed. Choose a later one.");
        }
    }
    schedule.repeatMinutes = 0;
    if (daily && Checked(IDC_SCHED_REPEAT)) {
        const auto repeat = Number(IDC_SCHED_REPEAT_MINUTES);
        if (!repeat || *repeat < 1 || *repeat > model::kMinutesPerDay) {
            return fail(IDC_SCHED_REPEAT_MINUTES, L"The schedule can start again every 1 to 1440 minutes.");
        }
        schedule.repeatMinutes = *repeat;
    }
    schedule.stopEnabled = Checked(IDC_SCHED_STOP_AT);
    schedule.stopMinutes = TimeOfDay(Item(IDC_SCHED_STOP_TIME));
    return true;
}

bool SchedulerWindow::ApplyProject()
{
    const grabber::GrabberProjectView* view = ChosenProject();
    if (view == nullptr || !callbacks_.postGrabber) {
        return false;
    }
    model::GrabberProject project = view->project;
    if (!ReadSchedule(project.schedule)) {
        return false;
    }
    project.schedule.startOnAppStart = false;  // projects start by time only
    const int mode = ComboBox_GetCurSel(Item(IDC_SCHED_PROJECT_MODE));
    project.scheduleMode = mode >= 0 ? kProjectModes[static_cast<size_t>(mode)].mode : model::GrabberScheduleMode::Explore;
    core::GrabberSaveProjectCommand save;
    save.project = project;
    callbacks_.postGrabber(std::move(save));
    loadedProject_ = project;
    edited_ = false;
    RefreshButtons();
    return true;
}

bool SchedulerWindow::Apply()
{
    if (chosenProject_) {
        return ApplyProject();
    }
    const core::QueueView* view = Chosen();
    if (view == nullptr || !snapshot_) {
        return false;
    }
    const auto fail = [this](int page, int control, const wchar_t* message) {
        ShowPage(page);
        Warn(message);
        ::SetFocus(Item(control));
        return false;
    };
    model::Queue queue = view->queue;
    const auto concurrent = Number(IDC_SCHED_CONCURRENT);
    if (!concurrent || *concurrent < 1 || *concurrent > model::kMaxQueueConcurrent) {
        return fail(kFilesPage, IDC_SCHED_CONCURRENT, L"A queue downloads 1 to 16 files at the same time.");
    }
    const auto retries = Number(IDC_SCHED_RETRIES);
    if (!retries || *retries < 0 || *retries > model::kMaxQueueRetries) {
        return fail(kFilesPage, IDC_SCHED_RETRIES, L"A queue tries a failed file again 0 to 99 times.");
    }
    queue.concurrent = *concurrent;
    queue.retriesPerFile = *retries;

    if (!ReadSchedule(queue.schedule)) {
        return false;
    }

    const int action = ComboBox_GetCurSel(Item(IDC_SCHED_ACTION));
    queue.completionAction = action >= 0 ? kActions[static_cast<size_t>(action)].action : model::CompletionAction::None;
    queue.forceTerminate = queue.completionAction == model::CompletionAction::Shutdown && Checked(IDC_SCHED_FORCE);
    queue.openFileWhenDone.clear();
    if (Checked(IDC_SCHED_OPEN_ON)) {
        const std::wstring file = Trimmed(Text(IDC_SCHED_OPEN_FILE));
        if (file.empty() || !std::filesystem::path(file).is_absolute()) {
            return fail(kSchedulePage, IDC_SCHED_OPEN_FILE, L"Choose the program or document to open, with its full path.");
        }
        queue.openFileWhenDone = base::ToUtf8(file);
    }

    model::DownloadQuota quota = snapshot_->settings.quota;
    quota.enabled = Checked(IDC_SCHED_QUOTA_ON);
    quota.warnBeforeStopping = Checked(IDC_SCHED_QUOTA_WARN);
    const auto megabytes = Number(IDC_SCHED_QUOTA_MB);
    const auto hours = Number(IDC_SCHED_QUOTA_HOURS);
    if (quota.enabled && (!megabytes || *megabytes < 1 || *megabytes > kMaxQuotaMegabytes)) {
        return fail(kLimitsPage, IDC_SCHED_QUOTA_MB, L"The download quota must be between 1 and 10,000,000 MB.");
    }
    if (quota.enabled && (!hours || *hours < 1 || *hours > kMaxQuotaHours)) {
        return fail(kLimitsPage, IDC_SCHED_QUOTA_HOURS, L"The quota period must be between 1 and 23 hours.");
    }
    if (megabytes && *megabytes >= 1 && *megabytes <= kMaxQuotaMegabytes) {
        quota.megabytes = *megabytes;
    }
    if (hours && *hours >= 1 && *hours <= kMaxQuotaHours) {
        quota.hours = *hours;
    }

    callbacks_.post(core::SaveQueueCommand{queue, {}});
    if (!SameQuota(quota, snapshot_->settings.quota)) {
        callbacks_.post(core::ModifySettingsCommand{[quota](model::Settings& s) { s.quota = quota; }});
    }
    loaded_ = queue;
    loadedQuota_ = quota;
    edited_ = false;
    RefreshButtons();
    return true;
}

bool SchedulerWindow::SettleChanges()
{
    if (!edited_) {
        return true;
    }
    const core::QueueView* view = chosenProject_ ? nullptr : Chosen();
    const grabber::GrabberProjectView* project = ChosenProject();
    const std::wstring name = project != nullptr ? base::ToUtf16(project->project.name)
                              : view != nullptr  ? base::ToUtf16(view->queue.name)
                                                 : std::wstring(L"the queue");
    const int answer = ShowMessage(Handle(), CurrentTheme(),
                                   MessageRequest{L"Apply the changes to \"" + name + L"\" first?", MessageKind::Question,
                                                  MessageButtons::YesNoCancel});
    if (Handle() == nullptr) {
        return false;  // the window was closed while the question was open (LDM exiting, say)
    }
    if (answer == IDCANCEL) {
        return false;
    }
    if (answer == IDYES) {
        return Apply();
    }
    // No: the fields go back to what is saved, so what was refused is neither shown nor saved
    // later with another change. Looked up again: a newer snapshot came while the question was open.
    edited_ = false;
    if (chosenProject_) {
        if (const grabber::GrabberProjectView* current = ChosenProject()) {
            LoadProject(*current);
        }
    } else if (const core::QueueView* current = Chosen()) {
        LoadQueue(*current);
    }
    LoadQuota();
    RefreshButtons();
    return true;
}

bool SchedulerWindow::ChooseProject(model::GrabberProjectId project)
{
    if (chosenProject_ == project) {
        RefreshQueueList();
        return true;
    }
    if (!SettleChanges()) {
        if (Handle() != nullptr) {
            RefreshQueueList();
        }
        return false;
    }
    chosenProject_ = project;
    RefreshQueueList();
    if (const grabber::GrabberProjectView* view = ChosenProject()) {
        LoadProject(*view);
    }
    return true;
}

bool SchedulerWindow::Choose(model::QueueId queue)
{
    if (chosen_ == queue && !chosenProject_) {
        RefreshQueueList();
        return true;
    }
    if (!SettleChanges()) {
        if (Handle() != nullptr) {
            RefreshQueueList();  // the list box shows the queue the fields belong to again
        }
        return false;
    }
    chosenProject_.reset();
    chosen_ = queue;
    RefreshQueueList();
    if (const core::QueueView* view = Chosen()) {
        LoadQueue(*view);
    }
    LoadQuota();
    return true;
}

void SchedulerWindow::Update(std::shared_ptr<const core::Snapshot> snapshot)
{
    if (Handle() == nullptr || !snapshot) {
        return;
    }
    snapshot_ = std::move(snapshot);
    if (selectWhenAdded_) {
        const auto added = std::find_if(snapshot_->queues.begin(), snapshot_->queues.end(),
                                        [this](const core::QueueView& view) { return view.queue.name == *selectWhenAdded_; });
        if (added != snapshot_->queues.end()) {
            selectWhenAdded_.reset();
            Choose(added->queue.id);
        }
    }
    if (chosenProject_) {
        RefreshQueueList();
        return;  // the projects follow UpdateProjects
    }
    const core::QueueView* view = Chosen();
    if (view == nullptr && !snapshot_->queues.empty()) {
        // The chosen queue was deleted: its edits go with it.
        edited_ = false;
        chosen_ = snapshot_->queues.front().queue.id;
        RefreshQueueList();
        LoadQueue(snapshot_->queues.front());
        return;
    }
    RefreshQueueList();
    if (view != nullptr && !edited_ && loaded_ != view->queue) {
        LoadQueue(*view);  // saved here or changed elsewhere, such as when its schedule fired
    }
    if (!edited_ && loadedQuota_ && !SameQuota(*loadedQuota_, snapshot_->settings.quota)) {
        LoadQuota();
    }
    RefreshFiles();
    RefreshButtons();
}

void SchedulerWindow::UpdateProjects(std::shared_ptr<const grabber::GrabberSnapshot> projects)
{
    projects_ = std::move(projects);
    if (Handle() == nullptr) {
        return;
    }
    RefreshQueueList();
    if (!chosenProject_) {
        return;
    }
    const grabber::GrabberProjectView* view = ChosenProject();
    if (view == nullptr) {
        // The project was deleted: back to the first queue.
        edited_ = false;
        chosenProject_.reset();
        if (snapshot_ && !snapshot_->queues.empty()) {
            chosen_ = snapshot_->queues.front().queue.id;
            RefreshQueueList();
            LoadQueue(snapshot_->queues.front());
        }
        return;
    }
    if (!edited_ && loadedProject_ &&
        (loadedProject_->schedule != view->project.schedule || loadedProject_->scheduleMode != view->project.scheduleMode)) {
        LoadProject(*view);
    }
    RefreshButtons();
}

void SchedulerWindow::MoveSelectedFiles(int step)
{
    std::vector<model::DownloadId> order = Members();
    const std::vector<model::DownloadId> selected = SelectedFiles();
    const auto isSelected = [&selected](model::DownloadId id) {
        return std::find(selected.begin(), selected.end(), id) != selected.end();
    };
    // Each selected file passes the unselected one before or after it; a block moves as one.
    if (step < 0) {
        for (size_t i = 1; i < order.size(); ++i) {
            if (isSelected(order[i]) && !isSelected(order[i - 1])) {
                std::swap(order[i], order[i - 1]);
            }
        }
    } else {
        for (size_t i = order.size(); i-- > 1;) {
            if (isSelected(order[i - 1]) && !isSelected(order[i])) {
                std::swap(order[i], order[i - 1]);
            }
        }
    }
    if (chosen_ && order != Members()) {
        callbacks_.post(core::ReorderQueueCommand{*chosen_, std::move(order)});
    }
}

void SchedulerWindow::RemoveSelectedFiles()
{
    std::vector<model::DownloadId> selected = SelectedFiles();
    if (!selected.empty()) {
        callbacks_.post(core::SetQueueCommand{std::move(selected), std::nullopt});
    }
}

void SchedulerWindow::CreateQueue()
{
    std::vector<std::wstring> taken;
    if (snapshot_) {
        for (const core::QueueView& view : snapshot_->queues) {
            taken.push_back(base::ToUtf16(view.queue.name));
        }
    }
    QueueNameDialog dialog(CurrentTheme(), std::move(taken));
    if (const auto name = dialog.Run(Handle())) {
        model::Queue queue;
        queue.name = *name;
        selectWhenAdded_ = *name;
        callbacks_.post(core::SaveQueueCommand{std::move(queue), {}});
    }
}

void SchedulerWindow::DeleteQueue()
{
    const core::QueueView* view = Chosen();
    if (view == nullptr || view->queue.IsBuiltin()) {
        return;
    }
    // Copied: a snapshot can replace the queue while the question is open.
    const model::QueueId id = view->queue.id;
    const std::wstring name = base::ToUtf16(view->queue.name);
    const int answer = ShowMessage(Handle(), CurrentTheme(),
                                   MessageRequest{L"Delete the queue \"" + name +
                                                      L"\"? Its downloads stay in the list, outside any queue.",
                                                  MessageKind::Question, MessageButtons::YesNo});
    if (answer == IDYES) {
        edited_ = false;
        callbacks_.post(core::DeleteQueueCommand{id});
    }
}

bool SchedulerWindow::OnCommand(int id, int code, HWND control)
{
    switch (id) {
    case IDOK:  // Enter applies; the window stays open
        Apply();
        return true;
    case IDCANCEL:
        if (SettleChanges()) {
            Close(IDCANCEL);
        }
        return true;
    case IDC_SCHED_QUEUES:
        if (code == LBN_SELCHANGE) {
            const int row = ListBox_GetCurSel(Item(IDC_SCHED_QUEUES));
            if (row >= 0 && static_cast<size_t>(row) < listed_.size()) {
                const Row chosen = listed_[static_cast<size_t>(row)];
                if (chosen.project) {
                    ChooseProject(*chosen.project);
                } else if (chosen.queue) {
                    Choose(*chosen.queue);
                }
            }
        }
        return true;
    case IDC_SCHED_PROJECT_MODE:
        if (code == CBN_SELCHANGE) {
            MarkEdited();
        }
        return true;
    case IDC_SCHED_NEW:
        CreateQueue();
        return true;
    case IDC_SCHED_DELETE:
        DeleteQueue();
        return true;
    case IDC_SCHED_START_NOW:
        // A queue runs with its saved settings, so changes are settled first.
        if (chosenProject_) {
            // SettleChanges may ask a question, during which a newer snapshot replaces the view;
            // the mode is the one just applied, or else the loaded one.
            if (ChosenProject() != nullptr && SettleChanges() && callbacks_.postGrabber && loadedProject_) {
                const model::GrabberScheduleMode mode = loadedProject_->scheduleMode;
                if (mode == model::GrabberScheduleMode::DownloadChecked) {
                    callbacks_.postGrabber(core::GrabberDownloadCommand{*chosenProject_});
                } else {
                    callbacks_.postGrabber(core::GrabberExploreCommand{
                        *chosenProject_, true, {}, mode == model::GrabberScheduleMode::ExploreDownload});
                }
            }
        } else if (chosen_ && SettleChanges()) {
            callbacks_.post(core::StartQueueCommand{*chosen_});
        }
        return true;
    case IDC_SCHED_STOP:
        if (chosenProject_ && callbacks_.postGrabber) {
            callbacks_.postGrabber(core::GrabberStopCommand{*chosenProject_});
        } else if (chosen_) {
            callbacks_.post(core::StopQueueCommand{*chosen_});
        }
        return true;
    case IDC_SCHED_APPLY:
        Apply();
        return true;
    case IDC_SCHED_UP:
        MoveSelectedFiles(-1);
        return true;
    case IDC_SCHED_DOWN:
        MoveSelectedFiles(1);
        return true;
    case IDC_SCHED_REMOVE:
        RemoveSelectedFiles();
        return true;
    case IDC_SCHED_OPEN_BROWSE:
        if (const auto file = PickOpenFile(Handle(), L"Programs and documents", L"*.*")) {
            SetText(IDC_SCHED_OPEN_FILE, *file);
            SetChecked(IDC_SCHED_OPEN_ON, true);
            EnableScheduleFields();
            MarkEdited();
        }
        return true;
    default:
        break;
    }
    const bool onPage = (id >= IDC_SCHED_PAGE_FIRST && id <= IDC_SCHED_PAGE_LAST) ||
                        (id >= IDC_SCHED_FILES_FIRST && id <= IDC_SCHED_FILES_LAST) ||
                        (id >= IDC_SCHED_LIMITS_FIRST && id <= IDC_SCHED_LIMITS_LAST);
    if (onPage && (code == BN_CLICKED || code == EN_CHANGE || code == CBN_SELCHANGE)) {
        EnableScheduleFields();
        MarkEdited();
        return true;
    }
    return Dialog::OnCommand(id, code, control);
}

bool SchedulerWindow::OnNotify(const NMHDR& header, LRESULT& result)
{
    switch (header.idFrom) {
    case IDC_SCHED_TAB:
        if (header.code == TCN_SELCHANGE) {
            // A project has only its schedule.
            ShowPage(chosenProject_ ? kSchedulePage : TabCtrl_GetCurSel(Item(IDC_SCHED_TAB)));
            result = 0;
            return true;
        }
        break;
    case IDC_SCHED_START_TIME:
    case IDC_SCHED_DATE:
    case IDC_SCHED_STOP_TIME:
        if (header.code == DTN_DATETIMECHANGE) {
            MarkEdited();
            result = 0;
            return true;
        }
        break;
    case IDC_SCHED_FILES:
        if (header.code == LVN_ITEMCHANGED) {
            RefreshButtons();
            result = 0;
            return true;
        }
        break;
    default:
        break;
    }
    return false;
}

void SchedulerWindow::OnDestroyed()
{
    // The callback deletes this object, so it runs from a copy that outlives it.
    const auto closed = callbacks_.closed;
    if (closed) {
        closed();
    }
}

}  // namespace ldm::ui

#include "ui/dialogs/GrabberDialogs.h"

#include "base/Utf.h"
#include "resource.h"
#include "ui/Format.h"
#include "ui/dialogs/MessageDialog.h"

#include <commctrl.h>

#include <algorithm>

namespace ldm::ui {

namespace {

constexpr int kMaxNameCharacters = 100;

std::wstring Trimmed(const std::wstring& text)
{
    const auto first = text.find_first_not_of(L" \t");
    if (first == std::wstring::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(L" \t") - first + 1);
}

void SetCell(HWND list, int row, int column, const std::wstring& text)
{
    ListView_SetItemText(list, row, column, const_cast<wchar_t*>(text.c_str()));  // copied by the list view
}

}  // namespace

std::vector<std::wstring> GrabberNames(const grabber::GrabberSnapshot* snapshot,
                                       std::optional<model::GrabberProjectId> except)
{
    std::vector<std::wstring> names;
    if (snapshot == nullptr) {
        return names;
    }
    for (const grabber::GrabberProjectView& view : snapshot->projects) {
        if (!except || view.project.id != *except) {
            names.push_back(base::ToUtf16(view.project.name));
        }
    }
    return names;
}

GrabberProjectsDialog::GrabberProjectsDialog(const Theme& theme, std::shared_ptr<const grabber::GrabberSnapshot> snapshot,
                                             Callbacks callbacks)
    : Dialog(IDD_GRABBER_PROJECTS, theme), snapshot_(std::move(snapshot)), callbacks_(std::move(callbacks))
{
}

std::optional<GrabberProjectsDialog::Choice> GrabberProjectsDialog::Run(HWND owner)
{
    RunModal(owner);
    return choice_;
}

void GrabberProjectsDialog::Update(std::shared_ptr<const grabber::GrabberSnapshot> snapshot)
{
    snapshot_ = std::move(snapshot);
    if (Handle() != nullptr) {
        Refresh();
    }
}

bool GrabberProjectsDialog::OnInit()
{
    const HWND list = Item(IDC_GP_LIST);
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    const struct {
        const wchar_t* title;
        int width;
    } columns[] = {{L"Name", 110}, {L"Kind", 70}, {L"Start page", 125}, {L"Last run", 95}};
    const int dpi = static_cast<int>(::GetDpiForWindow(Handle()));
    for (int i = 0; i < 4; ++i) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.pszText = const_cast<wchar_t*>(columns[i].title);  // copied by the list view
        column.cx = ::MulDiv(columns[i].width, dpi, 96);
        ListView_InsertColumn(list, i, &column);
    }
    if (CurrentTheme().IsDark()) {
        ListView_SetBkColor(list, CurrentTheme().Colors().window);
        ListView_SetTextBkColor(list, CurrentTheme().Colors().window);
        ListView_SetTextColor(list, CurrentTheme().Colors().text);
    }
    Refresh();
    if (!rows_.empty()) {
        ListView_SetItemState(list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    RefreshButtons();
    ::SetFocus(rows_.empty() ? Item(IDC_GP_NEW) : list);
    return false;
}

void GrabberProjectsDialog::Refresh()
{
    const HWND list = Item(IDC_GP_LIST);
    const std::optional<model::GrabberProjectId> selected =
        selectAfterRefresh_ ? selectAfterRefresh_ : (Selected() ? std::optional(Selected()->project.id) : std::nullopt);
    selectAfterRefresh_.reset();
    std::vector<const grabber::GrabberProjectView*> views;
    if (snapshot_) {
        for (const grabber::GrabberProjectView& view : snapshot_->projects) {
            views.push_back(&view);
        }
    }
    // Projects run most recently first, then the rest by name; templates last.
    std::stable_sort(views.begin(), views.end(), [](const auto* a, const auto* b) {
        if (a->project.isTemplate != b->project.isTemplate) {
            return !a->project.isTemplate;
        }
        return a->project.lastRunAtMs.value_or(0) > b->project.lastRunAtMs.value_or(0);
    });
    ::SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list);
    rows_.clear();
    for (const grabber::GrabberProjectView* view : views) {
        const int row = static_cast<int>(rows_.size());
        const std::wstring name = base::ToUtf16(view->project.name);
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = row;
        item.pszText = const_cast<wchar_t*>(name.c_str());
        ListView_InsertItem(list, &item);
        SetCell(list, row, 1, view->project.isTemplate ? L"Template" : view->damaged ? L"Damaged" : L"Project");
        SetCell(list, row, 2, base::ToUtf16(view->project.definition.startPage));
        SetCell(list, row, 3, view->project.lastRunAtMs ? FormatDateTime(view->project.lastRunAtMs) : std::wstring());
        rows_.push_back(view->project.id);
        if (selected && view->project.id == *selected) {
            ListView_SetItemState(list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(list, row, FALSE);
        }
    }
    ::SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    RefreshButtons();
}

const grabber::GrabberProjectView* GrabberProjectsDialog::Selected() const
{
    const int row = ListView_GetNextItem(Item(IDC_GP_LIST), -1, LVNI_SELECTED);
    if (row < 0 || static_cast<std::size_t>(row) >= rows_.size() || !snapshot_) {
        return nullptr;
    }
    for (const grabber::GrabberProjectView& view : snapshot_->projects) {
        if (view.project.id == rows_[static_cast<std::size_t>(row)]) {
            return &view;
        }
    }
    return nullptr;
}

void GrabberProjectsDialog::RefreshButtons()
{
    const grabber::GrabberProjectView* view = Selected();
    Enable(IDC_GP_FROM_TEMPLATE, view != nullptr && view->project.isTemplate);
    Enable(IDC_GP_OPEN, view != nullptr && !view->project.isTemplate && !view->damaged);
    Enable(IDC_GP_EDIT, view != nullptr && !view->damaged && !view->exploring && !view->downloading);
    Enable(IDC_GP_RENAME, view != nullptr && !view->exploring && !view->downloading && !view->damaged);
    Enable(IDC_GP_DELETE, view != nullptr && !view->open);
}

void GrabberProjectsDialog::Choose(Action action)
{
    const grabber::GrabberProjectView* view = Selected();
    if (action != Action::NewProject && view == nullptr) {
        return;
    }
    choice_ = Choice{action, view ? std::optional(view->project.id) : std::nullopt};
    Close(IDOK);
}

bool GrabberProjectsDialog::OnCommand(int id, int code, HWND control)
{
    switch (id) {
    case IDC_GP_NEW:
        Choose(Action::NewProject);
        return true;
    case IDC_GP_FROM_TEMPLATE:
        Choose(Action::NewFromTemplate);
        return true;
    case IDOK:
    case IDC_GP_OPEN:
        if (const grabber::GrabberProjectView* view = Selected()) {
            Choose(view->project.isTemplate ? Action::NewFromTemplate : Action::Open);
        }
        return true;
    case IDC_GP_EDIT:
        Choose(Action::Edit);
        return true;
    case IDC_GP_RENAME:
        if (const grabber::GrabberProjectView* view = Selected()) {
            const model::GrabberProjectId project = view->project.id;
            GrabberNameDialog dialog(CurrentTheme(), view->project.isTemplate ? L"Rename template" : L"Rename project",
                                     base::ToUtf16(view->project.name), GrabberNames(snapshot_.get(), project));
            if (const auto name = dialog.Run(Handle())) {
                selectAfterRefresh_ = project;
                callbacks_.rename(project, *name);
            }
        }
        return true;
    case IDC_GP_DELETE:
        if (const grabber::GrabberProjectView* view = Selected()) {
            // Copied: a newer snapshot replaces `view` while the question is open.
            const model::GrabberProjectId project = view->project.id;
            const std::wstring question =
                L"Delete \"" + base::ToUtf16(view->project.name) +
                L"\"? Its list of pages and files goes too, and its downloads that are not in the main list leave "
                L"the list. Files already on disk stay.";
            if (ShowMessage(Handle(), CurrentTheme(), MessageRequest{question, MessageKind::Question, MessageButtons::YesNo}) ==
                IDYES) {
                callbacks_.remove(project);
            }
        }
        return true;
    default:
        return Dialog::OnCommand(id, code, control);
    }
}

bool GrabberProjectsDialog::OnNotify(const NMHDR& header, LRESULT& result)
{
    if (header.idFrom != IDC_GP_LIST) {
        return false;
    }
    if (header.code == LVN_ITEMCHANGED) {
        RefreshButtons();
        result = 0;
        return true;
    }
    if (header.code == NM_DBLCLK) {
        if (const grabber::GrabberProjectView* view = Selected()) {
            Choose(view->project.isTemplate ? Action::NewFromTemplate : Action::Open);
        }
        result = 0;
        return true;
    }
    return false;
}

GrabberNameDialog::GrabberNameDialog(const Theme& theme, std::wstring title, std::wstring initial,
                                     std::vector<std::wstring> taken)
    : Dialog(IDD_GRABBER_NAME, theme), title_(std::move(title)), initial_(std::move(initial)), taken_(std::move(taken))
{
}

std::optional<std::string> GrabberNameDialog::Run(HWND owner)
{
    RunModal(owner);
    return name_;
}

bool GrabberNameDialog::OnInit()
{
    ::SetWindowTextW(Handle(), title_.c_str());
    ::SendMessageW(Item(IDC_GN_NAME), EM_LIMITTEXT, kMaxNameCharacters, 0);
    SetText(IDC_GN_NAME, initial_);
    ::SendMessageW(Item(IDC_GN_NAME), EM_SETSEL, 0, -1);
    ::SetFocus(Item(IDC_GN_NAME));
    return false;
}

bool GrabberNameDialog::OnCommand(int id, int code, HWND control)
{
    if (id != IDOK) {
        return Dialog::OnCommand(id, code, control);
    }
    const std::wstring name = Trimmed(Text(IDC_GN_NAME));
    if (name.empty()) {
        Warn(L"Enter a name.");
        return true;
    }
    const bool taken = std::any_of(taken_.begin(), taken_.end(), [&name](const std::wstring& other) {
        return ::CompareStringOrdinal(other.c_str(), static_cast<int>(other.size()), name.c_str(),
                                      static_cast<int>(name.size()), TRUE) == CSTR_EQUAL;
    });
    if (taken) {
        Warn(L"A project or template with this name already exists.");
        return true;
    }
    name_ = base::ToUtf8(name);
    Close(IDOK);
    return true;
}

}  // namespace ldm::ui

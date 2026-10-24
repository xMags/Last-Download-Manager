#include "ui/dialogs/GrabberWizard.h"

#include "base/Utf.h"
#include "grabber/ProjectDefinition.h"
#include "resource.h"
#include "ui/FileDialogs.h"
#include "ui/GrabberFields.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <filesystem>

namespace ldm::ui {

namespace {

constexpr int kPageCount = 4;

struct PageRange {
    int first = 0;
    int last = 0;
    const wchar_t* title = L"";
};

constexpr std::array<PageRange, kPageCount> kPages{{
    {IDC_GW_P1_FIRST, IDC_GW_P1_LAST, L"Step 1 of 4: the project and its start page"},
    {IDC_GW_P2_FIRST, IDC_GW_P2_LAST, L"Step 2 of 4: where the files are saved"},
    {IDC_GW_P3_FIRST, IDC_GW_P3_LAST, L"Step 3 of 4: which pages are explored"},
    {IDC_GW_P4_FIRST, IDC_GW_P4_LAST, L"Step 4 of 4: which files are listed"},
}};

constexpr int kMaxNameCharacters = 100;
constexpr std::int64_t kMaxKilobytes = std::int64_t{1} << 40;

std::wstring Trimmed(const std::wstring& text)
{
    const auto first = text.find_first_not_of(L" \t");
    if (first == std::wstring::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(L" \t") - first + 1);
}

bool SameIgnoringCase(const std::wstring& a, const std::wstring& b)
{
    return ::CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) ==
           CSTR_EQUAL;
}

}  // namespace

GrabberWizard::GrabberWizard(const Theme& theme, model::GrabberProject project, bool hasPassword,
                             std::vector<model::Category> categories, std::vector<std::wstring> takenNames)
    : Dialog(IDD_GRABBER_WIZARD, theme),
      project_(std::move(project)),
      hasPassword_(hasPassword),
      categories_(std::move(categories)),
      taken_(std::move(takenNames))
{
}

std::optional<GrabberWizard::Result> GrabberWizard::Run(HWND owner)
{
    RunModal(owner);
    return result_;
}

bool GrabberWizard::OnInit()
{
    loading_ = true;
    if (project_.isTemplate) {
        ::SetWindowTextW(Handle(), L"Site Grabber template");
    }
    for (const TemplateChoice& choice : TemplateChoices()) {
        ComboBox_AddString(Item(IDC_GW_TEMPLATE), choice.name);
    }
    for (const model::Category& category : categories_) {
        ComboBox_AddString(Item(IDC_GW_CATEGORY), base::ToUtf16(category.name).c_str());
    }
    ::SendMessageW(Item(IDC_GW_NAME), EM_LIMITTEXT, kMaxNameCharacters, 0);
    for (const int id : {IDC_GW_LEVELS_COUNT, IDC_GW_OTHER_LEVELS, IDC_GW_EXPLORE_CONCURRENCY, IDC_GW_DOWNLOAD_CONCURRENCY}) {
        ::SendMessageW(Item(id), EM_LIMITTEXT, 2, 0);
    }
    for (const int id : {IDC_GW_MIN_KB, IDC_GW_MAX_KB}) {
        ::SendMessageW(Item(id), EM_LIMITTEXT, 12, 0);
    }
    if (hasPassword_) {
        Edit_SetCueBannerText(Item(IDC_GW_PASSWORD), L"(stored)");
    }

    SetText(IDC_GW_NAME, base::ToUtf16(project_.name));
    SetText(IDC_GW_START, base::ToUtf16(project_.definition.startPage));
    SetText(IDC_GW_USER, base::ToUtf16(project_.loginUser));
    const auto& choices = TemplateChoices();
    const auto chosen = std::find_if(choices.begin(), choices.end(), [this](const TemplateChoice& choice) {
        return choice.kind == project_.definition.templateKind;
    });
    ComboBox_SetCurSel(Item(IDC_GW_TEMPLATE), chosen == choices.end() ? 0 : static_cast<int>(chosen - choices.begin()));
    SetText(IDC_GW_TEMPLATE_NOTE, chosen == choices.end() ? L"" : chosen->description);
    LoadFields(project_.definition);
    loading_ = false;
    ShowPage(0);
    ::SetFocus(Item(project_.name.empty() ? IDC_GW_NAME : IDC_GW_START));
    return false;
}

void GrabberWizard::LoadFields(const model::GrabberProjectDefinition& d)
{
    const bool wasLoading = loading_;
    loading_ = true;
    SetChecked(IDC_GW_COOKIES, d.useBrowserCookies);

    SetChecked(IDC_GW_SAVE_CATEGORY, d.saveMode == model::GrabberSaveMode::ByCategory);
    SetChecked(IDC_GW_SAVE_ONE_CATEGORY, d.saveMode == model::GrabberSaveMode::OneCategory);
    SetChecked(IDC_GW_SAVE_FOLDER, d.saveMode == model::GrabberSaveMode::OneFolder);
    int categoryIndex = 0;
    for (std::size_t i = 0; i < categories_.size(); ++i) {
        if (d.category && categories_[i].id == *d.category) {
            categoryIndex = static_cast<int>(i);
        }
    }
    ComboBox_SetCurSel(Item(IDC_GW_CATEGORY), categoryIndex);
    SetText(IDC_GW_FOLDER, base::ToUtf16(d.folder));
    SetChecked(IDC_GW_KEEP_FOLDERS, d.keepSiteFolders);
    SetChecked(IDC_GW_SAVE_PAGES, d.savePages || d.convertLinks);
    SetChecked(IDC_GW_OVERWRITE, d.overwrite);
    SetChecked(IDC_GW_ADD_ON_CLOSE, d.addToMainListOnClose);

    SetChecked(IDC_GW_WHOLE_SITE, !d.explore.baseSiteDepth);
    SetChecked(IDC_GW_LEVELS, d.explore.baseSiteDepth.has_value());
    SetNumber(IDC_GW_LEVELS_COUNT, d.explore.baseSiteDepth.value_or(1));
    SetNumber(IDC_GW_OTHER_LEVELS, d.explore.otherSitesDepth);
    SetChecked(IDC_GW_NO_PARENT, d.explore.noParentDirectories);
    SetChecked(IDC_GW_MAIN_DOMAIN, d.explore.mainDomainSites);
    SetText(IDC_GW_EXPLORE_INCLUDE, JoinMasks(d.explore.includePaths));
    SetText(IDC_GW_EXPLORE_EXCLUDE, JoinMasks(d.explore.excludePaths));
    SetNumber(IDC_GW_EXPLORE_CONCURRENCY, d.exploreConcurrency);

    SetText(IDC_GW_TYPES_INCLUDE, JoinMasks(d.files.includeTypes));
    SetText(IDC_GW_TYPES_EXCLUDE, JoinMasks(d.files.excludeTypes));
    SetText(IDC_GW_MIN_KB, d.files.minBytes ? std::to_wstring(*d.files.minBytes / 1024) : L"");
    SetText(IDC_GW_MAX_KB, d.files.maxBytes ? std::to_wstring(*d.files.maxBytes / 1024) : L"");
    SetText(IDC_GW_FILES_INCLUDE, JoinMasks(d.files.includePaths));
    SetText(IDC_GW_FILES_EXCLUDE, JoinMasks(d.files.excludePaths));
    SetChecked(IDC_GW_THIS_SITE, d.files.thisSiteOnly);
    SetChecked(IDC_GW_DUPLICATES, d.files.hideDuplicates);
    SetChecked(IDC_GW_IMMEDIATELY, d.files.downloadImmediately);
    SetChecked(IDC_GW_LINK_TEXT, d.linkTextAsDescription);
    SetNumber(IDC_GW_DOWNLOAD_CONCURRENCY, d.downloadConcurrency);
    loading_ = wasLoading;
    EnableFields();
}

void GrabberWizard::EnableFields()
{
    Enable(IDC_GW_CATEGORY, Checked(IDC_GW_SAVE_ONE_CATEGORY) && !categories_.empty());
    Enable(IDC_GW_FOLDER, Checked(IDC_GW_SAVE_FOLDER));
    Enable(IDC_GW_FOLDER_BROWSE, Checked(IDC_GW_SAVE_FOLDER));
    Enable(IDC_GW_LEVELS_COUNT, Checked(IDC_GW_LEVELS));
    Enable(IDC_GW_PASSWORD, !Trimmed(Text(IDC_GW_USER)).empty());
}

void GrabberWizard::ShowPage(int page)
{
    page_ = std::clamp(page, 0, kPageCount - 1);
    for (int i = 0; i < kPageCount; ++i) {
        for (int id = kPages[static_cast<std::size_t>(i)].first; id <= kPages[static_cast<std::size_t>(i)].last; ++id) {
            if (const HWND control = ::GetDlgItem(Handle(), id)) {
                ::ShowWindow(control, i == page_ ? SW_SHOW : SW_HIDE);
            }
        }
    }
    SetText(IDC_GW_TITLE, kPages[static_cast<std::size_t>(page_)].title);
    Enable(IDC_GW_BACK, page_ > 0);
    Enable(IDC_GW_NEXT, page_ + 1 < kPageCount);
}

model::GrabberTemplate GrabberWizard::ChosenTemplate() const
{
    const int index = ComboBox_GetCurSel(Item(IDC_GW_TEMPLATE));
    const auto& choices = TemplateChoices();
    return index >= 0 && static_cast<std::size_t>(index) < choices.size() ? choices[static_cast<std::size_t>(index)].kind
                                                                           : model::GrabberTemplate::Custom;
}

void GrabberWizard::ApplyChosenTemplate()
{
    // What the later pages hold now, then the template's filters over it; the save place stays.
    for (int page = 1; page < kPageCount; ++page) {
        (void)ReadPage(page, false);
    }
    project_.definition = grabber::ApplyTemplate(project_.definition, ChosenTemplate());
    const auto& choices = TemplateChoices();
    const auto chosen = std::find_if(choices.begin(), choices.end(),
                                     [this](const TemplateChoice& choice) { return choice.kind == ChosenTemplate(); });
    SetText(IDC_GW_TEMPLATE_NOTE, chosen == choices.end() ? L"" : chosen->description);
    LoadFields(project_.definition);
}

bool GrabberWizard::ReadPage(int page, bool strict)
{
    model::GrabberProjectDefinition& d = project_.definition;
    const auto fail = [this, page, strict](const std::wstring& message, int focus) {
        if (!strict) {
            return false;
        }
        if (page_ != page) {
            ShowPage(page);
        }
        Warn(message);
        ::SetFocus(Item(focus));
        return false;
    };
    switch (page) {
    case 0: {
        const std::wstring name = Trimmed(Text(IDC_GW_NAME));
        if (name.empty()) {
            return fail(L"Enter a name for the project.", IDC_GW_NAME);
        }
        if (std::any_of(taken_.begin(), taken_.end(), [&name](const std::wstring& other) { return SameIgnoringCase(other, name); })) {
            return fail(L"A project or template with this name already exists.", IDC_GW_NAME);
        }
        const auto start = ParseStartPage(Text(IDC_GW_START));
        if (!start) {
            return fail(L"Enter the address of the start page, such as https://example.com/.", IDC_GW_START);
        }
        project_.name = base::ToUtf8(name);
        d.startPage = *start;
        d.templateKind = ChosenTemplate();
        project_.loginUser = base::ToUtf8(Trimmed(Text(IDC_GW_USER)));
        d.useBrowserCookies = Checked(IDC_GW_COOKIES);
        return true;
    }
    case 1: {
        if (Checked(IDC_GW_SAVE_ONE_CATEGORY)) {
            const int index = ComboBox_GetCurSel(Item(IDC_GW_CATEGORY));
            if (index < 0 || static_cast<std::size_t>(index) >= categories_.size()) {
                return fail(L"Choose the category to save the files in.", IDC_GW_CATEGORY);
            }
            d.saveMode = model::GrabberSaveMode::OneCategory;
            d.category = categories_[static_cast<std::size_t>(index)].id;
        } else if (Checked(IDC_GW_SAVE_FOLDER)) {
            const std::wstring folder = Trimmed(Text(IDC_GW_FOLDER));
            if (folder.empty() || !std::filesystem::path(folder).is_absolute()) {
                return fail(L"Enter the full path of the folder to save the files in.", IDC_GW_FOLDER);
            }
            d.saveMode = model::GrabberSaveMode::OneFolder;
            d.folder = base::ToUtf8(folder);
        } else {
            d.saveMode = model::GrabberSaveMode::ByCategory;
        }
        if (d.saveMode != model::GrabberSaveMode::OneCategory) {
            d.category.reset();
        }
        d.keepSiteFolders = Checked(IDC_GW_KEEP_FOLDERS);
        d.savePages = Checked(IDC_GW_SAVE_PAGES);
        d.convertLinks = d.savePages;
        d.overwrite = Checked(IDC_GW_OVERWRITE);
        d.addToMainListOnClose = Checked(IDC_GW_ADD_ON_CLOSE);
        return true;
    }
    case 2: {
        if (Checked(IDC_GW_LEVELS)) {
            const auto levels = Number(IDC_GW_LEVELS_COUNT);
            if (!levels || *levels < 0 || *levels > model::kMaxGrabberDepth) {
                return fail(L"Enter how many levels of links to explore, from 0 to 99.", IDC_GW_LEVELS_COUNT);
            }
            d.explore.baseSiteDepth = *levels;
        } else {
            d.explore.baseSiteDepth.reset();
        }
        const auto other = Number(IDC_GW_OTHER_LEVELS);
        if (!other || *other < 0 || *other > model::kMaxGrabberDepth) {
            return fail(L"Enter how many levels to follow into other sites, from 0 to 99.", IDC_GW_OTHER_LEVELS);
        }
        const auto include = ParseMasks(Text(IDC_GW_EXPLORE_INCLUDE));
        const auto exclude = ParseMasks(Text(IDC_GW_EXPLORE_EXCLUDE));
        if (!include) {
            return fail(L"There are too many paths, or one is too long.", IDC_GW_EXPLORE_INCLUDE);
        }
        if (!exclude) {
            return fail(L"There are too many paths, or one is too long.", IDC_GW_EXPLORE_EXCLUDE);
        }
        const auto concurrency = Number(IDC_GW_EXPLORE_CONCURRENCY);
        if (!concurrency || *concurrency < 1 || *concurrency > model::kMaxGrabberConcurrency) {
            return fail(L"Enter how many pages to read at the same time, from 1 to 10.", IDC_GW_EXPLORE_CONCURRENCY);
        }
        d.explore.otherSitesDepth = *other;
        d.explore.noParentDirectories = Checked(IDC_GW_NO_PARENT);
        d.explore.mainDomainSites = Checked(IDC_GW_MAIN_DOMAIN);
        d.explore.includePaths = *include;
        d.explore.excludePaths = *exclude;
        d.exploreConcurrency = *concurrency;
        return true;
    }
    case 3: {
        const auto includeTypes = ParseMasks(Text(IDC_GW_TYPES_INCLUDE));
        const auto excludeTypes = ParseMasks(Text(IDC_GW_TYPES_EXCLUDE));
        const auto includePaths = ParseMasks(Text(IDC_GW_FILES_INCLUDE));
        const auto excludePaths = ParseMasks(Text(IDC_GW_FILES_EXCLUDE));
        for (const auto& [masks, id] : {std::pair{&includeTypes, IDC_GW_TYPES_INCLUDE}, std::pair{&excludeTypes, IDC_GW_TYPES_EXCLUDE},
                                       std::pair{&includePaths, IDC_GW_FILES_INCLUDE}, std::pair{&excludePaths, IDC_GW_FILES_EXCLUDE}}) {
            if (!*masks) {
                return fail(L"There are too many masks, or one is too long.", id);
            }
        }
        std::optional<std::int64_t> minBytes;
        std::optional<std::int64_t> maxBytes;
        for (const auto& [target, id] : {std::pair{&minBytes, IDC_GW_MIN_KB}, std::pair{&maxBytes, IDC_GW_MAX_KB}}) {
            const std::wstring text = Trimmed(Text(id));
            if (text.empty()) {
                continue;
            }
            std::int64_t kilobytes = 0;
            for (const wchar_t c : text) {
                if (c < L'0' || c > L'9' || kilobytes > kMaxKilobytes) {
                    return fail(L"Enter the size in kilobytes, or leave it empty.", id);
                }
                kilobytes = kilobytes * 10 + (c - L'0');
            }
            *target = kilobytes * 1024;
        }
        if (minBytes && maxBytes && *minBytes > *maxBytes) {
            return fail(L"The smallest size is larger than the largest.", IDC_GW_MIN_KB);
        }
        const auto concurrency = Number(IDC_GW_DOWNLOAD_CONCURRENCY);
        if (!concurrency || *concurrency < 1 || *concurrency > model::kMaxGrabberConcurrency) {
            return fail(L"Enter how many files to download at the same time, from 1 to 10.", IDC_GW_DOWNLOAD_CONCURRENCY);
        }
        d.files.includeTypes = *includeTypes;
        d.files.excludeTypes = *excludeTypes;
        d.files.includePaths = *includePaths;
        d.files.excludePaths = *excludePaths;
        d.files.minBytes = minBytes;
        d.files.maxBytes = maxBytes;
        d.files.thisSiteOnly = Checked(IDC_GW_THIS_SITE);
        d.files.hideDuplicates = Checked(IDC_GW_DUPLICATES);
        d.files.downloadImmediately = Checked(IDC_GW_IMMEDIATELY);
        d.linkTextAsDescription = Checked(IDC_GW_LINK_TEXT);
        d.downloadConcurrency = *concurrency;
        return true;
    }
    default:
        return true;
    }
}

bool GrabberWizard::OnCommand(int id, int code, HWND control)
{
    switch (id) {
    case IDC_GW_TEMPLATE:
        if (code == CBN_SELCHANGE && !loading_) {
            ApplyChosenTemplate();
        }
        return true;
    case IDC_GW_SAVE_CATEGORY:
    case IDC_GW_SAVE_ONE_CATEGORY:
    case IDC_GW_SAVE_FOLDER:
    case IDC_GW_WHOLE_SITE:
    case IDC_GW_LEVELS:
        EnableFields();
        return true;
    case IDC_GW_USER:
        if (code == EN_CHANGE) {
            EnableFields();
        }
        return true;
    case IDC_GW_PASSWORD:
        if (code == EN_CHANGE && !loading_) {
            passwordEdited_ = true;
        }
        return true;
    case IDC_GW_FOLDER_BROWSE:
        if (const auto folder = PickFolder(Handle(), Text(IDC_GW_FOLDER))) {
            SetText(IDC_GW_FOLDER, *folder);
        }
        return true;
    case IDC_GW_BACK:
        (void)ReadPage(page_, false);
        ShowPage(page_ - 1);
        return true;
    case IDC_GW_NEXT:
        if (ReadPage(page_, true)) {
            ShowPage(page_ + 1);
        }
        return true;
    case IDC_GW_FINISH:
        for (int page = 0; page < kPageCount; ++page) {
            if (!ReadPage(page, true)) {
                return true;
            }
        }
        result_ = Result{project_, std::nullopt};
        if (project_.loginUser.empty()) {
            result_->password = hasPassword_ ? std::optional<std::string>(std::string()) : std::nullopt;
        } else if (passwordEdited_) {
            result_->password = base::ToUtf8(Text(IDC_GW_PASSWORD));
        }
        Close(IDOK);
        return true;
    default:
        return Dialog::OnCommand(id, code, control);
    }
}

}  // namespace ldm::ui

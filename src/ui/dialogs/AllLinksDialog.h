#pragma once

#include "browser/Protocol.h"
#include "model/Category.h"
#include "ui/Dialog.h"
#include "ui/PageLinks.h"

#include <optional>
#include <string>
#include <vector>

namespace ldm::ui {

// Download All Links (checklist B-05, the reference's dialog 228): the links of a page or of the selected
// part of it, to check and add to the list. Types and sizes fill in through ApplyDetails while the
// dialog is open, as the extension checks the links.
class AllLinksDialog final : public Dialog {
public:
    enum class SaveTo {
        ByCategory,   // each file to the folder of the category its name matches
        OneCategory,
        OneFolder,
    };

    struct Pick {
        std::string url;
        std::optional<std::string> fileName;  // renamed, or the page's download attribute
        std::string description;
    };

    struct Choice {
        std::vector<Pick> picks;  // in the order shown
        SaveTo saveTo = SaveTo::ByCategory;
        std::optional<model::CategoryId> category;  // OneCategory
        std::string folder;                         // OneFolder
    };

    struct Initial {
        std::vector<LinkRow> rows;
        bool selection = false;  // "Download selected links": the title says so
        LinkFilter filter;
        bool useLinkText = false;
        std::vector<model::Category> categories;
        std::optional<model::CategoryId> category;  // preselected for OneCategory
        std::string folder;                         // preset for OneFolder
    };

    AllLinksDialog(const Theme& theme, Initial initial);
    [[nodiscard]] std::optional<Choice> Run(HWND owner);

    void ApplyDetails(const std::vector<browser::LinkDetails>& items);
    // The browser will not report more; links not checked yet stay without type and size.
    void StopChecking();

    // The options as the user left them, also after Cancel; LDM remembers them.
    [[nodiscard]] const LinkFilter& Filter() const noexcept { return initial_.filter; }
    [[nodiscard]] bool UseLinkText() const noexcept { return initial_.useLinkText; }

private:
    bool OnInit() override;
    bool OnCommand(int id, int code, HWND control) override;
    bool OnNotify(const NMHDR& header, LRESULT& result) override;
    bool OnMessage(UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result) override;

    void AddColumns();
    // Rebuilds the list from the rows and the filter, keeping the selection.
    void Refill();
    void ShowRowTexts(int item, const LinkRow& row);
    void UpdateCount();
    void UpdateSaveTo();
    void SetChecks(bool checked, bool selectedOnly);
    void EditSelected();
    void ShowListMenu(POINT screen);
    [[nodiscard]] std::vector<size_t> SelectedRows() const;
    [[nodiscard]] bool Finish();

    Initial initial_;
    std::vector<LinkRow>& rows_;  // initial_.rows
    std::vector<size_t> shown_;   // row indexes in list order
    int sortColumn_ = -1;         // -1: the page's order
    bool sortAscending_ = true;
    bool filling_ = false;
    bool checkingStopped_ = false;
    std::optional<Choice> result_;
};

// Edit... in Download All Links: the name and description of one link, or a name pattern for
// several (see NameFromPattern).
class LinkEditDialog final : public Dialog {
public:
    struct Values {
        std::wstring name;         // a name, or a pattern for several links
        std::wstring description;  // one link only
    };

    // `count` is the number of links being edited.
    LinkEditDialog(const Theme& theme, size_t count, Values values);
    [[nodiscard]] std::optional<Values> Run(HWND owner);

private:
    bool OnInit() override;
    bool OnCommand(int id, int code, HWND control) override;

    size_t count_;
    Values values_;
    std::optional<Values> result_;
};

}  // namespace ldm::ui

#pragma once

#include "ui/Dialog.h"
#include "ui/ListModel.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ldm::ui {

// Find (checklist L-06). Modeless; each Find Next press calls `findNext` with the options.
class FindDialog final : public Dialog {
public:
    FindDialog(const Theme& theme, FindOptions initial, std::function<bool(const FindOptions&)> findNext,
               std::function<void()> closed);
    void Open(HWND owner);
    [[nodiscard]] const FindOptions& Options() const noexcept { return options_; }

private:
    bool OnInit() override;
    bool OnCommand(int id, int code, HWND control) override;
    void OnDestroyed() override;
    void ReadOptions();

    FindOptions options_;
    std::function<bool(const FindOptions&)> findNext_;
    std::function<void()> closed_;
};

// View > Customize list columns (checklist L-02).
class ColumnsDialog final : public Dialog {
public:
    ColumnsDialog(const Theme& theme, std::vector<ColumnState> columns);
    [[nodiscard]] std::optional<std::vector<ColumnState>> Run(HWND owner);

private:
    bool OnInit() override;
    bool OnCommand(int id, int code, HWND control) override;
    bool OnNotify(const NMHDR& header, LRESULT& result) override;
    void Fill();
    void ReadChecks();
    void Move(int delta);
    [[nodiscard]] int Selected() const;

    std::vector<ColumnState> columns_;
    std::optional<std::vector<ColumnState>> result_;
    bool filling_ = false;
};

// Batch from the clipboard and import from a text file (checklist A-08, A-11): pick addresses.
class UrlListDialog final : public Dialog {
public:
    struct Choice {
        std::vector<std::string> urls;
        bool start = false;
    };

    UrlListDialog(const Theme& theme, std::wstring title, std::vector<std::string> urls);
    [[nodiscard]] std::optional<Choice> Run(HWND owner);

private:
    bool OnInit() override;
    bool OnCommand(int id, int code, HWND control) override;

    std::wstring title_;
    std::vector<std::string> urls_;
    std::optional<Choice> result_;
};

}  // namespace ldm::ui

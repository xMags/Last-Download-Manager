#pragma once

#include "model/Category.h"
#include "ui/Dialog.h"

#include <optional>

namespace ldm::ui {

// Category properties (checklist W-03): name, file types, sites, folder and "remember last folder".
class CategoryDialog final : public Dialog {
public:
    CategoryDialog(const Theme& theme, model::Category category);
    [[nodiscard]] std::optional<model::Category> Run(HWND owner);

private:
    bool OnInit() override;
    bool OnCommand(int id, int code, HWND control) override;

    model::Category category_;
    std::optional<model::Category> result_;
};

// Downloads > Speed limiter > Settings (checklist Q-09).
class SpeedLimitDialog final : public Dialog {
public:
    struct Choice {
        int kilobytesPerSecond = 500;
        bool onStartup = false;
    };

    SpeedLimitDialog(const Theme& theme, Choice initial);
    [[nodiscard]] std::optional<Choice> Run(HWND owner);

private:
    bool OnInit() override;
    bool OnCommand(int id, int code, HWND control) override;

    Choice initial_;
    std::optional<Choice> result_;
};

// Help > About (checklist V-07).
class AboutDialog final : public Dialog {
public:
    explicit AboutDialog(const Theme& theme);

private:
    bool OnInit() override;
    bool OnNotify(const NMHDR& header, LRESULT& result) override;
};

}  // namespace ldm::ui

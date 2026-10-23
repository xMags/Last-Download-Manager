#include "ui/BrowserSessions.h"

#include <cctype>
#include <sstream>
#include <utility>

namespace ldm::ui {

namespace {

// Pushed configs are not answers to a request.
constexpr char kPushId[] = "push";

}  // namespace

BrowserSessions::BrowserSessions(BrowserLink& link, std::string appVersion)
    : link_(link), appVersion_(std::move(appVersion))
{
}

void BrowserSessions::OnEvent(BrowserEvent event, const model::Settings& settings)
{
    if (!event.message) {
        sessions_.erase(event.connection);
        return;
    }
    browser::ParsedMessage& parsed = *event.message;
    if (auto* host = std::get_if<browser::HostInfo>(&parsed.message)) {
        sessions_[event.connection] = Session{std::move(*host), std::nullopt};
        return;
    }
    const auto session = sessions_.find(event.connection);
    if (session == sessions_.end()) {
        return;  // the bridge guarantees the host message first; ignore anything else
    }
    if (auto* hello = std::get_if<browser::Hello>(&parsed.message)) {
        session->second.hello = std::move(*hello);
        link_.Send(event.connection, browser::EncodeConfig(MakeConfig(settings), parsed.id));
    }
}

void BrowserSessions::OnSettingsChanged(const model::Settings& settings)
{
    // Snapshots arrive up to 10 times a second; settings rarely change.
    if (configInputs_ == settings.capturedExtensions) {
        return;
    }
    // The first call only records the starting point; nobody has a stale copy yet.
    const bool first = !configInputs_.has_value();
    configInputs_ = settings.capturedExtensions;
    if (first) {
        return;
    }
    const std::string config = browser::EncodeConfig(MakeConfig(settings), kPushId);
    for (const auto& [connection, session] : sessions_) {
        if (session.hello) {
            link_.Send(connection, config);
        }
    }
}

browser::Config BrowserSessions::MakeConfig(const model::Settings& settings) const
{
    browser::Config config;
    config.appVersion = appVersion_;
    std::istringstream words(settings.capturedExtensions);
    for (std::string word; words >> word;) {
        for (char& c : word) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        config.capturedExtensions.push_back(std::move(word));
    }
    return config;
}

}  // namespace ldm::ui

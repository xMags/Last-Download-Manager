#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace ldm::test {

struct FixtureOptions {
    std::int64_t size = 4 * 1024 * 1024;
    int delayMs = 2;
    int chunkBytes = 32 * 1024;
    // Served under /media/: the folder of generated HLS and DASH fixtures. Nothing when empty.
    std::filesystem::path mediaRoot;
};

// Runs tools/research/http-reference-fixture.mjs in a Node.js child process for the lifetime of
// the object. The child is in a kill-on-close job, so it never outlives the test process.
class FixtureServer {
public:
    // Returns nothing when Node.js is not installed; tests then skip.
    [[nodiscard]] static std::unique_ptr<FixtureServer> Launch(const FixtureOptions& options);
    [[nodiscard]] static std::optional<std::filesystem::path> FindNode();

    ~FixtureServer();
    FixtureServer(const FixtureServer&) = delete;
    FixtureServer& operator=(const FixtureServer&) = delete;

    [[nodiscard]] std::string Url(const std::string& path) const;
    // The same payload over FTP. `user` "norest" makes the server refuse REST.
    [[nodiscard]] std::string FtpUrl(const std::string& path, const std::string& user = {}) const;
    [[nodiscard]] std::int64_t Size() const noexcept { return size_; }

    // Control endpoints.
    [[nodiscard]] nlohmann::json Log() const;
    void Bump() const;
    void Reset() const;

private:
    FixtureServer() = default;

    void* job_ = nullptr;
    void* process_ = nullptr;
    void* stdinWrite_ = nullptr;
    int port_ = 0;
    int ftpPort_ = 0;
    std::int64_t size_ = 0;
};

// Byte i of every fixture payload route.
[[nodiscard]] inline std::uint8_t FixtureByte(std::int64_t offset)
{
    return static_cast<std::uint8_t>(offset % 251);
}

// Plain GET returning the body, for control endpoints.
[[nodiscard]] std::string HttpGet(const std::string& url);

}  // namespace ldm::test

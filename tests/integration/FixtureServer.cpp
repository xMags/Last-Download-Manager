#include "FixtureServer.h"

#include "base/Utf.h"

#include <curl/curl.h>
#include <windows.h>

#include <stdexcept>
#include <string>
#include <vector>

#ifndef LDM_FIXTURE_SCRIPT
#error LDM_FIXTURE_SCRIPT must name tools/research/http-reference-fixture.mjs
#endif

namespace ldm::test {

namespace {

size_t AppendToString(char* data, size_t size, size_t count, void* userdata)
{
    static_cast<std::string*>(userdata)->append(data, size * count);
    return size * count;
}

void CloseIfValid(void*& handle)
{
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        ::CloseHandle(handle);
    }
    handle = nullptr;
}

}  // namespace

std::string HttpGet(const std::string& url)
{
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> easy(curl_easy_init(), &curl_easy_cleanup);
    std::string body;
    curl_easy_setopt(easy.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(easy.get(), CURLOPT_WRITEFUNCTION, &AppendToString);
    curl_easy_setopt(easy.get(), CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(easy.get(), CURLOPT_PROXY, "");
    curl_easy_setopt(easy.get(), CURLOPT_TIMEOUT, 10L);
    const CURLcode result = curl_easy_perform(easy.get());
    if (result != CURLE_OK) {
        throw std::runtime_error(std::string("fixture control request failed: ") + curl_easy_strerror(result));
    }
    return body;
}

std::optional<std::filesystem::path> FixtureServer::FindNode()
{
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = ::SearchPathW(nullptr, L"node.exe", nullptr, MAX_PATH, buffer, nullptr);
    if (length == 0 || length >= MAX_PATH) {
        return std::nullopt;
    }
    return std::filesystem::path(buffer);
}

std::unique_ptr<FixtureServer> FixtureServer::Launch(const FixtureOptions& options)
{
    const auto node = FindNode();
    if (!node) {
        return nullptr;
    }
    std::unique_ptr<FixtureServer> server(new FixtureServer());  // private constructor
    server->size_ = options.size;

    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE stdoutRead = nullptr;
    HANDLE stdoutWrite = nullptr;
    HANDLE stdinRead = nullptr;
    HANDLE stdinWrite = nullptr;
    if (!::CreatePipe(&stdoutRead, &stdoutWrite, &inherit, 0) || !::CreatePipe(&stdinRead, &stdinWrite, &inherit, 0)) {
        throw std::runtime_error("CreatePipe failed");
    }
    ::SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(stdinWrite, HANDLE_FLAG_INHERIT, 0);
    server->stdinWrite_ = stdinWrite;

    server->job_ = ::CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    ::SetInformationJobObject(server->job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

    std::wstring commandLine = L"\"" + node->wstring() + L"\" \"" + base::ToUtf16(LDM_FIXTURE_SCRIPT) +
                               L"\" --quiet --size " + std::to_wstring(options.size) + L" --delay " +
                               std::to_wstring(options.delayMs) + L" --chunk " + std::to_wstring(options.chunkBytes);
    if (!options.mediaRoot.empty()) {
        commandLine += L" --media-root \"" + options.mediaRoot.wstring() + L"\"";
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = stdinRead;
    startup.hStdOutput = stdoutWrite;
    startup.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};
    const BOOL created = ::CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE,
                                          CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &process);
    ::CloseHandle(stdoutWrite);
    ::CloseHandle(stdinRead);
    if (!created) {
        ::CloseHandle(stdoutRead);
        throw std::runtime_error("could not start node");
    }
    ::AssignProcessToJobObject(server->job_, process.hProcess);
    ::ResumeThread(process.hThread);
    ::CloseHandle(process.hThread);
    server->process_ = process.hProcess;

    // The first stdout line is the ready event with the port.
    std::string line;
    char c = 0;
    DWORD read = 0;
    while (::ReadFile(stdoutRead, &c, 1, &read, nullptr) && read == 1 && c != '\n') {
        line.push_back(c);
    }
    ::CloseHandle(stdoutRead);
    const auto ready = nlohmann::json::parse(line, nullptr, false);
    if (ready.is_discarded() || ready.value("event", "") != "ready") {
        throw std::runtime_error("fixture did not start: " + line);
    }
    server->port_ = ready.at("port").get<int>();
    server->ftpPort_ = ready.value("ftpPort", 0);
    return server;
}

FixtureServer::~FixtureServer()
{
    CloseIfValid(stdinWrite_);  // the fixture exits when its stdin closes
    if (process_ != nullptr && ::WaitForSingleObject(process_, 5000) != WAIT_OBJECT_0) {
        ::TerminateProcess(process_, 1);
    }
    CloseIfValid(process_);
    CloseIfValid(job_);
}

std::string FixtureServer::Url(const std::string& path) const
{
    return "http://127.0.0.1:" + std::to_string(port_) + path;
}

std::string FixtureServer::FtpUrl(const std::string& path, const std::string& user) const
{
    const std::string login = user.empty() ? std::string() : user + ":x@";
    return "ftp://" + login + "127.0.0.1:" + std::to_string(ftpPort_) + path;
}

nlohmann::json FixtureServer::Log() const
{
    return nlohmann::json::parse(HttpGet(Url("/control/log")));
}

void FixtureServer::Bump() const
{
    (void)HttpGet(Url("/control/bump"));
}

void FixtureServer::Reset() const
{
    (void)HttpGet(Url("/control/reset"));
}

}  // namespace ldm::test

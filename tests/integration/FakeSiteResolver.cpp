// A stand-in for yt-dlp in the resolver tests. It reads what to do from files the test wrote into
// the folder named by LDM_FAKE_SITE_DIR, keyed by the video id of the page address it was given
// (the last argument): <id>.json goes to standard output, <id>.err to standard error, <id>.sleep
// holds milliseconds to wait first, and <id>.code the exit code. It writes its arguments, one per
// line, to <id>.args.
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

namespace {

std::string ReadAll(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void Write(FILE* to, const std::string& text)
{
    std::fwrite(text.data(), 1, text.size(), to);
    std::fflush(to);
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    wchar_t folder[MAX_PATH] = {};
    if (argc < 2 || ::GetEnvironmentVariableW(L"LDM_FAKE_SITE_DIR", folder, MAX_PATH) == 0) {
        return 90;
    }
    const std::wstring page = argv[argc - 1];
    const size_t equals = page.rfind(L"v=");
    if (equals == std::wstring::npos) {
        return 91;
    }
    const std::filesystem::path base = std::filesystem::path(folder) / page.substr(equals + 2);

    std::wofstream args(base.wstring() + L".args", std::ios::trunc);
    for (int i = 1; i < argc; ++i) {
        args << argv[i] << L'\n';
    }
    args.close();

    if (const std::string sleep = ReadAll(base.wstring() + L".sleep"); !sleep.empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(std::stoi(sleep)));
    }
    Write(stderr, ReadAll(base.wstring() + L".err"));
    Write(stdout, ReadAll(base.wstring() + L".json"));
    const std::string code = ReadAll(base.wstring() + L".code");
    return code.empty() ? 0 : std::stoi(code);
}

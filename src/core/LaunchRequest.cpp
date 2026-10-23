#include "core/LaunchRequest.h"

#include "base/Utf.h"
#include "core/FileNames.h"
#include "core/Url.h"

#include <filesystem>

namespace ldm::core {

CommandLineParse ParseCommandLine(const std::vector<std::wstring>& arguments)
{
    CommandLineParse parse;
    LaunchRequest& request = parse.request;
    const auto fail = [&](std::string message) {
        parse.error = std::move(message);
        return parse;
    };
    for (size_t i = 0; i < arguments.size(); ++i) {
        const std::wstring& argument = arguments[i];
        const auto value = [&]() -> std::optional<std::wstring> {
            if (i + 1 >= arguments.size()) {
                return std::nullopt;
            }
            return arguments[++i];
        };
        if (argument == L"--help" || argument == L"-h" || argument == L"/?") {
            request.help = true;
        } else if (argument == L"--silent") {
            request.silent = true;
        } else if (argument == L"--queue-only") {
            request.queueOnly = true;
        } else if (argument == L"--start-queue") {
            request.startQueue = true;
        } else if (argument == L"--tray") {
            request.tray = true;
        } else if (argument == L"--save-dir") {
            const auto folder = value();
            if (!folder || !std::filesystem::path(*folder).is_absolute()) {
                return fail("--save-dir needs a full folder path, such as C:\\Downloads.");
            }
            request.saveDir = base::ToUtf8(*folder);
        } else if (argument == L"--name") {
            const auto name = value();
            if (!name || name->empty()) {
                return fail("--name needs a file name.");
            }
            const std::string utf8 = base::ToUtf8(*name);
            if (SanitizeFileName(utf8) != utf8) {
                return fail("--name must be a plain file name without \\ / : * ? \" < > |.");
            }
            request.fileName = utf8;
        } else if (argument.rfind(L"--", 0) == 0) {
            return fail("Unknown switch " + base::ToUtf8(argument) + ". Run ldm.exe --help for the list.");
        } else {
            if (request.url) {
                return fail("Only one address can be given per launch.");
            }
            const std::string url = base::ToUtf8(argument);
            if (!ParseUrl(url)) {
                return fail("\"" + url + "\" is not an http, https, ftp or ftps address.");
            }
            request.url = url;
        }
    }
    if (!request.url && (request.saveDir || request.fileName || request.silent || request.queueOnly)) {
        return fail("--save-dir, --name, --silent and --queue-only need an address.");
    }
    return parse;
}

std::wstring CommandLineHelp()
{
    return L"ldm.exe [ADDRESS] [options]\n"
           L"\n"
           L"ADDRESS: an http, https, ftp or ftps address to download\n"
           L"--save-dir FOLDER: save into FOLDER instead of the category folder\n"
           L"--name FILE: save as FILE\n"
           L"--silent: add without showing any dialog\n"
           L"--queue-only: add without starting the download\n"
           L"--start-queue: reserved for download queues, which are not available yet\n"
           L"--tray: start hidden in the notification area\n"
           L"--help: show this help\n"
           L"\n"
           L"When Last Download Manager is already running, the request goes to it.";
}

}  // namespace ldm::core

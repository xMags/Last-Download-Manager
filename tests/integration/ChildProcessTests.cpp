// RunChildProcess against real processes (cmd.exe and the programs it starts): exit codes, output,
// time limits, stopping, and that nothing a program starts outlives its run.
#include "platform/win/ChildProcess.h"

#include <gtest/gtest.h>

#include <windows.h>

#include <shellapi.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

using ldm::platform::ChildProcessEnd;
using ldm::platform::ChildProcessRequest;
using ldm::platform::RunChildProcess;

std::filesystem::path Cmd()
{
    wchar_t system[MAX_PATH] = {};
    const UINT length = ::GetSystemDirectoryW(system, MAX_PATH);
    return std::filesystem::path(std::wstring(system, length)) / L"cmd.exe";
}

// cmd.exe reads its own command line, so the script is passed as words after /c.
ChildProcessRequest CmdRequest(std::vector<std::wstring> script)
{
    ChildProcessRequest request;
    request.program = Cmd();
    request.arguments = {L"/d", L"/c"};
    request.arguments.insert(request.arguments.end(), script.begin(), script.end());
    return request;
}

std::vector<std::wstring> SplitLikeTheRuntime(const std::wstring& commandLine)
{
    int count = 0;
    wchar_t** words = ::CommandLineToArgvW(commandLine.c_str(), &count);
    std::vector<std::wstring> result(words, words + count);
    ::LocalFree(words);
    return result;
}

TEST(ChildProcessTest, QuotedArgumentsAreReadBackUnchanged)
{
    const std::vector<std::wstring> cases = {L"plain", L"", L"two words", LR"(say "hi")", LR"(C:\dir\)",
                                             LR"(C:\dir with space\)", LR"(a\\"b)", LR"(\\server\share)", L"tab\there"};
    std::wstring line = L"program.exe";
    for (const auto& argument : cases) {
        line += L' ' + ldm::platform::QuoteArgument(argument);
    }
    const auto words = SplitLikeTheRuntime(line);
    ASSERT_EQ(words.size(), cases.size() + 1);
    for (size_t i = 0; i < cases.size(); ++i) {
        EXPECT_EQ(words[i + 1], cases[i]) << "case " << i;
    }
}

TEST(ChildProcessTest, ReportsExitCodeAndOutput)
{
    const auto result = RunChildProcess(CmdRequest({L"echo", L"hello&", L"exit", L"/b", L"7"}));
    ASSERT_TRUE(result) << result.GetError().message;
    EXPECT_EQ(result.Value().end, ChildProcessEnd::Exited);
    EXPECT_EQ(result.Value().exitCode, 7u);
    EXPECT_EQ(result.Value().output, "hello\r\n");
    EXPECT_FALSE(result.Value().outputTruncated);
}

TEST(ChildProcessTest, StandardErrorIsMixedInUnlessKeptApart)
{
    const std::vector<std::wstring> script{L"echo", L"data&", L"echo", L"warning", L"1>&2"};
    const auto mixed = RunChildProcess(CmdRequest(script));
    ASSERT_TRUE(mixed) << mixed.GetError().message;
    EXPECT_NE(mixed.Value().output.find("warning"), std::string::npos);
    EXPECT_TRUE(mixed.Value().errors.empty());

    auto request = CmdRequest(script);
    request.separateErrors = true;
    const auto apart = RunChildProcess(request);
    ASSERT_TRUE(apart) << apart.GetError().message;
    EXPECT_EQ(apart.Value().output, "data\r\n");
    EXPECT_EQ(apart.Value().errors, "warning \r\n");
}

// Errors beyond their own limit are drained too, while the output is kept whole.
TEST(ChildProcessTest, ErrorsBeyondTheirLimitAreDroppedWithoutStallingTheProgram)
{
    auto request = CmdRequest({L"(for", L"/l", L"%i", L"in", L"(1,1,20000)", L"do", L"@echo", L"0123456789", L"1>&2)",
                               L"&", L"echo", L"done"});
    request.separateErrors = true;
    request.limits.maxErrorBytes = 500;
    request.limits.timeout = std::chrono::seconds(60);
    const auto result = RunChildProcess(request);
    ASSERT_TRUE(result) << result.GetError().message;
    EXPECT_EQ(result.Value().end, ChildProcessEnd::Exited);
    EXPECT_EQ(result.Value().errors.size(), 500u);
    EXPECT_TRUE(result.Value().errorsTruncated);
    EXPECT_EQ(result.Value().output, "done\r\n");
}

TEST(ChildProcessTest, RefusesARelativeProgramPath)
{
    ChildProcessRequest request;
    request.program = L"cmd.exe";
    const auto result = RunChildProcess(request);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.GetError().code, ldm::base::ErrorCode::InvalidArgument);
}

// ping runs as cmd's child and holds the output pipe; the run still ends at the time limit,
// which it can only do when the job ended ping too.
TEST(ChildProcessTest, TimeLimitEndsTheProgramAndWhatItStarted)
{
    auto request = CmdRequest({L"ping", L"-n", L"30", L"127.0.0.1"});
    request.limits.timeout = std::chrono::milliseconds(500);
    const auto started = std::chrono::steady_clock::now();
    const auto result = RunChildProcess(request);
    ASSERT_TRUE(result) << result.GetError().message;
    EXPECT_EQ(result.Value().end, ChildProcessEnd::TimedOut);
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(10));
}

// cmd exits at once and leaves ping running with the output pipe; the run must not wait for it.
TEST(ChildProcessTest, WhatTheProgramLeftRunningIsEnded)
{
    auto request = CmdRequest({L"start", L"/b", L"ping", L"-n", L"30", L"127.0.0.1"});
    const auto started = std::chrono::steady_clock::now();
    const auto result = RunChildProcess(request);
    ASSERT_TRUE(result) << result.GetError().message;
    EXPECT_EQ(result.Value().end, ChildProcessEnd::Exited);
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(10));
}

TEST(ChildProcessTest, StopEventEndsTheRun)
{
    HANDLE stop = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ASSERT_NE(stop, nullptr);
    std::thread stopper([stop] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        ::SetEvent(stop);
    });
    const auto result = RunChildProcess(CmdRequest({L"ping", L"-n", L"30", L"127.0.0.1"}), stop);
    stopper.join();
    ::CloseHandle(stop);
    ASSERT_TRUE(result) << result.GetError().message;
    EXPECT_EQ(result.Value().end, ChildProcessEnd::Stopped);
}

// More output than the limit: the rest is drained, so the program finishes instead of blocking.
TEST(ChildProcessTest, OutputBeyondTheLimitIsDroppedWithoutStallingTheProgram)
{
    auto request = CmdRequest({L"for", L"/l", L"%i", L"in", L"(1,1,20000)", L"do", L"@echo", L"0123456789"});
    request.limits.maxOutputBytes = 1000;
    request.limits.timeout = std::chrono::seconds(60);
    const auto result = RunChildProcess(request);
    ASSERT_TRUE(result) << result.GetError().message;
    EXPECT_EQ(result.Value().end, ChildProcessEnd::Exited);
    EXPECT_EQ(result.Value().output.size(), 1000u);
    EXPECT_TRUE(result.Value().outputTruncated);
}

TEST(ChildProcessTest, RunsInTheGivenWorkingDirectory)
{
    auto request = CmdRequest({L"cd"});
    request.workingDirectory = std::filesystem::temp_directory_path();
    const auto result = RunChildProcess(request);
    ASSERT_TRUE(result) << result.GetError().message;
    std::filesystem::path reported(result.Value().output.substr(0, result.Value().output.find('\r')));
    EXPECT_TRUE(std::filesystem::equivalent(reported, *request.workingDirectory)) << result.Value().output;
}

}  // namespace

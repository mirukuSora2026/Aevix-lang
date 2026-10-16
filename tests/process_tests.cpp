// ========================
//
// Created By : miruku_sora2026
//
// File name : process_tests.cpp
//
// Description :
//     인수 보존, 출력 전달, 시간 제한과 임시 디렉터리 정리를 검증한다.
//     Verify argument preservation, output redirection, timeouts, and temporary cleanup.
//
// ========================
#include "aevix/Support/Process.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int processMain(const std::vector<std::string>& arguments)
{
    if (arguments.size() > 1 && arguments[1] == "--echo")
    {
#ifdef _WIN32
        _setmode(_fileno(stdout), _O_BINARY);
        _setmode(_fileno(stderr), _O_BINARY);
#endif
        for (std::size_t index = 2; index < arguments.size(); ++index) std::cout << arguments[index].size() << ':' << arguments[index] << '\n';
        std::cerr << "child stderr\n";
        return 42;
    }
    if (arguments.size() > 1 && arguments[1] == "--sleep")
    { std::this_thread::sleep_for(std::chrono::seconds(5)); return 0; }
    unsigned checks = 0, failures = 0;
    const auto expect = [&](bool condition, const char* message)
    { ++checks; if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; } };
    std::string error;
    const auto self = aevix::currentExecutable(error);
    expect(!self.empty() && std::filesystem::is_regular_file(self), "current executable exists");
    std::filesystem::path owned;
    {
        auto temporary = aevix::TemporaryDirectory::create("aevix-process-", error);
        expect(temporary.has_value(), "private temporary directory created");
        if (!temporary) return 1;
        owned = temporary->path();
        expect(std::filesystem::is_directory(owned), "temporary directory exists");
        auto moved = std::move(*temporary);
        expect(temporary->path().empty() && moved.path() == owned, "temporary ownership moves once");
        const std::vector<std::string> values{"", "space value", "quote\"value", "trailing\\", "slash\\\"quote", "$(touch should-not-exist); & | < >", "\xed\x95\x9c\xea\xb8\x80"};
        std::vector<std::string> command{aevix::utf8Path(self), "--echo"};
        command.insert(command.end(), values.begin(), values.end());
        aevix::ProcessOptions options;
        options.stdoutFile = owned / "stdout with spaces.log";
        options.stderrFile = owned / "stderr.log";
        options.timeout = std::chrono::seconds(3);
        const auto child = aevix::runProcess(command, options);
        expect(child.started && child.exitedNormally && child.exitCode == 42, "normal child status preserved");
        const auto read = [](const std::filesystem::path& path)
        { std::ifstream file(path, std::ios::binary); return std::string(std::istreambuf_iterator<char>(file), {}); };
        std::string expected;
        for (const auto& value : values) expected += std::to_string(value.size()) + ':' + value + '\n';
        expect(read(options.stdoutFile) == expected, "all argument bytes preserved without shell expansion");
        expect(read(options.stderrFile) == "child stderr\n", "stderr redirected independently");
        const auto absent = aevix::runProcess({aevix::utf8Path(owned / "missing-command")});
        expect(!absent.started && !absent.error.empty(), "missing executable diagnosed");
        expect(!aevix::runProcess({}).started, "empty argument list rejected");
        expect(!aevix::runProcess({std::string("bad\0argument", 12)}).started, "embedded NUL rejected");
        options.stdoutFile.clear(); options.stderrFile.clear();
        options.timeout = std::chrono::milliseconds(40);
        const auto stalled = aevix::runProcess({aevix::utf8Path(self), "--sleep"}, options);
        expect(stalled.started && stalled.timedOut && !stalled.exitedNormally, "timeout terminates and reaps child");
    }
    expect(!std::filesystem::exists(owned), "temporary directory cleaned after scope");
    expect(!aevix::TemporaryDirectory::create("../escape", error), "unsafe temporary prefix rejected");
    std::cout << checks << " process checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
{
    std::vector<std::string> arguments;
    for (int index = 0; index < argc; ++index) arguments.push_back(aevix::utf8Path(std::filesystem::path(argv[index])));
    return processMain(arguments);
}
#else
int main(int argc, char** argv) { return processMain({argv, argv + argc}); }
#endif

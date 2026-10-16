// ========================
//
// Created By : miruku_sora2026
//
// File name : Process.hpp
//
// Description :
//     운영체제별 프로세스 실행과 임시 디렉터리의 수명 관리를 추상화한다.
//     Abstract platform process execution and temporary-directory lifetimes.
//
// ========================
#pragma once
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aevix
{
    struct ProcessOptions final
    {
        std::filesystem::path stdoutFile;
        std::filesystem::path stderrFile;
        std::chrono::milliseconds timeout{0};
    };

    struct ProcessResult final
    {
        bool started = false;
        bool exitedNormally = false;
        bool timedOut = false;
        int exitCode = -1;
        int signal = 0;
        std::string error;
    };

    // 인수는 쉘 문법으로 해석하지 않으며 UTF-8로 전달한다.
    // Pass UTF-8 arguments without interpreting shell syntax.
    ProcessResult runProcess(const std::vector<std::string>& arguments,
                             const ProcessOptions& options = {});
    std::string utf8Path(const std::filesystem::path& path);
    std::filesystem::path pathFromUTF8(std::string_view text);
    std::filesystem::path currentExecutable(std::string& error);

    class TemporaryDirectory final
    {
    public:
        static std::optional<TemporaryDirectory> create(std::string_view prefix, std::string& error);
        ~TemporaryDirectory();
        TemporaryDirectory(TemporaryDirectory&& other) noexcept;
        TemporaryDirectory& operator=(TemporaryDirectory&& other) noexcept;
        TemporaryDirectory(const TemporaryDirectory&) = delete;
        TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
        const std::filesystem::path& path() const { return directory_; }
    private:
        explicit TemporaryDirectory(std::filesystem::path directory) : directory_(std::move(directory)) {}
        std::filesystem::path directory_;
    };
}

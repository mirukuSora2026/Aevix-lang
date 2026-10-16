// ========================
//
// Created By : miruku_sora2026
//
// File name : Process.cpp
//
// Description :
//     쉘을 거치지 않고 자식 프로세스를 실행하며 종료와 임시 자원을 관리한다.
//     Launch children without a shell and manage termination and temporary resources.
//
// ========================
#include "aevix/Support/Process.hpp"
#include <algorithm>
#include <cstdint>
#include <cerrno>
#include <cstring>
#include <limits>
#include <thread>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <random>
#include <sstream>
#else
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
extern char** environ;
#endif

namespace aevix
{
    std::string utf8Path(const std::filesystem::path& path)
    {
        const auto text = path.u8string();
        return {reinterpret_cast<const char*>(text.data()), text.size()};
    }
    std::filesystem::path pathFromUTF8(std::string_view text)
    {
        return std::filesystem::path(std::u8string(text.begin(), text.end()));
    }

    std::filesystem::path currentExecutable(std::string& error)
    {
        error.clear();
        const auto canonical = [](const std::filesystem::path& path)
        {
            std::error_code filesystemError;
            const auto resolved = std::filesystem::canonical(path, filesystemError);
            return filesystemError ? path : resolved;
        };
#ifdef _WIN32
        std::vector<wchar_t> buffer(1024);
        for (;;)
        {
            const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (!length) break;
            if (length < buffer.size()) return canonical(std::filesystem::path(std::wstring(buffer.data(), length)));
            if (buffer.size() >= 32768) break;
            buffer.resize(buffer.size() * 2);
        }
#elif defined(__APPLE__)
        std::uint32_t size = 0;
        _NSGetExecutablePath(nullptr, &size);
        std::vector<char> buffer(size);
        if (_NSGetExecutablePath(buffer.data(), &size) == 0) return canonical(std::filesystem::path(buffer.data()));
#elif defined(__linux__)
        std::vector<char> buffer(1024);
        for (;;)
        {
            const auto size = readlink("/proc/self/exe", buffer.data(), buffer.size());
            if (size < 0) break;
            if (static_cast<std::size_t>(size) < buffer.size()) return canonical(std::filesystem::path(std::string(buffer.data(), static_cast<std::size_t>(size))));
            if (buffer.size() >= 1024 * 1024) break;
            buffer.resize(buffer.size() * 2);
        }
#endif
        error = "Cannot locate the running executable.";
        return {};
    }

    namespace
    {
#ifdef _WIN32
        struct Handle final
        {
            HANDLE value = INVALID_HANDLE_VALUE;
            ~Handle() { if (value != INVALID_HANDLE_VALUE && value != nullptr) CloseHandle(value); }
        };
        std::wstring wide(const std::string& text)
        {
            if (text.empty()) return {};
            const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
            if (!size) return {};
            std::wstring result(static_cast<std::size_t>(size), L'\0');
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size);
            return result;
        }
        std::wstring quote(const std::wstring& argument)
        {
            // CRT 규칙대로 따옴표 앞과 마지막의 역슬래시를 두 배로 만든다.
            // Double backslashes before quotes and at the end using CRT argument rules.
            std::wstring result = L"\"";
            std::size_t slashes = 0;
            for (const auto character : argument)
            {
                if (character == L'\\') { ++slashes; continue; }
                result.append(character == L'"' ? slashes * 2 + 1 : slashes, L'\\');
                slashes = 0;
                result += character;
            }
            result.append(slashes * 2, L'\\');
            result += L'"';
            return result;
        }
        HANDLE childHandle(DWORD standard, const std::filesystem::path& destination, bool input)
        {
            SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
            if (!destination.empty())
                return CreateFileW(destination.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            const auto existing = GetStdHandle(standard);
            HANDLE duplicate = INVALID_HANDLE_VALUE;
            if (existing != INVALID_HANDLE_VALUE && existing != nullptr
                && DuplicateHandle(GetCurrentProcess(), existing, GetCurrentProcess(), &duplicate, 0, TRUE, DUPLICATE_SAME_ACCESS))
                return duplicate;
            return CreateFileW(L"NUL", input ? GENERIC_READ : GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        }
#else
        struct Descriptor final
        {
            int value = -1;
            ~Descriptor() { if (value >= 0) close(value); }
        };
        int redirectDescriptor(const std::filesystem::path& path)
        {
            if (path.empty()) return -1;
            int result = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
            if (result >= 0 && result < 3)
            {
                const int original = result;
                result = fcntl(original, F_DUPFD_CLOEXEC, 3);
                close(original);
            }
            return result;
        }
#endif
    }

    ProcessResult runProcess(const std::vector<std::string>& arguments, const ProcessOptions& options)
    {
        ProcessResult result;
        if (arguments.empty() || arguments.front().empty()) { result.error = "Empty process argument list."; return result; }
        for (const auto& argument : arguments)
            if (argument.find('\0') != std::string::npos) { result.error = "Process arguments cannot contain NUL."; return result; }
#ifdef _WIN32
        std::wstring command;
        for (const auto& argument : arguments)
        {
            const auto encoded = wide(argument);
            if (!argument.empty() && encoded.empty()) { result.error = "Process arguments must be valid UTF-8."; return result; }
            if (!command.empty()) command += L' ';
            command += quote(encoded);
        }
        if (command.size() >= 32767) { result.error = "Windows process command line is too long."; return result; }
        Handle input{childHandle(STD_INPUT_HANDLE, {}, true)};
        Handle output{childHandle(STD_OUTPUT_HANDLE, options.stdoutFile, false)};
        Handle error{childHandle(STD_ERROR_HANDLE, options.stderrFile, false)};
        if (input.value == INVALID_HANDLE_VALUE || output.value == INVALID_HANDLE_VALUE || error.value == INVALID_HANDLE_VALUE)
        { result.error = "Cannot prepare child standard handles: " + std::to_string(GetLastError()); return result; }
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = input.value;
        startup.StartupInfo.hStdOutput = output.value;
        startup.StartupInfo.hStdError = error.value;
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        std::vector<unsigned char> attributes(size);
        startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size))
        { result.error = "Cannot initialize child handle list."; return result; }
        HANDLE inherited[] = {input.value, output.value, error.value};
        const BOOL prepared = UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                                       inherited, sizeof(inherited), nullptr, nullptr);
        PROCESS_INFORMATION process{};
        const BOOL launched = prepared && CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &startup.StartupInfo, &process);
        const DWORD launchError = GetLastError();
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        if (!launched) { result.error = "Cannot launch process: " + std::to_string(launchError); return result; }
        Handle child{process.hProcess}, thread{process.hThread};
        result.started = true;
        const DWORD duration = options.timeout.count() > 0
            ? static_cast<DWORD>(std::min<std::int64_t>(options.timeout.count(), INFINITE - 1)) : INFINITE;
        const auto waited = WaitForSingleObject(child.value, duration);
        if (waited == WAIT_TIMEOUT)
        {
            result.timedOut = true;
            TerminateProcess(child.value, 124);
            WaitForSingleObject(child.value, INFINITE);
            result.error = "Process timed out.";
            return result;
        }
        DWORD code = 0;
        if (waited != WAIT_OBJECT_0 || !GetExitCodeProcess(child.value, &code))
        { result.error = "Cannot wait for process: " + std::to_string(GetLastError()); return result; }
        result.exitCode = static_cast<int>(code);
        result.exitedNormally = code < 0x80000000UL;
#else
        Descriptor output{redirectDescriptor(options.stdoutFile)}, error{redirectDescriptor(options.stderrFile)};
        if ((!options.stdoutFile.empty() && output.value < 0) || (!options.stderrFile.empty() && error.value < 0))
        { result.error = "Cannot open child output file: " + std::string(std::strerror(errno)); return result; }
        posix_spawn_file_actions_t actions;
        const int initialized = posix_spawn_file_actions_init(&actions);
        if (initialized != 0) { result.error = "Cannot initialize process actions."; return result; }
        int prepared = 0;
        for (const auto& redirect : {std::pair{output.value, STDOUT_FILENO}, std::pair{error.value, STDERR_FILENO}})
        {
            if (redirect.first < 0 || prepared != 0) continue;
            prepared = posix_spawn_file_actions_adddup2(&actions, redirect.first, redirect.second);
            if (!prepared) prepared = posix_spawn_file_actions_addclose(&actions, redirect.first);
        }
        std::vector<char*> argv;
        for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
        argv.push_back(nullptr);
        pid_t process = 0;
        const int launched = prepared ? prepared : posix_spawnp(&process, argv[0], &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        if (launched != 0) { result.error = "Cannot launch process: " + std::string(std::strerror(launched)); return result; }
        result.started = true;
        int status = 0;
        const auto deadline = std::chrono::steady_clock::now() + options.timeout;
        for (;;)
        {
            const auto waited = waitpid(process, &status, options.timeout.count() > 0 ? WNOHANG : 0);
            if (waited == process) break;
            if (waited < 0)
            {
                if (errno == EINTR) continue;
                result.error = "Cannot wait for process."; return result;
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                result.timedOut = true;
                kill(process, SIGKILL);
                while (waitpid(process, &status, 0) < 0 && errno == EINTR) {}
                result.error = "Process timed out.";
                return result;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        result.exitedNormally = WIFEXITED(status);
        result.signal = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
        result.exitCode = result.exitedNormally ? WEXITSTATUS(status) : 128 + result.signal;
#endif
        return result;
    }

    std::optional<TemporaryDirectory> TemporaryDirectory::create(std::string_view prefix, std::string& error)
    {
        error.clear();
        if (prefix.empty() || prefix.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string_view::npos)
        { error = "Invalid temporary-directory prefix."; return std::nullopt; }
        std::error_code filesystemError;
        const auto base = std::filesystem::temp_directory_path(filesystemError);
        if (filesystemError) { error = filesystemError.message(); return std::nullopt; }
#ifdef _WIN32
        try
        {
            std::random_device random;
            for (unsigned attempt = 0; attempt < 64; ++attempt)
            {
                std::ostringstream suffix;
                suffix << prefix << std::hex << random() << random() << random() << random();
                const auto candidate = base / suffix.str();
                if (CreateDirectoryW(candidate.c_str(), nullptr)) return TemporaryDirectory(candidate);
                if (GetLastError() != ERROR_ALREADY_EXISTS) break;
            }
        }
        catch (const std::exception& exception) { error = exception.what(); return std::nullopt; }
#else
        const auto pattern = (base / (std::string(prefix) + "XXXXXX")).string();
        std::vector<char> buffer(pattern.begin(), pattern.end()); buffer.push_back('\0');
        if (mkdtemp(buffer.data())) return TemporaryDirectory(std::filesystem::path(buffer.data()));
#endif
        error = "Cannot create temporary directory.";
        return std::nullopt;
    }
    TemporaryDirectory::~TemporaryDirectory()
    {
        if (!directory_.empty()) { std::error_code error; std::filesystem::remove_all(directory_, error); }
    }
    TemporaryDirectory::TemporaryDirectory(TemporaryDirectory&& other) noexcept : directory_(std::move(other.directory_))
    { other.directory_.clear(); }
    TemporaryDirectory& TemporaryDirectory::operator=(TemporaryDirectory&& other) noexcept
    {
        if (this != &other)
        {
            if (!directory_.empty()) { std::error_code error; std::filesystem::remove_all(directory_, error); }
            directory_ = std::move(other.directory_); other.directory_.clear();
        }
        return *this;
    }
}

// ========================
//
// Created By : miruku_sora2026
//
// File name : v03_os_runtime_tests.cpp
//
// Description :
//     파일·환경·시계·프로세스 연결층의 소유권, 경계 검사, 오류와 실제 OS 동작을 검증한다.
//     Verify ownership, bounds, errors, and real OS behavior of file, environment, clock, and process bridges.
//
// ========================
#include "aevix/Runtime/OsRuntime.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
#if !defined(_WIN32)
#include <csignal>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if !defined(AEVIX_OS_RUNTIME_TESTING)
#error "Compile this dedicated test with AEVIX_OS_RUNTIME_TESTING."
#endif

namespace
{
    using aevix::FileMode;
    using aevix::OsFile;
    using aevix::OsStatus;
    constexpr std::int32_t good = 0;
    constexpr std::int32_t failure = 1;
    constexpr std::int32_t invalid = 2;
    constexpr std::int32_t invalidUtf8 = 3;
    constexpr std::int32_t noMemory = 4;
    constexpr std::int32_t overflow = 5;
    constexpr std::int32_t stale = 6;
    constexpr auto readWriteCreate =
        std::uint32_t(FileMode::Read | FileMode::Write | FileMode::Create | FileMode::Truncate);
    unsigned checks = 0;
    bool require(bool condition, const char *expression, int line)
    {
        ++checks;
        if (!condition)
            std::fprintf(stderr, "OS runtime check failed at %d: %s\n", line, expression);
        return condition;
    }
#define REQUIRE(...)                                                                                                   \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!require(bool(__VA_ARGS__), #__VA_ARGS__, __LINE__))                                                       \
            return false;                                                                                              \
    } while (false)

    std::string utf8Path(const std::filesystem::path &path)
    {
        const auto encoded = path.u8string();
        return std::string(encoded.begin(), encoded.end());
    }
    std::filesystem::path pathFromUtf8(std::string_view text)
    {
        return std::filesystem::path(std::u8string(text.begin(), text.end()));
    }
    struct Scratch final
    {
        std::filesystem::path root;
        ~Scratch()
        {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }
    };
    std::int32_t openFile(const std::string &path, std::uint32_t mode, std::uint64_t &token)
    {
        return aevix_os_file_open_utf8(path.data(), path.size(), mode, &token);
    }

    /* 할당 실패가 파일 생성 전에 보고되고 기존 소유권을 손상시키지 않는지 확인한다.
     * Check that allocation failure precedes acquisition and preserves existing ownership.
     * @since 0.3 @deprecated_at none @removed_at none */
    bool allocationTests(const std::filesystem::path &root)
    {
        const auto path = utf8Path(root / "allocation failure.bin");
        std::uint64_t token = 99;
        aevix_os_test_fail_after(0);
        const auto copyFailure = openFile(path, readWriteCreate, token);
        aevix_os_test_fail_after(UINT64_MAX);
        REQUIRE(copyFailure == noMemory && token == 0);
        std::error_code error;
        REQUIRE(!std::filesystem::exists(pathFromUtf8(path), error) && !error);

#if defined(_WIN32)
        aevix_os_test_fail_after(2); // UTF-8 복사와 UTF-16 변환 뒤 레지스트리 할당 실패.
                                     // Fail registry allocation after UTF-8 copy and UTF-16 conversion.
#else
        aevix_os_test_fail_after(1); // 경로 복사 뒤 레지스트리 할당 실패.
                                     // Fail registry allocation after the path copy.
#endif
        const auto registryFailure = openFile(path, readWriteCreate, token);
        aevix_os_test_fail_after(UINT64_MAX);
        REQUIRE(registryFailure == noMemory && token == 0);
        REQUIRE(!std::filesystem::exists(pathFromUtf8(path), error) && !error);
        REQUIRE(openFile(path, readWriteCreate, token) == good && token != 0);
        REQUIRE(aevix_os_file_close(token) == good);

        char *normalized = reinterpret_cast<char *>(std::uintptr_t{1});
        std::uint64_t length = 99;
        aevix_os_test_fail_after(1);
        const auto normalizationFailure = aevix_os_path_normalize_utf8("a/../b", 6, &normalized, &length);
        aevix_os_test_fail_after(UINT64_MAX);
        REQUIRE(normalizationFailure == noMemory && normalized == nullptr && length == 0);
        return true;
    }

    /* 바이트 경계, EOF, 파일 길이와 재사용된 슬롯의 세대 검사를 실제 파일로 확인한다.
     * Use real files to check byte bounds, EOF, length, and generations of reused registry slots.
     * @since 0.3 @deprecated_at none @removed_at none */
    bool fileTests(const std::filesystem::path &root)
    {
        const auto path = utf8Path(root / std::filesystem::path(u8"파일 with spaces.bin"));
        const auto missing = utf8Path(root / "missing.bin");
        std::uint64_t token = 0, progress = 99, position = 99;
        REQUIRE(openFile(path, readWriteCreate, token) == good && token != 0);
        REQUIRE((token >> 63) == 0);
        const std::array<unsigned char, 4> bytes{0x41, 0, 0xff, 0x42};
        REQUIRE(aevix_os_file_write(token, bytes.data(), bytes.size(), &progress) == good && progress == bytes.size());
        REQUIRE(aevix_os_file_flush(token) == good);
        std::uint64_t length = 99;
        REQUIRE(aevix_os_file_size(token, &length) == good && length == 4);
        aevix::OsMetadata info;
        REQUIRE(aevix_os_file_metadata(token, &info) == good && info.kind == 1 && info.size == 4);
        REQUIRE(info.modifiedNanoseconds > 0);
        REQUIRE(aevix_os_file_seek(token, 1, 0, &position) == good && position == 1);
        std::array<unsigned char, 7> storage{0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa};
        REQUIRE(aevix_os_file_read(token, storage.data() + 1, 4, &progress) == good && progress == 3);
        REQUIRE(storage[0] == 0xaa && storage[1] == 0 && storage[2] == 0xff && storage[3] == 0x42);
        REQUIRE(storage[4] == 0xaa && storage[5] == 0xaa && storage[6] == 0xaa);
        REQUIRE(aevix_os_file_read(token, storage.data(), storage.size(), &progress) == good && progress == 0);
        REQUIRE(aevix_os_file_read(token, nullptr, 0, &progress) == good && progress == 0);
        REQUIRE(aevix_os_file_write(token, nullptr, 0, &progress) == good && progress == 0);
        REQUIRE(aevix_os_file_read(token, nullptr, 1, &progress) == invalid && progress == 0);
        REQUIRE(aevix_os_file_write(token, nullptr, 1, &progress) == invalid && progress == 0);
        REQUIRE(aevix_os_file_read(token, storage.data(), UINT64_MAX, &progress) == overflow && progress == 0);
        REQUIRE(aevix_os_file_write(token, reinterpret_cast<const void *>(UINTPTR_MAX - 1), 4, &progress) == overflow &&
                progress == 0);
        REQUIRE(aevix_os_file_seek(token, -1, 0, &position) == failure);
        REQUIRE(aevix_os_last_error() != 0 && aevix_os_error_kind() == 6);
        REQUIRE(aevix_os_file_seek(token, 3, 0, &position) == good && position == 3);
        REQUIRE(aevix_os_file_set_len(token, 2) == good);
        REQUIRE(aevix_os_file_size(token, &length) == good && length == 2);
        REQUIRE(aevix_os_file_seek(token, 0, 1, &position) == good && position == 3);
        REQUIRE(aevix_os_file_set_len(token, UINT64_MAX) == overflow);
        REQUIRE(aevix_os_file_set_len(token, 5) == good);
        REQUIRE(aevix_os_file_seek(token, 2, 0, &position) == good && position == 2);
        REQUIRE(aevix_os_file_read(token, storage.data(), 3, &progress) == good && progress == 3);
        REQUIRE(storage[0] == 0 && storage[1] == 0 && storage[2] == 0);

        const auto old = token;
        REQUIRE(aevix_os_file_close(old) == good);
        REQUIRE(aevix_os_file_close(old) == stale);
        REQUIRE(openFile(path, std::uint32_t(FileMode::Read | FileMode::Write), token) == good && token != old);
        aevix_os_file_cleanup(old);
        aevix_os_file_cleanup(old);
        REQUIRE(aevix_os_file_size(token, &length) == good && length == 5);
        std::uint64_t absent = 91;
        REQUIRE(openFile(missing, std::uint32_t(FileMode::Read), absent) == failure && absent == 0);
        REQUIRE(aevix_os_last_error() != 0 && aevix_os_error_kind() == 1);
        const auto savedError = aevix_os_last_error(), savedDomain = aevix_os_error_domain();
        aevix_os_file_cleanup(token);
        aevix_os_file_cleanup(0);
        REQUIRE(aevix_os_last_error() == savedError && aevix_os_error_domain() == savedDomain);
        REQUIRE(aevix_os_file_read(token, nullptr, 0, &progress) == stale);
        REQUIRE(aevix_os_last_error() == 0 && aevix_os_error_kind() == 0);
        REQUIRE(openFile(path, std::uint32_t(FileMode::Write | FileMode::Create | FileMode::Exclusive), token) ==
                    failure &&
                token == 0);
        REQUIRE(aevix_os_error_kind() == 2);

        const char badUtf8[] = {char(0xc0), char(0xaf)};
        const char nulPath[] = {'a', 0, 'b'};
        REQUIRE(aevix_os_file_open_utf8(badUtf8, 2, 1, &token) == invalidUtf8 && token == 0);
        REQUIRE(aevix_os_file_open_utf8(nulPath, 3, 1, &token) == invalid && token == 0);
        REQUIRE(aevix_os_file_open_utf8(nullptr, 1, 1, &token) == invalid && token == 0);
        REQUIRE(aevix_os_file_open_utf8(path.data(), path.size(), 64, &token) == invalid);
        REQUIRE(aevix_os_file_open_utf8(path.data(), path.size(), 1 | 4, &token) == invalid);
        REQUIRE(aevix_os_file_open_utf8(path.data(), path.size(), 2 | 32, &token) == invalid);
        REQUIRE(aevix_os_file_open_utf8(path.data(), path.size(), 1, nullptr) == invalid);
        REQUIRE(aevix_os_file_size(0, nullptr) == invalid);
        REQUIRE(aevix_os_file_seek(0, 0, 3, &position) == invalid);
        REQUIRE(aevix_os_file_read(0, nullptr, 0, nullptr) == invalid);
        REQUIRE(aevix_os_file_write(0, nullptr, 0, nullptr) == invalid);
        REQUIRE(aevix_os_file_metadata(0, nullptr) == invalid);
        REQUIRE(aevix_os_file_flush(0) == stale);
        return true;
    }

    /* 이동·대입·실패 시 소유권 보존을 C++ 타입 수준과 실제 읽기·쓰기로 확인한다.
     * Verify moves, assignment, and failure ownership with type checks and real I/O.
     * @since 0.3 @deprecated_at none @removed_at none */
    bool ownerTests(const std::filesystem::path &root)
    {
        static_assert(!std::is_copy_constructible_v<OsFile> && !std::is_copy_assignable_v<OsFile>);
        static_assert(std::is_nothrow_move_constructible_v<OsFile> && std::is_nothrow_move_assignable_v<OsFile>);
        static_assert(std::is_nothrow_destructible_v<OsFile> && !std::is_convertible_v<OsFile, std::uint64_t>);
        const auto path = utf8Path(root / "owner.bin");
        const auto missing = utf8Path(root / "absent-owner.bin");
        REQUIRE(aevix_os_test_live_files() == 0);
        OsFile first;
        REQUIRE(OsFile::open(path, FileMode::Read | FileMode::Write | FileMode::Create | FileMode::Truncate, first) ==
                OsStatus::Success);
        REQUIRE(bool(first));
        OsFile moved(std::move(first));
        REQUIRE(!first && bool(moved));
        REQUIRE(aevix_os_test_live_files() == 1);
        REQUIRE(OsFile::open(missing, FileMode::Read, moved) == OsStatus::OsFailure && bool(moved));
        std::array<std::byte, 3> data{std::byte{1}, std::byte{2}, std::byte{3}};
        std::uint64_t progress = 0, position = 0;
        REQUIRE(moved.write(aevix::NativeInputBytes(std::span<const std::byte>(data)), progress) == OsStatus::Success &&
                progress == 3);
        OsFile replacement;
        REQUIRE(OsFile::open(path, FileMode::Read, replacement) == OsStatus::Success);
        REQUIRE(aevix_os_test_live_files() == 2);
        replacement = std::move(moved);
        REQUIRE(!moved && bool(replacement));
        REQUIRE(aevix_os_test_live_files() == 1);
        auto &alias = replacement;
        replacement = std::move(alias);
        REQUIRE(bool(replacement));
        REQUIRE(replacement.seek(0, 0, position) == OsStatus::Success && position == 0);
        std::array<std::byte, 3> copied{};
        aevix::NativeOutputBytes output{std::span<std::byte>(copied)};
        REQUIRE(replacement.read(output, progress) == OsStatus::Success && progress == 3 && copied == data);
        REQUIRE(replacement.flush() == OsStatus::Success);
        REQUIRE(replacement.close() == OsStatus::Success && !replacement);
        REQUIRE(replacement.close() == OsStatus::InvalidHandle);
        REQUIRE(first.read(output, progress) == OsStatus::InvalidHandle && progress == 0);

        // 읽기 전용 핸들의 쓰기 실패가 이후 읽기 권한을 소비하지 않는다.
        // A write failure on a read-only handle does not consume its later read capability.
        REQUIRE(OsFile::open(path, FileMode::Read, first) == OsStatus::Success);
        REQUIRE(first.write(aevix::NativeInputBytes(std::span<const std::byte>(data)), progress) ==
                    OsStatus::OsFailure &&
                bool(first));
        REQUIRE(first.read(output, progress) == OsStatus::Success && progress == 3 && copied == data);
        REQUIRE(first.close() == OsStatus::Success);
        REQUIRE(aevix_os_test_live_files() == 0);
        {
            OsFile scoped;
            REQUIRE(OsFile::open(path, FileMode::Read, scoped) == OsStatus::Success);
            REQUIRE(aevix_os_test_live_files() == 1);
        }
        REQUIRE(aevix_os_test_live_files() == 0);
        return true;
    }

    bool normalization(const char *input, const char *expected)
    {
        char *output = nullptr;
        std::uint64_t size = 0;
        REQUIRE(aevix_os_path_normalize_utf8(input, std::strlen(input), &output, &size) == good);
        const bool matches =
            size == std::strlen(expected) && std::memcmp(output, expected, std::size_t(size)) == 0 && output[size] == 0;
        aevix_os_buffer_free(output);
        REQUIRE(matches);
        return true;
    }
    bool pathTests(const std::filesystem::path &root)
    {
        const auto directory = utf8Path(root / "subdir");
        REQUIRE(aevix_os_path_mkdir_utf8(directory.data(), directory.size()) == good);
        aevix::OsMetadata info;
        REQUIRE(aevix_os_path_metadata_utf8(directory.data(), directory.size(), 1, &info) == good && info.kind == 2);
        REQUIRE(aevix_os_path_mkdir_utf8(directory.data(), directory.size()) == failure && aevix_os_error_kind() == 2);
        REQUIRE(aevix_os_path_metadata_utf8(directory.data(), directory.size(), 2, &info) == invalid);
        REQUIRE(aevix_os_path_remove_utf8(directory.data(), directory.size()) == good);
        REQUIRE(aevix_os_path_metadata_utf8(directory.data(), directory.size(), 1, &info) == failure &&
                aevix_os_error_kind() == 1);
#if defined(_WIN32)
        REQUIRE(normalization("C:/a//b/../c/./", "C:\\a\\c"));
        REQUIRE(normalization("a/../../b", "..\\b"));
        REQUIRE(normalization("//server/share/../leaf", "\\\\server\\share\\leaf"));
        REQUIRE(normalization("C:a/../b", "C:b"));
#else
        REQUIRE(normalization("/a//b/../c/./", "/a/c"));
        REQUIRE(normalization("a/../../b", "../b"));
        REQUIRE(normalization("/../../a", "/a"));
        REQUIRE(normalization("a\\b/../c", "c"));
        const auto target = root / "owner.bin", link = root / "owner.link";
        std::error_code error;
        std::filesystem::create_symlink(target, link, error);
        REQUIRE(!error);
        const auto encoded = utf8Path(link);
        REQUIRE(aevix_os_path_metadata_utf8(encoded.data(), encoded.size(), 0, &info) == good && info.kind == 3);
        REQUIRE(aevix_os_path_metadata_utf8(encoded.data(), encoded.size(), 1, &info) == good && info.kind == 1);
#endif
        REQUIRE(normalization("", "."));
        REQUIRE(normalization("a/..", "."));
        REQUIRE(normalization(".", "."));
        char *output = nullptr;
        std::uint64_t length = 0;
        REQUIRE(aevix_os_path_normalize_utf8(nullptr, 0, &output, &length) == good && length == 1 && output[0] == '.');
        aevix_os_buffer_free(output);
        aevix_os_buffer_free(nullptr);
        REQUIRE(aevix_os_path_normalize_utf8(nullptr, 1, &output, &length) == invalid);
        return true;
    }

    bool environmentTests(const std::string &suffix)
    {
        const auto name = "AEVIX_NATIVE_TEST_" + suffix;
        REQUIRE(aevix_os_env_remove_utf8(name.data(), name.size()) == good);
        char *output = nullptr;
        std::uint64_t length = 0;
        std::uint32_t present = 9;
        REQUIRE(aevix_os_env_get_utf8(name.data(), name.size(), &output, &length, &present) == good && present == 0 &&
                !output && length == 0);
        const auto value = std::string("UTF-8 ") + "\xed\x95\x9c\xea\xb8\x80";
        REQUIRE(aevix_os_env_set_utf8(name.data(), name.size(), value.data(), value.size()) == good);
        REQUIRE(aevix_os_env_get_utf8(name.data(), name.size(), &output, &length, &present) == good && present == 1 &&
                length == value.size());
        const bool matches = std::memcmp(output, value.data(), value.size()) == 0 && output[length] == 0;
        aevix_os_buffer_free(output);
        REQUIRE(matches);
        aevix_os_test_fail_after(0);
        const auto setFailure = aevix_os_env_set_utf8(name.data(), name.size(), "changed", 7);
        aevix_os_test_fail_after(UINT64_MAX);
        REQUIRE(setFailure == noMemory);
        REQUIRE(aevix_os_env_get_utf8(name.data(), name.size(), &output, &length, &present) == good &&
                length == value.size());
        aevix_os_buffer_free(output);
        aevix_os_test_fail_after(0);
        const auto getFailure = aevix_os_env_get_utf8(name.data(), name.size(), &output, &length, &present);
        aevix_os_test_fail_after(UINT64_MAX);
        REQUIRE(getFailure == noMemory && !output && length == 0 && present == 0);
        REQUIRE(aevix_os_env_set_utf8(name.data(), name.size(), nullptr, 0) == good);
        REQUIRE(aevix_os_env_get_utf8(name.data(), name.size(), &output, &length, &present) == good && present == 1 &&
                length == 0 && output && output[0] == 0);
        aevix_os_buffer_free(output);
        REQUIRE(aevix_os_env_set_utf8("bad=name", 8, "x", 1) == invalid);
        REQUIRE(aevix_os_env_get_utf8(nullptr, 1, &output, &length, &present) == invalid);
        REQUIRE(aevix_os_env_remove_utf8("", 0) == invalid);
        const char invalidValue[] = {char(0xed), char(0xa0), char(0x80)};
        REQUIRE(aevix_os_env_set_utf8(name.data(), name.size(), invalidValue, 3) == invalidUtf8);
        const char nulValue[] = {'a', 0, 'b'};
        REQUIRE(aevix_os_env_set_utf8(name.data(), name.size(), nulValue, 3) == invalid);
        REQUIRE(aevix_os_env_remove_utf8(name.data(), name.size()) == good);
        REQUIRE(aevix_os_env_get_utf8(name.data(), name.size(), &output, &length, &present) == good && !present);
        return true;
    }

    bool clockTests()
    {
        std::uint64_t before = 0, after = 0;
        REQUIRE(aevix_os_monotonic_ns(&before) == good);
        REQUIRE(aevix_os_sleep_ns(1000000) == good);
        REQUIRE(aevix_os_monotonic_ns(&after) == good && after >= before && after - before >= 1000000);
        std::int64_t unixTime = 0;
        REQUIRE(aevix_os_unix_time_ns(&unixTime) == good && unixTime > 1577836800000000000ll);
        REQUIRE(aevix_os_monotonic_ns(nullptr) == invalid);
        REQUIRE(aevix_os_unix_time_ns(nullptr) == invalid);
        REQUIRE(aevix_os_sleep_ns(UINT64_MAX) == overflow);
        REQUIRE(aevix_os_sleep_ns(0) == good);
        return true;
    }

    std::int32_t spawn(const std::vector<std::string> &arguments, std::uint64_t &token)
    {
        std::vector<aevix::OsText> borrowed;
        for (const auto &argument : arguments)
            borrowed.push_back({argument.data(), argument.size()});
        return aevix_os_process_spawn(borrowed.data(), borrowed.size(), &token);
    }
    /* 첫 자식의 대기 중에도 둘째 자식을 실행할 수 있어야 한다. 자식 자체에 제한 시간을 둔다.
     * Spawning a second child must remain possible while waiting for the first; the child enforces a timeout.
     * @since 0.3 @deprecated_at none @removed_at none */
    bool independentProcessTests(const std::string &executable, const std::filesystem::path &root)
    {
        const auto marker = utf8Path(root / "dependency marker.txt");
        std::uint64_t first = 0, second = 0;
        REQUIRE(spawn({executable, "--await-marker-child", marker}, first) == good);
        std::atomic<bool> entered{false};
        std::int32_t firstStatus = -1;
        aevix::OsExit firstExit;
        std::thread waiting([&] {
            entered = true;
            firstStatus = aevix_os_process_wait(first, &firstExit);
        });
        while (!entered.load())
            std::this_thread::yield();
        (void)aevix_os_sleep_ns(20000000);
        const auto secondStatus = spawn({executable, "--marker-child", marker}, second);
        waiting.join();
        aevix::OsExit secondExit;
        const auto waited = secondStatus == good ? aevix_os_process_wait(second, &secondExit) : secondStatus;
        REQUIRE(secondStatus == good && waited == good && secondExit.code == 0 && secondExit.signal == 0);
        REQUIRE(firstStatus == good && firstExit.code == 0 && firstExit.signal == 0);
        return true;
    }
    bool processTests(const std::string &executable, const std::filesystem::path &root)
    {
        std::uint64_t token = 91;
        aevix_os_test_fail_after(0);
        const auto allocationFailure = spawn({executable, "--argv-child"}, token);
        aevix_os_test_fail_after(UINT64_MAX);
        REQUIRE(allocationFailure == noMemory && token == 0);
        REQUIRE(spawn({executable, "--argv-child", "space argument", "a\"b\\c\\", "", "; touch NEVER_SHELL_EXECUTED"},
                      token) == good &&
                token != 0);
        REQUIRE((token >> 63) == 1);
        std::uint64_t progress = 0;
        REQUIRE(aevix_os_file_read(token, nullptr, 0, &progress) == stale);
        aevix::OsExit result;
        REQUIRE(aevix_os_process_wait(token, &result) == good && result.code == 37 && result.signal == 0);
        REQUIRE(aevix_os_process_wait(token, &result) == stale);
        aevix_os_process_cleanup(token);
        aevix_os_process_cleanup(0);

        const auto marker = utf8Path(root / "child completed.txt");
        REQUIRE(spawn({executable, "--marker-child", marker}, token) == good);
        aevix_os_process_cleanup(token); // 정리는 자식을 죽이지 않고 완료를 기다린다.
                                         // Cleanup waits for completion without killing the child.
        std::error_code error;
        REQUIRE(std::filesystem::exists(pathFromUtf8(marker), error) && !error);
        REQUIRE(aevix_os_process_wait(token, &result) == stale);
        REQUIRE(spawn({utf8Path(root / "not-an-executable")}, token) == failure && token == 0);
        REQUIRE(aevix_os_last_error() != 0 && aevix_os_error_kind() == 1);
        const char invalidArgument[] = {'a', 0, 'b'};
        const std::array<aevix::OsText, 2> invalidArguments{
            {{executable.data(), executable.size()}, {invalidArgument, 3}}};
        REQUIRE(aevix_os_process_spawn(invalidArguments.data(), invalidArguments.size(), &token) == invalid &&
                token == 0);
        REQUIRE(aevix_os_process_spawn(nullptr, 0, &token) == invalid && token == 0);
        REQUIRE(aevix_os_process_spawn(invalidArguments.data(), UINT64_MAX, &token) == overflow && token == 0);
        REQUIRE(aevix_os_process_wait(0, &result) == stale);
        REQUIRE(aevix_os_process_wait(0, nullptr) == invalid);
        std::uint64_t file = 0;
        const auto filePath = utf8Path(root / "owner.bin");
        REQUIRE(openFile(filePath, std::uint32_t(FileMode::Read), file) == good);
        REQUIRE(aevix_os_process_wait(file, &result) == stale);
        REQUIRE(aevix_os_process_try_wait(file, &result) == stale);
        REQUIRE(aevix_os_process_kill(file) == stale);
        REQUIRE(aevix_os_file_close(file) == good);
        REQUIRE(aevix_os_process_try_wait(0, nullptr) == invalid);
        REQUIRE(aevix_os_process_try_wait(0, &result) == stale);
        REQUIRE(aevix_os_process_kill(0) == stale);
        const auto pollMarker = utf8Path(root / "polled child.txt");
        REQUIRE(spawn({executable, "--marker-child", pollMarker}, token) == good);
        std::int32_t polled = std::int32_t(OsStatus::Pending);
        for (unsigned attempt = 0; attempt < 200 && polled == std::int32_t(OsStatus::Pending); ++attempt)
        {
            result = {91, 91};
            polled = aevix_os_process_try_wait(token, &result);
            if (polled == std::int32_t(OsStatus::Pending))
            {
                REQUIRE(result.code == 0 && result.signal == 0);
                (void)aevix_os_sleep_ns(10000000);
            }
        }
        REQUIRE(polled == good && result.code == 0 && result.signal == 0);
        REQUIRE(aevix_os_process_try_wait(token, &result) == stale);
        const auto neverCreated = utf8Path(root / "not-created-marker");
        REQUIRE(spawn({executable, "--await-marker-child", neverCreated}, token) == good);
        REQUIRE(aevix_os_process_try_wait(token, &result) == std::int32_t(OsStatus::Pending));
        REQUIRE(aevix_os_process_kill(token) == good);
        REQUIRE(aevix_os_process_wait(token, &result) == good);
#if defined(_WIN32)
        REQUIRE(result.code == 1 && result.signal == 0);
#else
        REQUIRE(result.code == 0 && result.signal == SIGKILL);
#endif
        REQUIRE(aevix_os_process_kill(token) == stale);
#if !defined(_WIN32)
        REQUIRE(spawn({executable, "--signal-child"}, token) == good);
        REQUIRE(aevix_os_process_wait(token, &result) == good && result.signal == SIGTERM && result.code == 0);
        REQUIRE(spawn({executable, "--broken-pipe-child", utf8Path(root / "closed-reader.fifo")}, token) == good);
        REQUIRE(aevix_os_process_wait(token, &result) == good && result.signal == 0 && result.code == 0);
#endif
        return true;
    }

#if !defined(_WIN32)
    /* 기본 SIGPIPE 처리와 기존 대기 신호를 별도 자식 프로세스에서 함께 확인한다.
     * Verify default SIGPIPE handling and preservation of a pending signal in an isolated child process.
     * @since 0.3 @deprecated_at none @removed_at none */
    bool brokenPipeChild(const char *path)
    {
        REQUIRE(mkfifo(path, 0600) == 0);
        const auto reader = ::open(path, O_RDONLY | O_NONBLOCK);
        REQUIRE(reader >= 0);
        std::uint64_t token = 0, progress = 99;
        REQUIRE(aevix_os_file_open_utf8(path, std::strlen(path), std::uint32_t(FileMode::Write), &token) == good);
        REQUIRE(::close(reader) == 0);
        REQUIRE(std::signal(SIGPIPE, SIG_DFL) != SIG_ERR);
        sigset_t blocked{}, previous{}, pending{};
        sigemptyset(&blocked);
        sigaddset(&blocked, SIGPIPE);
        REQUIRE(pthread_sigmask(SIG_UNBLOCK, &blocked, &previous) == 0);
        REQUIRE(aevix_os_file_write(token, "x", 1, &progress) == failure && progress == 0);
        REQUIRE(aevix_os_last_error() == EPIPE);
        REQUIRE(sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 0);
        REQUIRE(pthread_sigmask(SIG_BLOCK, &blocked, nullptr) == 0);
        REQUIRE(pthread_kill(pthread_self(), SIGPIPE) == 0);
        REQUIRE(sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1);
        REQUIRE(aevix_os_file_write(token, "x", 1, &progress) == failure && progress == 0);
        REQUIRE(sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1);
        int received = 0;
        REQUIRE(sigwait(&blocked, &received) == 0 && received == SIGPIPE);
        REQUIRE(pthread_sigmask(SIG_SETMASK, &previous, nullptr) == 0);
        REQUIRE(aevix_os_file_close(token) == good);
        return true;
    }

    /* FIFO의 열기·읽기가 다른 엔트리를 막지 않는지 검사하며 원시 OS 구조자로 교착을 제한한다.
     * Check that FIFO open/read cannot block unrelated entries, with a raw OS rescuer bounding a regression deadlock.
     * @since 0.3 @deprecated_at none @removed_at none */
    bool independentFileTests(const std::filesystem::path &root)
    {
        const auto path = utf8Path(root / "independent.fifo");
        REQUIRE(mkfifo(path.c_str(), 0600) == 0);
        std::atomic<bool> finished{false}, rescued{false}, started{false};
        std::uint64_t reader = 0, writer = 0;
        std::int32_t readerStatus = -1;
        std::thread opening([&] {
            started = true;
            readerStatus = openFile(path, std::uint32_t(FileMode::Read), reader);
        });
        std::thread rescuer([&] {
            for (unsigned attempt = 0; attempt < 200 && !finished.load(); ++attempt)
                (void)aevix_os_sleep_ns(10000000);
            if (!finished.load()) {
                rescued = true;
                const auto native = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
                if (native >= 0) ::close(native);
            }
        });
        while (!started.load()) std::this_thread::yield();
        (void)aevix_os_sleep_ns(20000000);
        const auto writerStatus = openFile(path, std::uint32_t(FileMode::Write), writer);
        finished = true;
        opening.join();
        rescuer.join();
        REQUIRE(readerStatus == good && writerStatus == good && !rescued.load());

        const auto regular = utf8Path(root / "unrelated.bin");
        std::uint64_t file = 0;
        REQUIRE(openFile(regular, readWriteCreate, file) == good);
        finished = false;
        rescued = false;
        started = false;
        char received = 0;
        std::uint64_t readProgress = 0;
        std::thread reading([&] {
            started = true;
            readerStatus = aevix_os_file_read(reader, &received, 1, &readProgress);
        });
        std::thread readRescuer([&] {
            for (unsigned attempt = 0; attempt < 200 && !finished.load(); ++attempt)
                (void)aevix_os_sleep_ns(10000000);
            if (!finished.load()) {
                rescued = true;
                const auto native = ::open(path.c_str(), O_WRONLY | O_NONBLOCK);
                if (native >= 0) { (void)::write(native, "R", 1); ::close(native); }
            }
        });
        while (!started.load()) std::this_thread::yield();
        (void)aevix_os_sleep_ns(20000000);
        std::uint64_t progress = 0;
        const auto regularStatus = aevix_os_file_write(file, "U", 1, &progress);
        const auto fifoStatus = aevix_os_file_write(writer, "V", 1, &progress);
        finished = true;
        reading.join();
        readRescuer.join();
        REQUIRE(regularStatus == good && fifoStatus == good && readerStatus == good && readProgress == 1);
        REQUIRE(!rescued.load() && received == 'V');
        REQUIRE(aevix_os_file_close(file) == good);
        REQUIRE(aevix_os_file_close(reader) == good);
        REQUIRE(aevix_os_file_close(writer) == good);
        return true;
    }
#endif

    bool concurrencyTests(const std::filesystem::path &root)
    {
        REQUIRE(aevix_os_last_error() == 0);
        std::atomic<bool> success{true};
        std::array<std::thread, 4> threads;
        for (std::size_t index = 0; index < threads.size(); ++index)
            threads[index] = std::thread(
                [&, index]
                {
                    const auto path = utf8Path(root / ("thread-" + std::to_string(index)));
                    for (unsigned round = 0; round < 16; ++round)
                    {
                        std::uint64_t token = 0, progress = 0;
                        const auto value = static_cast<unsigned char>(index + round);
                        if (openFile(path, readWriteCreate, token) != good)
                        {
                            success = false;
                            return;
                        }
                        if (aevix_os_file_write(token, &value, 1, &progress) != good || progress != 1)
                            success = false;
                        if (aevix_os_file_close(token) != good)
                            success = false;
                        const auto absent = path + "-missing";
                        if (openFile(absent, std::uint32_t(FileMode::Read), token) != failure ||
                            aevix_os_error_kind() != 1)
                            success = false;
                    }
                });
        for (auto &thread : threads)
            thread.join();
        REQUIRE(success.load());
        REQUIRE(aevix_os_last_error() == 0);
        REQUIRE(aevix_os_test_live_files() == 0);
        return true;
    }
} // namespace

int main(int argc, char **argv)
{
    // 같은 실행 파일의 자식 모드로 셸 없는 인수 전달을 확인한다.
    // Child modes in this executable verify argument delivery without a shell.
    if (argc > 1 && std::strcmp(argv[1], "--argv-child") == 0)
        return argc == 6 && std::strcmp(argv[2], "space argument") == 0 && std::strcmp(argv[3], "a\"b\\c\\") == 0 &&
                       std::strcmp(argv[4], "") == 0 && std::strcmp(argv[5], "; touch NEVER_SHELL_EXECUTED") == 0
                   ? 37
                   : 93;
    if (argc == 3 && std::strcmp(argv[1], "--marker-child") == 0)
    {
        (void)aevix_os_sleep_ns(10000000);
        std::uint64_t token = 0, progress = 0;
        if (aevix_os_file_open_utf8(argv[2], std::strlen(argv[2]), readWriteCreate, &token) != good)
            return 94;
        const bool written = aevix_os_file_write(token, "done", 4, &progress) == good && progress == 4;
        return aevix_os_file_close(token) == good && written ? 0 : 95;
    }
    if (argc == 3 && std::strcmp(argv[1], "--await-marker-child") == 0)
    {
        for (unsigned attempt = 0; attempt < 200; ++attempt) {
            std::error_code error;
            if (std::filesystem::exists(pathFromUtf8(argv[2]), error) && !error) return 0;
            (void)aevix_os_sleep_ns(10000000);
        }
        return 88;
    }
#if !defined(_WIN32)
    if (argc == 2 && std::strcmp(argv[1], "--signal-child") == 0)
    {
        std::raise(SIGTERM);
        return 96;
    }
    if (argc == 3 && std::strcmp(argv[1], "--broken-pipe-child") == 0)
        return brokenPipeChild(argv[2]) ? 0 : 97;
#endif
    std::uint64_t stamp = 0;
    if (aevix_os_monotonic_ns(&stamp) != good)
        return 2;
    std::error_code error;
    const auto temporary = std::filesystem::temp_directory_path(error);
    if (error)
    {
        std::fprintf(stderr, "Cannot locate temporary directory.\n");
        return 2;
    }
    Scratch scratch{temporary / ("aevix-os-test-" + std::to_string(stamp))};
    if (!std::filesystem::create_directory(scratch.root, error) || error)
        return 2;
    const auto executable = std::filesystem::absolute(pathFromUtf8(argv[0]), error);
    if (error)
        return 2;
    if (!allocationTests(scratch.root) || !fileTests(scratch.root) || !ownerTests(scratch.root) ||
        !pathTests(scratch.root) || !environmentTests(std::to_string(stamp)) || !clockTests() ||
        !processTests(utf8Path(executable), scratch.root) ||
        !independentProcessTests(utf8Path(executable), scratch.root) || !concurrencyTests(scratch.root))
        return 1;
#if !defined(_WIN32)
    if (!independentFileTests(scratch.root)) return 1;
#endif
    std::printf("OS runtime: %u checks passed.\n", checks);
    return 0;
}

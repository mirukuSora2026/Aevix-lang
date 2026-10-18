// ========================
//
// Created By : miruku_sora2026
//
// File name : OsRuntime.cpp
//
// Description :
//     주소를 공개하지 않는 파일·프로세스 소유권과 검사된 동기식 운영체제 호출을 구현한다.
//     Implement non-address file/process ownership and checked synchronous operating-system calls.
//
// ========================
#include "aevix/Runtime/OsRuntime.hpp"

#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <type_traits>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char **environ;
#endif

namespace
{
    using aevix::OsStatus;
    constexpr auto ok = std::int32_t(OsStatus::Success);
    thread_local std::int32_t nativeError = 0;
    thread_local std::int32_t errorDomain = 0;
#if defined(AEVIX_OS_RUNTIME_TESTING)
    thread_local std::uint64_t allocationBudget = UINT64_MAX;
#endif
    bool allowAllocation() noexcept
    {
#if defined(AEVIX_OS_RUNTIME_TESTING)
        if (!allocationBudget)
            return false;
        if (allocationBudget != UINT64_MAX)
            --allocationBudget;
#endif
        return true;
    }
    void *allocate(std::size_t size) noexcept { return allowAllocation() ? std::malloc(size) : nullptr; }
    [[maybe_unused]] void *zeroAllocate(std::size_t count, std::size_t size) noexcept
    {
        return allowAllocation() ? std::calloc(count, size) : nullptr;
    }
    void begin() noexcept
    {
        nativeError = 0;
        errorDomain = 0;
    }
    constexpr std::int32_t status(OsStatus value) noexcept { return std::int32_t(value); }
    std::int32_t osFailure(std::int32_t error) noexcept
    {
        nativeError = error;
#if defined(_WIN32)
        errorDomain = 2;
#else
        errorDomain = 1;
#endif
        return status(OsStatus::OsFailure);
    }
    struct Buffer final
    {
        void *data = nullptr;
        ~Buffer() noexcept { std::free(data); }
        Buffer() noexcept = default;
        Buffer(const Buffer &) = delete;
        Buffer &operator=(const Buffer &) = delete;
    };
    bool utf8(const char *text, std::size_t size) noexcept
    {
        for (std::size_t offset = 0; offset < size;)
        {
            const auto first = static_cast<unsigned char>(text[offset++]);
            if (first < 0x80)
                continue;
            unsigned extra = 0;
            std::uint32_t value = 0, minimum = 0;
            if (first >= 0xc2 && first <= 0xdf)
            {
                extra = 1;
                value = first & 31;
                minimum = 0x80;
            }
            else if (first >= 0xe0 && first <= 0xef)
            {
                extra = 2;
                value = first & 15;
                minimum = 0x800;
            }
            else if (first >= 0xf0 && first <= 0xf4)
            {
                extra = 3;
                value = first & 7;
                minimum = 0x10000;
            }
            else
                return false;
            if (extra > size - offset)
                return false;
            while (extra--)
            {
                const auto next = static_cast<unsigned char>(text[offset++]);
                if ((next & 0xc0) != 0x80)
                    return false;
                value = value * 64 + (next & 63);
            }
            if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
                return false;
        }
        return true;
    }
    std::int32_t argument(const char *data, std::uint64_t length, Buffer &buffer, bool allowEmpty = false) noexcept
    {
        const auto checked = aevix::NativeInputBytes::checkedRaw(data, length);
        if (!checked)
            return status(checked.status == aevix::NativeBufferStatus::NullWithNonzeroLength ? OsStatus::InvalidInput
                                                                                             : OsStatus::SizeOverflow);
        if ((!length && !allowEmpty) || (length && std::memchr(data, 0, std::size_t(length))))
            return status(OsStatus::InvalidInput);
        if (!utf8(data, std::size_t(length)))
            return status(OsStatus::InvalidUtf8);
        if (length >= std::uint64_t(PTRDIFF_MAX))
            return status(OsStatus::SizeOverflow);
        buffer.data = allocate(std::size_t(length) + 1);
        if (!buffer.data)
            return status(OsStatus::AllocationFailure);
        if (length)
            std::memcpy(buffer.data, data, std::size_t(length));
        static_cast<char *>(buffer.data)[length] = 0;
        return ok;
    }
#if defined(_WIN32)
    std::int32_t wideArgument(const char *data, std::uint64_t length, Buffer &wide, bool allowEmpty = false) noexcept
    {
        if (length > INT_MAX)
            return status(OsStatus::SizeOverflow);
        Buffer text;
        if (auto result = argument(data, length, text, allowEmpty))
            return result;
        const auto count =
            length ? MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, int(length), nullptr, 0) : 0;
        if (length && !count)
            return osFailure(std::int32_t(GetLastError()));
        wide.data = allocate((std::size_t(count) + 1) * sizeof(wchar_t));
        if (!wide.data)
            return status(OsStatus::AllocationFailure);
        if (count && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, int(length),
                                          static_cast<wchar_t *>(wide.data), count))
            return osFailure(std::int32_t(GetLastError()));
        static_cast<wchar_t *>(wide.data)[count] = 0;
        return ok;
    }
    std::int32_t encodeWide(const wchar_t *data, std::size_t count, char **output, std::uint64_t *length) noexcept
    {
        if (count > INT_MAX)
            return status(OsStatus::SizeOverflow);
        const auto size =
            count ? WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, data, int(count), nullptr, 0, nullptr, nullptr)
                  : 0;
        if (count && !size)
            return status(OsStatus::InvalidUtf8);
        auto *text = static_cast<char *>(allocate(std::size_t(size) + 1));
        if (!text)
            return status(OsStatus::AllocationFailure);
        if (size && !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, data, int(count), text, size, nullptr, nullptr))
        {
            std::free(text);
            return status(OsStatus::InvalidUtf8);
        }
        text[size] = 0;
        *output = text;
        *length = std::uint64_t(size);
        return ok;
    }
    struct FileNative final
    {
        HANDLE handle = INVALID_HANDLE_VALUE;
        bool append = false;
    };
    struct ProcessNative final
    {
        HANDLE handle = nullptr;
    };
    void release(FileNative &value) noexcept
    {
        if (value.handle != INVALID_HANDLE_VALUE)
            CloseHandle(value.handle);
        value.handle = INVALID_HANDLE_VALUE;
    }
    void release(ProcessNative &value) noexcept
    {
        if (value.handle)
        {
            WaitForSingleObject(value.handle, INFINITE);
            CloseHandle(value.handle);
        }
        value.handle = nullptr;
    }
#else
    struct FileNative final
    {
        int descriptor = -1;
    };
    struct ProcessNative final
    {
        pid_t pid = -1;
    };
    void release(FileNative &value) noexcept
    {
        if (value.descriptor >= 0)
            ::close(value.descriptor);
        value.descriptor = -1;
    }
    void release(ProcessNative &value) noexcept
    {
        if (value.pid >= 0)
        {
            int result = 0;
            while (waitpid(value.pid, &result, 0) < 0 && errno == EINTR)
            {
            }
        }
        value.pid = -1;
    }
    /* 파이프 단절은 오류 값으로 돌려주고, 기존에 대기 중이던 SIGPIPE는 소비하지 않는다.
     * Return a broken-pipe error without consuming a SIGPIPE that was already pending.
     * 프로세스 전체의 시그널 핸들러는 바꾸지 않으며 현재 동기 호출 동안만 차단한다.
     * Do not change process-wide handlers; block only during this synchronous call on its current thread.
     */
    ssize_t writeWithoutSigpipe(int descriptor, const void *data, std::size_t size) noexcept
    {
        sigset_t blocked{}, previous{}, pending{};
        sigemptyset(&blocked);
        sigaddset(&blocked, SIGPIPE);
        const auto masked = pthread_sigmask(SIG_BLOCK, &blocked, &previous);
        if (masked)
        {
            errno = masked;
            return -1;
        }
        if (sigpending(&pending) < 0)
        {
            const auto error = errno;
            (void)pthread_sigmask(SIG_SETMASK, &previous, nullptr);
            errno = error;
            return -1;
        }
        const bool wasPending = sigismember(&pending, SIGPIPE) == 1;
        ssize_t completed;
        do
        {
            completed = ::write(descriptor, data, size);
        } while (completed < 0 && errno == EINTR);
        const auto error = errno;
        if (completed < 0 && error == EPIPE && !wasPending && sigpending(&pending) == 0 &&
            sigismember(&pending, SIGPIPE) == 1)
        {
            int received = 0, result = 0;
            do
            {
                result = sigwait(&blocked, &received);
            } while (result == EINTR);
        }
        (void)pthread_sigmask(SIG_SETMASK, &previous, nullptr);
        errno = error;
        return completed;
    }
#endif
    template <typename Native> struct Registry final
    {
        struct Entry final
        {
            std::mutex operation;
            std::uint32_t slot = 0, generation = 0;
            bool live = false;
            Native resource;
            Entry *next = nullptr;
        };
        struct Lease final
        {
            Entry *entry = nullptr;
            std::unique_lock<std::mutex> operation;
            Lease() noexcept = default;
            Lease(Entry *value, std::unique_lock<std::mutex> held) noexcept
                : entry(value), operation(std::move(held)) {}
            explicit operator bool() const noexcept { return entry != nullptr; }
            Entry *operator->() const noexcept { return entry; }
            Entry &operator*() const noexcept { return *entry; }
        };
        std::mutex mutex;
        Entry *entries = nullptr;
        std::uint32_t nextSlot = 1;
        // 종료 전에 이 런타임을 사용하는 작업이 모두 끝나야 한다.
        // Tasks using this runtime must be quiescent before coordinated shutdown.
        ~Registry() noexcept
        {
            while (entries)
            {
                auto *next = entries->next;
                if (entries->live)
                    release(entries->resource);
                delete entries;
                entries = next;
            }
        }
        /* 엔트리는 종료 때까지 주소를 유지한다. 전역 잠금 안에서는 엔트리 잠금을 기다리지 않는다.
         * Entries retain stable addresses until shutdown; never wait for an entry lock while holding the global lock.
         * 반환된 대여는 해당 엔트리의 OS 작업과 닫기·재사용만 직렬화한다.
         * The returned lease serializes OS operations, close, and reuse only for its own entry.
         */
        Lease acquire() noexcept
        {
            std::lock_guard registryLock(mutex);
            for (auto *entry = entries; entry; entry = entry->next)
            {
                std::unique_lock operation(entry->operation, std::try_to_lock);
                if (operation && !entry->live && entry->generation != 0x7fffffffu)
                {
                    ++entry->generation;
                    return {entry, std::move(operation)};
                }
            }
            if (!nextSlot)
                return {};
            if (!allowAllocation())
                return {};
            auto *entry = new (std::nothrow) Entry;
            if (!entry)
                return {};
            std::unique_lock operation(entry->operation);
            entry->slot = nextSlot++;
            entry->generation = 1;
            entry->next = entries;
            entries = entry;
            return {entry, std::move(operation)};
        }
        static std::uint64_t token(const Entry &entry) noexcept
        {
            return (std::is_same_v<Native, ProcessNative> ? (std::uint64_t{1} << 63) : 0) |
                   (std::uint64_t(entry.generation) << 32) | entry.slot;
        }
        Entry *find(std::uint64_t value) noexcept
        {
            if (!value)
                return nullptr;
            for (auto *entry = entries; entry; entry = entry->next)
                if (entry->live && token(*entry) == value)
                    return entry;
            return nullptr;
        }
        Lease lockToken(std::uint64_t value) noexcept
        {
            Entry *entry = nullptr;
            {
                std::lock_guard registryLock(mutex);
                entry = find(value);
            }
            if (!entry)
                return {};
            std::unique_lock operation(entry->operation);
            // 전역 잠금을 놓은 사이 닫히거나 재사용됐을 수 있으므로 세대를 다시 검사한다.
            // Recheck the generation because close or reuse may have occurred after global lookup.
            if (!entry->live || token(*entry) != value)
                return {};
            return {entry, std::move(operation)};
        }
        void publish(Entry &entry) noexcept
        {
            std::lock_guard registryLock(mutex);
            entry.live = true;
        }
        void retire(Entry &entry) noexcept
        {
            std::lock_guard registryLock(mutex);
            entry.live = false;
        }
    };
    Registry<FileNative> &files() noexcept
    {
        static Registry<FileNative> value;
        return value;
    }
    Registry<ProcessNative> &processes() noexcept
    {
        static Registry<ProcessNative> value;
        return value;
    }
    std::mutex &environmentMutex() noexcept
    {
        static std::mutex value;
        return value;
    }
    std::int32_t environmentName(const char *name, std::uint64_t length) noexcept
    {
        const auto checked = aevix::NativeInputBytes::checkedRaw(name, length);
        if (!checked)
            return status(checked.status == aevix::NativeBufferStatus::NullWithNonzeroLength ? OsStatus::InvalidInput
                                                                                             : OsStatus::SizeOverflow);
        if (!length || std::memchr(name, '=', std::size_t(length)))
            return status(OsStatus::InvalidInput);
        return ok;
    }
    bool validMode(std::uint32_t mode) noexcept
    {
        return !(mode & ~63u) && (mode & 3u) && (!(mode & (4u | 8u | 16u)) || (mode & 2u)) &&
               (!(mode & 32u) || (mode & 4u));
    }
    std::int32_t bufferStatus(aevix::NativeBufferStatus value) noexcept
    {
        return status(value == aevix::NativeBufferStatus::NullWithNonzeroLength ? OsStatus::InvalidInput
                                                                                : OsStatus::SizeOverflow);
    }
#if defined(_WIN32)
    std::int32_t metadata(HANDLE handle, aevix::OsMetadata &output) noexcept
    {
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(handle, &info))
            return osFailure(std::int32_t(GetLastError()));
        const std::uint64_t ticks =
            (std::uint64_t(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
        constexpr std::uint64_t epoch = 116444736000000000ull;
        const auto difference = ticks >= epoch ? ticks - epoch : epoch - ticks;
        if (difference > std::uint64_t(INT64_MAX) / 100)
            return status(OsStatus::SizeOverflow);
        output.size = (std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        output.modifiedNanoseconds = (ticks >= epoch ? 1 : -1) * std::int64_t(difference * 100);
        output.kind = (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ? 3
                      : (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)   ? 2
                                                                             : 1;
        output.permissions = info.dwFileAttributes;
        return ok;
    }
#else
    std::int32_t metadata(const struct stat &info, aevix::OsMetadata &output) noexcept
    {
#if defined(__APPLE__)
        const auto timestamp = info.st_mtimespec;
#else
        const auto timestamp = info.st_mtim;
#endif
        if (info.st_size < 0 || timestamp.tv_sec > INT64_MAX / 1000000000 || timestamp.tv_sec < INT64_MIN / 1000000000)
            return status(OsStatus::SizeOverflow);
        const auto base = std::int64_t(timestamp.tv_sec) * 1000000000;
        if (base > INT64_MAX - timestamp.tv_nsec)
            return status(OsStatus::SizeOverflow);
        output.size = std::uint64_t(info.st_size);
        output.modifiedNanoseconds = base + timestamp.tv_nsec;
        output.kind = S_ISREG(info.st_mode) ? 1 : S_ISDIR(info.st_mode) ? 2 : S_ISLNK(info.st_mode) ? 3 : 4;
        output.permissions = info.st_mode & 07777;
        return ok;
    }
#endif
} // namespace

extern "C"
{
#if defined(AEVIX_OS_RUNTIME_TESTING)
    void aevix_os_test_fail_after(std::uint64_t budget) noexcept { allocationBudget = budget; }
    std::uint64_t aevix_os_test_live_files() noexcept
    {
        auto &registry = files();
        std::lock_guard lock(registry.mutex);
        std::uint64_t count = 0;
        for (auto *entry = registry.entries; entry; entry = entry->next)
            if (entry->live)
                ++count;
        return count;
    }
#endif
    std::int32_t aevix_os_last_error() noexcept { return nativeError; }
    std::int32_t aevix_os_error_domain() noexcept { return errorDomain; }
    std::int32_t aevix_os_error_kind() noexcept
    {
        if (!errorDomain)
            return 0;
#if defined(_WIN32)
        switch (std::uint32_t(nativeError))
        {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            return 1;
        case ERROR_ALREADY_EXISTS:
        case ERROR_FILE_EXISTS:
            return 2;
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
            return 3;
        case ERROR_OPERATION_ABORTED:
            return 4;
        case ERROR_NO_DATA:
        case ERROR_PIPE_BUSY:
            return 5;
        case ERROR_INVALID_PARAMETER:
        case ERROR_INVALID_NAME:
        case ERROR_NEGATIVE_SEEK:
            return 6;
        default:
            return 7;
        }
#else
        switch (nativeError)
        {
        case ENOENT:
        case ENOTDIR:
            return 1;
        case EEXIST:
            return 2;
        case EACCES:
        case EPERM:
            return 3;
        case EINTR:
            return 4;
        case EAGAIN:
            return 5;
        case EINVAL:
            return 6;
        default:
            return 7;
        }
#endif
    }
    std::int32_t aevix_os_file_open_utf8(const char *path, std::uint64_t length, std::uint32_t mode,
                                         std::uint64_t *output) noexcept
    {
        begin();
        if (!output)
            return status(OsStatus::InvalidInput);
        *output = 0;
        if (!validMode(mode))
            return status(OsStatus::InvalidInput);
        Buffer text;
#if defined(_WIN32)
        if (auto result = wideArgument(path, length, text))
            return result;
#else
        if (auto result = argument(path, length, text))
            return result;
#endif
        auto &registry = files();
        auto entry = registry.acquire();
        if (!entry)
            return status(OsStatus::AllocationFailure);
#if defined(_WIN32)
        const DWORD access = ((mode & 1u) ? GENERIC_READ : 0) | ((mode & 2u) ? GENERIC_WRITE : 0);
        const DWORD disposition = (mode & 32u)  ? CREATE_NEW
                                  : (mode & 4u) ? ((mode & 8u) ? CREATE_ALWAYS : OPEN_ALWAYS)
                                                : ((mode & 8u) ? TRUNCATE_EXISTING : OPEN_EXISTING);
        const auto handle = CreateFileW(static_cast<wchar_t *>(text.data), access,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, disposition,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return osFailure(std::int32_t(GetLastError()));
        entry->resource.handle = handle;
        entry->resource.append = bool(mode & 16u);
#else
        int flags = (mode & 3u) == 3u ? O_RDWR : (mode & 2u) ? O_WRONLY : O_RDONLY;
        flags |= O_CLOEXEC;
        if (mode & 4u)
            flags |= O_CREAT;
        if (mode & 8u)
            flags |= O_TRUNC;
        if (mode & 16u)
            flags |= O_APPEND;
        if (mode & 32u)
            flags |= O_EXCL;
        int descriptor;
        do
        {
            descriptor = ::open(static_cast<char *>(text.data), flags, 0666);
        } while (descriptor < 0 && errno == EINTR);
        if (descriptor < 0)
            return osFailure(errno);
        entry->resource.descriptor = descriptor;
#endif
        registry.publish(*entry);
        *output = registry.token(*entry);
        return ok;
    }
    std::int32_t aevix_os_file_read(std::uint64_t token, void *data, std::uint64_t length,
                                    std::uint64_t *progress) noexcept
    {
        begin();
        if (!progress)
            return status(OsStatus::InvalidInput);
        *progress = 0;
        auto buffer = aevix::NativeOutputBytes::checkedRaw(data, length);
        if (!buffer)
            return bufferStatus(buffer.status);
        auto &registry = files();
        auto entry = registry.lockToken(token);
        if (!entry)
            return status(OsStatus::InvalidHandle);
        if (!length)
            return ok;
#if defined(_WIN32)
        DWORD completed = 0;
        const auto succeeded =
            ReadFile(entry->resource.handle, data, DWORD(length > MAXDWORD ? MAXDWORD : length), &completed, nullptr);
        *progress = completed;
        if (!succeeded)
        {
            const auto error = GetLastError();
            if (error != ERROR_HANDLE_EOF)
                return osFailure(std::int32_t(error));
        }
#else
        ssize_t completed;
        do
        {
            completed = ::read(entry->resource.descriptor, data, std::size_t(length));
        } while (completed < 0 && errno == EINTR);
        if (completed < 0)
            return osFailure(errno);
        *progress = std::uint64_t(completed);
#endif
        return ok;
    }
    std::int32_t aevix_os_file_write(std::uint64_t token, const void *data, std::uint64_t length,
                                     std::uint64_t *progress) noexcept
    {
        begin();
        if (!progress)
            return status(OsStatus::InvalidInput);
        *progress = 0;
        const auto buffer = aevix::NativeInputBytes::checkedRaw(data, length);
        if (!buffer)
            return bufferStatus(buffer.status);
        auto &registry = files();
        auto entry = registry.lockToken(token);
        if (!entry)
            return status(OsStatus::InvalidHandle);
        if (!length)
            return ok;
#if defined(_WIN32)
        DWORD completed = 0;
        OVERLAPPED append{};
        append.Offset = MAXDWORD;
        append.OffsetHigh = MAXDWORD;
        const auto succeeded = WriteFile(entry->resource.handle, data, DWORD(length > MAXDWORD ? MAXDWORD : length),
                                         &completed, entry->resource.append ? &append : nullptr);
        *progress = completed;
        if (!succeeded)
            return osFailure(std::int32_t(GetLastError()));
#else
        const auto completed = writeWithoutSigpipe(entry->resource.descriptor, data, std::size_t(length));
        if (completed < 0)
            return osFailure(errno);
        *progress = std::uint64_t(completed);
#endif
        return ok;
    }
    std::int32_t aevix_os_file_seek(std::uint64_t token, std::int64_t offset, std::uint32_t origin,
                                    std::uint64_t *output) noexcept
    {
        begin();
        if (!output || origin > 2)
            return status(OsStatus::InvalidInput);
        *output = 0;
        auto &registry = files();
        auto entry = registry.lockToken(token);
        if (!entry)
            return status(OsStatus::InvalidHandle);
#if defined(_WIN32)
        LARGE_INTEGER movement{}, position{};
        movement.QuadPart = offset;
        if (!SetFilePointerEx(entry->resource.handle, movement, &position,
                              origin == 0   ? FILE_BEGIN
                              : origin == 1 ? FILE_CURRENT
                                            : FILE_END))
            return osFailure(std::int32_t(GetLastError()));
        *output = std::uint64_t(position.QuadPart);
#else
        const auto position = lseek(entry->resource.descriptor, off_t(offset),
                                    origin == 0   ? SEEK_SET
                                    : origin == 1 ? SEEK_CUR
                                                  : SEEK_END);
        if (position < 0)
            return osFailure(errno);
        *output = std::uint64_t(position);
#endif
        return ok;
    }
    std::int32_t aevix_os_file_flush(std::uint64_t token) noexcept
    {
        begin();
        auto &registry = files();
        auto entry = registry.lockToken(token);
        if (!entry)
            return status(OsStatus::InvalidHandle);
#if defined(_WIN32)
        if (!FlushFileBuffers(entry->resource.handle))
            return osFailure(std::int32_t(GetLastError()));
#else
        int result;
        do
        {
            result = fsync(entry->resource.descriptor);
        } while (result < 0 && errno == EINTR);
        if (result < 0)
            return osFailure(errno);
#endif
        return ok;
    }
    std::int32_t aevix_os_file_close(std::uint64_t token) noexcept
    {
        begin();
        auto &registry = files();
        auto entry = registry.lockToken(token);
        if (!entry)
            return status(OsStatus::InvalidHandle);
        registry.retire(*entry);
#if defined(_WIN32)
        const auto handle = std::exchange(entry->resource.handle, INVALID_HANDLE_VALUE);
        if (!CloseHandle(handle))
            return osFailure(std::int32_t(GetLastError()));
#else
        const auto descriptor = std::exchange(entry->resource.descriptor, -1);
        if (::close(descriptor) < 0)
            return osFailure(errno);
#endif
        return ok;
    }
    void aevix_os_file_cleanup(std::uint64_t token) noexcept
    {
        if (!token)
            return;
        const auto previousError = nativeError, previousDomain = errorDomain;
        (void)aevix_os_file_close(token);
        nativeError = previousError;
        errorDomain = previousDomain;
    }
    std::int32_t aevix_os_file_metadata(std::uint64_t token, aevix::OsMetadata *output) noexcept
    {
        begin();
        if (!output)
            return status(OsStatus::InvalidInput);
        *output = {};
        auto &registry = files();
        auto entry = registry.lockToken(token);
        if (!entry)
            return status(OsStatus::InvalidHandle);
#if defined(_WIN32)
        return metadata(entry->resource.handle, *output);
#else
        struct stat info{};
        if (fstat(entry->resource.descriptor, &info) < 0)
            return osFailure(errno);
        return metadata(info, *output);
#endif
    }
    std::int32_t aevix_os_file_size(std::uint64_t token, std::uint64_t *output) noexcept
    {
        begin();
        if (!output)
            return status(OsStatus::InvalidInput);
        *output = 0;
        aevix::OsMetadata info;
        const auto result = aevix_os_file_metadata(token, &info);
        if (!result)
            *output = info.size;
        return result;
    }
    std::int32_t aevix_os_file_set_len(std::uint64_t token, std::uint64_t length) noexcept
    {
        begin();
        if (length > std::uint64_t(INT64_MAX))
            return status(OsStatus::SizeOverflow);
        auto &registry = files();
        auto entry = registry.lockToken(token);
        if (!entry)
            return status(OsStatus::InvalidHandle);
#if defined(_WIN32)
        FILE_END_OF_FILE_INFO info{};
        info.EndOfFile.QuadPart = LONGLONG(length);
        if (!SetFileInformationByHandle(entry->resource.handle, FileEndOfFileInfo, &info, sizeof(info)))
            return osFailure(std::int32_t(GetLastError()));
#else
        int result;
        do
        {
            result = ftruncate(entry->resource.descriptor, off_t(length));
        } while (result < 0 && errno == EINTR);
        if (result < 0)
            return osFailure(errno);
#endif
        return ok;
    }
    std::int32_t aevix_os_path_metadata_utf8(const char *path, std::uint64_t length, std::uint32_t follow,
                                             aevix::OsMetadata *output) noexcept
    {
        begin();
        if (!output || follow > 1)
            return status(OsStatus::InvalidInput);
        *output = {};
        Buffer text;
#if defined(_WIN32)
        if (auto result = wideArgument(path, length, text))
            return result;
        const auto handle =
            CreateFileW(static_cast<wchar_t *>(text.data), FILE_READ_ATTRIBUTES,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                        FILE_FLAG_BACKUP_SEMANTICS | (follow ? 0 : FILE_FLAG_OPEN_REPARSE_POINT), nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return osFailure(std::int32_t(GetLastError()));
        const auto result = metadata(handle, *output);
        CloseHandle(handle);
        return result;
#else
        if (auto result = argument(path, length, text))
            return result;
        struct stat info{};
        if ((follow ? stat(static_cast<char *>(text.data), &info) : lstat(static_cast<char *>(text.data), &info)) < 0)
            return osFailure(errno);
        return metadata(info, *output);
#endif
    }
    void aevix_os_buffer_free(void *data) noexcept { std::free(data); }
    std::int32_t aevix_os_path_normalize_utf8(const char *path, std::uint64_t length, char **output,
                                              std::uint64_t *outputLength) noexcept
    {
        begin();
        if (!output || !outputLength)
            return status(OsStatus::InvalidInput);
        *output = nullptr;
        *outputLength = 0;
        Buffer source;
        if (auto result = argument(path, length, source, true))
            return result;
        struct Segment final
        {
            std::size_t restore, begin, length;
        };
        if (length > (std::uint64_t(PTRDIFF_MAX) / sizeof(Segment)) - 1)
            return status(OsStatus::SizeOverflow);
        Buffer storage, segments;
        storage.data = allocate(std::size_t(length) + 4);
        segments.data = allocate((std::size_t(length) + 1) * sizeof(Segment));
        if (!storage.data || !segments.data)
            return status(OsStatus::AllocationFailure);
        auto *result = static_cast<char *>(storage.data);
        auto *stack = static_cast<Segment *>(segments.data);
#if defined(_WIN32)
        const auto separator = [](char value) { return value == '/' || value == '\\'; };
        constexpr char slash = '\\';
#else
        const auto separator = [](char value) { return value == '/'; };
        constexpr char slash = '/';
#endif
        std::size_t input = 0, used = 0, count = 0, protectedParts = 0;
        bool absolute = false, driveRelative = false, unc = false;
#if defined(_WIN32)
        const auto alpha = [](char value) { return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z'); };
        if (length >= 2 && alpha(path[0]) && path[1] == ':')
        {
            result[used++] = path[0];
            result[used++] = ':';
            input = 2;
            driveRelative = true;
            if (input < length && separator(path[input]))
            {
                result[used++] = slash;
                absolute = true;
                driveRelative = false;
            }
        }
        else if (length >= 2 && separator(path[0]) && separator(path[1]))
        {
            result[used++] = slash;
            result[used++] = slash;
            input = 2;
            absolute = true;
            unc = true;
        }
#endif
        if (!input && length && separator(path[0]))
        {
            result[used++] = slash;
            absolute = true;
        }
        while (input < length)
        {
            while (input < length && separator(path[input]))
                ++input;
            const auto start = input;
            while (input < length && !separator(path[input]))
                ++input;
            const auto size = input - start;
            if (!size || (size == 1 && path[start] == '.'))
                continue;
            const bool parent = size == 2 && path[start] == '.' && path[start + 1] == '.';
            if (parent && count > protectedParts)
            {
                const auto &previous = stack[count - 1];
                if (!(previous.length == 2 && result[previous.begin] == '.' && result[previous.begin + 1] == '.'))
                {
                    used = previous.restore;
                    --count;
                    continue;
                }
            }
            if (parent && absolute)
                continue;
            const auto restore = used;
            if (used && result[used - 1] != slash && !(driveRelative && used == 2))
                result[used++] = slash;
            stack[count++] = {restore, used, size};
            std::memcpy(result + used, path + start, size);
            used += size;
            if (unc && protectedParts < 2)
                protectedParts = count;
        }
        if (!used)
            result[used++] = '.';
        result[used] = 0;
        *output = result;
        *outputLength = used;
        storage.data = nullptr;
        return ok;
    }
    std::int32_t aevix_os_path_mkdir_utf8(const char *path, std::uint64_t length) noexcept
    {
        begin();
        Buffer text;
#if defined(_WIN32)
        if (auto result = wideArgument(path, length, text))
            return result;
        if (!CreateDirectoryW(static_cast<wchar_t *>(text.data), nullptr))
            return osFailure(std::int32_t(GetLastError()));
#else
        if (auto result = argument(path, length, text))
            return result;
        if (mkdir(static_cast<char *>(text.data), 0777) < 0)
            return osFailure(errno);
#endif
        return ok;
    }
    std::int32_t aevix_os_path_remove_utf8(const char *path, std::uint64_t length) noexcept
    {
        begin();
        Buffer text;
#if defined(_WIN32)
        if (auto result = wideArgument(path, length, text))
            return result;
        const auto attributes = GetFileAttributesW(static_cast<wchar_t *>(text.data));
        if (attributes == INVALID_FILE_ATTRIBUTES)
            return osFailure(std::int32_t(GetLastError()));
        const auto succeeded = (attributes & FILE_ATTRIBUTE_DIRECTORY)
                                   ? RemoveDirectoryW(static_cast<wchar_t *>(text.data))
                                   : DeleteFileW(static_cast<wchar_t *>(text.data));
        if (!succeeded)
            return osFailure(std::int32_t(GetLastError()));
#else
        if (auto result = argument(path, length, text))
            return result;
        if (::remove(static_cast<char *>(text.data)) < 0)
            return osFailure(errno);
#endif
        return ok;
    }
    std::int32_t aevix_os_env_get_utf8(const char *name, std::uint64_t nameLength, char **output, std::uint64_t *length,
                                       std::uint32_t *present) noexcept
    {
        begin();
        if (!output || !length || !present)
            return status(OsStatus::InvalidInput);
        *output = nullptr;
        *length = 0;
        *present = 0;
        if (auto result = environmentName(name, nameLength))
            return result;
        Buffer key;
#if defined(_WIN32)
        if (auto result = wideArgument(name, nameLength, key))
            return result;
        std::lock_guard lock(environmentMutex());
        SetLastError(ERROR_SUCCESS);
        const auto count = GetEnvironmentVariableW(static_cast<wchar_t *>(key.data), nullptr, 0);
        if (!count)
        {
            const auto error = GetLastError();
            if (error == ERROR_ENVVAR_NOT_FOUND)
                return ok;
            if (error != ERROR_SUCCESS)
                return osFailure(std::int32_t(error));
            const wchar_t empty = 0;
            const auto result = encodeWide(&empty, 0, output, length);
            if (!result)
                *present = 1;
            return result;
        }
        Buffer value;
        value.data = allocate(std::size_t(count) * sizeof(wchar_t));
        if (!value.data)
            return status(OsStatus::AllocationFailure);
        SetLastError(ERROR_SUCCESS);
        const auto read =
            GetEnvironmentVariableW(static_cast<wchar_t *>(key.data), static_cast<wchar_t *>(value.data), count);
        if ((!read && GetLastError() != ERROR_SUCCESS) || read >= count)
            return osFailure(std::int32_t(GetLastError()));
        const auto result = encodeWide(static_cast<wchar_t *>(value.data), read, output, length);
        if (!result)
            *present = 1;
        return result;
#else
        if (auto result = argument(name, nameLength, key))
            return result;
        std::lock_guard lock(environmentMutex());
        const auto *value = std::getenv(static_cast<char *>(key.data));
        if (!value)
            return ok;
        const auto size = std::strlen(value);
        if (!utf8(value, size))
            return status(OsStatus::InvalidUtf8);
        auto *result = static_cast<char *>(allocate(size + 1));
        if (!result)
            return status(OsStatus::AllocationFailure);
        std::memcpy(result, value, size + 1);
        *output = result;
        *length = size;
        *present = 1;
        return ok;
#endif
    }
    std::int32_t aevix_os_env_set_utf8(const char *name, std::uint64_t nameLength, const char *value,
                                       std::uint64_t valueLength) noexcept
    {
        begin();
        if (auto result = environmentName(name, nameLength))
            return result;
        Buffer key, text;
#if defined(_WIN32)
        if (auto result = wideArgument(name, nameLength, key))
            return result;
        if (auto result = wideArgument(value, valueLength, text, true))
            return result;
        std::lock_guard lock(environmentMutex());
        if (!SetEnvironmentVariableW(static_cast<wchar_t *>(key.data), static_cast<wchar_t *>(text.data)))
            return osFailure(std::int32_t(GetLastError()));
#else
        if (auto result = argument(name, nameLength, key))
            return result;
        if (auto result = argument(value, valueLength, text, true))
            return result;
        std::lock_guard lock(environmentMutex());
        if (setenv(static_cast<char *>(key.data), static_cast<char *>(text.data), 1) < 0)
            return osFailure(errno);
#endif
        return ok;
    }
    std::int32_t aevix_os_env_remove_utf8(const char *name, std::uint64_t nameLength) noexcept
    {
        begin();
        if (auto result = environmentName(name, nameLength))
            return result;
        Buffer key;
#if defined(_WIN32)
        if (auto result = wideArgument(name, nameLength, key))
            return result;
        std::lock_guard lock(environmentMutex());
        if (!SetEnvironmentVariableW(static_cast<wchar_t *>(key.data), nullptr) &&
            GetLastError() != ERROR_ENVVAR_NOT_FOUND)
            return osFailure(std::int32_t(GetLastError()));
#else
        if (auto result = argument(name, nameLength, key))
            return result;
        std::lock_guard lock(environmentMutex());
        if (unsetenv(static_cast<char *>(key.data)) < 0)
            return osFailure(errno);
#endif
        return ok;
    }
    std::int32_t aevix_os_monotonic_ns(std::uint64_t *output) noexcept
    {
        begin();
        if (!output)
            return status(OsStatus::InvalidInput);
        *output = 0;
        const auto value =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count();
        if (value < 0)
            return status(OsStatus::SizeOverflow);
        *output = std::uint64_t(value);
        return ok;
    }
    std::int32_t aevix_os_unix_time_ns(std::int64_t *output) noexcept
    {
        begin();
        if (!output)
            return status(OsStatus::InvalidInput);
        *output =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        return ok;
    }
    std::int32_t aevix_os_sleep_ns(std::uint64_t duration) noexcept
    {
        begin();
        if (duration > std::uint64_t(INT64_MAX))
            return status(OsStatus::SizeOverflow);
#if defined(_WIN32)
        auto milliseconds = duration / 1000000 + bool(duration % 1000000);
        while (milliseconds)
        {
            const auto part = DWORD(milliseconds > MAXDWORD - 1 ? MAXDWORD - 1 : milliseconds);
            Sleep(part);
            milliseconds -= part;
        }
#else
        timespec request{time_t(duration / 1000000000), long(duration % 1000000000)}, remainder{};
        while (nanosleep(&request, &remainder) < 0)
        {
            if (errno != EINTR)
                return osFailure(errno);
            request = remainder;
        }
#endif
        return ok;
    }
    std::int32_t aevix_os_process_spawn(const aevix::OsText *arguments, std::uint64_t count,
                                        std::uint64_t *output) noexcept
    {
        begin();
        if (!output)
            return status(OsStatus::InvalidInput);
        *output = 0;
        if (!count || !arguments)
            return status(OsStatus::InvalidInput);
        if (count >= std::uint64_t(PTRDIFF_MAX) / sizeof(aevix::OsText))
            return status(OsStatus::SizeOverflow);
        const auto array = aevix::NativeInputBytes::checkedRaw(arguments, count * sizeof(aevix::OsText));
        if (!array)
            return bufferStatus(array.status);
        auto &registry = processes();
        auto entry = registry.acquire();
        if (!entry)
            return status(OsStatus::AllocationFailure);
#if defined(_WIN32)
        Buffer command;
        command.data = allocate(32768 * sizeof(wchar_t));
        if (!command.data)
            return status(OsStatus::AllocationFailure);
        auto *encoded = static_cast<wchar_t *>(command.data);
        std::size_t used = 0;
        const auto append = [&](wchar_t character)
        {
            if (used >= 32767)
                return false;
            encoded[used++] = character;
            return true;
        };
        for (std::uint64_t index = 0; index < count; ++index)
        {
            Buffer text;
            if (auto result = wideArgument(arguments[index].data, arguments[index].size, text, index != 0))
                return result;
            if ((index && !append(L' ')) || !append(L'"'))
                return status(OsStatus::SizeOverflow);
            auto *value = static_cast<wchar_t *>(text.data);
            std::size_t slashes = 0;
            for (std::size_t position = 0; value[position]; ++position)
            {
                if (value[position] == L'\\')
                {
                    ++slashes;
                    continue;
                }
                const auto amount = value[position] == L'"' ? slashes * 2 + 1 : slashes;
                for (std::size_t run = 0; run < amount; ++run)
                    if (!append(L'\\'))
                        return status(OsStatus::SizeOverflow);
                slashes = 0;
                if (!append(value[position]))
                    return status(OsStatus::SizeOverflow);
            }
            for (std::size_t run = 0; run < slashes * 2; ++run)
                if (!append(L'\\'))
                    return status(OsStatus::SizeOverflow);
            if (!append(L'"'))
                return status(OsStatus::SizeOverflow);
        }
        encoded[used] = 0;
        STARTUPINFOW start{};
        start.cb = sizeof(start);
        PROCESS_INFORMATION process{};
        std::lock_guard environmentLock(environmentMutex());
        if (!CreateProcessW(nullptr, encoded, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &start, &process))
            return osFailure(std::int32_t(GetLastError()));
        CloseHandle(process.hThread);
        entry->resource.handle = process.hProcess;
#else
        Buffer pointers;
        pointers.data = zeroAllocate(std::size_t(count) + 1, sizeof(char *));
        if (!pointers.data)
            return status(OsStatus::AllocationFailure);
        auto **argv = static_cast<char **>(pointers.data);
        struct ArgumentsCleanup final
        {
            char **values;
            std::size_t count;
            ~ArgumentsCleanup() noexcept
            {
                for (std::size_t i = 0; i < count; ++i)
                    std::free(values[i]);
            }
        } cleanup{argv, 0};
        for (std::uint64_t index = 0; index < count; ++index)
        {
            Buffer value;
            if (auto result = argument(arguments[index].data, arguments[index].size, value, index != 0))
                return result;
            argv[index] = static_cast<char *>(value.data);
            value.data = nullptr;
            ++cleanup.count;
        }
        std::lock_guard environmentLock(environmentMutex());
        pid_t pid = -1;
        const auto result = posix_spawnp(&pid, argv[0], nullptr, nullptr, argv, environ);
        if (result)
            return osFailure(result);
        entry->resource.pid = pid;
#endif
        registry.publish(*entry);
        *output = registry.token(*entry);
        return ok;
    }
    std::int32_t aevix_os_process_wait(std::uint64_t token, aevix::OsExit *output) noexcept
    {
        begin();
        if (!output)
            return status(OsStatus::InvalidInput);
        *output = {};
        auto &registry = processes();
        auto entry = registry.lockToken(token);
        if (!entry)
            return status(OsStatus::InvalidHandle);
#if defined(_WIN32)
        if (WaitForSingleObject(entry->resource.handle, INFINITE) != WAIT_OBJECT_0)
            return osFailure(std::int32_t(GetLastError()));
        DWORD code = 0;
        if (!GetExitCodeProcess(entry->resource.handle, &code))
            return osFailure(std::int32_t(GetLastError()));
        output->code = std::int32_t(code);
        CloseHandle(entry->resource.handle);
        entry->resource.handle = nullptr;
#else
        int value = 0;
        pid_t result;
        do
        {
            result = waitpid(entry->resource.pid, &value, 0);
        } while (result < 0 && errno == EINTR);
        if (result < 0)
            return osFailure(errno);
        output->code = WIFEXITED(value) ? WEXITSTATUS(value) : 0;
        output->signal = WIFSIGNALED(value) ? WTERMSIG(value) : 0;
        entry->resource.pid = -1;
#endif
        registry.retire(*entry);
        return ok;
    }
    std::int32_t aevix_os_process_try_wait(std::uint64_t token, aevix::OsExit *output) noexcept
    {
        begin();
        if (!output) return status(OsStatus::InvalidInput);
        *output = {};
        auto &registry = processes();
        auto entry = registry.lockToken(token);
        if (!entry) return status(OsStatus::InvalidHandle);
#if defined(_WIN32)
        const DWORD state = WaitForSingleObject(entry->resource.handle, 0);
        if (state == WAIT_TIMEOUT) return status(OsStatus::Pending);
        if (state != WAIT_OBJECT_0) return osFailure(std::int32_t(GetLastError()));
        DWORD code = 0;
        if (!GetExitCodeProcess(entry->resource.handle, &code)) return osFailure(std::int32_t(GetLastError()));
        output->code = std::int32_t(code);
        CloseHandle(entry->resource.handle);
        entry->resource.handle = nullptr;
#else
        int value = 0;
        pid_t result;
        do { result = waitpid(entry->resource.pid, &value, WNOHANG); } while (result < 0 && errno == EINTR);
        if (result == 0) return status(OsStatus::Pending);
        if (result < 0) return osFailure(errno);
        output->code = WIFEXITED(value) ? WEXITSTATUS(value) : 0;
        output->signal = WIFSIGNALED(value) ? WTERMSIG(value) : 0;
        entry->resource.pid = -1;
#endif
        registry.retire(*entry);
        return ok;
    }
    std::int32_t aevix_os_process_kill(std::uint64_t token) noexcept
    {
        begin();
        auto entry = processes().lockToken(token);
        if (!entry) return status(OsStatus::InvalidHandle);
#if defined(_WIN32)
        if (!TerminateProcess(entry->resource.handle, 1))
        {
            const auto reason = std::int32_t(GetLastError());
            if (WaitForSingleObject(entry->resource.handle, 0) != WAIT_OBJECT_0) return osFailure(reason);
        }
#else
        if (kill(entry->resource.pid, SIGKILL) != 0 && errno != ESRCH) return osFailure(errno);
#endif
        return ok;
    }
    void aevix_os_process_cleanup(std::uint64_t token) noexcept
    {
        if (!token)
            return;
        auto &registry = processes();
        auto entry = registry.lockToken(token);
        if (entry)
        {
            release(entry->resource);
            registry.retire(*entry);
        }
    }
}

namespace aevix
{
    OsStatus OsFile::open(std::string_view path, FileMode mode, OsFile &output) noexcept
    {
        std::uint64_t token = 0;
        const auto result = OsStatus(aevix_os_file_open_utf8(path.data(), path.size(), std::uint32_t(mode), &token));
        if (result == OsStatus::Success)
        {
            aevix_os_file_cleanup(output.token_);
            output.token_ = token;
        }
        return result;
    }
    OsStatus OsFile::read(NativeOutputBytes &buffer, std::uint64_t &completed) noexcept
    {
        return OsStatus(aevix_os_file_read(token_, buffer.bytes().data(), buffer.size(), &completed));
    }
    OsStatus OsFile::write(NativeInputBytes buffer, std::uint64_t &completed) noexcept
    {
        return OsStatus(aevix_os_file_write(token_, buffer.bytes().data(), buffer.size(), &completed));
    }
} // namespace aevix

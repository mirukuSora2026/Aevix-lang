// ========================
//
// Created By : miruku_sora2026
//
// File name : OsRuntime.hpp
//
// Description :
//     타입별 레지스트리 토큰과 동기식 버퍼를 사용하는 CPU 운영체제 연결 계약을 정의한다.
//     Define CPU operating-system contracts using typed registry ownership and synchronous buffers.
//
// ========================
#pragma once

#include "aevix/Runtime/NativeBoundary.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

namespace aevix
{
    /*
     * 운영체제 오류의 의미를 보존하며, 오류 코드는 별도 스레드 지역 메타데이터에 둔다.
     * Preserve OS failures with separate thread-local native-error metadata.
     * @since 0.3
     * @deprecated_at none
     * @removed_at none
     */
    enum class OsStatus : std::int32_t
    {
        Success = 0,
        OsFailure = 1,
        InvalidInput = 2,
        InvalidUtf8 = 3,
        AllocationFailure = 4,
        SizeOverflow = 5,
        InvalidHandle = 6,
        Unsupported = 7,
        Pending = 8
    };
    /* 조합 가능한 파일 열기 권한과 생성 정책이다.
     * Composable open permissions and creation policies.
     * @since 0.3 @deprecated_at none @removed_at none */
    enum class FileMode : std::uint32_t
    {
        Read = 1,
        Write = 2,
        Create = 4,
        Truncate = 8,
        Append = 16,
        Exclusive = 32
    };
    constexpr FileMode operator|(FileMode left, FileMode right) noexcept
    {
        return FileMode(std::uint32_t(left) | std::uint32_t(right));
    }

    /* 동기 호출 동안 유효한 UTF-8 인수이다.
     * A UTF-8 argument valid for a synchronous call.
     * @since 0.3 @deprecated_at none @removed_at none */
    struct OsText final
    {
        const char *data = nullptr;
        std::uint64_t size = 0;
    };
    /* 크기·수정 시각·파일 종류를 담는 소유권 없는 값이다.
     * A value containing size, modification time, and file kind.
     * @since 0.3 @deprecated_at none @removed_at none */
    struct OsMetadata final
    {
        std::uint64_t size = 0;
        std::int64_t modifiedNanoseconds = 0;
        std::uint32_t kind = 0; // 1=파일, 2=디렉터리, 3=심볼릭 링크, 4=기타.
                                // 1=file, 2=directory, 3=symbolic link, 4=other.
        std::uint32_t permissions = 0;
    };
    /* 정상 종료 코드 또는 POSIX 종료 신호를 담는다.
     * Contain an exit code or a POSIX termination signal.
     * @since 0.3 @deprecated_at none @removed_at none */
    struct OsExit final
    {
        std::int32_t code = 0;
        std::int32_t signal = 0;
    };
} // namespace aevix

/*
 * 아래 ABI는 컴파일러 연결층 전용이다. 토큰은 주소·fd·HANDLE이 아니며 소스로 공개하지 않는다.
 * This ABI is compiler-bridge-only; tokens are not addresses, fds, or HANDLEs and are not source-visible.
 * 모든 입력 뷰는 동기 호출 동안만 대여하며 보관·반환하거나 Aevix로 재진입하지 않는다.
 * All input views are borrowed only for a synchronous call; no retention, returned loans, or Aevix reentry.
 * 원시 ABI의 주소·길이 검사는 실제 할당 크기와 생존 여부를 증명하지 않는다.
 * Raw ABI address/length checks do not prove actual allocation extent or lifetime.
 * 출력 버퍼는 malloc 소유이며 aevix_os_buffer_free로 정확히 한 번 해제한다.
 * Returned buffers are malloc-owned and must be freed exactly once with aevix_os_buffer_free.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
extern "C"
{
#if defined(AEVIX_OS_RUNTIME_TESTING)
    /* 전용 테스트 빌드에서만 이후 런타임 할당의 성공 횟수를 제한한다.
     * Limit successful subsequent runtime allocations in dedicated test builds only.
     * @since 0.3 @deprecated_at none @removed_at none */
    void aevix_os_test_fail_after(std::uint64_t) noexcept;
    /* 전용 테스트에서만 살아 있는 파일 토큰 수를 조회한다.
     * Query the live file-token count in dedicated tests only.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::uint64_t aevix_os_test_live_files() noexcept;
#endif
    /* 오류 메타데이터 조회는 값을 초기화하지 않는다.
     * Query errors without clearing them.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_os_last_error() noexcept;
    /* 0=없음, 1=errno, 2=Win32.
     * 0=none, 1=errno, 2=Win32.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_os_error_domain() noexcept;
    /* 0=없음, 1=부재, 2=이미 존재, 3=권한, 4=중단, 5=대기 필요, 6=잘못된 입력, 7=기타.
     * 0=none, 1=not found, 2=already exists, 3=permission, 4=interrupted, 5=would block, 6=invalid input, 7=other.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_os_error_kind() noexcept;
    /* UTF-8과 NUL을 검사한 경로를 열고 타입별 세대 토큰을 만든다.
     * Open a checked UTF-8 path with a generational file token.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_os_file_open_utf8(const char *, std::uint64_t, std::uint32_t, std::uint64_t *) noexcept;
    /* 한 번의 동기 읽기이며 EOF는 성공과 진행량 0이다.
     * One synchronous read; EOF is success with zero progress.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_os_file_read(std::uint64_t, void *, std::uint64_t, std::uint64_t *) noexcept;
    /* 일부 쓰기도 성공일 수 있으며 완료 바이트를 반환한다.
     * A successful write may be partial and reports completed bytes.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_os_file_write(std::uint64_t, const void *, std::uint64_t, std::uint64_t *) noexcept;
    /* 원점 0/1/2는 시작/현재/끝이다.
     * Origins 0/1/2 mean start/current/end.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_os_file_seek(std::uint64_t, std::int64_t, std::uint32_t, std::uint64_t *) noexcept;
    /* OS의 파일 flush를 요청한다.
     * Request an OS file flush.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_os_file_flush(std::uint64_t) noexcept;
    /* 실패해도 토큰을 소비하며 POSIX close를 재시도하지 않는다.
     * Consume the token even on failure; never retry POSIX close.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_os_file_close(std::uint64_t) noexcept;
    /* 존재하는 토큰만 정리하며 중복 정리는 OS 자원에 접근하지 않는다.
     * Clean only live tokens; duplicates never touch an OS resource.
     * @since 0.3 @deprecated_at none @removed_at none */
    void aevix_os_file_cleanup(std::uint64_t) noexcept;
    /* 열린 파일의 메타데이터를 조회한다.
     * Query metadata for an open file.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_os_file_metadata(std::uint64_t, aevix::OsMetadata *) noexcept;
    /* 파일 길이를 바이트로 읽는다.
     * Read file length in bytes.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_os_file_size(std::uint64_t, std::uint64_t *) noexcept;
    /* 파일 위치와 소유권을 유지하며 길이를 바꾼다.
     * Change length while preserving position and ownership.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_os_file_set_len(std::uint64_t, std::uint64_t) noexcept;
    /* 경로 메타데이터이며 follow는 0 또는 1이다.
     * Query path metadata with follow set to 0 or 1.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_os_path_metadata_utf8(const char *, std::uint64_t, std::uint32_t, aevix::OsMetadata *) noexcept;
    /* 파일시스템에 접근하지 않는 플랫폼별 어휘 정규화이다.
     * Platform lexical normalization without filesystem access.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_os_path_normalize_utf8(const char *, std::uint64_t, char **, std::uint64_t *) noexcept;
    /* 디렉터리를 만들며 이미 존재하면 OS 오류를 보고한다.
     * Create one directory; report an OS error if it already exists.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_os_path_mkdir_utf8(const char *, std::uint64_t) noexcept;
    /* 파일 또는 빈 디렉터리를 삭제한다.
     * Remove a file or empty directory.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_os_path_remove_utf8(const char *, std::uint64_t) noexcept;
    /* 소유 출력 버퍼를 해제한다.
     * Free an owning output buffer.
     * @since 0.3 @deprecated_at none @removed_at none */
    void aevix_os_buffer_free(void *) noexcept;
    /* 이름이 없으면 성공과 present=0을 반환한다.
     * Missing variables return success and present=0.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_os_env_get_utf8(const char *, std::uint64_t, char **, std::uint64_t *, std::uint32_t *) noexcept;
    /* 유효한 UTF-8 환경 값을 설정한다.
     * Set a valid UTF-8 environment value.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_os_env_set_utf8(const char *, std::uint64_t, const char *, std::uint64_t) noexcept;
    /* 환경 변수를 제거한다.
     * Remove an environment variable.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_os_env_remove_utf8(const char *, std::uint64_t) noexcept;
    /* 단조 시계의 나노초를 읽는다.
     * Read nanoseconds from a monotonic clock.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_os_monotonic_ns(std::uint64_t *) noexcept;
    /* UNIX epoch 기준의 부호 있는 나노초를 읽는다.
     * Read signed nanoseconds since the UNIX epoch.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_os_unix_time_ns(std::int64_t *) noexcept;
    /* 현재 스레드를 지정한 기간 동안 대기시킨다.
     * Suspend the current thread for the requested duration.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_os_sleep_ns(std::uint64_t) noexcept;
    /* argv[0]은 실행 파일이며 셸을 자동 실행하지 않는다.
     * argv[0] selects the executable; no shell is invoked implicitly.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_os_process_spawn(const aevix::OsText *, std::uint64_t, std::uint64_t *) noexcept;
    /* 완료를 기다린 뒤 토큰을 소비한다.
     * Wait for completion, then consume the token.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_os_process_wait(std::uint64_t, aevix::OsExit *) noexcept;
    /* 실행 중이면 Pending을 반환하며, 완료를 관측한 경우에만 토큰을 소비한다.
     * Return Pending while running; consume the token only when completion is observed.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_os_process_try_wait(std::uint64_t, aevix::OsExit *) noexcept;
    /* 강제 종료를 요청하며, 토큰의 회수는 별도의 wait가 담당한다.
     * Request forced termination; a separate wait remains responsible for reaping the token.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_os_process_kill(std::uint64_t) noexcept;
    /* 자식을 종료시키지 않고 기다려 회수한다.
     * Wait and reap without terminating the child.
     * @since 0.3 @deprecated_at none @removed_at none */
    void aevix_os_process_cleanup(std::uint64_t) noexcept;
}

namespace aevix
{
    /*
     * 원시 토큰 조회 없이 파일을 한 번 정리하는 이동 전용 C++ 소유자이다.
     * A move-only C++ file owner with exactly-once cleanup and no raw-token accessor.
     * 이 래퍼는 소스의 스레드 전송 권한을 제공하지 않는다.
     * This wrapper does not grant source-language thread-transfer permission.
     * @since 0.3
     * @deprecated_at none
     * @removed_at none
     */
    class OsFile final
    {
      public:
        OsFile() noexcept = default;
        OsFile(const OsFile &) = delete;
        OsFile &operator=(const OsFile &) = delete;
        OsFile(OsFile &&other) noexcept : token_(std::exchange(other.token_, 0)) {}
        OsFile &operator=(OsFile &&other) noexcept
        {
            if (this != &other)
            {
                aevix_os_file_cleanup(token_);
                token_ = std::exchange(other.token_, 0);
            }
            return *this;
        }
        ~OsFile() noexcept { aevix_os_file_cleanup(token_); }
        explicit operator bool() const noexcept { return token_ != 0; }
        /* 실패하면 기존 output 소유권을 보존한다.
     * Preserve output ownership on failure.
         * @since 0.3 @deprecated_at none @removed_at none */
        static OsStatus open(std::string_view path, FileMode mode, OsFile &output) noexcept;
        /* 동기 버퍼 대여만 사용한다.
     * Use a synchronous buffer loan only.
         * @since 0.3 @deprecated_at none @removed_at none */
        OsStatus read(NativeOutputBytes &buffer, std::uint64_t &completed) noexcept;
        /* 동기 버퍼 대여만 사용한다.
     * Use a synchronous buffer loan only.
         * @since 0.3 @deprecated_at none @removed_at none */
        OsStatus write(NativeInputBytes buffer, std::uint64_t &completed) noexcept;
        /* 닫기는 성공 여부와 무관하게 이 소유자를 비운다.
     * Close empties this owner regardless of success.
         * @since 0.3 @deprecated_at none @removed_at none */
        OsStatus close() noexcept { return OsStatus(aevix_os_file_close(std::exchange(token_, 0))); }
        /* 위치를 바꾸며 실패하면 토큰을 보존한다.
     * Seek while preserving the token on failure.
         * @since 0.3 @deprecated_at none @removed_at none */
        OsStatus seek(std::int64_t offset, std::uint32_t origin, std::uint64_t &position) noexcept
        {
            return OsStatus(aevix_os_file_seek(token_, offset, origin, &position));
        }
        /* OS flush 결과를 반환한다.
     * Return the OS flush outcome.
         * @since 0.3 @deprecated_at none @removed_at none */
        OsStatus flush() noexcept { return OsStatus(aevix_os_file_flush(token_)); }

      private:
        std::uint64_t token_ = 0;
    };
} // namespace aevix

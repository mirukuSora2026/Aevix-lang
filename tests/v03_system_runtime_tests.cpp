// ========================
//
// Created By : miruku_sora2026
//
// File name : v03_system_runtime_tests.cpp
//
// Description :
//     줄·토큰 입력, Unicode·수치 서식, 로그와 스트림 실패를 실제 표준 스트림으로 검사한다.
//     Exercise line/token input, Unicode/numeric formatting, logs, and failures through real standard streams.
//
// ========================
#include "aevix/Runtime/SystemRuntime.hpp"
#include <array>
#include <barrier>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#define AEVIX_DUP _dup
#define AEVIX_DUP2 _dup2
#define AEVIX_CLOSE _close
#define AEVIX_FILENO _fileno
#else
#include <unistd.h>
#define AEVIX_DUP dup
#define AEVIX_DUP2 dup2
#define AEVIX_CLOSE close
#define AEVIX_FILENO fileno
#endif

namespace
{
    unsigned checks = 0, failures = 0;
    void expect(bool condition, std::string_view message)
    { ++checks; if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; } }
    struct Redirect
    {
        std::FILE* stream;
        std::FILE* temporary = std::tmpfile();
        int saved = -1;
        explicit Redirect(std::FILE* source) : stream(source)
        {
            if (!temporary) { std::cerr << "tmpfile failed\n"; std::exit(2); }
            std::fflush(stream);
            saved = AEVIX_DUP(AEVIX_FILENO(stream));
            if (saved < 0 || AEVIX_DUP2(AEVIX_FILENO(temporary), AEVIX_FILENO(stream)) < 0) std::exit(2);
            std::clearerr(stream);
#if defined(_WIN32)
            _setmode(AEVIX_FILENO(stream), _O_BINARY);
#endif
        }
        ~Redirect()
        {
            std::fflush(stream);
            AEVIX_DUP2(saved, AEVIX_FILENO(stream)); AEVIX_CLOSE(saved);
            std::clearerr(stream); std::fclose(temporary);
        }
        std::string contents()
        {
            std::fflush(stream); std::rewind(temporary);
            std::string result; char buffer[256];
            while (const auto count = std::fread(buffer, 1, sizeof(buffer), temporary)) result.append(buffer, count);
            return result;
        }
    };
    template<class T> void formatted(T value, std::uint32_t kind, std::uint32_t bits, std::string_view expected)
    {
        void* data = nullptr; std::uint64_t length = 0, capacity = 0;
        const auto status = aevix_system_format_scalar(&value, kind, bits, &data, &length, &capacity);
        expect(status == 0 && length <= capacity && std::string_view(static_cast<char*>(data), length) == expected,
            "scalar uses its exact documented representation");
        std::free(data);
    }
    void read(std::uint8_t mode, std::uint32_t expectedStatus, std::string_view expected)
    {
        void* data = reinterpret_cast<void*>(1); std::uint64_t length = 999, capacity = 999;
        const auto status = aevix_system_input(mode, &data, &length, &capacity);
        expect(status == expectedStatus, "input status distinguishes success, EOF, and malformed UTF-8");
        if (status == 0) expect(length <= capacity && std::string_view(static_cast<char*>(data), length) == expected,
            "input preserves line spaces and token contents");
        else expect(!data && !length && !capacity, "failed input leaves no owned buffer or stale extent");
        std::free(data);
    }
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    formatted<std::int8_t>(-128, 0, 8, "-128"); formatted<std::uint8_t>(255, 1, 8, "255");
    formatted<std::int16_t>(-32768, 0, 16, "-32768"); formatted<std::uint16_t>(65535, 1, 16, "65535");
    formatted<std::int32_t>(std::numeric_limits<std::int32_t>::min(), 0, 32, "-2147483648");
    formatted<std::uint32_t>(std::numeric_limits<std::uint32_t>::max(), 1, 32, "4294967295");
    formatted<std::int64_t>(std::numeric_limits<std::int64_t>::min(), 0, 64, "-9223372036854775808");
    formatted<std::uint64_t>(std::numeric_limits<std::uint64_t>::max(), 1, 64, "18446744073709551615");
    formatted(std::array<std::uint64_t, 2>{0, 0x8000000000000000ULL}, 0, 128, "-170141183460469231731687303715884105728");
    formatted(std::array<std::uint64_t, 2>{~std::uint64_t(0), ~std::uint64_t(0)}, 1, 128, "340282366920938463463374607431768211455");
    formatted<std::int64_t>(0, 0, 64, "0"); formatted<float>(1.25f, 2, 32, "1.25");
    formatted<double>(-0.0, 2, 64, "-0"); formatted<double>(1.25, 2, 64, "1.25");
    formatted<double>(std::numeric_limits<double>::infinity(), 2, 64, "inf");
    formatted<double>(-std::numeric_limits<double>::infinity(), 2, 64, "-inf");
    formatted<double>(std::numeric_limits<double>::quiet_NaN(), 2, 64, "nan");
    formatted<double>(-std::numeric_limits<double>::quiet_NaN(), 2, 64, "nan");
    formatted<std::uint8_t>(1, 3, 1, "true"); formatted<std::uint8_t>(0, 3, 1, "false");
    formatted<std::uint32_t>(0xd55c, 4, 32, "한"); formatted<std::uint32_t>(0x1f600, 4, 32, "😀");
    formatted<std::uint32_t>(0, 4, 32, std::string_view("\0", 1));
    formatted<std::uint8_t>(0, 5, 8, "0x00"); formatted<std::uint8_t>(255, 5, 8, "0xff");
    for (const auto [kind, bits] : {std::pair{0U,7U}, {1U,256U}, {2U,16U}, {3U,8U}, {4U,16U}, {5U,16U}, {99U,8U}})
    {
        std::uint64_t scalar = 1; void* data = reinterpret_cast<void*>(1); std::uint64_t length = 999, capacity = 999;
        expect(aevix_system_format_scalar(&scalar, kind, bits, &data, &length, &capacity) == 3
            && !data && !length && !capacity, "unsupported scalar descriptors fail before dereferencing their storage");
    }
    {
        Redirect output(stdout);
        expect(aevix_system_write("ab", 2, 0, 0) == 0, "plain write succeeds");
        expect(aevix_system_write("cd", 2, 0, 1) == 0, "println appends one newline");
        struct Part { const void* data; std::uint64_t length; std::uint32_t kind; std::uint32_t bits; };
        std::int32_t number = 42; const Part parts[] = {{"answer=",7,6,0},{&number,0,0,32}};
        expect(aevix_system_output(parts, 2, 1, 0, nullptr, 0) == 0, "typed output formats before writing");
        const Part invalid[] = {{"not-written",11,6,0},{&number,0,0,7}};
        expect(aevix_system_output(invalid, 2, 0, 0, nullptr, 0) == 4, "invalid later part cannot write an earlier prefix");
        expect(aevix_system_flush(0) == 0, "stdout explicitly flushes");
        expect(output.contents() == "abcd\nanswer=42\n", "exact stdout bytes match every output call");
    }
    {
        Redirect output(stderr);
        const bool good = aevix_system_log(0,"ready",5,nullptr,0) == 0
            && aevix_system_log(1,"retry",5,"W1001",5) == 0
            && aevix_system_log(2,"failed",6,"E1001",5) == 0;
        const auto actual = output.contents();
        expect(good && actual == "[INFO] ready\n[WARN W1001] retry\n[ERROR E1001] failed\n", "levels and optional codes have stable log framing");
    }
    expect(aevix_system_write(nullptr,1,0,0) == 1 && aevix_system_error_code() != 0
        && aevix_system_error_progress() == 0, "invalid bounded text reports a native error without progress");
    expect(aevix_system_flush(2) == 1, "invalid stream fails safely");
    {
        // 다른 작업의 정상 출력이 실패한 작업의 오류 상태를 지우지 않는지 번갈아 검사한다.
        // Alternate success and failure across workers to ensure their error state remains isolated.
        std::barrier rendezvous(2);
        bool failedWorker = true, successfulWorker = true;
        const auto callerError = aevix_system_error_code();
        std::thread first([&] {
            for (unsigned attempt = 0; attempt < 256; ++attempt)
            {
                failedWorker = (aevix_system_write(nullptr, 0, 2, 0) == 1) && failedWorker;
                const auto ownError = aevix_system_error_code();
                rendezvous.arrive_and_wait();
                rendezvous.arrive_and_wait();
                failedWorker = ownError != 0 && aevix_system_error_code() == ownError
                    && aevix_system_error_progress() == 0 && failedWorker;
            }
        });
        std::thread second([&] {
            for (unsigned attempt = 0; attempt < 256; ++attempt)
            {
                rendezvous.arrive_and_wait();
                successfulWorker = (aevix_system_write(nullptr, 0, 0, 0) == 0) && successfulWorker;
                rendezvous.arrive_and_wait();
                successfulWorker = aevix_system_error_code() == 0
                    && aevix_system_error_progress() == 0 && successfulWorker;
            }
        });
        first.join(); second.join();
        expect(failedWorker && successfulWorker, "256 interleaved native IO results retain per-worker errors");
        expect(aevix_system_error_code() == callerError, "worker IO cannot overwrite the caller thread error");
    }
    {
        Redirect output(stdout);
        AEVIX_CLOSE(AEVIX_FILENO(stdout));
        expect(aevix_system_write("failed",6,0,0) == 1 && aevix_system_error_code() != 0
            && aevix_system_error_progress() == 0, "a real closed output descriptor reports its OS error");
        expect(aevix_system_flush(0) == 0 || aevix_system_error_code() != 0,
            "flushing after a failed unbuffered write cannot invent unsent progress");
    }
    {
        // stdin을 읽기 전에 버퍼링을 꺼서 테스트의 fd 교체가 숨은 입력 버퍼와 섞이지 않게 한다.
        // Disable buffering before reading stdin so descriptor redirection cannot mix hidden input buffers.
        std::setvbuf(stdin, nullptr, _IONBF, 0);
        Redirect input(stdin);
        const std::string source = "he ll o\n\n12 34 true\nlast\r\n한😀\n" + std::string("\xc0\x80\n", 3) + "tail";
        std::fwrite(source.data(), 1, source.size(), input.temporary); std::fflush(input.temporary); std::rewind(input.temporary);
        read(0,0,"he ll o"); read(0,0,""); read(1,0,"12"); read(1,0,"34"); read(1,0,"true");
        read(0,0,""); read(0,0,"last"); read(0,0,"한😀"); read(0,5,""); read(0,0,"tail"); read(0,1,"");
    }
    void* data = reinterpret_cast<void*>(1); std::uint64_t length = 4, capacity = 4;
    expect(aevix_system_input(99,&data,&length,&capacity) == 4 && !data && !length && !capacity
        && aevix_system_error_code() != 0 && aevix_system_error_progress() == 0, "bad native input calls reset status and output ownership");
    {
        Redirect input(stdin);
        AEVIX_CLOSE(AEVIX_FILENO(stdin));
        expect(aevix_system_input(0,&data,&length,&capacity) == 4 && !data && !length && !capacity
            && aevix_system_error_code() != 0, "a real closed input descriptor is IO failure, not EOF");
    }
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}

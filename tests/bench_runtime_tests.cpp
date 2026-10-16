// ========================
//
// Created By : miruku_sora2026
//
// File name : bench_runtime_tests.cpp
//
// Description :
//     실제 자식 프로세스로 중첩·동시 벤치마크와 자동 RSS 보고의 무결성을 검사한다.
//     Check nested and concurrent benchmarks and automatic RSS reports using real child processes.
//
// ========================
#include "aevix/Runtime/BenchRuntime.hpp"
#include "ProcessTestSupport.hpp"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
    unsigned checks = 0, failures = 0;
    void expect(bool value, const char* label)
    {
        ++checks;
        if (!value) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
    }
    std::uint64_t cpuWork(unsigned iterations)
    {
        volatile std::uint64_t value = 17;
        for (unsigned index = 0; index < iterations; ++index) value = value * 6364136223846793005ULL + index + 1;
        return value;
    }
    int child(std::string_view mode)
    {
        if (mode == "idle") { aevix_bench_end(0); aevix_bench_end(12345); aevix_bench_report(); return 0; }
        if (mode == "unfinished")
        {
            const auto token = aevix_bench_begin("bench.unfinished");
            if (!token) return 1;
            cpuWork(10000); return 0;
        }
        const auto outer = aevix_bench_begin("bench.outer");
        const auto inner = aevix_bench_begin("bench.inner");
        const auto recursive = aevix_bench_begin("bench.inner");
        if (!outer || !inner || !recursive || inner == recursive) return 1;
        std::vector<unsigned char> touched(8 * 1024 * 1024);
        for (std::size_t index = 0; index < touched.size(); index += 4096) touched[index] = static_cast<unsigned char>(index / 4096);
        cpuWork(100000);
        std::this_thread::sleep_for(std::chrono::milliseconds(35));
        aevix_bench_end(recursive); aevix_bench_end(recursive); aevix_bench_end(inner); aevix_bench_end(outer);
        for (unsigned index = 0; index < 4; ++index)
        {
            const auto token = aevix_bench_begin("bench.repeat");
            if (!token) return 2;
            cpuWork(50000); aevix_bench_end(token);
        }
        std::vector<std::thread> workers;
        for (unsigned index = 0; index < 4; ++index)
            workers.emplace_back([]
            {
                for (unsigned iteration = 0; iteration < 20; ++iteration)
                {
                    const auto token = aevix_bench_begin("bench.concurrent");
                    cpuWork(1000); aevix_bench_end(token);
                }
            });
        for (auto& worker : workers) worker.join();
        const auto escaped = aevix_bench_begin("bench.\"quoted\"\n"); cpuWork(1000); aevix_bench_end(escaped);
        if (mode == "manual") aevix_bench_report();
        return touched[4096] == 1 ? 0 : 3;
    }
    struct Capture final { bool exited = false; std::string report; };
    Capture capture(const char* mode)
    {
        std::string error;
        auto temporary = aevix::TemporaryDirectory::create("aevix-bench-test", error);
        const auto executable = aevix::currentExecutable(error);
        if (!temporary || executable.empty()) { std::cerr << error << '\n'; return {}; }
        aevix::ProcessOptions options;
        options.timeout = std::chrono::seconds(10);
        options.stderrFile = temporary->path() / "bench.stderr";
        options.stdoutFile = temporary->path() / "bench.stdout";
        const auto process = aevix::runProcess({aevix::utf8Path(executable), "--child", mode}, options);
        std::ifstream input(options.stderrFile);
        return {aevix::test::exitedWith(process, 0), std::string(std::istreambuf_iterator<char>(input), {})};
    }
    std::string row(const std::string& report, const char* name)
    {
        const auto prefix = std::string("function=\"") + name + "\" ";
        const auto start = report.find(prefix);
        if (start == std::string::npos) return {};
        const auto end = report.find('\n', start);
        return report.substr(start, end - start);
    }
    long double number(const std::string& line, const char* field)
    {
        const auto prefix = std::string(field) + "=";
        const auto start = line.find(prefix);
        if (start == std::string::npos) return -1;
        try { return std::stold(line.substr(start + prefix.size())); }
        catch (...) { return -1; }
    }
    std::size_t count(const std::string& text, std::string_view needle)
    {
        std::size_t result = 0, position = 0;
        while ((position = text.find(needle, position)) != std::string::npos) { ++result; position += needle.size(); }
        return result;
    }
    void verify(const Capture& output)
    {
        expect(output.exited, "measured child exits normally after joining its sampler");
        expect(output.report.find("sampled process RSS (not per-function allocations)") != std::string::npos, "report labels process RSS and its attribution limits");
        expect(output.report.find("arithmetic sample mean") != std::string::npos && output.report.find("interval_ms=10") != std::string::npos, "report states sample averaging and the periodic interval");
        const auto outer = row(output.report, "bench.outer"), inner = row(output.report, "bench.inner"), repeated = row(output.report, "bench.repeat"), concurrent = row(output.report, "bench.concurrent");
        expect(number(outer, "calls") == 1 && number(inner, "calls") == 2, "nested and recursive calls use independent tokens");
        expect(number(repeated, "calls") == 4 && number(concurrent, "calls") == 80, "repeated and concurrent calls aggregate without losing updates");
        for (const auto& line : {outer, inner, repeated, concurrent})
        {
            const auto calls = number(line, "calls"), total = number(line, "time_total_ms"), mean = number(line, "time_mean_ms"), maximum = number(line, "time_max_ms");
            expect(calls > 0 && total >= 0 && maximum >= 0 && maximum <= total + 0.000002L && std::fabs(mean * calls - total) <= 0.000002L * calls, "wall totals, arithmetic means, and maxima are internally consistent");
            const auto samples = number(line, "samples");
            if (samples > 0)
            {
                const auto peak = number(line, "memory_peak_mib"), average = number(line, "memory_mean_mib");
                expect(peak > 0 && average > 0 && peak + 0.000002L >= average, "RSS values use real positive samples and peak bounds the sample mean");
            }
            else expect(line.find("memory_peak_mib=unavailable memory_mean_mib=unavailable") != std::string::npos, "unavailable RSS is explicitly reported without fabricated zeros");
            expect(number(line, "active_calls") == 0, "completed calls leave no active invocation records");
        }
        expect(output.report.find("function=\"bench.\\\"quoted\\\"\\x0a\"") != std::string::npos, "function names cannot inject report lines");
        expect(count(output.report, "Aevix @bench: wall time") == 1, "manual reporting and normal exit do not duplicate an unchanged snapshot");
    }
}
int main(int argc, char** argv)
{
    if (argc == 3 && std::string_view(argv[1]) == "--child") return child(argv[2]);
    const auto idle = capture("idle");
    expect(idle.exited && idle.report.empty(), "unannotated execution and invalid end tokens have no profiler output");
    verify(capture("auto")); verify(capture("manual"));
    const auto unfinished = capture("unfinished");
    expect(unfinished.exited && number(row(unfinished.report, "bench.unfinished"), "calls") == 1
        && number(row(unfinished.report, "bench.unfinished"), "active_calls") == 0, "normal exit safely closes a still-active measurement");
    std::cout << checks << " bench runtime checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}

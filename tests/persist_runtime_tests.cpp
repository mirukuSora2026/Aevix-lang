// ========================
//
// Created By : miruku_sora2026
//
// File name : persist_runtime_tests.cpp
//
// Description :
//     실제 스레드와 자식 프로세스로 persist 일회 초기화와 순환·종료 규칙을 검증한다.
//     Verify persist once initialization, cycles, and shutdown rules with real threads and child processes.
//
// ========================
#include "aevix/Runtime/PersistRuntime.hpp"
#include "ProcessTestSupport.hpp"
#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>
#include <vector>
#include <string_view>

namespace
{
    unsigned checks = 0, failures = 0;
    void expect(bool value, const char* label)
    {
        ++checks;
        if (!value) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
    }
    void* domain = nullptr;
    void* first = nullptr;
    void* second = nullptr;
    std::atomic<unsigned> initializations{0};
    std::atomic<unsigned> ready{0};
    std::atomic<bool> release{false};
    std::vector<int> order;
    void initialize() { ++initializations; }
    void noOp() {}
    void cleanFirst() { order.push_back(1); }
    void cleanSecond() { order.push_back(2); }
    void recursiveFirst();
    void recursiveSecond() { aevix_persist_ensure(&domain, &first, "first", recursiveFirst, nullptr); }
    void recursiveFirst() { aevix_persist_ensure(&domain, &second, "second", recursiveSecond, nullptr); }
    void badCleanup() { aevix_persist_ensure(&domain, &second, "second", noOp, nullptr); }
    void threadFirst()
    {
        ready.fetch_add(1);
        while (ready.load() != 2) std::this_thread::yield();
        aevix_persist_ensure(&domain, &second, "second", noOp, nullptr);
    }
    void threadSecond()
    {
        ready.fetch_add(1);
        while (ready.load() != 2) std::this_thread::yield();
        aevix_persist_ensure(&domain, &first, "first", noOp, nullptr);
    }
    void blockingInitialize()
    {
        ready.store(1);
        while (!release.load()) std::this_thread::yield();
        ++initializations;
    }
    int failureCase(std::string_view name)
    {
        if (name == "same-thread")
            aevix_persist_ensure(&domain, &first, "first", recursiveFirst, nullptr);
        else if (name == "cross-thread")
        {
            std::thread left([] { aevix_persist_ensure(&domain, &first, "first", threadFirst, nullptr); });
            std::thread right([] { aevix_persist_ensure(&domain, &second, "second", threadSecond, nullptr); });
            left.join(); right.join();
        }
        else if (name == "cleanup-access")
        {
            aevix_persist_ensure(&domain, &first, "first", noOp, badCleanup);
            aevix_persist_shutdown(&domain);
        }
        else if (name == "closed-access")
        {
            aevix_persist_ensure(&domain, &first, "first", noOp, nullptr);
            aevix_persist_shutdown(&domain);
            aevix_persist_ensure(&domain, &first, "first", noOp, nullptr);
        }
        return 99;
    }
    bool aborts(const char* name)
    {
        std::string error;
        auto temporary = aevix::TemporaryDirectory::create("aevix-persist-test", error);
        const auto executable = aevix::currentExecutable(error);
        if (!temporary || executable.empty()) { std::cerr << error << '\n'; return false; }
        aevix::ProcessOptions options;
        options.timeout = std::chrono::seconds(5);
        options.stderrFile = temporary->path() / "failure.stderr";
        // 새 프로세스는 부모의 종료된 도메인이나 초기화 레지스트리를 물려받지 않는다.
        // A fresh process never inherits the parent's closed domains or initialization registry.
        const auto process = aevix::runProcess({aevix::utf8Path(executable), "--child", name}, options);
        return aevix::test::persistAbort(process, options.stderrFile);
    }
}
int main(int argc, char** argv)
{
    if (argc == 3 && std::string_view(argv[1]) == "--child") return failureCase(argv[2]);
    {
        std::vector<std::thread> workers;
        for (unsigned i = 0; i < 12; ++i) workers.emplace_back([] { aevix_persist_ensure(&domain, &first, "once", initialize, cleanFirst); });
        for (auto& worker : workers) worker.join();
        expect(initializations == 1, "one initialization across concurrent callers");
        aevix_persist_ensure(&domain, &second, "second", initialize, cleanSecond);
        aevix_persist_shutdown(&domain);
        expect(initializations == 2 && order == std::vector<int>({2, 1}), "reverse successful completion cleanup");
        aevix_persist_shutdown(&domain);
        expect(order == std::vector<int>({2, 1}), "idempotent shutdown");
    }
    expect(aborts("same-thread"), "same-thread initialization cycle aborts");
    expect(aborts("cross-thread"), "cross-thread initialization cycle aborts");
    expect(aborts("cleanup-access"), "cleanup cannot access another persist");
    expect(aborts("closed-access"), "access after cleanup aborts");
    {
        domain = nullptr; first = nullptr; second = nullptr; ready = 0; release = false; initializations = 0;
        std::atomic<bool> shutdownDone{false};
        std::thread worker([] { aevix_persist_ensure(&domain, &first, "blocking", blockingInitialize, nullptr); });
        while (!ready.load()) std::this_thread::yield();
        std::thread closer([&] { aevix_persist_shutdown(&domain); shutdownDone = true; });
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        expect(!shutdownDone.load(), "shutdown waits for active initializer");
        release = true; worker.join(); closer.join();
        expect(shutdownDone && initializations == 1, "shutdown completes after initializer");
    }
    std::cout << checks << " persist runtime checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}

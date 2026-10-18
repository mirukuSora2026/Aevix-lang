// ========================
//
// Created By : miruku_sora2026
//
// File name : jit_phases.cpp
//
// Description :
//     고정 O2 ORC 세션의 공개 API 경계를 공통 시계로 분리하고 모든 호출 결과를 검사한다.
//     Separate public API boundaries of a fixed-O2 ORC session with the shared clock and verify every invocation.
//
// ========================
#include "aevix/JIT/JITEngine.hpp"
#include "shared_timer.h"
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef AEVIXBENCH_EXPECTED_STATE
#error "The preparation script must provide the independently checked LCG result."
#endif

// JIT는 이 네이티브 함수의 본문을 보지 못하며 매 호출의 입력을 외부 심벌에서 받는다.
// JIT code cannot see this native function body and obtains input from an opaque external symbol per call.
extern "C" AEVIXBENCH_CLOCK_API std::uint64_t aevixbench_seed() { return 3; }

namespace {
std::uint64_t now(bool functional) { return functional ? 0 : aevixbench_clock_ns(); }
std::uint64_t elapsed(std::uint64_t begin, std::uint64_t end) {
    if (end < begin) throw std::runtime_error("Shared monotonic clock moved backwards.");
    return end - begin;
}
void verify(const aevix::JITInvocationResult& result) {
    if (!result.succeeded) throw std::runtime_error("JIT invocation failed: " + result.message);
    if (result.value != 0) throw std::runtime_error("LCG oracle mismatch; main returned " + std::to_string(result.value));
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 2 && !(argc == 3 && std::string(argv[2]) == "--functional-only")) {
            std::cerr << "usage: jit_phases RAW_IR [--functional-only]\n";
            return 2;
        }
        const bool functional = argc == 3;
        std::ifstream input(argv[1], std::ios::binary);
        if (!input) throw std::runtime_error("Cannot read raw phase IR.");
        const std::string ir((std::istreambuf_iterator<char>(input)), {});
        if (input.bad() || ir.empty()) throw std::runtime_error("Raw phase IR is empty or unreadable.");
        if (!aevix::JITEngine::available()) throw std::runtime_error("The linked library does not enable LLVM ORC.");
        aevix::JITOptions options;
        options.optimization = 2;
        std::string error;
        auto begin = now(functional);
        auto engine = aevix::JITEngine::create(options, error);
        const auto createNs = elapsed(begin, now(functional));
        if (!engine) throw std::runtime_error("Engine creation failed: " + error);
        // 심벌 등록은 측정된 addModule 구간 밖에 있으며 함수 포인터는 세션보다 오래 산다.
        // Symbol definition is outside the measured addModule interval; its function outlives the session.
        if (!engine->defineSymbol("aevixbench_seed", reinterpret_cast<std::uintptr_t>(&aevixbench_seed), error))
            throw std::runtime_error("Opaque seed symbol definition failed: " + error);
        begin = now(functional);
        const auto module = engine->addModuleTracked(ir, error);
        const auto addNs = elapsed(begin, now(functional));
        if (!module) throw std::runtime_error("Module registration failed: " + error);
        begin = now(functional);
        const auto first = engine->invokeI32("main");
        const auto firstNs = elapsed(begin, now(functional));
        verify(first);
        std::vector<std::uint64_t> warm;
        warm.reserve(40);
        for (unsigned index = 0; index < 40; ++index) {
            begin = now(functional);
            const auto result = engine->invokeI32("main");
            const auto duration = elapsed(begin, now(functional));
            verify(result);
            warm.push_back(duration);
        }
        begin = now(functional);
        engine.reset();
        const auto teardownNs = elapsed(begin, now(functional));
        // 상태 검사와 출력은 모든 개별 시간 구간 밖에서 수행한다.
        // Result validation and output occur outside every individual timed span.
        std::cout << "{\"schema_version\":1,\"fixed_optimization\":2,\"tiering\":false,"
                     "\"seed\":3,\"rounds\":65536,\"expected_lcg_state\":" << AEVIXBENCH_EXPECTED_STATE
                  << ",\"verified_calls\":41,\"all_returned_zero\":true,\"functional_only\":"
                  << (functional ? "true" : "false")
                  << ",\"clock\":\"CLOCK_MONOTONIC_RAW from shared aevixbench_clock_ns\","
                     "\"untimed_setup\":[\"read raw IR\",\"define opaque seed symbol\"],";
        if (functional) {
            std::cout << "\"phases_ns\":null,\"warm_invocations_ns\":null";
        } else {
            std::cout << "\"phases_ns\":{\"engine_create\":" << createNs
                      << ",\"add_module_combined\":" << addNs
                      << ",\"first_invoke_combined\":" << firstNs
                      << ",\"teardown\":" << teardownNs << "},\"warm_invocations_ns\":[";
            for (std::size_t index = 0; index < warm.size(); ++index) {
                if (index) std::cout << ',';
                std::cout << warm[index];
            }
            std::cout << ']';
        }
        std::cout << "}\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "JIT phase probe error: " << failure.what() << '\n';
        return 1;
    }
}

// ========================
//
// Created By : miruku_sora2026
//
// File name : RangeMaterializationProbe.cpp
//
// Description :
//     생성된 코드의 명시적 관찰 지점에서 할당 크기·실패 정리·배열 초기화를 검사한다.
//     Check allocation sizes, failure cleanup and array initialization at explicit generated-code observer seams.
//
// ========================
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

extern "C" std::int32_t range_test_entry();
namespace
{
    std::size_t attempts = 0, allocated = 0, released = 0, failAt = 0;
    std::uint64_t stores = 0;
    bool failAll = false, bad = false;
    std::array<void*, 32> live{};
#if RANGE_PROBE_KIND == 3
    constexpr std::array<std::size_t, 6> expectedBytes{
        9223372036854775800ull, 9223372036854775800ull,
        9223372036854775792ull, 9223372036854775792ull,
        9223372036854775800ull, 9223372036854775800ull};
#else
    constexpr std::array<std::size_t, 12> expectedBytes{3, 3, 6, 6, 12, 12, 24, 24, 48, 48, 24, 24};
#endif
}
extern "C" void* range_test_malloc(std::size_t bytes)
{
    ++attempts;
#if RANGE_PROBE_KIND == 2 || RANGE_PROBE_KIND == 4
    bad = true;
    return nullptr;
#else
    if (attempts > expectedBytes.size() || bytes != expectedBytes[attempts - 1])
    { bad = true; return nullptr; }
#if RANGE_PROBE_KIND == 3
    return nullptr;
#else
    if (failAll || attempts == failAt) return nullptr;
    void* pointer = std::malloc(bytes);
    if (!pointer) { bad = true; return nullptr; }
    ++allocated;
    for (auto& slot : live) if (!slot) { slot = pointer; return pointer; }
    bad = true;
    return pointer;
#endif
#endif
}
extern "C" void range_test_free(void* pointer)
{
    if (!pointer) return;
    for (auto& slot : live)
        if (slot == pointer) { slot = nullptr; ++released; std::free(pointer); return; }
    bad = true;
}
extern "C" void range_test_array_store() { ++stores; }
int main()
{
#if RANGE_PROBE_KIND == 1
    for (std::size_t mode = 0; mode <= 13; ++mode)
    {
        attempts = allocated = released = 0;
        failAt = mode <= 12 ? mode : 0;
        failAll = mode == 13; bad = false; live.fill(nullptr);
        const auto result = range_test_entry();
        const auto expected = mode == 13 ? 0u : mode == 0 ? 12u : 11u;
        bool remaining = false; for (const auto pointer : live) remaining = remaining || pointer;
        if (result || bad || attempts != 12 || allocated != expected || released != expected || remaining)
        {
            std::printf("mode=%zu result=%d attempts=%zu allocated=%zu released=%zu bad=%d remaining=%d\n",
                        mode, result, attempts, allocated, released, bad, remaining);
            return 21;
        }
    }
    std::puts("14 allocation modes: exact byte sizes, each OOM position, fail-all, no live output buffers");
#elif RANGE_PROBE_KIND == 2
    if (range_test_entry() || attempts || allocated || released || bad) return 22;
    std::puts("No allocation: empty/count-overflow/byte-overflow/fixed success/fixed mismatch, all 12 integer types");
#elif RANGE_PROBE_KIND == 3
    if (range_test_entry() || attempts != 6 || allocated || released || bad) return 23;
    std::puts("Exact INT64_MAX/element-size boundary and byte count accepted before controlled OOM");
#elif RANGE_PROBE_KIND == 4
    if (range_test_entry() || stores != EXPECTED_STORES || attempts || bad)
    { std::printf("explicit ArrayRepeat stores=%llu\n", static_cast<unsigned long long>(stores)); return 24; }
    std::printf("explicit ArrayRepeat stores=%llu\n", static_cast<unsigned long long>(stores));
#else
#error RANGE_PROBE_KIND must be 1, 2, 3 or 4
#endif
    return 0;
}

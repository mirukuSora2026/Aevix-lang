// ========================
//
// Created By : miruku_sora2026
//
// File name : native_contract_tests.cpp
//
// Description :
//     닫힌 네이티브 계약의 소유권·대여·정리 효과와 알 수 없는 연산 거부를 검사한다.
//     Check native ownership, loan, and cleanup contracts and rejection of unknown operations.
//
// ========================

#include "aevix/Native/Contract.hpp"

#include <array>
#include <iostream>

namespace
{
    unsigned checks = 0, failures = 0;
    void expect(bool condition, const char* message)
    {
        ++checks;
        if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
    }
}

int main()
{
    using namespace aevix;
    for (const auto id : {NativeOperationId::Release, NativeOperationId::ReadBytes, NativeOperationId::WriteBytes})
    {
        const auto contract = nativeContract(id);
        expect(contract.has_value(), "known operation has a contract");
        if (!contract) continue;
        expect(validateNativeContract(*contract).succeeded, "canonical contract is accepted");
        expect(nativeBridgeOperation(nativeBridgeSymbol(id)) == id, "internal symbol round trips by exact identity");
        for (const auto member : std::array{
            &NativeContract::synchronous, &NativeContract::retainsLoan, &NativeContract::returnsLoan,
            &NativeContract::invokesAevix, &NativeContract::accessesAevixStorage, &NativeContract::usableDuringCleanup})
        {
            auto changed = *contract;
            changed.*member = !(changed.*member);
            const auto result = validateNativeContract(changed);
            expect(!result.succeeded && !result.message.empty(), "altered effects are rejected with a reason");
        }
        auto changed = *contract;
        changed.handle = static_cast<NativeHandleMode>(255);
        expect(!validateNativeContract(changed).succeeded, "unknown handle ownership is rejected");
        changed = *contract;
        changed.buffer = static_cast<NativeBufferMode>(255);
        expect(!validateNativeContract(changed).succeeded, "unknown buffer access is rejected");
        changed = *contract;
        changed.result = static_cast<NativeResultShape>(255);
        expect(!validateNativeContract(changed).succeeded, "unknown result shape is rejected");
    }
    const auto read = *nativeContract(NativeOperationId::ReadBytes);
    const auto write = *nativeContract(NativeOperationId::WriteBytes);
    const auto release = *nativeContract(NativeOperationId::Release);
    expect(read.handle == NativeHandleMode::ExclusiveLoan && write.handle == NativeHandleMode::ExclusiveLoan,
        "read and write both borrow mutable native state exclusively");
    expect(read.buffer == NativeBufferMode::ExclusiveLoan && write.buffer == NativeBufferMode::ReadOnlyLoan,
        "native input and output have distinct buffer directions");
    expect(release.handle == NativeHandleMode::Consume && release.buffer == NativeBufferMode::None
        && release.result == NativeResultShape::Cleanup && release.usableDuringCleanup,
        "cleanup consumes only its owner and cannot borrow a buffer");
    auto falseCleanup = read;
    falseCleanup.usableDuringCleanup = true;
    expect(!validateNativeContract(falseCleanup).succeeded, "IO is not granted cleanup privilege");
    for (unsigned value = 0; value < 256; ++value)
    {
        if (value >= 1 && value <= 3) continue;
        const auto id = static_cast<NativeOperationId>(value);
        expect(!nativeContract(id) && nativeBridgeSymbol(id).empty(), "unlisted IDs have no capability");
        NativeContract unknown; unknown.operation = id;
        expect(!validateNativeContract(unknown).succeeded, "unlisted IDs cannot carry valid effects");
    }
    for (const auto symbol : {"", "read_bytes", "native.read_bytes", "aevix_native_read_bytes_", "Aevix_native_read_bytes", "aevix_native_unknown"})
        expect(!nativeBridgeOperation(symbol), "lookalike names do not resolve to operations");
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}

// ========================
//
// Created By : miruku_sora2026
//
// File name : native_boundary_tests.cpp
//
// Description :
//     타입별 단일 자원 정리, 버퍼 경계, 부분 진행과 실패 후 소유권 보존을 검사한다.
//     Check typed exactly-once cleanup, buffer boundaries, partial progress, and ownership after failure.
//
// ========================

#include "aevix/Runtime/NativeBoundary.hpp"

#include <array>
#include <iostream>
#include <limits>
#include <type_traits>

namespace
{
    using namespace aevix;
    struct FileTag {};
    struct SocketTag {};

    template <typename T>
    concept ExposesRawResource = requires(T& value) { value.get(); } ||
                                 requires(T& value) { value.release_raw(); };

    static_assert(!std::is_copy_constructible_v<NativeOwner<FileTag>>);
    static_assert(!std::is_copy_assignable_v<NativeOwner<FileTag>>);
    static_assert(std::is_nothrow_move_constructible_v<NativeOwner<FileTag>>);
    static_assert(std::is_nothrow_move_assignable_v<NativeOwner<FileTag>>);
    static_assert(std::is_nothrow_destructible_v<NativeOwner<FileTag>>);
    static_assert(!std::is_constructible_v<NativeOwner<SocketTag>, NativeOwner<FileTag>>);
    static_assert(!std::is_convertible_v<NativeOwner<FileTag>, std::uintptr_t>);
    static_assert(!ExposesRawResource<NativeOwner<FileTag>>);
    static_assert(std::is_copy_constructible_v<NativeInputBytes>);
    static_assert(!std::is_copy_constructible_v<NativeOutputBytes>);
    static_assert(!std::is_copy_assignable_v<NativeOutputBytes>);
    static_assert(std::is_nothrow_move_constructible_v<NativeOutputBytes>);

    int failures = 0;
    void expect(bool condition, const char* message)
    {
        if (!condition)
        {
            std::cerr << message << '\n';
            ++failures;
        }
    }

    template <typename T>
    void moveAssign(T& destination, T& source)
    {
        destination = std::move(source);
    }

    struct State final
    {
        int destroyed = 0;
        int reads = 0;
        int writes = 0;
        std::array<std::byte, 8> written{};
        std::span<std::byte> replacement;
        NativeAdapterOutcome next;
    };

    class FakeFile final : public NativeResource<FileTag>
    {
    public:
        explicit FakeFile(State& state) noexcept : state_(state) {}
        ~FakeFile() noexcept override { ++state_.destroyed; }

        NativeAdapterOutcome readBytes(NativeOutputBytes& destination) noexcept override
        {
            ++state_.reads;
            if (!state_.replacement.empty()) destination = NativeOutputBytes(state_.replacement);
            const auto count = state_.next.completedBytes < destination.size()
                                   ? state_.next.completedBytes : destination.size();
            for (std::size_t i = 0; i < count; ++i) destination.bytes()[i] = std::byte(i + 1);
            return state_.next;
        }

        NativeAdapterOutcome writeBytes(NativeInputBytes source) noexcept override
        {
            ++state_.writes;
            const auto extent = source.size() < state_.written.size() ? source.size() : state_.written.size();
            const auto count = state_.next.completedBytes < extent ? state_.next.completedBytes : extent;
            for (std::size_t i = 0; i < count; ++i) state_.written[i] = source.bytes()[i];
            return state_.next;
        }

    private:
        State& state_;
    };

    NativeOwner<FileTag> makeOwner(State& state)
    {
        return NativeOwner<FileTag>(std::make_unique<FakeFile>(state));
    }

    void ownership()
    {
        State first;
        State replaced;
        {
            auto original = makeOwner(first);
            auto moved = std::move(original);
            expect(!original && moved, "A move must empty the old owner.");
            auto target = makeOwner(replaced);
            target = std::move(moved);
            expect(replaced.destroyed == 1 && !moved && target,
                   "Move assignment must clean the replaced resource exactly once.");
            moveAssign(target, target);
            expect(bool(target) && first.destroyed == 0, "Self move must preserve the resource.");
            target.reset();
            target.reset();
            expect(first.destroyed == 1 && !target, "Repeated reset must clean exactly once.");
        }
        expect(first.destroyed == 1 && replaced.destroyed == 1,
               "Moved-from and reset destructors must not repeat cleanup.");
        {
            auto scoped = makeOwner(first);
        }
        expect(first.destroyed == 2, "Scope exit must clean a live resource.");
    }

    void buffers()
    {
        std::array<std::byte, 4> bytes{};
        auto input = NativeInputBytes::checkedRaw(bytes.data(), bytes.size());
        auto output = NativeOutputBytes::checkedRaw(bytes.data(), bytes.size());
        expect(bool(input) && bool(output) && input.buffer.size() == 4 && output.buffer.size() == 4,
               "Valid raw buffer ranges must preserve the requested extent.");
        auto inputCopy = input.buffer;
        expect(inputCopy.bytes().data() == bytes.data(), "Read-only views must be copyable.");
        auto moved = std::move(output.buffer);
        expect(output.buffer.empty() && moved.size() == 4, "Moving a mutable view must empty its source.");
        NativeOutputBytes assigned;
        assigned = std::move(moved);
        expect(moved.empty() && assigned.size() == 4, "Mutable view move assignment must transfer the extent.");
        moveAssign(assigned, assigned);
        expect(assigned.size() == 4, "Mutable view self move must preserve its extent.");

        expect(bool(NativeInputBytes::checkedRaw(nullptr, 0)) &&
               bool(NativeOutputBytes::checkedRaw(nullptr, 0)), "Null empty buffers must be accepted.");
        expect(NativeInputBytes::checkedRaw(nullptr, 1).status == NativeBufferStatus::NullWithNonzeroLength &&
               NativeOutputBytes::checkedRaw(nullptr, 1).status == NativeBufferStatus::NullWithNonzeroLength,
               "Null nonempty buffers must be rejected.");
        expect(NativeInputBytes::checkedRaw(bytes.data(), std::numeric_limits<std::uint64_t>::max()).status ==
                   NativeBufferStatus::LengthOutOfRange,
               "Lengths beyond native pointer arithmetic must be rejected.");
        const auto nearEnd = std::numeric_limits<std::uintptr_t>::max() - 1;
        const auto fake = reinterpret_cast<void*>(nearEnd);
        expect(NativeInputBytes::checkedRaw(fake, 2).status == NativeBufferStatus::AddressOverflow &&
               NativeOutputBytes::checkedRaw(fake, 2).status == NativeBufferStatus::AddressOverflow,
               "An overflowing one-past address must be rejected without dereferencing it.");
        auto empty = NativeOutputBytes::checkedRaw(fake, 0);
        expect(bool(empty) && empty.buffer.empty() && empty.buffer.bytes().data() == nullptr,
               "An empty raw buffer must have a canonical empty view.");
    }

    void progress()
    {
        State state;
        std::array<std::byte, 6> storage{std::byte{9}, std::byte{9}, std::byte{9},
                                         std::byte{9}, std::byte{9}, std::byte{9}};
        NativeOutputBytes destination(std::span<std::byte>(storage).subspan(1, 4));
        auto owner = makeOwner(state);
        state.next = {NativeAdapterStatus::Success, 2, {}};
        auto result = owner.readBytes(destination);
        expect(bool(result) && result.completedBytes == 2 && storage[0] == std::byte{9} &&
               storage[1] == std::byte{1} && storage[2] == std::byte{2} &&
               storage[3] == std::byte{9} && storage[5] == std::byte{9},
               "A partial read must report progress and respect the supplied span.");

        state.next = {NativeAdapterStatus::PlatformError, 1, {7, -13}};
        const auto failed = owner.writeBytes(NativeInputBytes(std::span<const std::byte>(storage).subspan(1, 4)));
        expect(!failed && failed.status == NativeIoStatus::PlatformError && failed.completedBytes == 1 &&
               failed.platformError == NativePlatformError{7, -13} && owner && state.destroyed == 0 &&
               state.written[0] == storage[1] && state.written[1] == std::byte{},
               "An OS error must retain partial progress, metadata, and resource ownership.");

        state.next = {NativeAdapterStatus::Success, 2, {}};
        result = owner.writeBytes(NativeInputBytes(std::span<const std::byte>(storage).subspan(1, 4)));
        expect(bool(result) && result.completedBytes == 2 && state.written[0] == storage[1] &&
               state.written[1] == storage[2] && state.written[2] == std::byte{},
               "A partial write must retain the original input bounds and report its progress.");

        state.next = {NativeAdapterStatus::Success, 5, {}};
        result = owner.readBytes(destination);
        expect(result.status == NativeIoStatus::AdapterContractViolation && result.completedBytes == 0 && owner,
               "An adapter cannot report progress beyond the bounded destination.");
        state.next = {NativeAdapterStatus::PlatformError, 5, {7, 9}};
        result = owner.writeBytes(NativeInputBytes(std::span<const std::byte>(storage).first(4)));
        expect(result.status == NativeIoStatus::AdapterContractViolation && result.completedBytes == 0 && owner,
               "Failed writes must also obey the bounded byte count.");
        state.next = {static_cast<NativeAdapterStatus>(99), 0, {}};
        expect(owner.readBytes(destination).status == NativeIoStatus::AdapterContractViolation,
               "Invalid adapter status tags must be rejected.");
        state.next = {NativeAdapterStatus::Success, 0, {1, 2}};
        expect(owner.readBytes(destination).status == NativeIoStatus::AdapterContractViolation,
               "Successful adapters cannot return contradictory platform-error metadata.");

        state.replacement = state.written;
        state.next = {NativeAdapterStatus::Success, 5, {}};
        expect(owner.readBytes(destination).status == NativeIoStatus::AdapterContractViolation,
               "An adapter changing a wrapper cannot enlarge the original operation bounds.");
        state.replacement = {};

        state.next = {NativeAdapterStatus::Success, 0, {}};
        NativeOutputBytes empty;
        expect(bool(owner.readBytes(empty)) && bool(owner.writeBytes({})),
               "Empty synchronous operations must accept zero progress.");
        expect(owner && state.destroyed == 0, "Operation failures must never consume the owner.");
        owner.reset();
        const auto readCalls = state.reads;
        const auto writeCalls = state.writes;
        expect(owner.readBytes(destination).status == NativeIoStatus::NoResource &&
               owner.writeBytes({}).status == NativeIoStatus::NoResource &&
               readCalls == state.reads && writeCalls == state.writes,
               "Empty owners must fail before invoking an adapter.");
        expect(state.destroyed == 1, "A resource must remain available for one cleanup after failed operations.");
    }
}

int main()
{
    ownership();
    buffers();
    progress();
    if (failures) return 1;
    std::cout << "Native boundary ownership, buffers, and adapter progress passed.\n";
}

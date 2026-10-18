// ========================
//
// Created By : miruku_sora2026
//
// File name : NativeBoundary.hpp
//
// Description :
//     신뢰된 C++ 연결층의 타입별 자원 소유권과 동기식 바이트 대여 경계를 정의한다.
//     Define typed resource ownership and synchronous byte loans in the trusted C++ bridge.
//
// ========================

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>

namespace aevix
{
    enum class NativeBufferStatus : std::uint8_t
    {
        Success,
        NullWithNonzeroLength,
        LengthOutOfRange,
        AddressOverflow
    };

    template <typename Buffer>
    struct NativeBufferResult;

    // span 생성자의 전제는 실제로 살아 있는 해당 크기의 저장소가 존재한다는 것이다.
    // The span constructor requires live storage of the stated extent.
    class NativeInputBytes final
    {
    public:
        NativeInputBytes() noexcept = default;
        explicit NativeInputBytes(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}

        [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return bytes_; }
        [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }
        [[nodiscard]] bool empty() const noexcept { return bytes_.empty(); }

        // 이 검사는 할당 크기나 포인터의 생존 여부를 증명하지 않는다.
        // This check does not prove allocation extent or pointer lifetime.
        [[nodiscard]] static NativeBufferResult<NativeInputBytes>
        checkedRaw(const void* data, std::uint64_t length) noexcept;

    private:
        std::span<const std::byte> bytes_;
    };

    // 수정 뷰 자체의 복제를 막는다. C++ 호출자는 배타적인 대여를 보장해야 한다.
    // Prevent copying the mutable view itself; C++ callers must ensure exclusive access.
    class NativeOutputBytes final
    {
    public:
        NativeOutputBytes() noexcept = default;
        explicit NativeOutputBytes(std::span<std::byte> bytes) noexcept : bytes_(bytes) {}
        NativeOutputBytes(const NativeOutputBytes&) = delete;
        NativeOutputBytes& operator=(const NativeOutputBytes&) = delete;
        NativeOutputBytes(NativeOutputBytes&& other) noexcept
            : bytes_(std::exchange(other.bytes_, {})) {}
        NativeOutputBytes& operator=(NativeOutputBytes&& other) noexcept
        {
            if (this != &other) bytes_ = std::exchange(other.bytes_, {});
            return *this;
        }

        [[nodiscard]] std::span<std::byte> bytes() noexcept { return bytes_; }
        [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }
        [[nodiscard]] bool empty() const noexcept { return bytes_.empty(); }

        // 길이·주소 산술만 검사한다. 실제 저장소의 유효성은 신뢰된 ABI 호출자의 책임이다.
        // Check length and address arithmetic only; storage validity belongs to the trusted ABI caller.
        [[nodiscard]] static NativeBufferResult<NativeOutputBytes>
        checkedRaw(void* data, std::uint64_t length) noexcept;

    private:
        std::span<std::byte> bytes_;
    };

    template <typename Buffer>
    struct NativeBufferResult final
    {
        NativeBufferStatus status = NativeBufferStatus::Success;
        Buffer buffer;
        [[nodiscard]] explicit operator bool() const noexcept
        {
            return status == NativeBufferStatus::Success;
        }
    };

    // 플랫폼 오류는 아직 공개 Aevix 오류 타입이 아닌 내부 원시 메타데이터이다.
    // Platform errors are internal primitive metadata, not a public Aevix error type.
    struct NativePlatformError final
    {
        std::uint32_t domain = 0;
        std::int64_t code = 0;
        bool operator==(const NativePlatformError&) const = default;
    };

    enum class NativeAdapterStatus : std::uint8_t { Success, PlatformError };

    struct NativeAdapterOutcome final
    {
        NativeAdapterStatus status = NativeAdapterStatus::Success;
        std::size_t completedBytes = 0;
        NativePlatformError platformError;
    };

    enum class NativeIoStatus : std::uint8_t
    {
        Success,
        PlatformError,
        NoResource,
        AdapterContractViolation
    };

    struct NativeIoOutcome final
    {
        NativeIoStatus status = NativeIoStatus::Success;
        std::size_t completedBytes = 0;
        NativePlatformError platformError;
        [[nodiscard]] explicit operator bool() const noexcept
        {
            return status == NativeIoStatus::Success;
        }
    };

    namespace native_detail
    {
        [[nodiscard]] NativeIoOutcome validateOutcome(NativeAdapterOutcome outcome,
                                                     std::size_t requestedBytes) noexcept;
    }

    // 어댑터는 신뢰된 C++ 코드이다. 대여를 보관·반환하거나 Aevix로 재진입하지 않는다.
    // Adapters are trusted C++ code: they must not retain or return loans or re-enter Aevix.
    // 임의의 네이티브 구현 본문이나 버퍼 밖의 접근을 이 인터페이스로 증명할 수는 없다.
    // This interface cannot prove arbitrary native bodies or prevent out-of-buffer access.
    template <typename Tag>
    class NativeResource
    {
    public:
        NativeResource() noexcept = default;
        NativeResource(const NativeResource&) = delete;
        NativeResource& operator=(const NativeResource&) = delete;
        virtual ~NativeResource() noexcept = default;

        virtual NativeAdapterOutcome readBytes(NativeOutputBytes& destination) noexcept = 0;
        virtual NativeAdapterOutcome writeBytes(NativeInputBytes source) noexcept = 0;
    };

    // Tag는 컴파일 타임 자원 식별자이다. OS 핸들·주소를 꺼내거나 복제하는 API는 없다.
    // Tag identifies a resource at compile time; no API extracts or clones an OS handle or address.
    // 이 클래스는 스레드 전송 권한을 제공하지 않는다. 접근의 외부 동기화가 필요하다.
    // This class grants no thread-transfer permission; access requires external synchronization.
    template <typename Tag>
    class NativeOwner final
    {
    public:
        NativeOwner() noexcept = default;
        explicit NativeOwner(std::unique_ptr<NativeResource<Tag>> resource) noexcept
            : resource_(std::move(resource)) {}
        NativeOwner(const NativeOwner&) = delete;
        NativeOwner& operator=(const NativeOwner&) = delete;
        NativeOwner(NativeOwner&&) noexcept = default;
        NativeOwner& operator=(NativeOwner&&) noexcept = default;
        ~NativeOwner() noexcept = default;

        [[nodiscard]] explicit operator bool() const noexcept { return bool(resource_); }
        void reset() noexcept { resource_.reset(); }

        [[nodiscard]] NativeIoOutcome readBytes(NativeOutputBytes& destination) noexcept
        {
            if (!resource_) return {NativeIoStatus::NoResource, 0, {}};
            const auto requestedBytes = destination.size();
            const auto outcome = resource_->readBytes(destination);
            return native_detail::validateOutcome(outcome, requestedBytes);
        }

        [[nodiscard]] NativeIoOutcome writeBytes(NativeInputBytes source) noexcept
        {
            if (!resource_) return {NativeIoStatus::NoResource, 0, {}};
            const auto requestedBytes = source.size();
            const auto outcome = resource_->writeBytes(source);
            return native_detail::validateOutcome(outcome, requestedBytes);
        }

    private:
        std::unique_ptr<NativeResource<Tag>> resource_;
    };
}

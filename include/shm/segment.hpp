#pragma once

#include <atomic>
#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace lle::shm {

enum class SegmentOperation : uint8_t {
    Validate,
    Open,
    Resize,
    Stat,
    Map,
    Unmap,
    Close,
    Unlink,
};

struct SegmentError {
    SegmentOperation operation;
    int error_number;
};

class SharedMemorySegment {
  public:
    static std::expected<SharedMemorySegment, SegmentError>
    create_shm(std::string name, std::size_t size) noexcept;

    static std::expected<SharedMemorySegment, SegmentError>
    attach_shm(std::string name) noexcept;

    SharedMemorySegment() noexcept = default;

    ~SharedMemorySegment();

    SharedMemorySegment(const SharedMemorySegment&) = delete;
    SharedMemorySegment& operator=(const SharedMemorySegment&) = delete;

    SharedMemorySegment(SharedMemorySegment&& other) noexcept;
    SharedMemorySegment& operator=(SharedMemorySegment&& other) noexcept;

    std::expected<void, SegmentError> close() noexcept;
    std::expected<void, SegmentError> unlink() noexcept;

    // inline getters

    [[nodiscard]] std::span<std::byte> bytes() noexcept {
        return {address_, size_};
    }
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return {address_, size_};
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }
    [[nodiscard]] bool is_open() const noexcept {
        return descriptor_ != -1 && address_ != nullptr;
    }
    [[nodiscard]] bool is_owner() const noexcept {
        return owner_;
    }

  private:
    static constexpr std::size_t max_segment_name_length{255};

    SharedMemorySegment(
        std::string name,
        int descriptor,
        std::byte* address,
        std::size_t size,
        bool owner
    ) noexcept;

    [[nodiscard]] static bool is_shm_name_valid(std::string_view name) noexcept;

    std::string name_;
    int descriptor_{-1};
    std::byte* address_{nullptr};
    std::size_t size_{0};
    bool owner_{false};
    bool linked_{false};
};

} // namespace lle::shm

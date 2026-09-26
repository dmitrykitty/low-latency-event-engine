#pragma once

#include <cstddef>
#include <cstdint>

namespace lle::protocol::internals {

[[nodiscard]] constexpr bool
has_bytes(std::size_t size, std::size_t offset, std::size_t count) noexcept {
    return offset <= size && count <= size - offset;
}

inline void write_u16(std::byte* dst, std::uint16_t value) noexcept {
    dst[0] = static_cast<std::byte>(value);
    dst[1] = static_cast<std::byte>(value >> 8U);
}

inline void write_u32(std::byte* dst, std::uint32_t value) noexcept {
    write_u16(dst, static_cast<std::uint16_t>(value));
    write_u16(dst + 2, static_cast<std::uint16_t>(value >> 16U));
}

inline void write_u64(std::byte* dst, std::uint64_t value) noexcept {
    write_u32(dst, static_cast<std::uint32_t>(value));
    write_u32(dst + 4, static_cast<std::uint32_t>(value >> 32U));
}

[[nodiscard]] inline std::uint16_t read_u16(const std::byte* src) noexcept {
    return static_cast<std::uint16_t>(src[0]) | (static_cast<std::uint16_t>(src[1]) << 8U);
}

[[nodiscard]] inline std::uint32_t read_u32(const std::byte* src) noexcept {
    return static_cast<std::uint32_t>(read_u16(src)) |
           (static_cast<std::uint32_t>(read_u16(src + 2)) << 16U);
}

[[nodiscard]] inline std::uint64_t read_u64(const std::byte* src) noexcept {
    return static_cast<std::uint64_t>(read_u32(src)) |
           (static_cast<std::uint64_t>(read_u32(src + 4)) << 32U);
}

} // namespace lle::protocol::internals

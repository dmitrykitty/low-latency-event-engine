#pragma once
#include <cstddef>

namespace lle::detail {
// alignment must be a nonzero power of two; the result must fit in size_t.
[[nodiscard]] constexpr std::size_t align_up(std::size_t value, std::size_t alignment) noexcept {
    const auto remainder = value & (alignment - 1);
    if (remainder == 0) {
        return value;
    }
    return value + alignment - remainder;
}
} // namespace lle::detail
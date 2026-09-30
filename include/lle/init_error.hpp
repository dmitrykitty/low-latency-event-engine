#pragma once

#include <cstdint>

namespace lle {

enum class InitError : std::uint8_t {
    InvalidConfig,
    SharedMemory,
    Ring,
    AddressResolution,
    Socket,
    SessionId,
};

} // namespace lle

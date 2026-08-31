#pragma once

#include "lle/config.hpp"
#include "lle/stats.hpp"

namespace lle {

class Receiver {
public:
    explicit Receiver(ReceiverConfig config);

    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;
    Receiver(Receiver&&) = delete;
    Receiver& operator=(Receiver&&) = delete;

    void run();
    void request_stop() noexcept;

    [[nodiscard]] EngineStats stats() const noexcept;
};

} // namespace lle


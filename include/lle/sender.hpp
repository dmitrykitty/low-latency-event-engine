#pragma once

#include "lle/config.hpp"
#include "lle/stats.hpp"

namespace lle {

class Sender {
public:
    explicit Sender(SenderConfig config);

    Sender(const Sender&) = delete;
    Sender& operator=(const Sender&) = delete;
    Sender(Sender&&) = delete;
    Sender& operator=(Sender&&) = delete;

    void run();
    void request_stop() noexcept;

    [[nodiscard]] EngineStats stats() const noexcept;
};

} // namespace lle


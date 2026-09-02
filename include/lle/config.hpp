#pragma once

#include "lle/event.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace lle {

struct Endpoint {
    std::string address;
    std::uint16_t port{};
};

struct SenderConfig {
    std::string ingress_shm_name;
    std::vector<Endpoint> receivers;
    std::uint32_t max_datagram_bytes{1416};
    std::uint32_t retransmit_window_packets{65'536};
    DeliveryMode delivery_mode{DeliveryMode::BestEffort};
    int cpu{-1};
    bool busy_spin_ingress{true};
};

struct ReceiverConfig {
    Endpoint listen;
    Endpoint nack_target;
    std::string egress_shm_name;
    DeliveryMode delivery_mode{DeliveryMode::BestEffort};
    std::uint32_t reorder_window_packets{4096};
    std::uint32_t recovery_timeout_us{100};
    int cpu{-1};
    int busy_poll_us{0};
};

} // namespace lle

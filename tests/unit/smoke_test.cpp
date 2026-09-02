#include "lle/config.hpp"
#include "lle/event.hpp"
#include "lle/version.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

int main() {
    if (lle::version().empty()) {
        std::cerr << "project version must not be empty\n";
        return EXIT_FAILURE;
    }

    const lle::SenderConfig config{};
    if (config.max_datagram_bytes != 1416U) {
        std::cerr << "unexpected default datagram cap\n";
        return EXIT_FAILURE;
    }

    static_assert(sizeof(lle::Sequence) == 8U);
    static_assert(sizeof(lle::StreamId) == 4U);
    return EXIT_SUCCESS;
}

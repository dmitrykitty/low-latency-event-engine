#include "lle/version.hpp"

namespace lle {

std::string_view version() noexcept {
    return LLE_VERSION_STRING;
}

} // namespace lle


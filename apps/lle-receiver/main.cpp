#include "lle/version.hpp"

#include <iostream>

int main() {
    std::cout << "lle-receiver " << lle::version() << " (transport scaffold)\n";
    return 0;
}


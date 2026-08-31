#include "lle/version.hpp"

#include <iostream>

int main() {
    std::cout << "lle-sender " << lle::version() << " (transport scaffold)\n";
    return 0;
}


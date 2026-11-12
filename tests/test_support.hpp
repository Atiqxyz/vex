#pragma once

#include <iostream>
#include <string_view>

struct TestContext {
    int failures{0};

    void check(const bool condition, const std::string_view message) {
        if (!condition) {
            ++failures;
            std::cerr << "FAIL: " << message << '\n';
        }
    }
};

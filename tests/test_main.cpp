#include "test_support.hpp"
void run_source_tests(TestContext& tests);
void run_lexer_tests(TestContext& tests);
void run_parser_tests(TestContext& tests);
int main() {
    TestContext tests;
    run_source_tests(tests);
    run_lexer_tests(tests);
    run_parser_tests(tests);
    if (tests.failures != 0) {
        std::cerr << tests.failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All VEX frontend tests passed.\n";
    return 0;
}

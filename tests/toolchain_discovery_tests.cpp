#include "test_support.hpp"

#include "vex/detail/toolchain_discovery.hpp"

#include <filesystem>

namespace {

void test_bundled_llvm_path_uses_installation_root(TestContext& tests) {
    const std::filesystem::path executable =
        std::filesystem::path("portable-vex") / "bin" / "vex";
    const std::filesystem::path compiler =
        vex::detail::bundled_llvm_compiler_path(executable);

#ifdef _WIN32
    const std::filesystem::path expected =
        std::filesystem::path("portable-vex") / "toolchain" / "bin" / "clang.exe";
#else
    const std::filesystem::path expected =
        std::filesystem::path("portable-vex") / "toolchain" / "bin" / "clang";
#endif

    tests.check(compiler == expected,
                "bundled LLVM discovery resolves from the VEX installation root");
}

void test_bundled_llvm_path_rejects_empty_executable_path(TestContext& tests) {
    tests.check(vex::detail::bundled_llvm_compiler_path({}).empty(),
                "bundled LLVM discovery handles an unknown executable path");
}

} // namespace

void run_toolchain_discovery_tests(TestContext& tests) {
    test_bundled_llvm_path_uses_installation_root(tests);
    test_bundled_llvm_path_rejects_empty_executable_path(tests);
}

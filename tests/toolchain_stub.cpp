#include <filesystem>
#include <iostream>
#include <string_view>
#include <system_error>

int main(const int argc, char* argv[]) {
    std::filesystem::path output;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "-o" && index + 1 < argc) {
            output = argv[++index];
            break;
        }
        constexpr std::string_view emit_option = "-femit-bin=";
        if (argument.starts_with(emit_option)) {
            output = argument.substr(emit_option.size());
            break;
        }
    }

    if (!output.empty()) {
        std::error_code error;
        std::filesystem::copy_file(
            argv[0],
            output,
            std::filesystem::copy_options::overwrite_existing,
            error);
        if (error) {
            std::cerr << "mock toolchain could not create its output: "
                      << error.message() << '\n';
            return 1;
        }
        return 0;
    }

    std::cout << "VEX_BUNDLED_CLANG_FOUND\n";
    return 0;
}

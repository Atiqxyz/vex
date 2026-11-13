#include <filesystem>
#include <iostream>
#include <string_view>
#include <system_error>

int main(const int argc, char* argv[]) {
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::string_view(argv[index]) != "-o") continue;

        std::error_code error;
        std::filesystem::copy_file(
            argv[0],
            argv[index + 1],
            std::filesystem::copy_options::overwrite_existing,
            error);
        if (error) {
            std::cerr << "mock Clang could not create its output: "
                      << error.message() << '\n';
            return 1;
        }
        return 0;
    }

    std::cout << "VEX_BUNDLED_CLANG_FOUND\n";
    return 0;
}

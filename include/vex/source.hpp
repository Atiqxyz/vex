#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vex {

struct SourceFileUnit {
    std::string path;
    std::string contents;
    std::string module;
};

struct SourcePosition {
    std::size_t offset{0};
    std::size_t line{1};
    std::size_t column{1};
};

struct SourceSpan {
    std::size_t begin{0};
    std::size_t end{0};

    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return end >= begin ? end - begin : 0;
    }

    [[nodiscard]] constexpr bool empty() const noexcept {
        return begin == end;
    }

    [[nodiscard]] constexpr bool is_valid_for(const std::size_t source_size) const noexcept {
        return begin <= end && end <= source_size;
    }
};

class SourceFile {
public:
    SourceFile(std::string path, std::string contents);
    // Concatenates files into one translation unit while retaining each file's
    // origin for diagnostics. File order is declaration order.
    [[nodiscard]] static SourceFile from_files(
        std::vector<std::pair<std::string, std::string>> files);
    // Keeps each file's logical module identity alongside its diagnostic path.
    [[nodiscard]] static SourceFile from_modules(
        std::vector<SourceFileUnit> files, std::string root_module);

    [[nodiscard]] const std::string& path() const noexcept;
    [[nodiscard]] const std::string& root_module() const noexcept;
    [[nodiscard]] std::string_view contents() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

    // Offsets and columns are byte-based. Lines and columns are one-based.
    [[nodiscard]] SourcePosition position(std::size_t offset) const noexcept;
    [[nodiscard]] std::string_view line_text(std::size_t one_based_line) const noexcept;
    [[nodiscard]] const std::string& path_at(std::size_t offset) const noexcept;
    [[nodiscard]] const std::string& module_at(std::size_t offset) const noexcept;
    [[nodiscard]] const std::string& module_for_path(std::string_view path) const noexcept;
    [[nodiscard]] SourcePosition origin_position(std::size_t offset) const noexcept;
    [[nodiscard]] std::string_view origin_line_text(std::size_t offset) const noexcept;
    [[nodiscard]] std::string_view slice(SourceSpan span) const noexcept;

private:
    struct Origin {
        std::string path;
        std::string contents;
        std::string module;
        std::size_t combined_offset{0};
        std::vector<std::size_t> line_starts;
    };

    struct CombinedSourcesTag {};
    SourceFile(std::vector<SourceFileUnit> files, CombinedSourcesTag);
    void index_lines();
    void index_origin(Origin& origin);

    std::string path_;
    std::string root_module_;
    std::string contents_;
    std::vector<std::size_t> line_starts_;
    std::vector<Origin> origins_;
};

} // namespace vex

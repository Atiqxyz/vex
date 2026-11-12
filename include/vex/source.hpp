#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace vex {

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

    [[nodiscard]] const std::string& path() const noexcept;
    [[nodiscard]] std::string_view contents() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

    // Offsets and columns are byte-based. Lines and columns are one-based.
    [[nodiscard]] SourcePosition position(std::size_t offset) const noexcept;
    [[nodiscard]] std::string_view line_text(std::size_t one_based_line) const noexcept;
    [[nodiscard]] std::string_view slice(SourceSpan span) const noexcept;

private:
    void index_lines();

    std::string path_;
    std::string contents_;
    std::vector<std::size_t> line_starts_;
};

} // namespace vex

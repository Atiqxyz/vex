#include "vex/source.hpp"

#include <algorithm>
#include <utility>

namespace vex {

SourceFile::SourceFile(std::string path, std::string contents)
    : path_(std::move(path)), contents_(std::move(contents)) {
    index_lines();
}

const std::string& SourceFile::path() const noexcept {
    return path_;
}

std::string_view SourceFile::contents() const noexcept {
    return contents_;
}

std::size_t SourceFile::size() const noexcept {
    return contents_.size();
}

void SourceFile::index_lines() {
    line_starts_.clear();
    line_starts_.push_back(0);

    for (std::size_t index = 0; index < contents_.size(); ++index) {
        if (contents_[index] == '\r') {
            if (index + 1 < contents_.size() && contents_[index + 1] == '\n') {
                ++index;
            }
            line_starts_.push_back(index + 1);
        } else if (contents_[index] == '\n') {
            line_starts_.push_back(index + 1);
        }
    }
}

SourcePosition SourceFile::position(std::size_t offset) const noexcept {
    offset = std::min(offset, contents_.size());
    const auto next = std::upper_bound(line_starts_.begin(), line_starts_.end(), offset);
    const std::size_t line_index =
        static_cast<std::size_t>(std::distance(line_starts_.begin(), next) - 1);
    const std::size_t line_start = line_starts_[line_index];

    return SourcePosition{offset, line_index + 1, offset - line_start + 1};
}

std::string_view SourceFile::line_text(const std::size_t one_based_line) const noexcept {
    if (one_based_line == 0 || one_based_line > line_starts_.size()) {
        return {};
    }

    const std::size_t start = line_starts_[one_based_line - 1];
    std::size_t end = one_based_line < line_starts_.size()
        ? line_starts_[one_based_line]
        : contents_.size();

    while (end > start && (contents_[end - 1] == '\n' || contents_[end - 1] == '\r')) {
        --end;
    }

    return std::string_view(contents_).substr(start, end - start);
}

std::string_view SourceFile::slice(const SourceSpan span) const noexcept {
    if (!span.is_valid_for(contents_.size())) {
        return {};
    }
    return std::string_view(contents_).substr(span.begin, span.size());
}

} // namespace vex

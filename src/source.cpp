#include "vex/source.hpp"

#include <algorithm>
#include <utility>

namespace vex {

SourceFile::SourceFile(std::string path, std::string contents)
    : path_(std::move(path)), contents_(std::move(contents)) {
    index_lines();
    Origin origin{path_, contents_, 0, {}};
    index_origin(origin);
    origins_.push_back(std::move(origin));
}

SourceFile::SourceFile(std::vector<std::pair<std::string, std::string>> files) {
    if (files.empty()) {
        path_ = "<empty compilation unit>";
        Origin origin{path_, {}, 0, {0}};
        origins_.push_back(std::move(origin));
        line_starts_.push_back(0);
        return;
    }
    path_ = files.front().first;
    for (auto& file : files) {
        if (!contents_.empty() && contents_.back() != '\n' &&
            contents_.back() != '\r') contents_.push_back('\n');
        Origin origin;
        origin.path = std::move(file.first);
        origin.contents = std::move(file.second);
        origin.combined_offset = contents_.size();
        contents_ += origin.contents;
        index_origin(origin);
        origins_.push_back(std::move(origin));
    }
    index_lines();
}

void SourceFile::index_origin(Origin& origin) {
    origin.line_starts.clear();
    origin.line_starts.push_back(0);
    for (std::size_t index = 0; index < origin.contents.size(); ++index) {
        if (origin.contents[index] == '\r') {
            if (index + 1 < origin.contents.size() &&
                origin.contents[index + 1] == '\n') ++index;
            origin.line_starts.push_back(index + 1);
        } else if (origin.contents[index] == '\n') {
            origin.line_starts.push_back(index + 1);
        }
    }
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

const std::string& SourceFile::path_at(std::size_t offset) const noexcept {
    if (origins_.empty()) return path_;
    offset = std::min(offset, contents_.size());
    const auto found = std::upper_bound(
        origins_.begin(), origins_.end(), offset,
        [](const std::size_t value, const Origin& origin) {
            return value < origin.combined_offset;
        });
    return (found == origins_.begin() ? origins_.front() : *std::prev(found)).path;
}

SourcePosition SourceFile::origin_position(std::size_t offset) const noexcept {
    if (origins_.empty()) return position(offset);
    offset = std::min(offset, contents_.size());
    const auto found = std::upper_bound(
        origins_.begin(), origins_.end(), offset,
        [](const std::size_t value, const Origin& origin) {
            return value < origin.combined_offset;
        });
    const Origin& origin = found == origins_.begin() ? origins_.front() : *std::prev(found);
    const std::size_t local = std::min(offset - origin.combined_offset,
                                       origin.contents.size());
    const auto next = std::upper_bound(origin.line_starts.begin(),
                                       origin.line_starts.end(), local);
    const std::size_t line_index =
        static_cast<std::size_t>(std::distance(origin.line_starts.begin(), next) - 1);
    const std::size_t line_start = origin.line_starts[line_index];
    return SourcePosition{local, line_index + 1u, local - line_start + 1u};
}

std::string_view SourceFile::origin_line_text(std::size_t offset) const noexcept {
    if (origins_.empty()) return {};
    offset = std::min(offset, contents_.size());
    const auto found = std::upper_bound(
        origins_.begin(), origins_.end(), offset,
        [](const std::size_t value, const Origin& origin) {
            return value < origin.combined_offset;
        });
    const Origin& origin = found == origins_.begin() ? origins_.front() : *std::prev(found);
    const std::size_t local = std::min(offset - origin.combined_offset,
                                       origin.contents.size());
    const auto next = std::upper_bound(origin.line_starts.begin(),
                                       origin.line_starts.end(), local);
    const std::size_t line_index =
        static_cast<std::size_t>(std::distance(origin.line_starts.begin(), next) - 1);
    const std::size_t start = origin.line_starts[line_index];
    std::size_t end = line_index + 1u < origin.line_starts.size()
        ? origin.line_starts[line_index + 1u] : origin.contents.size();
    while (end > start &&
           (origin.contents[end - 1u] == '\n' || origin.contents[end - 1u] == '\r')) --end;
    return std::string_view(origin.contents).substr(start, end - start);
}

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

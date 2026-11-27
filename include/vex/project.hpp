#pragma once
#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
namespace vex {
struct ProjectConfig {
    std::filesystem::path root;
    std::filesystem::path manifest;
    std::filesystem::path entry;
    std::filesystem::path output;
    std::string name;
    std::string backend;
};
[[nodiscard]] std::optional<ProjectConfig> discover_project_config(std::ostream& diagnostics);
[[nodiscard]] bool initialize_project(const std::string& requested_name, std::ostream& diagnostics);
} // namespace vex

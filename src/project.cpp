#include "vex/project.hpp"

#include <cctype>
#include <fstream>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace vex {
namespace fs = std::filesystem;
namespace {

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r");
    return value.substr(first, last - first + 1u);
}

void remove_comment(std::string& line) {
    bool quoted = false;
    bool escaped = false;
    for (std::size_t index = 0; index < line.size(); ++index) {
        const char value = line[index];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (quoted && value == '\\') {
            escaped = true;
            continue;
        }
        if (value == '"') {
            quoted = !quoted;
            continue;
        }
        if (!quoted && value == '#') {
            line.resize(index);
            return;
        }
    }
}

bool parse_string(const std::string& value, std::string& result) {
    if (value.size() < 2u || value.front() != '"' || value.back() != '"')
        return false;
    result.clear();
    for (std::size_t index = 1u; index + 1u < value.size(); ++index) {
        char character = value[index];
        if (character == '\\') {
            if (++index + 1u >= value.size()) return false;
            character = value[index];
            if (character != '\\' && character != '"') return false;
        }
        if (character == '"' || character == '\0' || character == '\n' || character == '\r')
            return false;
        result.push_back(character);
    }
    return true;
}

bool valid_project_name(const std::string_view name) {
    if (name.empty() || name == "." || name == "..") return false;
    for (const unsigned char character : name) {
        if (!(std::isalnum(character) || character == '_' ||
              character == '-' || character == '.')) return false;
    }
    return true;
}

bool safe_relative_path(const fs::path& path) {
    if (path.empty() || path.is_absolute() ||
        path.has_root_name() || path.has_root_directory()) return false;
    for (const fs::path& component : path) {
        if (component == "..") return false;
    }
    return true;
}

void report_manifest_error(std::ostream& diagnostics,
                           const std::size_t line,
                           const std::string& message) {
    diagnostics << "error: vex.toml:" << line << ": " << message << '\n';
}

std::optional<ProjectConfig> load_project_config(
    const fs::path& manifest, std::ostream& diagnostics) {
    std::error_code error;
    const fs::path canonical_manifest = fs::canonical(manifest, error);
    if (error || !fs::is_regular_file(canonical_manifest, error) || error) {
        diagnostics << "error: cannot open project manifest '"
                    << manifest.string() << "'\n";
        return std::nullopt;
    }

    std::ifstream input(canonical_manifest, std::ios::binary);
    if (!input) {
        diagnostics << "error: cannot read project manifest '"
                    << canonical_manifest.string() << "'\n";
        return std::nullopt;
    }

    std::string name;
    std::string entry = "src/main.vex";
    std::string backend = "llvm";
    std::string output;
    std::string section;
    bool saw_project_section = false;
    std::unordered_set<std::string> keys;
    std::string line_text;
    std::size_t line_number = 0u;

    while (std::getline(input, line_text)) {
        ++line_number;
        remove_comment(line_text);
        const std::string line = trim(line_text);
        if (line.empty()) continue;

        if (line.front() == '[') {
            if (line != "[project]" || saw_project_section) {
                report_manifest_error(diagnostics, line_number,
                    "only one [project] section is supported");
                return std::nullopt;
            }
            section = "project";
            saw_project_section = true;
            continue;
        }

        if (section != "project") {
            report_manifest_error(diagnostics, line_number,
                "project settings must be inside a [project] section");
            return std::nullopt;
        }

        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) {
            report_manifest_error(diagnostics, line_number,
                "expected key = \"value\"");
            return std::nullopt;
        }

        const std::string key = trim(line.substr(0u, equals));
        const std::string raw_value = trim(line.substr(equals + 1u));
        if (key.empty()) {
            report_manifest_error(diagnostics, line_number,
                "project setting name cannot be empty");
            return std::nullopt;
        }
        if (!keys.emplace(key).second) {
            report_manifest_error(diagnostics, line_number,
                "duplicate project setting '" + key + "'");
            return std::nullopt;
        }
        if (key != "name" && key != "entry" &&
            key != "backend" && key != "output") {
            report_manifest_error(diagnostics, line_number,
                "unknown project setting '" + key + "'");
            return std::nullopt;
        }

        std::string value;
        if (!parse_string(raw_value, value)) {
            report_manifest_error(diagnostics, line_number,
                "project values must be quoted strings");
            return std::nullopt;
        }
        if (key == "name") name = std::move(value);
        else if (key == "entry") entry = std::move(value);
        else if (key == "backend") backend = std::move(value);
        else if (key == "output") output = std::move(value);
    }

    if (input.bad()) {
        diagnostics << "error: failed while reading project manifest '"
                    << canonical_manifest.string() << "'\n";
        return std::nullopt;
    }
    if (!saw_project_section) {
        report_manifest_error(diagnostics, 1u,
            "missing [project] section");
        return std::nullopt;
    }

    const fs::path root = canonical_manifest.parent_path();
    if (name.empty()) name = root.filename().string();
    if (name.empty()) name = "vex";
    if (!valid_project_name(name)) {
        report_manifest_error(diagnostics, 1u,
            "project name may contain only letters, digits, '.', '_' and '-'");
        return std::nullopt;
    }
    if (backend != "llvm" && backend != "c") {
        report_manifest_error(diagnostics, 1u,
            "backend must be either \"llvm\" or \"c\"");
        return std::nullopt;
    }

    const fs::path entry_path(entry);
    if (!safe_relative_path(entry_path)) {
        report_manifest_error(diagnostics, 1u,
            "entry must be a project-relative path without '..' components");
        return std::nullopt;
    }

    if (output.empty()) output = (fs::path("build") / name).generic_string();
    fs::path output_path(output);
    if (!safe_relative_path(output_path)) {
        report_manifest_error(diagnostics, 1u,
            "output must be a project-relative path without '..' components");
        return std::nullopt;
    }
#ifdef _WIN32
    if (!output_path.has_extension()) output_path += ".exe";
#endif

    const fs::path resolved_entry = root / entry_path;
    if (!fs::is_regular_file(resolved_entry, error) || error) {
        diagnostics << "error: project entry file does not exist or is not a regular file: "
                    << resolved_entry.string() << '\n';
        return std::nullopt;
    }

    return ProjectConfig{
        root, canonical_manifest, resolved_entry,
        root / output_path, std::move(name), std::move(backend)
    };
}

} // namespace

std::optional<ProjectConfig> discover_project_config(
    std::ostream& diagnostics) {
    std::error_code error;
    fs::path directory = fs::current_path(error);
    if (error) {
        diagnostics << "error: cannot determine the current directory: "
                    << error.message() << '\n';
        return std::nullopt;
    }

    for (;;) {
        const fs::path manifest = directory / "vex.toml";
        if (fs::exists(manifest, error) && !error)
            return load_project_config(manifest, diagnostics);
        error.clear();
        const fs::path parent = directory.parent_path();
        if (parent.empty() || parent == directory) break;
        directory = parent;
    }

    diagnostics << "error: no vex.toml found in this directory or its parents; "
                   "run 'vex init' to create a project\n";
    return std::nullopt;
}

bool initialize_project(
    const std::string& requested_name, std::ostream& diagnostics) {
    std::error_code error;
    const fs::path root = fs::current_path(error);
    if (error) {
        diagnostics << "error: cannot determine the current directory: "
                    << error.message() << '\n';
        return false;
    }

    std::string name = requested_name.empty() ? root.filename().string() : requested_name;
    if (name.empty()) name = "vex";
    if (!valid_project_name(name)) {
        diagnostics << "error: project name may contain only letters, digits, '.', '_' and '-'\n";
        return false;
    }

    const fs::path manifest = root / "vex.toml";
    const fs::path entry = root / "src" / "main.vex";
    if (fs::exists(manifest, error) || fs::exists(entry, error)) {
        diagnostics << "error: refusing to overwrite an existing vex.toml or src/main.vex\n";
        return false;
    }

    if (!fs::create_directories(entry.parent_path(), error) && error) {
        diagnostics << "error: cannot create project source directory: "
                    << error.message() << '\n';
        return false;
    }

    {
        std::ofstream config(manifest, std::ios::binary);
        if (!config) {
            diagnostics << "error: cannot create " << manifest.string() << '\n';
            return false;
        }
        config << "[project]\n"
               << "name = \"" << name << "\"\n"
               << "entry = \"src/main.vex\"\n"
               << "backend = \"llvm\"\n"
               << "output = \"build/" << name << "\"\n";
        if (!config) {
            diagnostics << "error: failed while writing " << manifest.string() << '\n';
            return false;
        }
    }

    {
        std::ofstream source(entry, std::ios::binary);
        if (!source) {
            fs::remove(manifest, error);
            diagnostics << "error: cannot create " << entry.string() << '\n';
            return false;
        }
        source << "make main() {\n"
               << "    say(\"Hello from " << name << "!\")\n"
               << "}\n";
        if (!source) {
            fs::remove(manifest, error);
            fs::remove(entry, error);
            diagnostics << "error: failed while writing " << entry.string() << '\n';
            return false;
        }
    }

    diagnostics << "Created VEX project '" << name
                << "' with entry src/main.vex and LLVM backend\n";
    return true;
}

} // namespace vex

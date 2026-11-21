#include "vex/module.hpp"

#include "vex/diagnostic.hpp"
#include "vex/lexer.hpp"
#include "vex/parser.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace vex {
namespace fs = std::filesystem;
namespace {

bool decode_import_path(const std::string_view literal, std::string& decoded) {
    if (literal.size() < 2u || literal.front() != '"' || literal.back() != '"')
        return false;
    decoded.clear();
    for (std::size_t index = 1u; index + 1u < literal.size(); ++index) {
        char value = literal[index];
        if (value == '\\') {
            if (++index + 1u >= literal.size()) return false;
            switch (literal[index]) {
            case 'n': value = '\n'; break;
            case 'r': value = '\r'; break;
            case 't': value = '\t'; break;
            case '0': value = '\0'; break;
            case '\\': value = '\\'; break;
            case '"': value = '"'; break;
            case '\'': value = '\''; break;
            default: return false;
            }
        }
        if (value == '\0') return false;
        decoded.push_back(value);
    }
    return !decoded.empty();
}

void report_import_error(std::ostream& diagnostics, const SourceFile& source,
                         const SourceSpan span, std::string message) {
    print_diagnostics(diagnostics, source, {
        Diagnostic{DiagnosticSeverity::Error, "MOD001", std::move(message), span}
    });
}

} // namespace

std::optional<SourceFile> load_source_bundle_with_imports(
    const std::vector<std::string>& paths, std::ostream& diagnostics) {
    if (paths.empty()) {
        diagnostics << "error: at least one VEX source file is required\n";
        return std::nullopt;
    }

    // Presence means a path is active in the DFS or already loaded.
    std::unordered_set<std::string> visited;
    std::unordered_set<std::string> explicit_roots;
    std::vector<std::pair<std::string, std::string>> files;

    const auto visit = [&](const auto& self, const fs::path& requested,
                           const SourceFile* importer, const SourceSpan import_span,
                           const bool duplicate_explicit_root) -> bool {
        std::error_code error;
        const fs::path canonical = fs::canonical(requested, error);
        if (error) {
            const std::string message = "cannot resolve source file '" +
                requested.string() + "': " + error.message();
            if (importer) report_import_error(diagnostics, *importer, import_span, message);
            else diagnostics << "error: " << message << '\n';
            return false;
        }
        if (!fs::is_regular_file(canonical, error) || error) {
            const std::string message = "source path is not a regular file: " +
                canonical.string();
            if (importer) report_import_error(diagnostics, *importer, import_span, message);
            else diagnostics << "error: " << message << '\n';
            return false;
        }

        const std::string key = canonical.generic_string();
        if (visited.find(key) != visited.end()) {
            // Supplying the same root twice remains a duplicate-declaration error.
            if (!duplicate_explicit_root) return true;
            std::ifstream duplicate_input(canonical, std::ios::binary);
            if (!duplicate_input) {
                diagnostics << "error: could not open source file: "
                            << canonical.string() << '\n';
                return false;
            }
            std::string duplicate_contents{
                std::istreambuf_iterator<char>(duplicate_input),
                std::istreambuf_iterator<char>()};
            if (duplicate_input.bad()) {
                diagnostics << "error: failed while reading source file: "
                            << canonical.string() << '\n';
                return false;
            }
            files.emplace_back(canonical.string(), std::move(duplicate_contents));
            return true;
        }
        visited.emplace(key);

        std::ifstream input(canonical, std::ios::binary);
        if (!input) {
            const std::string message = "could not open source file: " + canonical.string();
            if (importer) report_import_error(diagnostics, *importer, import_span, message);
            else diagnostics << "error: " << message << '\n';
            return false;
        }
        std::string contents{
            std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        if (input.bad()) {
            const std::string message = "failed while reading source file: " + canonical.string();
            if (importer) report_import_error(diagnostics, *importer, import_span, message);
            else diagnostics << "error: " << message << '\n';
            return false;
        }

        const SourceFile source(canonical.string(), contents);
        const LexResult lexed = Lexer(source).scan();
        if (lexed.has_errors()) {
            print_diagnostics(diagnostics, source, lexed.diagnostics);
            return false;
        }
        const ParseResult parsed = Parser(source, lexed.tokens).parse();
        if (parsed.has_errors()) {
            print_diagnostics(diagnostics, source, parsed.diagnostics);
            return false;
        }

        for (const StmtPtr& statement : parsed.program.statements) {
            if (!statement) continue;
            const auto* import = std::get_if<ImportDeclStmt>(&statement->data);
            if (!import) continue;
            std::string relative_path;
            if (!decode_import_path(source.slice(import->path_literal), relative_path)) {
                report_import_error(diagnostics, source, import->path_literal,
                    "import path must be a nonempty quoted relative path without NUL bytes");
                return false;
            }
            const fs::path relative(relative_path);
            if (relative.is_absolute() || relative.has_root_name() ||
                relative.has_root_directory()) {
                report_import_error(diagnostics, source, import->path_literal,
                    "import paths must be relative to the importing file");
                return false;
            }
            if (!self(self, canonical.parent_path() / relative,
                      &source, import->path_literal, false)) return false;
        }

        files.emplace_back(canonical.string(), std::move(contents));
        return true;
    };

    for (const std::string& path : paths) {
        std::error_code error;
        const fs::path canonical_root = fs::canonical(fs::path(path), error);
        const bool duplicate_root = !error &&
            !explicit_roots.emplace(canonical_root.generic_string()).second;
        if (!visit(visit, fs::path(path), nullptr, {}, duplicate_root))
            return std::nullopt;
    }
    return SourceFile::from_files(std::move(files));
}

void strip_resolved_imports(Program& program) {
    std::erase_if(program.statements, [](const StmtPtr& statement) {
        return statement && std::holds_alternative<ImportDeclStmt>(statement->data);
    });
}

} // namespace vex

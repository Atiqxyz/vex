#include "test_support.hpp"

#include "vex/source.hpp"

void test_combined_source_preserves_file_origins(TestContext& tests) {
    const vex::SourceFile source({
        {"main.vex", "make main() {\n    helper()\n}\n"},
        {"math.vex", "make helper() {\n    let value: int = \"wrong\"\n}\n"}
    });
    const std::size_t offset = source.contents().find("wrong");
    const vex::SourcePosition position = source.origin_position(offset);
    tests.check(source.path_at(offset) == "math.vex",
                "combined translation units retain the physical source path");
    tests.check(position.line == 2u && position.column == 23u,
                "combined translation units retain local line and column positions");
    tests.check(source.origin_line_text(offset) == "    let value: int = \"wrong\"",
                "combined translation units resolve lines from the originating file");
    tests.check(source.slice({offset, offset + 5u}) == "wrong",
                "combined translation-unit spans slice the shared parser buffer");
}



void run_source_tests(TestContext& tests) {
    const vex::SourceFile source{"sample.vex", "alpha\r\nbeta\ngamma"};

    tests.check(source.path() == "sample.vex", "source path is retained");
    tests.check(source.size() == 17, "source byte length is retained");
    tests.check(source.line_text(1) == "alpha", "CRLF first line is indexed");
    tests.check(source.line_text(2) == "beta", "LF second line is indexed");
    tests.check(source.line_text(3) == "gamma", "final line is indexed");

    const vex::SourcePosition beta = source.position(7);
    tests.check(beta.line == 2 && beta.column == 1,
                "source position after CRLF is line 2, column 1");

    const vex::SourcePosition end = source.position(source.size());
    tests.check(end.line == 3 && end.column == 6,
                "end-of-file position is computed");

    tests.check(source.slice({0, 5}) == "alpha", "source spans slice without copying");
    tests.check(source.slice({4, 3}).empty(), "invalid reversed source span is rejected");
    test_combined_source_preserves_file_origins(tests);
}

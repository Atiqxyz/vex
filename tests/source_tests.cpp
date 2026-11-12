#include "test_support.hpp"

#include "vex/source.hpp"

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
}

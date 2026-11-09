// unity_tests.cpp -- the ombdc unit tests as one translation unit.
//
// The front end's sources (without its main.cpp), the reporter and the
// tests, compiled as one TU by build.bat and build.sh.

#include "../src/token.cpp"
#include "../src/file.cpp"
#include "../src/lexer.cpp"
#include "../src/ast.cpp"
#include "../src/parser.cpp"
#include "../src/catalog.cpp"
#include "../src/resolve.cpp"

#include "unit_test.cpp"
#include "lexer_tests.cpp"
#include "parser_tests.cpp"
#include "resolve_tests.cpp"
#include "test_main.cpp"

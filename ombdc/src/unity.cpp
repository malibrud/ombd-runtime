// unity.cpp -- ombdc as one translation unit.
//
// Every source file of the front end, plus the driver, compiled as one TU by
// build.bat and build.sh. Each .cpp includes the headers it uses, so they
// also compile on their own.

#include "token.cpp"
#include "file.cpp"
#include "lexer.cpp"
#include "ast.cpp"
#include "parser.cpp"
#include "catalog.cpp"
#include "resolve.cpp"
#include "main.cpp"

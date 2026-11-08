// parser.h -- turning tokens into a syntax tree (Specification.md §2, Appendix A).

#pragma once
#include "ast.h"
#include "file.h"
#include "token.h"

// Where parsing stopped, and why.
struct ParseError
{
    uint32_t offset;             // byte offset in the source buffer
    uint32_t line;               // from 1
    uint32_t col;                // from 1, counting bytes
    char     message[256];
};

// Parse the tokens of `source` into `ast`. On success `error` is untouched.
// On the first error, `error` is filled, `ast` is freed and false is
// returned. The tree's nodes refer to `tokens` by index, so keep the tokens
// for as long as the tree.
bool tryParse( const Source *source, const TokenArray *tokens, Ast *ast, ParseError *error );

// Print `path:line:col: error: message` to stderr.
void ParseError_print( const Source *source, const ParseError *error );

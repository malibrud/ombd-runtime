// token.h -- the tokens of an OMBD source.
//
// The lexer turns a source into a TokenArray (Specification.md §1, U15).
// A token is a view into the source text: it records where its bytes are,
// never a copy of them. Numbers and strings keep their spelling.

#pragma once
#include <stdint.h>

// Every token kind of Specification.md §1.9, in the order of the §1.7 table.
// Punctuators are named by their shape, not their role: `<` is both a
// comparison and a quaternion opener, and only the parser knows which.
// The three product operators are the exception, because each has one role
// and two spellings (an ASCII one and a non-ASCII alias).
enum TokenKind
{
    TOK_EOF,                     // always the last token; len 0

    // words and literals
    TOK_IDENT,                   // [A-Za-z_][A-Za-z0-9_]*'*   (D120)
    TOK_KEYWORD,                 // for and or not mod          (D121)
    TOK_NUMBER,                  // 12  0.833  8.0e-4           (D117)
    TOK_STRING,                  // "OZX": the token spans the quotes (D122)

    // single
    TOK_LPAREN,                  // (
    TOK_RPAREN,                  // )
    TOK_LBRACKET,                // [
    TOK_RBRACKET,                // ]
    TOK_LBRACE,                  // {
    TOK_RBRACE,                  // }
    TOK_LANGLE,                  // <
    TOK_RANGLE,                  // >
    TOK_COMMA,                   // ,
    TOK_SEMICOLON,               // ;
    TOK_COLON,                   // :
    TOK_DOT,                     // .
    TOK_EQUAL,                   // =
    TOK_PLUS,                    // +
    TOK_MINUS,                   // -
    TOK_STAR,                    // *
    TOK_SLASH,                   // /
    TOK_CARET,                   // ^

    // reserved single
    TOK_AT,                      // @   (lexed; any use is a parse error, D141)

    // compound brackets (D114)
    TOK_LBRACKET_LBRACKET,       // [[
    TOK_RBRACKET_RBRACKET,       // ]]
    TOK_LBRACKET_BAR,            // [|
    TOK_BAR_RBRACKET,            // |]
    TOK_LANGLE_BAR,              // <|
    TOK_BAR_RANGLE,              // |>
    TOK_LANGLE_LBRACKET,         // <[
    TOK_RBRACKET_RANGLE,         // ]>

    // arrows and range (D114)
    TOK_LANGLE_MINUS_RANGLE,     // <->
    TOK_LANGLE_MINUS,            // <-
    TOK_MINUS_RANGLE,            // ->
    TOK_DOT_DOT,                 // ..

    // comparisons (D123)
    TOK_LANGLE_EQUAL,            // <=
    TOK_RANGLE_EQUAL,            // >=
    TOK_EQUAL_EQUAL,             // ==
    TOK_BANG_EQUAL,              // !=

    // products and apply (D123, D130); each has a non-ASCII alias
    TOK_DOT_PRODUCT,             // \.  or  · (U+00B7, bytes C2 B7)
    TOK_CROSS_PRODUCT,           // \x  or  × (U+00D7, bytes C3 97)
    TOK_APPLY,                   // \>  or  ▷ (U+25B7, bytes E2 96 B7)

    TOK_KIND_COUNT               // the number of kinds; not a token
};

// Token flags (Specification.md §1.2). A line end is whitespace, so a token
// with LINE_BEFORE always has SPACE_BEFORE too. The first token in a file has
// both (D156).
enum TokenFlag
{
    SPACE_BEFORE = 0x0001,       // whitespace or a comment came before it
    LINE_BEFORE  = 0x0002,       // a line end came before it (or a block comment holding one)
};

struct Token
{
    TokenKind kind;
    uint16_t  flags;             // TokenFlag bits
    uint32_t  offset;            // byte offset of the first byte in the source buffer
    uint32_t  len;               // bytes in the token
    uint32_t  line;              // from 1
    uint32_t  col;               // from 1, counting bytes; a tab is one column
};

struct TokenArray
{
    Token   *data;
    uint32_t count;              // tokens in use
    uint32_t capacity;           // tokens allocated
};

// Allocate room for `capacity` tokens. The array starts with count 0.
void TokenArray_make( TokenArray *tokens, uint32_t capacity );

// Append one token, growing the allocation when it is full.
void TokenArray_append( TokenArray *tokens, Token token );

// Free the allocation and leave the array empty.
void TokenArray_free( TokenArray *tokens );

// The kind's name as `ombdc --tokens` prints it: "IDENT", "LANGLE_MINUS", ...
const char *TokenKind_name( TokenKind kind );

// A punctuator's ASCII spelling: "<-", "[[", "\.", ... The non-ASCII aliases
// give their ASCII twin's spelling. Words, literals and EOF give "".
const char *TokenKind_spelling( TokenKind kind );

// Write the `ombdc --tokens` dump of `tokens`, one line per token:
//
//     line:col KIND 'text' FLAGS
//
// `text` is the source buffer the tokens point into. FLAGS is two characters,
// 'S' or '-' for SPACE_BEFORE and then 'L' or '-' for LINE_BEFORE.
// Like snprintf: writes at most `size` bytes into `buffer` (ending with a NUL
// when there is room) and returns the size of the whole dump, NUL excluded.
// Call it with a NULL buffer to learn the size.
uint32_t TokenArray_format( const TokenArray *tokens, const char *text, char *buffer, uint32_t size );

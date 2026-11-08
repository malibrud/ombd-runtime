// lexer.h -- turning a source into tokens (Specification.md §1).

#pragma once
#include "token.h"
#include "file.h"

// The lexical errors of Specification.md §1.8, in its order.
enum LexErrorKind
{
    LEX_CONTROL_BYTE,            //  1. a control byte other than tab, \r or \n
    LEX_NON_ASCII,               //  2. a non-ASCII byte outside a comment, a string or a listed operator
    LEX_UNTERMINATED_COMMENT,    //  3. reported at the outermost /*
    LEX_STRAY_CLOSER,            //  4. a */ outside any comment, reported at the *
    LEX_UNTERMINATED_STRING,     //  5. reported at the opening "
    LEX_BACKSLASH_IN_STRING,     //  6. a backslash in a string, reported there
    LEX_BAD_BACKSLASH,           //  7. a backslash outside a string not followed by . x or >
    LEX_LONE_BANG,               //  8. a ! not starting != (a stray | is a parse error, D168)
    LEX_STRAY_PRIME,             //  9. a ' that does not end an identifier
    LEX_BAD_PUNCTUATION,         // 10. # $ % & ? ~ `

    LEX_ERROR_KIND_COUNT         // the number of kinds; not an error
};

// Where lexing stopped. Every error but an unterminated comment or string is
// reported at the byte that caused it.
struct LexError
{
    LexErrorKind kind;
    uint32_t     offset;         // byte offset in the source buffer
    uint32_t     line;           // from 1
    uint32_t     col;            // from 1, counting bytes
};

// Lex the whole source into `tokens`, ending with an EOF token.
// On success `error` is untouched. On the first error, `error` is filled,
// `tokens` is freed and false is returned.
bool tryLex( const Source *source, TokenArray *tokens, LexError *error );

// Print `path:line:col: error: message` to stderr.
void LexError_print( const Source *source, const LexError *error );

// The kind's name: "LEX_CONTROL_BYTE", ...
const char *LexErrorKind_name( LexErrorKind kind );

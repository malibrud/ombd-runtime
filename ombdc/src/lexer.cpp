// lexer.cpp -- the OMBD lexer (Specification.md §1).
//
// A flat, hand-written scanner. It needs no parser state (D114, U15): it
// skips whitespace and comments, recording them only as the next token's
// flags, then switches on the token's first byte and takes the longest token
// that matches (maximal munch).
//
// The source is NUL-terminated (file.h), so looking one byte ahead is always
// safe: a lookahead stops at the NUL, which matches nothing. A NUL before the
// end of the buffer is a byte of the file, and a control-byte error.

#include "lexer.h"
#include <stdio.h>
#include <string.h>

// The lexer's state. It lives on tryLex's stack, so lexing is re-entrant.
struct Lexer
{
    const uint8_t *text;         // the source, with a NUL at text[size]
    uint32_t       size;         // bytes in the source
    uint32_t       pos;          // offset of the next byte to read
    uint32_t       line;         // the line of text[pos], from 1
    uint32_t       lineStart;    // offset of the first byte of that line
    uint32_t       flags;        // TokenFlag bits gathered for the next token
    LexError      *error;        // filled by the first error
};

static bool trySkipSpaceAndComments( Lexer *lex );
static bool trySkipLineComment( Lexer *lex );
static bool trySkipBlockComment( Lexer *lex );
static bool tryScanToken( Lexer *lex, Token *token );
static void scanWord( Lexer *lex, Token *token );
static void scanNumber( Lexer *lex, Token *token );
static bool tryScanString( Lexer *lex, Token *token );

//////////////////////////////////////////////////////////////////////////////
// Public API
//////////////////////////////////////////////////////////////////////////////

bool tryLex( const Source *source, TokenArray *tokens, LexError *error )
{
    Lexer lex;
    lex.text      = (const uint8_t *)source->text;
    lex.size      = source->size;
    lex.pos       = 0;
    lex.line      = 1;
    lex.lineStart = 0;
    lex.flags     = SPACE_BEFORE | LINE_BEFORE;   // the start of a file is a line start (D156)
    lex.error     = error;

    // Skip a leading byte-order mark (§1.1). Columns count from after it;
    // offsets still count the file's bytes.
    if ( lex.size >= 3 && lex.text[0] == 0xEF && lex.text[1] == 0xBB && lex.text[2] == 0xBF )
    {
        lex.pos       = 3;
        lex.lineStart = 3;
    }

    // Model files average a little over four bytes per token.
    TokenArray_make( tokens, lex.size / 4 + 16 );

    for ( ;; )
    {
        if ( !trySkipSpaceAndComments( &lex ) ) goto FAIL;

        Token token;
        token.kind   = TOK_EOF;
        token.flags  = (uint16_t)lex.flags;
        token.offset = lex.pos;
        token.len    = 0;
        token.line   = lex.line;
        token.col    = lex.pos - lex.lineStart + 1;
        lex.flags    = 0;

        if ( !tryScanToken( &lex, &token ) ) goto FAIL;

        // No token spans a line end, so the length is just how far pos moved.
        token.len = lex.pos - token.offset;
        TokenArray_append( tokens, token );

        if ( token.kind == TOK_EOF ) return true;
    }

FAIL:
    TokenArray_free( tokens );
    return false;
}

void LexError_print( const Source *source, const LexError *error )
{
    uint8_t c = (uint8_t)source->text[ error->offset ];

    fprintf( stderr, "%s:%u:%u: error: ", source->path, error->line, error->col );
    switch ( error->kind )
    {
    case LEX_CONTROL_BYTE:
        fprintf( stderr, "control byte 0x%02X is not allowed; only tab, CR and LF are", c );
        break;
    case LEX_NON_ASCII:
        fprintf( stderr, "non-ASCII byte 0x%02X outside a comment or string; "
                         "only the operators \xC2\xB7 \xC3\x97 \xE2\x96\xB7 may be non-ASCII", c );
        break;
    case LEX_UNTERMINATED_COMMENT:
        fprintf( stderr, "unterminated block comment" );
        break;
    case LEX_STRAY_CLOSER:
        fprintf( stderr, "'*/' outside a comment" );
        break;
    case LEX_UNTERMINATED_STRING:
        fprintf( stderr, "unterminated string; a string ends on the line it starts" );
        break;
    case LEX_BACKSLASH_IN_STRING:
        fprintf( stderr, "'\\' inside a string; it is reserved for escapes" );
        break;
    case LEX_BAD_BACKSLASH:
        fprintf( stderr, "'\\' must be followed by '.', 'x' or '>'" );
        break;
    case LEX_LONE_BAR_OR_BANG:
        if ( c == '|' ) fprintf( stderr, "lone '|'; it must begin '|]' or '|>'" );
        else            fprintf( stderr, "lone '!'; it must begin '!='" );
        break;
    case LEX_STRAY_PRIME:
        fprintf( stderr, "''' must end an identifier, as in Y'" );
        break;
    case LEX_BAD_PUNCTUATION:
        fprintf( stderr, "'%c' is not an OMBD character", c );
        break;
    default:
        fprintf( stderr, "unknown lexical error" );
        break;
    }
    fprintf( stderr, "\n" );
}

static const char *lexErrorKindNames[] =
{
    "LEX_CONTROL_BYTE",
    "LEX_NON_ASCII",
    "LEX_UNTERMINATED_COMMENT",
    "LEX_STRAY_CLOSER",
    "LEX_UNTERMINATED_STRING",
    "LEX_BACKSLASH_IN_STRING",
    "LEX_BAD_BACKSLASH",
    "LEX_LONE_BAR_OR_BANG",
    "LEX_STRAY_PRIME",
    "LEX_BAD_PUNCTUATION",
};
static_assert( sizeof( lexErrorKindNames ) / sizeof( lexErrorKindNames[0] ) == LEX_ERROR_KIND_COUNT,
               "lexErrorKindNames must have one entry per LexErrorKind" );

const char *LexErrorKind_name( LexErrorKind kind )
{
    if ( kind < 0 || kind >= LEX_ERROR_KIND_COUNT ) return "?";
    return lexErrorKindNames[ kind ];
}

//////////////////////////////////////////////////////////////////////////////
// Helpers
//////////////////////////////////////////////////////////////////////////////

static bool isLetter( uint8_t c )
{
    return ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' );
}

static bool isDigit( uint8_t c )
{
    return c >= '0' && c <= '9';
}

// A control byte other than the whitespace bytes tab, \r and \n (§1.1).
// NUL counts: only the terminating NUL at text[size] is not part of the file.
static bool isControlByte( uint8_t c )
{
    if ( c == '\t' || c == '\r' || c == '\n' ) return false;
    return c < 0x20 || c == 0x7F;
}

// Record an error at a position and return false, for `return fail(...)`.
static bool fail( Lexer *lex, LexErrorKind kind, uint32_t offset, uint32_t line, uint32_t col )
{
    lex->error->kind   = kind;
    lex->error->offset = offset;
    lex->error->line   = line;
    lex->error->col    = col;
    return false;
}

// Record an error at byte `offset`, which is on the current line.
static bool failAt( Lexer *lex, LexErrorKind kind, uint32_t offset )
{
    return fail( lex, kind, offset, lex->line, offset - lex->lineStart + 1 );
}

// Step over the '\n' at pos and start the next line.
static void newLine( Lexer *lex )
{
    lex->pos++;
    lex->line++;
    lex->lineStart = lex->pos;
}

//////////////////////////////////////////////////////////////////////////////
// Whitespace and comments (§1.1, §1.3)
//////////////////////////////////////////////////////////////////////////////

// Skip everything up to the next token, gathering its flags.
static bool trySkipSpaceAndComments( Lexer *lex )
{
    const uint8_t *text = lex->text;
    for ( ;; )
    {
        switch ( text[ lex->pos ] )
        {
        case ' ':
        case '\t':
        case '\r':                               // a lone \r does not start a line
            lex->flags |= SPACE_BEFORE;
            lex->pos++;
            break;

        case '\n':
            lex->flags |= SPACE_BEFORE | LINE_BEFORE;
            newLine( lex );
            break;

        case '/':
            if ( text[ lex->pos + 1 ] == '/' )
            {
                if ( !trySkipLineComment( lex ) ) return false;
                break;
            }
            if ( text[ lex->pos + 1 ] == '*' )
            {
                if ( !trySkipBlockComment( lex ) ) return false;
                break;
            }
            return true;                         // a slash token

        default:
            return true;
        }
    }
}

// Skip `// ...` up to, but not including, the line end. Inside it, `/*`
// and `*/` have no effect.
static bool trySkipLineComment( Lexer *lex )
{
    const uint8_t *text = lex->text;

    lex->flags |= SPACE_BEFORE;
    lex->pos += 2;
    for ( ;; )
    {
        uint8_t c = text[ lex->pos ];
        if ( c == '\n' ) return true;
        if ( c == '\0' && lex->pos == lex->size ) return true;
        if ( isControlByte( c ) ) return failAt( lex, LEX_CONTROL_BYTE, lex->pos );
        lex->pos++;
    }
}

// Skip `/* ... */`. Block comments nest: each `/*` opens a level and each
// `*/` closes one. Nothing else inside is recognized, so `//` and `"` there
// have no effect. Markers pair left to right, so `/*/` opens and does not close.
static bool trySkipBlockComment( Lexer *lex )
{
    const uint8_t *text = lex->text;

    // An unterminated comment is reported at its outermost /*.
    uint32_t openOffset = lex->pos;
    uint32_t openLine   = lex->line;
    uint32_t openCol    = lex->pos - lex->lineStart + 1;

    lex->flags |= SPACE_BEFORE;
    lex->pos += 2;
    uint32_t depth = 1;
    while ( depth > 0 )
    {
        uint8_t c = text[ lex->pos ];
        if ( c == '/' && text[ lex->pos + 1 ] == '*' )
        {
            depth++;
            lex->pos += 2;
        }
        else if ( c == '*' && text[ lex->pos + 1 ] == '/' )
        {
            depth--;
            lex->pos += 2;
        }
        else if ( c == '\n' )
        {
            lex->flags |= LINE_BEFORE;
            newLine( lex );
        }
        else if ( c == '\0' && lex->pos == lex->size )
        {
            return fail( lex, LEX_UNTERMINATED_COMMENT, openOffset, openLine, openCol );
        }
        else if ( isControlByte( c ) )
        {
            return failAt( lex, LEX_CONTROL_BYTE, lex->pos );
        }
        else
        {
            lex->pos++;                          // any other byte, non-ASCII included
        }
    }
    return true;
}

//////////////////////////////////////////////////////////////////////////////
// Tokens
//////////////////////////////////////////////////////////////////////////////

// Take a punctuator of `count` bytes.
static bool take( Lexer *lex, Token *token, TokenKind kind, uint32_t count )
{
    token->kind = kind;
    lex->pos += count;
    return true;
}

// Scan the token starting at pos into `token`, whose position and flags are
// already set. Every case takes the longest token that matches.
static bool tryScanToken( Lexer *lex, Token *token )
{
    const uint8_t *t = lex->text + lex->pos;     // t[0] is the first byte, t[1] the next
    uint32_t pos = lex->pos;

    switch ( t[0] )
    {
    case '\0':
        if ( pos == lex->size ) return take( lex, token, TOK_EOF, 0 );
        return failAt( lex, LEX_CONTROL_BYTE, pos );

    // single
    case '(': return take( lex, token, TOK_LPAREN,    1 );
    case ')': return take( lex, token, TOK_RPAREN,    1 );
    case '{': return take( lex, token, TOK_LBRACE,    1 );
    case '}': return take( lex, token, TOK_RBRACE,    1 );
    case ',': return take( lex, token, TOK_COMMA,     1 );
    case ';': return take( lex, token, TOK_SEMICOLON, 1 );
    case ':': return take( lex, token, TOK_COLON,     1 );
    case '+': return take( lex, token, TOK_PLUS,      1 );
    case '^': return take( lex, token, TOK_CARET,     1 );
    case '@': return take( lex, token, TOK_AT,        1 );

    // Comments were skipped already, so this slash starts neither // nor /*.
    case '/': return take( lex, token, TOK_SLASH,     1 );

    case '[':                                    // [[  [|  [
        if ( t[1] == '[' ) return take( lex, token, TOK_LBRACKET_LBRACKET, 2 );
        if ( t[1] == '|' ) return take( lex, token, TOK_LBRACKET_BAR,      2 );
        return take( lex, token, TOK_LBRACKET, 1 );

    case ']':                                    // ]]  ]>  ]
        if ( t[1] == ']' ) return take( lex, token, TOK_RBRACKET_RBRACKET, 2 );
        if ( t[1] == '>' ) return take( lex, token, TOK_RBRACKET_RANGLE,   2 );
        return take( lex, token, TOK_RBRACKET, 1 );

    case '<':                                    // <->  <-  <|  <[  <=  <
        if ( t[1] == '-' )
        {
            if ( t[2] == '>' ) return take( lex, token, TOK_LANGLE_MINUS_RANGLE, 3 );
            return take( lex, token, TOK_LANGLE_MINUS, 2 );
        }
        if ( t[1] == '|' ) return take( lex, token, TOK_LANGLE_BAR,      2 );
        if ( t[1] == '[' ) return take( lex, token, TOK_LANGLE_LBRACKET, 2 );
        if ( t[1] == '=' ) return take( lex, token, TOK_LANGLE_EQUAL,    2 );
        return take( lex, token, TOK_LANGLE, 1 );

    case '>':                                    // >=  >
        if ( t[1] == '=' ) return take( lex, token, TOK_RANGLE_EQUAL, 2 );
        return take( lex, token, TOK_RANGLE, 1 );

    case '|':                                    // |]  |>
        if ( t[1] == ']' ) return take( lex, token, TOK_BAR_RBRACKET, 2 );
        if ( t[1] == '>' ) return take( lex, token, TOK_BAR_RANGLE,   2 );
        return failAt( lex, LEX_LONE_BAR_OR_BANG, pos );

    case '!':                                    // !=
        if ( t[1] == '=' ) return take( lex, token, TOK_BANG_EQUAL, 2 );
        return failAt( lex, LEX_LONE_BAR_OR_BANG, pos );

    case '-':                                    // ->  -
        if ( t[1] == '>' ) return take( lex, token, TOK_MINUS_RANGLE, 2 );
        return take( lex, token, TOK_MINUS, 1 );

    case '.':                                    // ..  .
        if ( t[1] == '.' ) return take( lex, token, TOK_DOT_DOT, 2 );
        return take( lex, token, TOK_DOT, 1 );

    case '=':                                    // ==  =
        if ( t[1] == '=' ) return take( lex, token, TOK_EQUAL_EQUAL, 2 );
        return take( lex, token, TOK_EQUAL, 1 );

    case '*':                                    // *, but never */
        if ( t[1] == '/' ) return failAt( lex, LEX_STRAY_CLOSER, pos );
        return take( lex, token, TOK_STAR, 1 );

    case '\\':                                   // \.  \x  \>
        if ( t[1] == '.' ) return take( lex, token, TOK_DOT_PRODUCT,   2 );
        if ( t[1] == 'x' ) return take( lex, token, TOK_CROSS_PRODUCT, 2 );
        if ( t[1] == '>' ) return take( lex, token, TOK_APPLY,         2 );
        return failAt( lex, LEX_BAD_BACKSLASH, pos );

    // The listed non-ASCII operators (§1.7), matched byte for byte.
    case 0xC2:                                   // · U+00B7
        if ( t[1] == 0xB7 ) return take( lex, token, TOK_DOT_PRODUCT, 2 );
        return failAt( lex, LEX_NON_ASCII, pos );
    case 0xC3:                                   // × U+00D7
        if ( t[1] == 0x97 ) return take( lex, token, TOK_CROSS_PRODUCT, 2 );
        return failAt( lex, LEX_NON_ASCII, pos );
    case 0xE2:                                   // ▷ U+25B7
        if ( t[1] == 0x96 && t[2] == 0xB7 ) return take( lex, token, TOK_APPLY, 3 );
        return failAt( lex, LEX_NON_ASCII, pos );

    case '"':
        return tryScanString( lex, token );

    // Word scanning takes every prime that ends an identifier, so a prime
    // seen here ends nothing.
    case '\'':
        return failAt( lex, LEX_STRAY_PRIME, pos );

    case '#': case '$': case '%': case '&': case '?': case '~': case '`':
        return failAt( lex, LEX_BAD_PUNCTUATION, pos );

    default:
        if ( isLetter( t[0] ) || t[0] == '_' )
        {
            scanWord( lex, token );
            return true;
        }
        if ( isDigit( t[0] ) )
        {
            scanNumber( lex, token );
            return true;
        }
        if ( t[0] >= 0x80 ) return failAt( lex, LEX_NON_ASCII, pos );

        // Every printable ASCII byte has a case above, and whitespace never
        // reaches here, so only a control byte is left.
        return failAt( lex, LEX_CONTROL_BYTE, pos );
    }
}

// The five reserved words (§1.4, D121).
static const char *keywords[] = { "for", "and", "or", "not", "mod" };

static bool isKeyword( const uint8_t *word, uint32_t count )
{
    for ( uint32_t i = 0 ; i < sizeof( keywords ) / sizeof( keywords[0] ) ; i++ )
    {
        if ( strlen( keywords[i] ) == count && memcmp( keywords[i], word, count ) == 0 ) return true;
    }
    return false;
}

// IDENT = [A-Za-z_] [A-Za-z0-9_]* "'"*   (§1.4, D120)
// A word spelled exactly like a reserved word is a KEYWORD.
static void scanWord( Lexer *lex, Token *token )
{
    const uint8_t *text = lex->text;
    uint32_t pos = lex->pos + 1;

    while ( isLetter( text[pos] ) || isDigit( text[pos] ) || text[pos] == '_' ) pos++;
    while ( text[pos] == '\'' ) pos++;           // primes belong to the word

    lex->pos    = pos;
    token->kind = isKeyword( text + token->offset, pos - token->offset ) ? TOK_KEYWORD : TOK_IDENT;
}

// NUMBER = DIGITS [ "." DIGITS ] [ ("e" | "E") ["+" | "-"] DIGITS ]   (§1.5, D117)
// Each optional part joins only when it is complete. Otherwise the number
// ends where the incomplete part begins: `0..n` is 0, .., n and `2e` is 2, e.
static void scanNumber( Lexer *lex, Token *token )
{
    const uint8_t *text = lex->text;
    uint32_t pos = lex->pos;

    while ( isDigit( text[pos] ) ) pos++;

    // The fraction: a point joins only if a digit follows it.
    if ( text[pos] == '.' && isDigit( text[pos + 1] ) )
    {
        pos++;
        while ( isDigit( text[pos] ) ) pos++;
    }

    // The exponent: an e joins only if a digit follows it and its optional sign.
    if ( text[pos] == 'e' || text[pos] == 'E' )
    {
        uint32_t digits = pos + 1;
        if ( text[digits] == '+' || text[digits] == '-' ) digits++;
        if ( isDigit( text[digits] ) )
        {
            pos = digits;
            while ( isDigit( text[pos] ) ) pos++;
        }
    }

    lex->pos    = pos;
    token->kind = TOK_NUMBER;
}

// STRING = '"' { any byte except '"', '\', 0x0A } '"'   (§1.6, D122)
// The token spans both quotes; its value is the bytes between them.
static bool tryScanString( Lexer *lex, Token *token )
{
    const uint8_t *text = lex->text;
    uint32_t pos = lex->pos + 1;                 // after the opening quote

    for ( ;; )
    {
        uint8_t c = text[pos];
        if ( c == '"' ) break;
        if ( c == '\\' ) return failAt( lex, LEX_BACKSLASH_IN_STRING, pos );

        // A line end or the end of the file before the closing quote is
        // reported at the opening quote.
        if ( c == '\n' || ( c == '\0' && pos == lex->size ) )
        {
            return fail( lex, LEX_UNTERMINATED_STRING, token->offset, token->line, token->col );
        }
        if ( isControlByte( c ) ) return failAt( lex, LEX_CONTROL_BYTE, pos );
        pos++;                                   // any other byte, non-ASCII included
    }

    lex->pos    = pos + 1;                       // after the closing quote
    token->kind = TOK_STRING;
    return true;
}

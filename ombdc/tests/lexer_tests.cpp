// lexer_tests.cpp -- table-driven tests of the lexer.
//
// Four tables:
//   appendixCCases  every token-stream vector of Specification.md Appendix C,
//                   written in the Appendix's own notation
//   moreTokenCases  further token streams in the same notation
//   dumpCases       positions and flags, as `ombdc --tokens` prints them
//   errorCases      every lexical error of §1.8, with its kind and position
// and the golden dumps of two example models.
//
// The source is compiled with /utf-8, so non-ASCII literals below are UTF-8.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/file.h"
#include "../src/lexer.h"
#include "unit_test.h"

//////////////////////////////////////////////////////////////////////////////
// Helpers
//////////////////////////////////////////////////////////////////////////////

// Copy `size` bytes for display, showing control bytes as \xNN.
static void test_escape( const char *in, uint32_t size, char *out, uint32_t outSize )
{
    uint32_t used = 0;
    for ( uint32_t i = 0 ; i < size && used + 5 < outSize ; i++ )
    {
        uint8_t c = (uint8_t)in[i];
        if ( c < 0x20 || c == 0x7F ) used += (uint32_t)snprintf( out + used, outSize - used, "\\x%02X", c );
        else                         out[used++] = (char)c;
    }
    out[used] = '\0';
}

// Write the tokens in Appendix C's notation: one space between tokens, `·`
// before a token with SPACE_BEFORE (except the first, which the Appendix
// leaves unmarked), words and literals as KIND(text), a string as
// STRING(value), punctuators by their ASCII spelling. EOF is left out.
static void test_formatAppendixC( const TokenArray *tokens, const char *text, char *out, uint32_t outSize )
{
    uint32_t used = 0;
    out[0] = '\0';
    for ( uint32_t i = 0 ; i + 1 < tokens->count ; i++ )
    {
        const Token *token  = &tokens->data[i];
        const char  *gap    = ( i == 0 ) ? "" : " ";
        const char  *mark   = ( i > 0 && ( token->flags & SPACE_BEFORE ) ) ? "·" : "";
        const char  *spelling = text + token->offset;
        int          len    = (int)token->len;
        int          n      = 0;

        switch ( token->kind )
        {
        case TOK_IDENT:
        case TOK_KEYWORD:
        case TOK_NUMBER:
            n = snprintf( out + used, outSize - used, "%s%s%s(%.*s)", gap, mark,
                          TokenKind_name( token->kind ), len, spelling );
            break;
        case TOK_STRING:
            n = snprintf( out + used, outSize - used, "%s%sSTRING(%.*s)", gap, mark, len - 2, spelling + 1 );
            break;
        default:
            n = snprintf( out + used, outSize - used, "%s%s%s", gap, mark, TokenKind_spelling( token->kind ) );
            break;
        }
        if ( n < 0 || (uint32_t)n >= outSize - used ) return;   // truncated; the comparison will fail
        used += (uint32_t)n;
    }
}

// Check the shape every successful lex must have: exactly one EOF, last, and
// a first token with both flags (D156).
static bool test_hasValidShape( const TokenArray *tokens )
{
    if ( tokens->count == 0 ) return false;
    for ( uint32_t i = 0 ; i + 1 < tokens->count ; i++ )
    {
        if ( tokens->data[i].kind == TOK_EOF ) return false;
    }
    if ( tokens->data[ tokens->count - 1 ].kind != TOK_EOF ) return false;
    return tokens->data[0].flags == ( SPACE_BEFORE | LINE_BEFORE );
}

// Lex `size` bytes of `input`; on success return a malloc'd `--tokens` dump.
static char *test_dump( const char *input, uint32_t size )
{
    Source     source;
    TokenArray tokens;
    LexError   error;

    Source_make( &source, "test", input, size );
    if ( !tryLex( &source, &tokens, &error ) )
    {
        Source_free( &source );
        return NULL;
    }
    uint32_t dumpSize = TokenArray_format( &tokens, source.text, NULL, 0 );
    char    *dump     = (char *)malloc( (size_t)dumpSize + 1 );
    TokenArray_format( &tokens, source.text, dump, dumpSize + 1 );

    TokenArray_free( &tokens );
    Source_free( &source );
    return dump;
}

//////////////////////////////////////////////////////////////////////////////
// Token streams in Appendix C's notation
//////////////////////////////////////////////////////////////////////////////

struct TokenCase
{
    const char *input;
    const char *expected;        // Appendix C notation; "" for no tokens
};

// Specification.md Appendix C, row by row. The rows that are lexical errors
// are in errorCases below. Rows with two inputs are two cases here.
static TokenCase appendixCCases[] =
{
    { "x<-3",                 "IDENT(x) <- NUMBER(3)" },
    { "x < -3",               "IDENT(x) ·< ·- NUMBER(3)" },
    { "x<=3",                 "IDENT(x) <= NUMBER(3)" },
    { "<-1>",                 "<- NUMBER(1) >" },
    { "< -1>",                "< ·- NUMBER(1) >" },
    { "0..n-1",               "NUMBER(0) .. IDENT(n) - NUMBER(1)" },
    { "0.0 .. 0.25",          "NUMBER(0.0) ·.. ·NUMBER(0.25)" },
    { "1e6 8.0e-4 4.0E+6",    "NUMBER(1e6) ·NUMBER(8.0e-4) ·NUMBER(4.0E+6)" },
    { "5.x",                  "NUMBER(5) . IDENT(x)" },
    { "2e",                   "NUMBER(2) IDENT(e)" },
    { "10kg",                 "NUMBER(10) IDENT(kg)" },
    { "kg-m2",                "IDENT(kg) - IDENT(m2)" },
    { "rpm/N^0.5-m^0.5",      "IDENT(rpm) / IDENT(N) ^ NUMBER(0.5) - IDENT(m) ^ NUMBER(0.5)" },
    { "Y''",                  "IDENT(Y'')" },
    { "[a, b[i]]",            "[ IDENT(a) , ·IDENT(b) [ IDENT(i) ]]" },
    { "[[[",                  "[[ [" },
    { "<[ p, <1> ]>",         "<[ ·IDENT(p) , ·< NUMBER(1) > ·]>" },
    { "a <-> b",              "IDENT(a) ·<-> ·IDENT(b)" },
    { "u \\. v",              "IDENT(u) ·\\. ·IDENT(v)" },
    { "u · v",                "IDENT(u) ·\\. ·IDENT(v)" },
    { "F ▷ P",                "IDENT(F) ·\\> ·IDENT(P)" },
    { "F \\> P",              "IDENT(F) ·\\> ·IDENT(P)" },
    { "a . b",                "IDENT(a) ·. ·IDENT(b)" },
    { "diag(1 2 3)",          "IDENT(diag) ( NUMBER(1) ·NUMBER(2) ·NUMBER(3) )" },
    { "(i -1)",               "( IDENT(i) ·- NUMBER(1) )" },
    { "/* a /* b */ c */ d",  "IDENT(d)" },
    { "/* // */ x",           "IDENT(x)" },
    { "\"OZX\"",              "STRING(OZX)" },
    { "// μ ≈ p",             "" },
    { "for x in",             "KEYWORD(for) ·IDENT(x) ·IDENT(in)" },
    { "true",                 "IDENT(true)" },
};

// More streams, covering every punctuator and the edges of each rule.
static TokenCase moreTokenCases[] =
{
    // every punctuator of §1.7, spaced
    { "( ) [ ] { } < > , ; : . = + - * / ^ @",
      "( ·) ·[ ·] ·{ ·} ·< ·> ·, ·; ·: ·. ·= ·+ ·- ·* ·/ ·^ ·@" },
    { "[[ ]] [| |] <| |> <[ ]>",       "[[ ·]] ·[| ·|] ·<| ·|> ·<[ ·]>" },
    { "<-> <- -> ..",                  "<-> ·<- ·-> ·.." },
    { "<= >= == !=",                   "<= ·>= ·== ·!=" },
    { "\\. \\x \\> · × ▷",             "\\. ·\\x ·\\> ·\\. ·\\x ·\\>" },

    // maximal munch, unspaced
    { "a==b",                          "IDENT(a) == IDENT(b)" },
    { "a=b",                           "IDENT(a) = IDENT(b)" },
    { "x=-3",                          "IDENT(x) = - NUMBER(3)" },
    { "a->b",                          "IDENT(a) -> IDENT(b)" },
    { "a<->b",                         "IDENT(a) <-> IDENT(b)" },
    { "a!=b",                          "IDENT(a) != IDENT(b)" },
    { "]]]",                           "]] ]" },
    { "]]>",                           "]] >" },
    { "[|]",                           "[| ]" },
    { "|]]",                           "|] ]" },
    { "<|>",                           "<| >" },
    { "|>=",                           "|> =" },
    { "<<",                            "< <" },
    { "...",                           ".. ." },
    { "u\\xv",                         "IDENT(u) \\x IDENT(v)" },
    { "u×v",                           "IDENT(u) \\x IDENT(v)" },
    { "F▷P",                           "IDENT(F) \\> IDENT(P)" },
    { "F\\>P",                         "IDENT(F) \\> IDENT(P)" },
    { "a/b",                           "IDENT(a) / IDENT(b)" },
    { "a*b",                           "IDENT(a) * IDENT(b)" },
    { "a**b",                          "IDENT(a) * * IDENT(b)" },

    // words (§1.4)
    { "for and or not mod",            "KEYWORD(for) ·KEYWORD(and) ·KEYWORD(or) ·KEYWORD(not) ·KEYWORD(mod)" },
    { "form android For _for for'",    "IDENT(form) ·IDENT(android) ·IDENT(For) ·IDENT(_for) ·IDENT(for')" },
    { "_ _a a_1 bRod brod",            "IDENT(_) ·IDENT(_a) ·IDENT(a_1) ·IDENT(bRod) ·IDENT(brod)" },
    { "<|Z: 6, Y': 3, X'': 6|>",
      "<| IDENT(Z) : ·NUMBER(6) , ·IDENT(Y') : ·NUMBER(3) , ·IDENT(X'') : ·NUMBER(6) |>" },
    { "system namespace world time open case default",
      "IDENT(system) ·IDENT(namespace) ·IDENT(world) ·IDENT(time) ·IDENT(open) ·IDENT(case) ·IDENT(default)" },

    // numbers (§1.5)
    { "0 007 2500 0.833",              "NUMBER(0) ·NUMBER(007) ·NUMBER(2500) ·NUMBER(0.833)" },
    { "1.5.2",                         "NUMBER(1.5) . NUMBER(2)" },
    { ".5",                            ". NUMBER(5)" },
    { "5.",                            "NUMBER(5) ." },
    { "0...1",                         "NUMBER(0) .. . NUMBER(1)" },
    { "2e+",                           "NUMBER(2) IDENT(e) +" },
    { "2E-x",                          "NUMBER(2) IDENT(E) - IDENT(x)" },
    { "1e+-2",                         "NUMBER(1) IDENT(e) + - NUMBER(2)" },
    { "2.e5",                          "NUMBER(2) . IDENT(e5)" },
    { "1e5.5",                         "NUMBER(1e5) . NUMBER(5)" },
    { "1_000",                         "NUMBER(1) IDENT(_000)" },
    { "-3",                            "- NUMBER(3)" },
    { "4.0e6kg",                       "NUMBER(4.0e6) IDENT(kg)" },

    // strings (§1.6)
    { "\"\"",                          "STRING()" },
    { "\"ZX\" \"OZX\"",                "STRING(ZX) ·STRING(OZX)" },
    { "\"a·b μ\"",                     "STRING(a·b μ)" },
    { "\"/* not a comment */\"",       "STRING(/* not a comment */)" },
    { "\"// nor this\"",               "STRING(// nor this)" },
    { "\"a\tb\"",                      "STRING(a\tb)" },
    { "\"x\"y",                        "STRING(x) IDENT(y)" },

    // comments (§1.3)
    { "a/*c*/b",                       "IDENT(a) ·IDENT(b)" },
    { "a//c\nb",                       "IDENT(a) ·IDENT(b)" },
    { "a // c */ \nb",                 "IDENT(a) ·IDENT(b)" },
    { "/* \" */ x",                    "IDENT(x)" },
    { "/* /*/ */ */ x",                "IDENT(x)" },
    { "/** doc */ x /// line",         "IDENT(x)" },
    { "/* a */* b",                    "* ·IDENT(b)" },
    { "/* μ ≈ √ — */ x",               "IDENT(x)" },
    { "/**/x",                         "IDENT(x)" },
    { "/*/**/*/x",                     "IDENT(x)" },
};

static void test_tokenCases( const char *moduleName, TokenCase *cases, uint32_t count )
{
    ut_beginModule( moduleName );
    for ( uint32_t i = 0 ; i < count ; i++ )
    {
        TokenCase *kase = &cases[i];
        uint32_t   size = (uint32_t)strlen( kase->input );

        Source     source;
        TokenArray tokens;
        LexError   error;
        char       got[1024];
        char       shown[256];

        Source_make( &source, "test", kase->input, size );
        bool lexed = tryLex( &source, &tokens, &error );
        bool pass  = lexed;
        got[0] = '\0';
        if ( lexed )
        {
            test_formatAppendixC( &tokens, source.text, got, sizeof( got ) );
            pass = test_hasValidShape( &tokens ) && strcmp( got, kase->expected ) == 0;
            TokenArray_free( &tokens );
        }

        test_escape( kase->input, size, shown, sizeof( shown ) );
        ut_reportTestResult( pass, "%s", shown );
        if ( !lexed )
        {
            printf( "        lexical error %s at %u:%u\n", LexErrorKind_name( error.kind ), error.line, error.col );
        }
        else if ( !pass )
        {
            printf( "        expected: %s\n", kase->expected );
            printf( "        got:      %s\n", got );
        }
        Source_free( &source );
    }
    ut_endModule();
}

//////////////////////////////////////////////////////////////////////////////
// Positions and flags, as `ombdc --tokens` prints them
//////////////////////////////////////////////////////////////////////////////

struct DumpCase
{
    const char *desc;
    const char *input;
    const char *expected;        // the --tokens dump
};

static DumpCase dumpCases[] =
{
    { "empty file: just EOF, with both flags",
      "",
      "1:1 EOF '' SL\n" },
    { "one token, then EOF with no space",
      "x",
      "1:1 IDENT 'x' SL\n"
      "1:2 EOF '' --\n" },
    { "whitespace only",
      "  \n ",
      "2:2 EOF '' SL\n" },
    { "first token after leading space still has both flags",
      "\tx",
      "1:2 IDENT 'x' SL\n"
      "1:3 EOF '' --\n" },
    { "LF line ends",
      "a\nb\n",
      "1:1 IDENT 'a' SL\n"
      "2:1 IDENT 'b' SL\n"
      "3:1 EOF '' SL\n" },
    { "CRLF line ends lex like LF",
      "a\r\nb\r\n",
      "1:1 IDENT 'a' SL\n"
      "2:1 IDENT 'b' SL\n"
      "3:1 EOF '' SL\n" },
    { "a lone CR is whitespace, not a line end",
      "a\rb",
      "1:1 IDENT 'a' SL\n"
      "1:3 IDENT 'b' S-\n"
      "1:4 EOF '' --\n" },
    { "a tab is one column",
      "a\t\tb",
      "1:1 IDENT 'a' SL\n"
      "1:4 IDENT 'b' S-\n"
      "1:5 EOF '' --\n" },
    { "line comment at EOF with no newline",
      "x // end",
      "1:1 IDENT 'x' SL\n"
      "1:9 EOF '' S-\n" },
    { "line comment ending in a newline",
      "x // end\n",
      "1:1 IDENT 'x' SL\n"
      "2:1 EOF '' SL\n" },
    { "comment-only file",
      "// only a comment",
      "1:18 EOF '' SL\n" },
    { "block comment on one line sets SPACE only",
      "a /* c */b",
      "1:1 IDENT 'a' SL\n"
      "1:10 IDENT 'b' S-\n"
      "1:11 EOF '' --\n" },
    { "block comment holding a line end sets LINE",
      "a/*\n*/b",
      "1:1 IDENT 'a' SL\n"
      "2:3 IDENT 'b' SL\n"
      "2:4 EOF '' --\n" },
    { "block comment at EOF",
      "a /* c */",
      "1:1 IDENT 'a' SL\n"
      "1:10 EOF '' S-\n" },
    { "tight tokens have no flags",
      "kg-m2",
      "1:1 IDENT 'kg' SL\n"
      "1:3 MINUS '-' --\n"
      "1:4 IDENT 'm2' --\n"
      "1:6 EOF '' --\n" },
    { "multi-byte operators: columns count bytes",
      "a·b×c▷d",
      "1:1 IDENT 'a' SL\n"
      "1:2 DOT_PRODUCT '·' --\n"
      "1:4 IDENT 'b' --\n"
      "1:5 CROSS_PRODUCT '×' --\n"
      "1:7 IDENT 'c' --\n"
      "1:8 APPLY '▷' --\n"
      "1:11 IDENT 'd' --\n"
      "1:12 EOF '' --\n" },
    { "a string token spans its quotes",
      "s: \"OZX\";",
      "1:1 IDENT 's' SL\n"
      "1:2 COLON ':' --\n"
      "1:4 STRING '\"OZX\"' S-\n"
      "1:9 SEMICOLON ';' --\n"
      "1:10 EOF '' --\n" },
    { "non-ASCII in a comment counts bytes for columns",
      "/* μ */ x",
      "1:10 IDENT 'x' SL\n"
      "1:11 EOF '' --\n" },
    { "BOM skipped: the first token is at column 1",
      "\xEF\xBB\xBFx",
      "1:1 IDENT 'x' SL\n"
      "1:2 EOF '' --\n" },
    { "BOM-only file",
      "\xEF\xBB\xBF",
      "1:1 EOF '' SL\n" },
};

static void test_dumpCases( void )
{
    ut_beginModule( "Positions and flags" );
    for ( uint32_t i = 0 ; i < sizeof( dumpCases ) / sizeof( dumpCases[0] ) ; i++ )
    {
        DumpCase *kase = &dumpCases[i];
        char     *got  = test_dump( kase->input, (uint32_t)strlen( kase->input ) );
        bool      pass = got != NULL && strcmp( got, kase->expected ) == 0;

        ut_reportTestResult( pass, "%s", kase->desc );
        if ( !pass )
        {
            printf( "        expected:\n%s", kase->expected );
            printf( "        got:\n%s", got ? got : "(lexical error)\n" );
        }
        free( got );
    }

    // CRLF and LF give the same tokens: the same dump, offsets aside.
    {
        const char *lf   = "gravity: [0, 0, -9.81] m/s2;\n// note\nsystem p {\n  /* a\n  b */ x = 1;\n}\n";
        const char *crlf = "gravity: [0, 0, -9.81] m/s2;\r\n// note\r\nsystem p {\r\n  /* a\r\n  b */ x = 1;\r\n}\r\n";
        char *a = test_dump( lf,   (uint32_t)strlen( lf ) );
        char *b = test_dump( crlf, (uint32_t)strlen( crlf ) );
        ut_reportTestResult( a != NULL && b != NULL && strcmp( a, b ) == 0, "a CRLF file dumps the same as its LF twin" );
        free( a );
        free( b );
    }

    // A BOM is skipped, but offsets still count the file's bytes.
    {
        Source     source;
        TokenArray tokens;
        LexError   error;
        Source_make( &source, "test", "\xEF\xBB\xBFx", 4 );
        bool pass = tryLex( &source, &tokens, &error );
        if ( pass )
        {
            pass = tokens.data[0].offset == 3 && tokens.data[0].len == 1 && tokens.data[1].offset == 4;
            TokenArray_free( &tokens );
        }
        ut_reportTestResult( pass, "after a BOM, offsets still count the BOM's bytes" );
        Source_free( &source );
    }

    // Offsets and lengths are views into the source.
    {
        Source     source;
        TokenArray tokens = {};
        LexError   error;
        const char *text = "ab <-> \"cd\"";
        Source_make( &source, "test", text, (uint32_t)strlen( text ) );
        bool pass = tryLex( &source, &tokens, &error ) && tokens.count == 4;
        if ( pass )
        {
            pass = tokens.data[0].offset == 0 && tokens.data[0].len == 2
                && tokens.data[1].offset == 3 && tokens.data[1].len == 3
                && tokens.data[2].offset == 7 && tokens.data[2].len == 4
                && tokens.data[3].offset == 11 && tokens.data[3].len == 0;
        }
        if ( tokens.data != NULL ) TokenArray_free( &tokens );
        ut_reportTestResult( pass, "token offsets and lengths" );
        Source_free( &source );
    }
    ut_endModule();
}

//////////////////////////////////////////////////////////////////////////////
// Lexical errors (§1.8)
//////////////////////////////////////////////////////////////////////////////

struct ErrorCase
{
    const char  *input;
    uint32_t     size;           // 0: strlen(input); set it for inputs holding a NUL
    LexErrorKind kind;
    uint32_t     line;
    uint32_t     col;
};

static ErrorCase errorCases[] =
{
    // Appendix C's error rows
    { "\\X",                  0, LEX_BAD_BACKSLASH,        1, 1 },
    { "*/",                   0, LEX_STRAY_CLOSER,         1, 1 },
    { "\"a\\b\"",             0, LEX_BACKSLASH_IN_STRING,  1, 3 },
    { "#",                    0, LEX_BAD_PUNCTUATION,      1, 1 },
    { "μ = 1;",               0, LEX_NON_ASCII,            1, 1 },

    // 1. control bytes, anywhere
    { "a\x01",                0, LEX_CONTROL_BYTE,         1, 2 },
    { "\x7F",                 0, LEX_CONTROL_BYTE,         1, 1 },
    { "a\0b",                 3, LEX_CONTROL_BYTE,         1, 2 },
    { "\0",                   1, LEX_CONTROL_BYTE,         1, 1 },
    { "a\fb",                 0, LEX_CONTROL_BYTE,         1, 2 },
    { "a\vb",                 0, LEX_CONTROL_BYTE,         1, 2 },
    { "// a\x02",             0, LEX_CONTROL_BYTE,         1, 5 },
    { "// a\0",               5, LEX_CONTROL_BYTE,         1, 5 },
    { "/*\n\x03*/",           0, LEX_CONTROL_BYTE,         2, 1 },
    { "\"a\x04\"",            0, LEX_CONTROL_BYTE,         1, 3 },
    { "\"a\x7F\"",            0, LEX_CONTROL_BYTE,         1, 3 },

    // 2. non-ASCII outside a comment or string
    { "x\n  μ",               0, LEX_NON_ASCII,            2, 3 },
    { "θ0",                   0, LEX_NON_ASCII,            1, 1 },
    { "a\xC2\x41",            0, LEX_NON_ASCII,            1, 2 },
    { "a\xC2",                0, LEX_NON_ASCII,            1, 2 },
    { "a\xC3\xB7",            0, LEX_NON_ASCII,            1, 2 },
    { "a\xE2\x96\x41",        0, LEX_NON_ASCII,            1, 2 },
    { "a\xE2\x96",            0, LEX_NON_ASCII,            1, 2 },
    { "≈",                    0, LEX_NON_ASCII,            1, 1 },
    { "x\xEF\xBB\xBF",        0, LEX_NON_ASCII,            1, 2 },
    { "\xB7",                 0, LEX_NON_ASCII,            1, 1 },

    // 3. unterminated block comment, at its outermost /*
    { "/* never closed",      0, LEX_UNTERMINATED_COMMENT, 1, 1 },
    { "x\n  /* a /* b */ c",  0, LEX_UNTERMINATED_COMMENT, 2, 3 },
    { "/* a\n /* b */\n",     0, LEX_UNTERMINATED_COMMENT, 1, 1 },
    { "/*/",                  0, LEX_UNTERMINATED_COMMENT, 1, 1 },
    { "/*",                   0, LEX_UNTERMINATED_COMMENT, 1, 1 },

    // 4. stray closer, at its *
    { "a */",                 0, LEX_STRAY_CLOSER,         1, 3 },
    { "a*/b",                 0, LEX_STRAY_CLOSER,         1, 2 },
    { "/* a */ */",           0, LEX_STRAY_CLOSER,         1, 9 },
    { "/* a */* b */",        0, LEX_STRAY_CLOSER,         1, 12 },

    // 5. unterminated string, at its opening quote
    { "\"abc\nx\"",           0, LEX_UNTERMINATED_STRING,  1, 1 },
    { "x = \"abc",            0, LEX_UNTERMINATED_STRING,  1, 5 },
    { "a\r\n  \"b\r\n",       0, LEX_UNTERMINATED_STRING,  2, 3 },
    { "\"",                   0, LEX_UNTERMINATED_STRING,  1, 1 },

    // 6. a backslash in a string, at the backslash (before any later error)
    { "\"a\\",                0, LEX_BACKSLASH_IN_STRING,  1, 3 },
    { "\"C:\\path\"",         0, LEX_BACKSLASH_IN_STRING,  1, 4 },

    // 7. a backslash outside a string not followed by . x >
    { "\\",                   0, LEX_BAD_BACKSLASH,        1, 1 },
    { "u \\X v",              0, LEX_BAD_BACKSLASH,        1, 3 },
    { "\\\n",                 0, LEX_BAD_BACKSLASH,        1, 1 },
    { "\\ .",                 0, LEX_BAD_BACKSLASH,        1, 1 },

    // 8. lone | or !
    { "|",                    0, LEX_LONE_BAR_OR_BANG,     1, 1 },
    { "a | b",                0, LEX_LONE_BAR_OR_BANG,     1, 3 },
    { "a || b",               0, LEX_LONE_BAR_OR_BANG,     1, 3 },
    { "!",                    0, LEX_LONE_BAR_OR_BANG,     1, 1 },
    { "!a",                   0, LEX_LONE_BAR_OR_BANG,     1, 1 },
    { "a ! = b",              0, LEX_LONE_BAR_OR_BANG,     1, 3 },

    // 9. a prime that does not end an identifier
    { "'a",                   0, LEX_STRAY_PRIME,          1, 1 },
    { "Y' '",                 0, LEX_STRAY_PRIME,          1, 4 },
    { "5'",                   0, LEX_STRAY_PRIME,          1, 2 },
    { "\"a\"'",               0, LEX_STRAY_PRIME,          1, 4 },

    // 10. other ASCII punctuation
    { "$",                    0, LEX_BAD_PUNCTUATION,      1, 1 },
    { "a % b",                0, LEX_BAD_PUNCTUATION,      1, 3 },
    { "&",                    0, LEX_BAD_PUNCTUATION,      1, 1 },
    { "a ? b",                0, LEX_BAD_PUNCTUATION,      1, 3 },
    { "~",                    0, LEX_BAD_PUNCTUATION,      1, 1 },
    { "`",                    0, LEX_BAD_PUNCTUATION,      1, 1 },
    { "x\n\n   #include",     0, LEX_BAD_PUNCTUATION,      3, 4 },
};

static void test_errorCases( void )
{
    ut_beginModule( "Lexical errors" );
    for ( uint32_t i = 0 ; i < sizeof( errorCases ) / sizeof( errorCases[0] ) ; i++ )
    {
        ErrorCase *kase = &errorCases[i];
        uint32_t   size = kase->size ? kase->size : (uint32_t)strlen( kase->input );

        Source     source;
        TokenArray tokens;
        LexError   error;
        char       shown[256];

        Source_make( &source, "test", kase->input, size );
        bool lexed = tryLex( &source, &tokens, &error );
        bool pass  = !lexed
                  && error.kind == kase->kind
                  && error.line == kase->line
                  && error.col  == kase->col
                  && tokens.data == NULL && tokens.count == 0;

        test_escape( kase->input, size, shown, sizeof( shown ) );
        ut_reportTestResult( pass, "%-22s %s at %u:%u", shown, LexErrorKind_name( kase->kind ), kase->line, kase->col );
        if ( lexed )
        {
            printf( "        lexed without error\n" );
            TokenArray_free( &tokens );
        }
        else if ( !pass )
        {
            printf( "        got %s at %u:%u\n", LexErrorKind_name( error.kind ), error.line, error.col );
        }
        Source_free( &source );
    }
    ut_endModule();
}

//////////////////////////////////////////////////////////////////////////////
// Golden dumps of example models
//////////////////////////////////////////////////////////////////////////////

// Lex `examplesDir/name.ombd` and compare its dump with `goldenDir/name.tokens`.
static void test_goldenDump( const char *examplesDir, const char *goldenDir, const char *name )
{
    char modelPath[1024];
    char goldenPath[1024];
    snprintf( modelPath,  sizeof( modelPath ),  "%s/%s.ombd",   examplesDir, name );
    snprintf( goldenPath, sizeof( goldenPath ), "%s/%s.tokens", goldenDir,   name );

    Source model;
    Source golden;
    bool   haveModel  = tryReadFile( modelPath,  &model );
    bool   haveGolden = tryReadFile( goldenPath, &golden );
    char  *dump       = NULL;
    bool   pass       = false;

    if ( haveModel && haveGolden )
    {
        dump = test_dump( model.text, model.size );
        pass = dump != NULL && strlen( dump ) == golden.size && memcmp( dump, golden.text, golden.size ) == 0;
    }
    ut_reportTestResult( pass, "%s.ombd matches %s.tokens", name, name );

    // Point at the first line that differs.
    if ( dump != NULL && !pass )
    {
        uint32_t line = 1;
        uint32_t i    = 0;
        while ( dump[i] != '\0' && i < golden.size && dump[i] == golden.text[i] )
        {
            if ( dump[i] == '\n' ) line++;
            i++;
        }
        printf( "        first difference on dump line %u\n", line );
    }

    free( dump );
    if ( haveModel )  Source_free( &model );
    if ( haveGolden ) Source_free( &golden );
}

//////////////////////////////////////////////////////////////////////////////
// All lexer tests
//////////////////////////////////////////////////////////////////////////////

void lexer_ut( const char *examplesDir, const char *goldenDir )
{
    test_tokenCases( "Appendix C token streams", appendixCCases,
                     (uint32_t)( sizeof( appendixCCases ) / sizeof( appendixCCases[0] ) ) );
    test_tokenCases( "More token streams", moreTokenCases,
                     (uint32_t)( sizeof( moreTokenCases ) / sizeof( moreTokenCases[0] ) ) );
    test_dumpCases();
    test_errorCases();

    ut_beginModule( "Golden dumps" );
    test_goldenDump( examplesDir, goldenDir, "Pendulum" );
    test_goldenDump( examplesDir, goldenDir, "SliderCrank" );
    ut_endModule();
}

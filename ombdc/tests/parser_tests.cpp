// parser_tests.cpp -- table-driven tests of the parser.
//
// Trees are compared as one-line S-expressions:
//   a name, number or string is its text, and a unit is its whole run;
//   a unary or binary operation is (op operand ...): (+ a (* b c));
//   an axis line is (AXIS in name ...);
//   every other node is (KIND child ...): (DEFINITION x (ITEMS 1)).
//
// Four tables:
//   expressionCases  `x = <input>;` inside a system; the expected text is the item list
//   statementCases   <input> inside `system t { … }`; the expected text is the statements
//   topLevelCases    <input> as a whole file; the expected text is the top statements
//   errorCases       a whole file, the error's position and a fragment of its message
// and the golden `--ast` dumps of two example models.
//
// The source is compiled with /utf-8, so non-ASCII literals below are UTF-8.

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/file.h"
#include "../src/lexer.h"
#include "../src/parser.h"
#include "unit_test.h"

//////////////////////////////////////////////////////////////////////////////
// Helpers
//////////////////////////////////////////////////////////////////////////////

struct ParsedSource
{
    Source     source;
    TokenArray tokens;
    Ast        ast;
    bool       lexed;
    bool       parsed;
    LexError   lexError;
    ParseError parseError;
};

// Lex and parse `text` (NUL-terminated).
static void ParsedSource_make( ParsedSource *ps, const char *text )
{
    Source_make( &ps->source, "test", text, (uint32_t)strlen( text ) );
    ps->parsed = false;
    ps->lexed  = tryLex( &ps->source, &ps->tokens, &ps->lexError );
    if ( ps->lexed ) ps->parsed = tryParse( &ps->source, &ps->tokens, &ps->ast, &ps->parseError );
}

static void ParsedSource_free( ParsedSource *ps )
{
    if ( ps->parsed ) Ast_free( &ps->ast );
    if ( ps->lexed )  TokenArray_free( &ps->tokens );
    Source_free( &ps->source );
}

static void test_append( char *out, uint32_t size, uint32_t *used, const char *format, ... )
{
    if ( *used >= size ) return;
    va_list args;
    va_start( args, format );
    int n = vsnprintf( out + *used, size - *used, format, args );
    va_end( args );
    if ( n > 0 ) *used += (uint32_t)n;
}

static void test_sexpr( const ParsedSource *ps, uint32_t index, char *out, uint32_t size, uint32_t *used )
{
    const Node  *node  = &ps->ast.nodes[ index ];
    const Token *token = &ps->tokens.data[ node->token ];
    const char  *text  = ps->source.text;

    switch ( node->kind )
    {
    case NODE_NAME:
    case NODE_NUMBER:
    case NODE_STRING:
        test_append( out, size, used, "%.*s", (int)token->len, text + token->offset );
        return;

    case NODE_UNIT:
    {
        const Token *last = &ps->tokens.data[ node->lastToken ];
        test_append( out, size, used, "%.*s", (int)( last->offset + last->len - token->offset ), text + token->offset );
        return;
    }

    case NODE_UNARY:
    case NODE_BINARY:
        test_append( out, size, used, "(%.*s", (int)token->len, text + token->offset );
        break;

    case NODE_AXIS:
        test_append( out, size, used, "(AXIS %.*s", (int)token->len, text + token->offset );
        break;

    default:
        test_append( out, size, used, "(%s", NodeKind_name( node->kind ) );
        break;
    }

    for ( uint32_t c = node->child ; c != 0 ; c = ps->ast.nodes[ c ].next )
    {
        test_append( out, size, used, " " );
        test_sexpr( ps, c, out, size, used );
    }
    test_append( out, size, used, ")" );
}

// The S-expressions of `parent`'s children, separated by spaces.
static void test_childrenSexpr( const ParsedSource *ps, uint32_t parent, char *out, uint32_t size )
{
    uint32_t used = 0;
    out[0] = '\0';
    for ( uint32_t c = ps->ast.nodes[ parent ].child ; c != 0 ; c = ps->ast.nodes[ c ].next )
    {
        if ( used > 0 ) test_append( out, size, &used, " " );
        test_sexpr( ps, c, out, size, &used );
    }
}

// The n-th child of `parent`, counting from 0.
static uint32_t test_child( const ParsedSource *ps, uint32_t parent, uint32_t n )
{
    uint32_t c = ps->ast.nodes[ parent ].child;
    while ( c != 0 && n > 0 )
    {
        c = ps->ast.nodes[ c ].next;
        n--;
    }
    return c;
}

static void test_reportFailure( const ParsedSource *ps, const char *expected, const char *got )
{
    if ( !ps->lexed )
    {
        printf( "        lexical error %s at %u:%u\n", LexErrorKind_name( ps->lexError.kind ),
                ps->lexError.line, ps->lexError.col );
    }
    else if ( !ps->parsed )
    {
        printf( "        parse error at %u:%u: %s\n", ps->parseError.line, ps->parseError.col,
                ps->parseError.message );
    }
    else
    {
        printf( "        expected: %s\n", expected );
        printf( "        got:      %s\n", got );
    }
}

//////////////////////////////////////////////////////////////////////////////
// Expressions: `x = <input>;` inside a system
//////////////////////////////////////////////////////////////////////////////

struct TreeCase
{
    const char *input;
    const char *expected;
};

static TreeCase expressionCases[] =
{
    // precedence and associativity (§2.8)
    { "a + b * c",                        "(+ a (* b c))" },
    { "a - b - c",                        "(- (- a b) c)" },
    { "a * b / c mod d",                  "(mod (/ (* a b) c) d)" },
    { "-a * b",                           "(* (- a) b)" },
    { "- - a",                            "(- (- a))" },
    { "^F \\> P",                         "(\\> (^ F) P)" },
    { "F \\> G \\> P",                    "(\\> F (\\> G P))" },
    { "F ▷ P",                            "(▷ F P)" },
    { "u \\. v + w \\x z",                "(+ (\\. u v) (\\x w z))" },
    { "u · v × w",                        "(× (· u v) w)" },
    { "a * F \\> P",                      "(* a (\\> F P))" },
    { "0..n-1",                           "(.. 0 (- n 1))" },
    { "0.0 .. 0.25",                      "(.. 0.0 0.25)" },
    { "(a + b) * c",                      "(* (+ a b) c)" },
    { "(i -1)",                           "(- i 1)" },
    { "(-1) mod 4",                       "(mod (- 1) 4)" },

    // postfix: access, call, index (D133, D153)
    { "a.b.c",                            "(ACCESS (ACCESS a b) c)" },
    { "a . b",                            "(ACCESS a b)" },
    { "bPiston.cg.pos.x",                 "(ACCESS (ACCESS (ACCESS bPiston cg) pos) x)" },
    { "f(x, y)",                          "(CALL f x y)" },
    { "f()",                              "(CALL f)" },
    { "a[i][j]",                          "(INDEX (INDEX a i) j)" },
    { "shoe[(i+1) mod n]",                "(INDEX shoe (mod (+ i 1) n))" },
    { "mid(pivot, rod_end)",              "(CALL mid pivot rod_end)" },
    { "a.f(x).y",                         "(ACCESS (CALL (ACCESS a f) x) y)" },

    // units (§2.6)
    { "10 kg",                            "(QUANTITY 10 kg)" },
    { "9.81 m/s2",                        "(QUANTITY 9.81 m/s2)" },
    { "1200 N-m/rad",                     "(QUANTITY 1200 N-m/rad)" },
    { "2 rpm/N^0.5-m^0.5",                "(QUANTITY 2 rpm/N^0.5-m^0.5)" },
    { "1 kN/m^3.1",                       "(QUANTITY 1 kN/m^3.1)" },
    { "5 m - x",                          "(- (QUANTITY 5 m) x)" },
    { "5 m-x",                            "(QUANTITY 5 m-x)" },
    { "5 m -x",                           "(- (QUANTITY 5 m) x)" },
    { "0.5 in * sin(2*pi * 0.25 Hz * time)",
      "(* (QUANTITY 0.5 in) (CALL sin (* (* (* 2 pi) (QUANTITY 0.25 Hz)) time)))" },
    { "diag(1, 2, 3) kg-m2",              "(QUANTITY (CALL diag 1 2 3) kg-m2)" },
    { "max(a, 3 rpm)",                    "(CALL max a (QUANTITY 3 rpm))" },
    { "max(0 x)",                         "(CALL max (QUANTITY 0 x))" },
    { "(1, 2, 3) in",                     "(QUANTITY (POINT 1 2 3) in)" },
    { "[0, 5, 10] s",                     "(QUANTITY (LIST 0 5 10) s)" },
    { "1e6 Hz",                           "(QUANTITY 1e6 Hz)" },
    { "rpm",                              "rpm" },

    // points and grouping (D132, D164)
    { "(0, 0, -1)",                       "(POINT 0 0 (- 1))" },
    { "( 17.655, -47.635, 32.905)",       "(POINT 17.655 (- 47.635) 32.905)" },
    { "(a)",                              "a" },

    // space-separated lists (§2.7)
    { "[0, 1, 0]",                        "(LIST 0 1 0)" },
    { "[1 0 0]",                          "(LIST 1 0 0)" },
    { "[0 -1 0]",                         "(LIST 0 (- 1) 0)" },
    { "[1 - 2]",                          "(LIST (- 1 2))" },
    { "[1-2]",                            "(LIST (- 1 2))" },
    { "[1- 2]",                           "(LIST (- 1 2))" },
    { "[1 -2 + 3]",                       "(LIST 1 (+ (- 2) 3))" },
    { "[2 * a -b]",                       "(LIST (* 2 a) (- b))" },
    { "[a b]",                            "(LIST a b)" },
    { "[a (b)]",                          "(LIST a b)" },
    { "[a(b)]",                           "(LIST (CALL a b))" },
    { "[a . b]",                          "(LIST (ACCESS a b))" },
    { "[a * b]",                          "(LIST (* a b))" },
    { "[1, 2 3]",                         "(LIST 1 2 3)" },
    { "[]",                               "(LIST)" },
    { "[1 m]",                            "(LIST 1 m)" },
    { "[1 2] m",                          "(QUANTITY (LIST 1 2) m)" },
    { "[a, b[i]]",                        "(LIST a (INDEX b i))" },
    { "[a [i]]",                          "(LIST a (LIST i))" },
    { "[ [1 2] [3 4] ]",                  "(LIST (LIST 1 2) (LIST 3 4))" },
    { "[f(1 m) 2]",                       "(LIST (CALL f (QUANTITY 1 m)) 2)" },
    { "[(i -1) 2]",                       "(LIST (- i 1) 2)" },
    { "[0 .. 1]",                         "(LIST (.. 0 1))" },
    { "[left, right]",                    "(LIST left right)" },
    { "[\"a\" \"b\"]",                    "(LIST \"a\" \"b\")" },

    // grids (D143)
    { "[[1 0 0; 0 1 0; 0 0 1]]",          "(GRID (ROW 1 0 0) (ROW 0 1 0) (ROW 0 0 1))" },
    { "[[ -14, -20 ; 120, 180 ]] N-m",    "(QUANTITY (GRID (ROW (- 14) (- 20)) (ROW 120 180)) N-m)" },
    { "[ [[1 0; 0 1]], [[0 1; 1 0]] ]",   "(LIST (GRID (ROW 1 0) (ROW 0 1)) (GRID (ROW 0 1) (ROW 1 0)))" },

    // closers matched where they are needed, never split (D168)
    { "[[a b; c d[i]]]",                  "(GRID (ROW a b) (ROW c (INDEX d i)))" },
    { "[a, b[c[i]]]",                     "(LIST a (INDEX b (INDEX c i)))" },
    { "<[ p, q[i]]>",                     "(FRAME p (INDEX q i))" },
    { "<| q[i]|>",                        "(ORIENTATION (INDEX q i))" },
    { "[| a b c[i] N ; d e f N |]",       "(SPATIAL (HALF a b (INDEX c i) N) (HALF d e f N))" },
    { "<[ p, <1>]>",                      "(FRAME p (QUATERNION 1))" },

    // spatial vectors (D64, D148)
    { "[| ka, kb, kc N/m ; ra, rb, rc N-m/rad |]",
      "(SPATIAL (HALF ka kb kc N/m) (HALF ra rb rc N-m/rad))" },
    { "[| 300000, 300000, 450000 N/m ; 4000 4000 3000 N-m/rad |]",
      "(SPATIAL (HALF 300000 300000 450000 N/m) (HALF 4000 4000 3000 N-m/rad))" },

    // quaternions (D38, D128, D131)
    { "<1>",                              "(QUATERNION 1)" },
    { "<w; x y z>",                       "(QUATERNION w x y z)" },
    { "< -1; 0 0 0>",                     "(QUATERNION (- 1) 0 0 0)" },
    { "<0.7; 0 0 -0.7>",                  "(QUATERNION 0.7 0 0 (- 0.7))" },
    { "<0.7; 0, 0, 0.7>",                 "(QUATERNION 0.7 0 0 0.7)" },
    { "<a + b>",                          "(QUATERNION (+ a b))" },
    { "<1; 0 0 a[i]>",                    "(QUATERNION 1 0 0 (INDEX a i))" },

    // orientations (D38, D140)
    { "<| [[1 0 0; 0 1 0; 0 0 1]] |>",    "(ORIENTATION (GRID (ROW 1 0 0) (ROW 0 1 0) (ROW 0 0 1)))" },
    { "<| <w; x, y, z> |>",               "(ORIENTATION (QUATERNION w x y z))" },
    { "<| 90 deg, [1, 2, 4] |>",          "(ORIENTATION (QUANTITY 90 deg) (LIST 1 2 4))" },
    { "<| Z: 6, Y': 3, X'': 6 |> deg",    "(QUANTITY (ORIENTATION (PROPERTY Z 6) (PROPERTY Y' 3) (PROPERTY X'' 6)) deg)" },
    { "<| Z: 6, Y: 3, X: 6 |> deg",       "(QUANTITY (ORIENTATION (PROPERTY Z 6) (PROPERTY Y 3) (PROPERTY X 6)) deg)" },
    { "<| Z: 1, X': 2, Z'': 3 |>",        "(ORIENTATION (PROPERTY Z 1) (PROPERTY X' 2) (PROPERTY Z'' 3))" },
    { "<| (1,2,3), (-6,0,3.8), (0.34,5.93,77.2), \"OZX\" |>",
      "(ORIENTATION (POINT 1 2 3) (POINT (- 6) 0 3.8) (POINT 0.34 5.93 77.2) \"OZX\")" },
    { "<| [1,2,3], [-6,0,3.8], \"ZX\" |>",
      "(ORIENTATION (LIST 1 2 3) (LIST (- 6) 0 3.8) \"ZX\")" },

    // frames (D148, D149)
    { "<[ engine_mount, <1> ]>",          "(FRAME engine_mount (QUATERNION 1))" },
    { "<[ (1, 2, 3) in, q ]>",            "(FRAME (QUANTITY (POINT 1 2 3) in) q)" },
    { "<[ p, q[i] ]>",                    "(FRAME p (INDEX q i))" },

    // strings and typed blocks
    { "\"OZX\"",                          "\"OZX\"" },
    { "f(map { interp: linear; })",       "(CALL f (TYPED_BLOCK map (BLOCK (DECLARATION interp (ITEMS linear)))))" },
};

static void test_expressionCases( void )
{
    ut_beginModule( "Expressions" );
    for ( uint32_t i = 0 ; i < sizeof( expressionCases ) / sizeof( expressionCases[0] ) ; i++ )
    {
        TreeCase *kase = &expressionCases[i];
        char      text[1024];
        char      got[2048];
        snprintf( text, sizeof( text ), "system t {\nx = %s;\n}\n", kase->input );

        ParsedSource ps;
        ParsedSource_make( &ps, text );
        bool pass = false;
        got[0] = '\0';
        if ( ps.parsed )
        {
            // SOURCE > SYSTEM > BLOCK > DEFINITION > ITEMS
            uint32_t block      = test_child( &ps, test_child( &ps, 0, 0 ), 1 );
            uint32_t definition = test_child( &ps, block, 0 );
            uint32_t items      = test_child( &ps, definition, 1 );
            test_childrenSexpr( &ps, items, got, sizeof( got ) );
            pass = strcmp( got, kase->expected ) == 0;
        }
        ut_reportTestResult( pass, "x = %s;", kase->input );
        if ( !pass ) test_reportFailure( &ps, kase->expected, got );
        ParsedSource_free( &ps );
    }
    ut_endModule();
}

//////////////////////////////////////////////////////////////////////////////
// Statements: <input> inside `system t { … }`
//////////////////////////////////////////////////////////////////////////////

static TreeCase statementCases[] =
{
    // definitions and declarations (§2.3)
    { "x = 1;",                                   "(DEFINITION x (ITEMS 1))" },
    { "mass: 10 kg;",                             "(DECLARATION mass (ITEMS (QUANTITY 10 kg)))" },
    { "sr = a / b, default: 0;",                  "(DEFINITION sr (ITEMS (/ a b) (PROPERTY default 0)))" },
    { "coil = TSDA, rate: 250 lb/in, free: 14 in;",
      "(DEFINITION coil (ITEMS TSDA (PROPERTY rate (QUANTITY 250 lb/in)) (PROPERTY free (QUANTITY 14 in))))" },
    { "jAxle: revolute, at: pivot, axis: [0, 1, 0];",
      "(DECLARATION jAxle (ITEMS revolute (PROPERTY at pivot) (PROPERTY axis (LIST 0 1 0))))" },
    { "jTie: distance, tie_outer, tie_inner;",    "(DECLARATION jTie (ITEMS distance tie_outer tie_inner))" },
    { "jWrist: universal, at: w, axes: [0, 1, 0], [0, 0, 1];",
      "(DECLARATION jWrist (ITEMS universal (PROPERTY at w) (PROPERTY axes (LIST 0 1 0)) (LIST 0 0 1)))" },
    { "throttle: 1, open;",                       "(DECLARATION throttle (ITEMS 1 open))" },
    { "units: in, lb;",                           "(DECLARATION units (ITEMS in lb))" },
    { "limits: 0.0 .. 0.25;",                     "(DECLARATION limits (ITEMS (.. 0.0 0.25)))" },
    { "1: 3.50;",                                 "(DECLARATION 1 (ITEMS 3.50))" },
    { "R: -3.30;",                                "(DECLARATION R (ITEMS (- 3.30)))" },
    { "locked: bool;",                            "(DECLARATION locked (ITEMS bool))" },

    // block forms (D92, D152)
    { "tSlope = tire { mu: 0.85; }",
      "(DEFINITION tSlope (TYPED_BLOCK tire (BLOCK (DECLARATION mu (ITEMS 0.85)))))" },
    { "diffRear: differential { ratio: 3.42; }",
      "(DECLARATION diffRear (TYPED_BLOCK differential (BLOCK (DECLARATION ratio (ITEMS 3.42)))))" },
    { "case: { mass: 1 kg; }",
      "(DECLARATION case (BLOCK (DECLARATION mass (ITEMS (QUANTITY 1 kg)))))" },
    { "ratios: { 1: 3.50; R: -3.30; }",
      "(DECLARATION ratios (BLOCK (DECLARATION 1 (ITEMS 3.50)) (DECLARATION R (ITEMS (- 3.30)))))" },

    // scopes and nested systems (D138)
    { "points { p = (0, 0, 0); }",
      "(SCOPE points (BLOCK (DEFINITION p (ITEMS (POINT 0 0 0)))))" },
    { "bRod { mass: 10 kg; }",                    "(SCOPE bRod (BLOCK (DECLARATION mass (ITEMS (QUANTITY 10 kg)))))" },
    { "system steering { }",                      "(SYSTEM steering (BLOCK))" },
    { "points { } bodies { }",                    "(SCOPE points (BLOCK)) (SCOPE bodies (BLOCK))" },

    // drives and bare references (D19, D78, D82)
    { "jAxle.angle.pos <- theta;",                "(DRIVE (ACCESS (ACCESS jAxle angle) pos) theta)" },
    { "throttle <- speedGov.output;",             "(DRIVE throttle (ACCESS speedGov output))" },
    { "engine_rpm;",                              "(REFERENCE engine_rpm)" },
    { "gearbox.gear;",                            "(REFERENCE (ACCESS gearbox gear))" },
    { "outputs { update: 1 ms; piston_x; }",
      "(SCOPE outputs (BLOCK (DECLARATION update (ITEMS (QUANTITY 1 ms))) (REFERENCE piston_x)))" },

    // comprehensions (§2.5)
    { "pin[i]: revolute, at: hinge[i] for i in 0..n-1;",
      "(DECLARATION (INDEX pin i) (ITEMS revolute (PROPERTY at (INDEX hinge i))) (FOR i (.. 0 (- n 1))))" },
    { "corner[s] = c for s in [left, right];",
      "(DEFINITION (INDEX corner s) (ITEMS c) (FOR s (LIST left right)))" },

    // assembly chains (D30, D131)
    { "assembly { world <jAxle> bRod; }",
      "(SCOPE assembly (BLOCK (CHAIN world (SLOT jAxle) bRod)))" },
    { "assembly { a <j1> b <j2> c.d; }",
      "(SCOPE assembly (BLOCK (CHAIN a (SLOT j1) b (SLOT j2) (ACCESS c d))))" },
    { "assembly { engine.crank <shaft> converter.pump; }",
      "(SCOPE assembly (BLOCK (CHAIN (ACCESS engine crank) (SLOT shaft) (ACCESS converter pump))))" },
    { "assembly { shoe[i] <pin[i]> shoe[(i+1) mod n] for i in 0..n-1; }",
      "(SCOPE assembly (BLOCK (CHAIN (INDEX shoe i) (SLOT (INDEX pin i)) (INDEX shoe (mod (+ i 1) n)) "
      "(FOR i (.. 0 (- n 1))))))" },

    // maps (D55, D160)
    { "m = map { in rpm: [1000, 3000] rpm; out t: [1, 2]; interp: linear; }",
      "(DEFINITION m (TYPED_BLOCK map (BLOCK (AXIS in rpm (ITEMS (QUANTITY (LIST 1000 3000) rpm))) "
      "(AXIS out t (ITEMS (LIST 1 2))) (DECLARATION interp (ITEMS linear)))))" },
    { "m = map { in time: [0, 5] s, edges: clamp; out a: [0, 15] deg; }",
      "(DEFINITION m (TYPED_BLOCK map (BLOCK (AXIS in time (ITEMS (QUANTITY (LIST 0 5) s) (PROPERTY edges clamp))) "
      "(AXIS out a (ITEMS (QUANTITY (LIST 0 15) deg))))))" },
    { "maps { m = map { in x: [0, 1]; out y: [0, 1]; } }",
      "(SCOPE maps (BLOCK (DEFINITION m (TYPED_BLOCK map (BLOCK (AXIS in x (ITEMS (LIST 0 1))) "
      "(AXIS out y (ITEMS (LIST 0 1))))))))" },

    // fsm states and guards (D78, D133, D157)
    { "c: fsm { states { open { locked: false; when sr > 0.92 and throttle < 0.8 -> locked; } } }",
      "(DECLARATION c (TYPED_BLOCK fsm (BLOCK (SCOPE states (BLOCK (SCOPE open (BLOCK "
      "(DECLARATION locked (ITEMS false)) (WHEN (and (> sr 0.92) (< throttle 0.8)) locked))))))))" },
    { "c: fsm { states { s { when not a < b or c == d -> t; } } }",
      "(DECLARATION c (TYPED_BLOCK fsm (BLOCK (SCOPE states (BLOCK (SCOPE s (BLOCK "
      "(WHEN (or (not (< a b)) (== c d)) t))))))))" },
    { "c: fsm { states { s { when a and b and c -> t; when gear != 1 -> u; } } }",
      "(DECLARATION c (TYPED_BLOCK fsm (BLOCK (SCOPE states (BLOCK (SCOPE s (BLOCK "
      "(WHEN (and (and a b) c) t) (WHEN (!= gear 1) u))))))))" },
    { "c: fsm { inputs { sr; } outputs { locked: bool; } }",
      "(DECLARATION c (TYPED_BLOCK fsm (BLOCK (SCOPE inputs (BLOCK (REFERENCE sr))) "
      "(SCOPE outputs (BLOCK (DECLARATION locked (ITEMS bool)))))))" },

    // regions are ordinary declarations (D108)
    { "pan: polygon, s1, s2, s3, soil: clay;",
      "(DECLARATION pan (ITEMS polygon s1 s2 s3 (PROPERTY soil clay)))" },
};

static void test_statementCases( void )
{
    ut_beginModule( "Statements" );
    for ( uint32_t i = 0 ; i < sizeof( statementCases ) / sizeof( statementCases[0] ) ; i++ )
    {
        TreeCase *kase = &statementCases[i];
        char      text[1024];
        char      got[2048];
        snprintf( text, sizeof( text ), "system t {\n%s\n}\n", kase->input );

        ParsedSource ps;
        ParsedSource_make( &ps, text );
        bool pass = false;
        got[0] = '\0';
        if ( ps.parsed )
        {
            uint32_t block = test_child( &ps, test_child( &ps, 0, 0 ), 1 );   // SOURCE > SYSTEM > BLOCK
            test_childrenSexpr( &ps, block, got, sizeof( got ) );
            pass = strcmp( got, kase->expected ) == 0;
        }
        ut_reportTestResult( pass, "%s", kase->input );
        if ( !pass ) test_reportFailure( &ps, kase->expected, got );
        ParsedSource_free( &ps );
    }
    ut_endModule();
}

//////////////////////////////////////////////////////////////////////////////
// The top level (§2.1): <input> as a whole file
//////////////////////////////////////////////////////////////////////////////

static TreeCase topLevelCases[] =
{
    { "",                                         "" },
    { "// only a comment\n",                      "" },
    { "gravity: [0, 0, -9.81] m/s2;",
      "(DECLARATION gravity (ITEMS (QUANTITY (LIST 0 0 (- 9.81)) m/s2)))" },
    { "system s { }",                             "(SYSTEM s (BLOCK))" },
    { "system a { system b { } }",                "(SYSTEM a (BLOCK (SYSTEM b (BLOCK))))" },
    { "namespace a.b { use c.d; provides x, y; system s { } }",
      "(NAMESPACE (ACCESS a b) (BLOCK (USE (ACCESS c d)) (PROVIDES x y) (SYSTEM s (BLOCK))))" },
    { "namespace a { namespace b { } }",          "(NAMESPACE a (BLOCK (NAMESPACE b (BLOCK))))" },
    { "namespace { gravity: g; system s { } }",
      "(NAMESPACE (BLOCK (DECLARATION gravity (ITEMS g)) (SYSTEM s (BLOCK))))" },
    { "simulation { horizon: 1 s; outputs { update: 1 ms; piston_x; } }",
      "(SIMULATION (BLOCK (DECLARATION horizon (ITEMS (QUANTITY 1 s))) "
      "(SCOPE outputs (BLOCK (DECLARATION update (ITEMS (QUANTITY 1 ms))) (REFERENCE piston_x)))))" },
    { "gravity: g;\nsystem s { }\nsimulation { }",
      "(DECLARATION gravity (ITEMS g)) (SYSTEM s (BLOCK)) (SIMULATION (BLOCK))" },
};

static void test_topLevelCases( void )
{
    ut_beginModule( "Top level" );
    for ( uint32_t i = 0 ; i < sizeof( topLevelCases ) / sizeof( topLevelCases[0] ) ; i++ )
    {
        TreeCase *kase = &topLevelCases[i];
        char      got[2048];

        ParsedSource ps;
        ParsedSource_make( &ps, kase->input );
        bool pass = false;
        got[0] = '\0';
        if ( ps.parsed )
        {
            test_childrenSexpr( &ps, 0, got, sizeof( got ) );
            pass = strcmp( got, kase->expected ) == 0;
        }
        ut_reportTestResult( pass, "%s", kase->input );
        if ( !pass ) test_reportFailure( &ps, kase->expected, got );
        ParsedSource_free( &ps );
    }
    ut_endModule();
}

//////////////////////////////////////////////////////////////////////////////
// Parse errors
//////////////////////////////////////////////////////////////////////////////

struct ParseErrorCase
{
    const char *input;           // a whole file
    uint32_t    line;
    uint32_t    col;
    const char *fragment;        // a piece of the expected message
};

static ParseErrorCase parseErrorCases[] =
{
    // the spec's own examples (§0, §2)
    { "system t {\nx = [[[1 0; 0 1]] ];\n}",           2, 5,  "'[[[' is not an opener" },
    { "system t {\nx = a\ny = 2;\n}",                   3, 1,  "expected ';' at the end of the previous line" },
    // A spaced IDENT after a number is its unit, even across a line end
    // (LINE_BEFORE has no syntactic meaning, §1.2): `1` and `y` are one value.
    { "system t {\nx = 1\ny = 2;\n}",                   3, 3,  "expected ';'" },
    { "system t {\nx = a < b;\n}",                      2, 7,  "legal only in a 'when' guard" },
    { "system t {\nx = a and b;\n}",                    2, 7,  "legal only in a 'when' guard" },
    { "system t {\nx = not a;\n}",                      2, 5,  "'not' is legal only in a 'when' guard" },
    { "system t {\nx = 10kg;\n}",                       2, 7,  "write a space between a number and its unit: '10 kg'" },
    { "system t {\nx = 2e;\n}",                         2, 6,  "write a space between a number and its unit" },
    { "system t {\nx = diag(1 2 3);\n}",                2, 12, "separated by commas" },
    { "system t {\nx = (1 2 3);\n}",                    2, 8,  "separated by commas" },
    { "system t {\nx = diag (1, 2, 3);\n}",             2, 10, "expected ';' to end the statement" },
    { "system t {\nx = <-1>;\n}",                       2, 5,  "'<-' is the drive arrow" },
    { "system t {\nc: fsm { states { s { when a < b < c -> s; } } }\n}",
                                                        2, 34, "comparisons do not chain" },
    { "system t {\nx = 0..1..2;\n}",                    2, 9,  "'..' does not chain" },
    { "system t {\npoints { };\n}",                     2, 11, "takes no ';'" },
    { "system s { };",                                  1, 13, "takes no ';'" },
    { "system t {\nx = tire { };\n}",                   2, 13, "takes no ';'" },
    { "system t {\nx = Y';\n}",                         2, 5,  "primed name" },
    { "system t {\nx: rotate, Y': 3;\n}",               2, 12, "primed name" },
    { "system t {\nx = a.Y';\n}",                       2, 7,  "primed name" },
    { "system t {\nx = @a;\n}",                         2, 5,  "'@' is reserved" },
    { "system t {\nassembly { a <-> b; }\n}",           2, 14, "'<->' has no statement form" },

    // literal shapes (§2.9)
    { "system t {\nx = (1, 2);\n}",                     2, 5,  "a point has exactly three entries" },
    { "system t {\nx = ();\n}",                         2, 5,  "a point has exactly three entries" },
    { "system t {\nx = (a) m;\n}",                      2, 9,  "a unit can't follow grouping" },
    { "system t {\nx = [[1 0 0]];\n}",                  2, 5,  "at least two rows" },
    { "system t {\nx = [[1 0; 0 1 0]];\n}",             2, 12, "same number of entries" },
    { "system t {\nx = <1; 0 0>;\n}",                   2, 5,  "a quaternion is <w> or <w; x y z>" },
    { "system t {\nx = <1 2>;\n}",                      2, 8,  "expected '>' to close the quaternion" },
    { "system t {\nx = [| a b ; c d e N |];\n}",        2, 8,  "exactly three entries" },
    { "system t {\nx = [| a b c ; d e f N |];\n}",      2, 14, "the half's unit" },
    { "system t {\nx = <[ p ]>;\n}",                    2, 5,  "a frame is <[ point, orientation ]>" },
    { "system t {\nx = <[ p, q ]> m;\n}",               2, 16, "a frame takes no unit" },
    { "system t {\nx = <[ p, Y': q ]>;\n}",             2, 11, "primed name" },

    // compound closers are two tight tokens (D168)
    { "system t {\nx = [[1 0; 0 1] ];\n}",              2, 17, "must be adjacent" },
    { "system t {\nx = [| a b c N ; d e f N | ];\n}",   2, 28, "must be adjacent" },
    { "system t {\nx = <| q | >;\n}",                   2, 12, "must be adjacent" },
    { "system t {\nx = <[ p, q ] >;\n}",                2, 15, "must be adjacent" },
    { "system t {\nx = <[ p, q ]/**/>;\n}",             2, 18, "must be adjacent" },
    { "system t {\nx = [[1 0; 0 1];\n}",                2, 16, "expected ']' to complete ']]'" },
    { "system t {\nx = a | b;\n}",                      2, 7,  "'|' is legal only in the closers" },
    { "system t {\nx = [a | b];\n}",                    2, 8,  "'|' is legal only in the closers" },
    { "system t {\nx = <| Z: 1, Y'': 2, X': 3 |>;\n}",  2, 8,  "0, 1 and 2 primes in order" },
    { "system t {\nx = <| Z: 1, Q: 2, X: 3 |>;\n}",     2, 14, "an Euler label is X, Y or Z" },
    { "system t {\nx = <| Z: 1, Y: 2 |>;\n}",           2, 5,  "three labeled entries" },
    { "system t {\nx = <| q |> deg;\n}",                2, 13, "legal only for Euler angles" },

    // units (§2.6)
    { "system t {\nx = 5 m-3;\n}",                      2, 9,  "after '-' in a unit comes a unit name" },
    { "system t {\nx = 5 m^x;\n}",                      2, 9,  "after '^' in a unit comes a number" },

    // bracket entries (§2.7)
    { "system t {\nx = [0 +1 0];\n}",                   2, 8,  "there is no prefix '+'" },
    { "system t {\nx = +1;\n}",                         2, 5,  "there is no prefix '+'" },
    { "system t {\nx = [<1> <0; 1 0 0>];\n}",           2, 10, "separate quaternions in a list with commas" },
    { "system t {\nx = [1, 2,];\n}",                    2, 11, "must be followed by an entry" },
    { "system t {\nx = [1 2;\n}",                       2, 9,  "expected ',' or the end of the bracket" },

    // statements (§2.2, §2.3, §2.4)
    { "system t {\nx = 1 2;\n}",                        2, 7,  "expected ';' to end the statement, or ','" },
    { "system t {\nx <j> y;\n}",                        2, 3,  "expected ':', '=', '<-' or ';'" },
    { "system t {\n1 = 2;\n}",                          2, 3,  "':' after a number key" },
    { "system t {\nassembly { a b; }\n}",               2, 14, "an assembly line is 'a <connector> b;'" },
    { "system t {\nassembly { a <j b; }\n}",            2, 17, "'>' to close the connector slot" },
    { "system t {\nc: fsm { states { x: 1; } }\n}",     2, 19, "holds only states" },
    { "system t {\nc: fsm { states { s { when a -> 1; } } }\n}",
                                                        2, 33, "expected a name" },
    { "system t {\nx = 1;\n",                           3, 1,  "expected '}' to close the block opened at 1:10" },
    { "system t {\n}\n}",                               3, 1,  "at the top level" },

    // the top level (§2.1)
    { "x = 1;",                                         1, 1,  "expected 'gravity:', 'namespace', 'system' or 'simulation'" },
    { "use a;",                                         1, 1,  "legal only inside a named namespace" },
    { "namespace { provides x; }",                      1, 13, "legal only inside a named namespace" },
    { "namespace a { namespace { } }",                  1, 15, "anonymous namespace is legal only at the top level" },
    { "namespace a { gravity: g; }",                    1, 15, "expected 'use', 'provides', 'namespace' or 'system'" },
    { "gravity: 1, 2;",                                 1, 11, "gravity takes one value" },
};

static void test_parseErrorCases( void )
{
    ut_beginModule( "Parse errors" );
    for ( uint32_t i = 0 ; i < sizeof( parseErrorCases ) / sizeof( parseErrorCases[0] ) ; i++ )
    {
        ParseErrorCase *kase = &parseErrorCases[i];
        ParsedSource    ps;
        ParsedSource_make( &ps, kase->input );

        bool pass = ps.lexed && !ps.parsed
                 && ps.parseError.line == kase->line
                 && ps.parseError.col  == kase->col
                 && strstr( ps.parseError.message, kase->fragment ) != NULL;

        // Show the input on one line.
        char shown[128];
        uint32_t n = 0;
        for ( const char *s = kase->input ; *s != '\0' && n + 3 < sizeof( shown ) ; s++ )
        {
            if ( *s == '\n' ) { shown[n++] = '\\'; shown[n++] = 'n'; }
            else              { shown[n++] = *s; }
        }
        shown[n] = '\0';

        ut_reportTestResult( pass, "%s  at %u:%u", shown, kase->line, kase->col );
        if ( !pass )
        {
            if ( !ps.lexed )      printf( "        lexical error at %u:%u\n", ps.lexError.line, ps.lexError.col );
            else if ( ps.parsed ) printf( "        parsed without error\n" );
            else                  printf( "        got %u:%u: %s\n", ps.parseError.line, ps.parseError.col,
                                          ps.parseError.message );
        }
        ParsedSource_free( &ps );
    }
    ut_endModule();
}

//////////////////////////////////////////////////////////////////////////////
// Golden AST dumps
//////////////////////////////////////////////////////////////////////////////

// Parse `examplesDir/name.ombd` and compare its dump with `goldenDir/name.ast`.
static void test_goldenAst( const char *examplesDir, const char *goldenDir, const char *name )
{
    char modelPath[1024];
    char goldenPath[1024];
    snprintf( modelPath,  sizeof( modelPath ),  "%s/%s.ombd", examplesDir, name );
    snprintf( goldenPath, sizeof( goldenPath ), "%s/%s.ast",  goldenDir,   name );

    Source model;
    Source golden;
    bool   haveModel  = tryReadFile( modelPath,  &model );
    bool   haveGolden = tryReadFile( goldenPath, &golden );
    char  *dump       = NULL;
    bool   pass       = false;

    if ( haveModel && haveGolden )
    {
        TokenArray tokens;
        LexError   lexError;
        Ast        ast;
        ParseError parseError;
        if ( tryLex( &model, &tokens, &lexError ) )
        {
            if ( tryParse( &model, &tokens, &ast, &parseError ) )
            {
                uint32_t size = Ast_format( &ast, &tokens, model.text, NULL, 0 );
                dump = (char *)malloc( (size_t)size + 1 );
                Ast_format( &ast, &tokens, model.text, dump, size + 1 );
                pass = size == golden.size && memcmp( dump, golden.text, size ) == 0;
                Ast_free( &ast );
            }
            else
            {
                printf( "        parse error at %u:%u: %s\n", parseError.line, parseError.col, parseError.message );
            }
            TokenArray_free( &tokens );
        }
    }
    ut_reportTestResult( pass, "%s.ombd matches %s.ast", name, name );

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
// All parser tests
//////////////////////////////////////////////////////////////////////////////

void parser_ut( const char *examplesDir, const char *goldenDir )
{
    test_expressionCases();
    test_statementCases();
    test_topLevelCases();
    test_parseErrorCases();

    ut_beginModule( "Golden ASTs" );
    test_goldenAst( examplesDir, goldenDir, "Pendulum" );
    test_goldenAst( examplesDir, goldenDir, "SliderCrank" );
    ut_endModule();
}

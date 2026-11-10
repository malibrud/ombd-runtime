// types_tests.cpp -- table-driven tests of value types and units.
//
// A checked model is compared as one string: every named value in the order
// declared, as `name=type dimension`, with the literal's factor added when it
// carries one, joined by " ; ". A failing model gives the position of the
// error and a fragment of its message.
//
// The helpers test_makeUnit, test_freeUnit and test_showText come from
// resolve_tests.cpp, in the same translation unit.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/file.h"
#include "../src/resolve.h"
#include "../src/types.h"
#include "unit_test.h"

//////////////////////////////////////////////////////////////////////////////
// The closure table, transcribed from §3.4
//////////////////////////////////////////////////////////////////////////////

static const ValueType testAlgebraTypes[] =
{
    TYPE_SCALAR, TYPE_POINT, TYPE_VECTOR, TYPE_ORIENTATION, TYPE_MATRIX, TYPE_FRAME,
};

static const char testAlgebraLetters[] = "sPVOMF";

static ValueType test_typeOfLetter( char letter )
{
    const char *found = strchr( testAlgebraLetters, letter );
    return testAlgebraTypes[ found - testAlgebraLetters ];
}

struct ClosureRow
{
    BinaryOp      op;
    char          left;
    char          right;
    char          result;
    DimensionRule rule;
};

static ClosureRow closureRows[] =
{
    // s ± s -> s
    { OP_ADD,      's', 's', 's', RULE_SAME },
    { OP_SUBTRACT, 's', 's', 's', RULE_SAME },
    // s*s, s/s -> s
    { OP_MULTIPLY, 's', 's', 's', RULE_PRODUCT },
    { OP_DIVIDE,   's', 's', 's', RULE_QUOTIENT },
    // s mod s -> s
    { OP_MOD,      's', 's', 's', RULE_SAME },
    // P - P -> V
    { OP_SUBTRACT, 'P', 'P', 'V', RULE_SAME },
    // P + V, V + P, P - V -> P
    { OP_ADD,      'P', 'V', 'P', RULE_SAME },
    { OP_ADD,      'V', 'P', 'P', RULE_SAME },
    { OP_SUBTRACT, 'P', 'V', 'P', RULE_SAME },
    // V ± V -> V
    { OP_ADD,      'V', 'V', 'V', RULE_SAME },
    { OP_SUBTRACT, 'V', 'V', 'V', RULE_SAME },
    // s*V, V*s, V/s -> V
    { OP_MULTIPLY, 's', 'V', 'V', RULE_PRODUCT },
    { OP_MULTIPLY, 'V', 's', 'V', RULE_PRODUCT },
    { OP_DIVIDE,   'V', 's', 'V', RULE_QUOTIENT },
    // M ± M, s*M, M*s, M/s -> M
    { OP_ADD,      'M', 'M', 'M', RULE_SAME },
    { OP_SUBTRACT, 'M', 'M', 'M', RULE_SAME },
    { OP_MULTIPLY, 's', 'M', 'M', RULE_PRODUCT },
    { OP_MULTIPLY, 'M', 's', 'M', RULE_PRODUCT },
    { OP_DIVIDE,   'M', 's', 'M', RULE_QUOTIENT },
    // V \. V -> s, V \x V -> V
    { OP_DOT,      'V', 'V', 's', RULE_PRODUCT },
    { OP_CROSS,    'V', 'V', 'V', RULE_PRODUCT },
    // O * O -> O, F * F -> F, M * M -> M, O * M, M * O -> M
    { OP_MULTIPLY, 'O', 'O', 'O', RULE_PRODUCT },
    { OP_MULTIPLY, 'F', 'F', 'F', RULE_PRODUCT },
    { OP_MULTIPLY, 'M', 'M', 'M', RULE_PRODUCT },
    { OP_MULTIPLY, 'O', 'M', 'M', RULE_PRODUCT },
    { OP_MULTIPLY, 'M', 'O', 'M', RULE_PRODUCT },
    // O \> V -> V, M \> V -> V, O \> M -> M
    { OP_APPLY,    'O', 'V', 'V', RULE_PRODUCT },
    { OP_APPLY,    'M', 'V', 'V', RULE_PRODUCT },
    { OP_APPLY,    'O', 'M', 'M', RULE_PRODUCT },
    // F \> P -> P, F \> V -> V, F \> M -> M
    { OP_APPLY,    'F', 'P', 'P', RULE_PRODUCT },
    { OP_APPLY,    'F', 'V', 'V', RULE_PRODUCT },
    { OP_APPLY,    'F', 'M', 'M', RULE_PRODUCT },
};

static const char *const testOpNames[ OP_COUNT ] = { "+", "-", "*", "/", "mod", "\\.", "\\x", "\\>" };

// Every combination of each operator: the rows above are legal, and only they.
static void test_binaryClosure( void )
{
    ut_beginModule( "Closure table" );

    for ( uint32_t op = 0 ; op < OP_COUNT ; op++ )
    {
        uint32_t wrong = 0;
        for ( uint32_t l = 0 ; l < 6 ; l++ )
        {
            for ( uint32_t r = 0 ; r < 6 ; r++ )
            {
                const ClosureRow *row = NULL;
                for ( uint32_t i = 0 ; i < sizeof( closureRows ) / sizeof( closureRows[0] ) ; i++ )
                {
                    const ClosureRow *candidate = &closureRows[i];
                    if ( (uint32_t)candidate->op == op && candidate->left == testAlgebraLetters[l] && candidate->right == testAlgebraLetters[r] )
                    {
                        row = candidate;
                    }
                }

                ValueType     result = TYPE_NONE;
                DimensionRule rule   = RULE_SAME;
                bool          legal  = tryBinaryType( (BinaryOp)op, testAlgebraTypes[l], testAlgebraTypes[r], &result, &rule );
                bool          right  = row == NULL ? !legal
                                                   : legal && result == test_typeOfLetter( row->result ) && rule == row->rule;
                if ( !right )
                {
                    printf( "        %c %s %c is wrong\n", testAlgebraLetters[l], testOpNames[op], testAlgebraLetters[r] );
                    wrong++;
                }
            }
        }
        ut_reportTestResult( wrong == 0, "every combination of '%s'", testOpNames[op] );
    }

    // -s, -V, -M keep their type, and nothing else negates.
    uint32_t wrong = 0;
    for ( uint32_t i = 0 ; i < 6 ; i++ )
    {
        ValueType result = TYPE_NONE;
        char      letter = testAlgebraLetters[i];
        bool      legal  = tryUnaryType( UNARY_NEGATE, testAlgebraTypes[i], &result );
        bool      should = letter == 's' || letter == 'V' || letter == 'M';
        if ( legal != should || ( legal && result != testAlgebraTypes[i] ) ) wrong++;
    }
    ut_reportTestResult( wrong == 0, "unary '-'" );

    // ^O, ^M, ^F keep their type; ^s, ^P, ^V are errors.
    wrong = 0;
    for ( uint32_t i = 0 ; i < 6 ; i++ )
    {
        ValueType result = TYPE_NONE;
        char      letter = testAlgebraLetters[i];
        bool      legal  = tryUnaryType( UNARY_INVERSE, testAlgebraTypes[i], &result );
        bool      should = letter == 'O' || letter == 'M' || letter == 'F';
        if ( legal != should || ( legal && result != testAlgebraTypes[i] ) ) wrong++;
    }
    ut_reportTestResult( wrong == 0, "unary '^'" );

    ut_endModule();
}

//////////////////////////////////////////////////////////////////////////////
// Helpers
//////////////////////////////////////////////////////////////////////////////

// Every named value, as described at the top of this file.
static void test_typeSummary( const Model *m, char *out, uint32_t size )
{
    uint32_t used = 0;
    out[0] = '\0';
    for ( uint32_t i = 1 ; i < m->symbolCount ; i++ )
    {
        const Symbol *s = &m->symbols[i];
        if ( s->kind != SYM_VALUE ) continue;

        const Unit      *u     = &m->units[ s->unit ];
        const ValueInfo *named = &u->values[ s->node ];
        uint32_t         key   = u->ast.nodes[ s->node ].child;
        uint32_t         items = u->ast.nodes[ key ].next;
        double           factor = u->values[ u->ast.nodes[ items ].child ].factor;

        char type[64];
        char dimension[ DIMENSION_TEXT_SIZE ];
        char factorText[64] = "";
        ValueInfo_formatType( named, type, sizeof( type ) );
        Dimension_format( &named->dimension, dimension, sizeof( dimension ) );
        if ( factor != 0.0 ) snprintf( factorText, sizeof( factorText ), " factor %g", factor );

        int n = snprintf( out + used, size - used, "%s%.*s=%s %s%s", used > 0 ? " ; " : "", (int)s->length, s->name, type, dimension, factorText );
        if ( n > 0 ) used += (uint32_t)n;
    }
}

//////////////////////////////////////////////////////////////////////////////
// Models that check
//////////////////////////////////////////////////////////////////////////////

struct TypeCase
{
    const char *text;
    const char *summary;
};

static TypeCase typeCases[] =
{
    // literals, and the defaults of `units:` (D147)
    { "system s {\nunits: m, kg;\np = (0, 0, 1);\n}",              "p=point L factor 1" },
    { "system s {\nunits: in, lb;\np = (0, 0, 1);\n}",             "p=point L factor 0.0254" },
    { "system s {\np = (0, 0, 1) mm;\n}",                          "p=point L factor 0.001" },
    { "system s {\nx = 5;\n}",                                     "x=scalar 1" },
    { "system s {\nx = 5 kg;\n}",                                  "x=scalar M factor 1" },
    { "system s {\nv = [1, 2, 3];\n}",                             "v=list[3] 1" },
    { "system s {\nv = [1, 2, 3] m/s;\n}",                         "v=list[3] L T-1 factor 1" },
    { "system s {\nv = [1, 2] mm;\n}",                             "v=list[2] L factor 0.001" },
    { "system s {\ng = [[1 2; 3 4; 5 6]] N;\n}",                   "g=grid[3x2] L M T-2 factor 1" },
    { "system s {\nd = diag(1, 2, 3) kg-m2;\n}",                   "d=matrix L2 M factor 1" },
    { "system s {\nd = diag(1, 2, 3);\n}",                         "d=matrix 1" },
    { "system s {\nt = \"abc\";\n}",                               "t=string 1" },

    // `units:` is order-independent, and nested scopes inherit and override it
    { "system s {\np = (0, 0, 1);\nunits: m;\n}",                  "p=point L factor 1" },
    { "system s {\npoints { units: ft; p = (0, 0, 1); }\n}",       "p=point L factor 0.3048" },
    { "system s {\nunits: in, lb;\nbodies { b { points { q = (0, 0, 1); } mass: 12; cg: q; inertia: diag(1, 2, 3) lb-in2; } }\n}",
      "q=point L factor 0.0254" },
    { "system a {\nunits: in;\nsystem b { p = (0, 0, 1); }\n}",    "p=point L factor 0.0254" },
    { "system a {\nunits: in, lb;\nsystem b {\nunits: mm;\nbodies { c { mass: 2; cg: (0, 0, 0); inertia: diag(1, 2, 3) kg-m2; } }\np = (0, 0, 1);\n}\n}",
      "p=point L factor 0.001" },

    // named values, in any order; a name carries no unit of its own
    { "system s {\nunits: m;\nq = p;\np = (0, 0, 1);\n}",          "q=point L ; p=point L factor 1" },
    { "system s {\na = 2 m;\nb = a;\nc = b;\n}",                   "a=scalar L factor 1 ; b=scalar L ; c=scalar L" },

    // arithmetic on scalars (§3.4)
    { "system s {\na = 2 m;\nb = 3 m;\nc = a + b;\n}",             "a=scalar L factor 1 ; b=scalar L factor 1 ; c=scalar L" },
    { "system s {\na = 2 m;\nb = 3 mm;\nc = a - b;\n}",            "a=scalar L factor 1 ; b=scalar L factor 0.001 ; c=scalar L" },
    { "system s {\na = 2 m;\nb = 3 s;\nc = a * b;\n}",             "a=scalar L factor 1 ; b=scalar T factor 1 ; c=scalar L T" },
    { "system s {\na = 2 m;\nb = 3 s;\nc = a / b;\n}",             "a=scalar L factor 1 ; b=scalar T factor 1 ; c=scalar L T-1" },
    { "system s {\nx = 7 mod 3;\n}",                               "x=scalar 1" },
    { "system s {\nx = 7 m mod 3 m;\n}",                           "x=scalar L" },
    { "system s {\nx = -5 kg;\n}",                                 "x=scalar M" },
    { "system s {\nx = 2 * 3;\n}",                                 "x=scalar 1" },
    { "system s {\nx = pi;\n}",                                    "x=scalar 1" },
    { "system s {\nx = time;\n}",                                  "x=scalar T" },
    { "system s {\nx = 2 * pi * time;\n}",                         "x=scalar T" },

    // beside a value with a dimension, a bare literal takes that dimension in
    // a sum, and stays dimensionless in a product
    { "system s {\nunits: m;\nx = 2 m;\ny = x + 1;\n}",            "x=scalar L factor 1 ; y=scalar L" },
    { "system s {\nunits: m;\nx = 2 m;\ny = 1 + x;\n}",            "x=scalar L factor 1 ; y=scalar L" },
    { "system s {\nunits: m;\nx = 2 m;\ny = x mod 3;\n}",          "x=scalar L factor 1 ; y=scalar L" },
    { "system s {\nx = 2 m;\ny = 3 * x;\n}",                       "x=scalar L factor 1 ; y=scalar L" },
    { "system s {\nx = 1 + 2;\n}",                                 "x=scalar 1" },

    // points and vectors
    { "system s {\nunits: m;\np = (0, 0, 0);\nq = (0, 0, 1);\nv = q - p;\n}",
      "p=point L factor 1 ; q=point L factor 1 ; v=vector L" },
    { "system s {\nunits: m;\np = (0, 0, 0);\nq = p + [0, 0, 1];\n}",
      "p=point L factor 1 ; q=point L" },
    { "system s {\nunits: m;\np = (0, 0, 0);\nq = [0, 0, 1] + p;\n}",
      "p=point L factor 1 ; q=point L" },
    { "system s {\nunits: m;\nq = [0, 0, 1] + (1, 2, 3);\n}",      "q=point L" },
    { "system s {\nunits: in;\np = (0, 0, 0);\nq = p - [0, 0, 1];\n}",
      "p=point L factor 0.0254 ; q=point L" },
    { "system s {\nv = [1, 2, 3] + [4, 5, 6];\n}",                 "v=vector 1" },
    { "system s {\nv = 2 * [1, 2, 3] m;\n}",                       "v=vector L" },
    { "system s {\nv = [1, 2, 3] m / 2 s;\n}",                     "v=vector L T-1" },
    { "system s {\nv = -[1, 2, 3] m;\n}",                          "v=vector L" },
    { "system s {\nx = [1, 2, 3] N \\. [0, 1, 0] m;\n}",           "x=scalar L2 M T-2" },
    { "system s {\nv = [1, 2, 3] m \\x [0, 1, 0] N;\n}",           "v=vector L2 M T-2" },

    // matrices (D42, D143)
    { "system s {\nm = ^diag(1, 2, 3) kg-m2;\n}",                  "m=matrix L-2 M-1" },
    { "system s {\nv = diag(1, 2, 3) kg-m2 \\> [0, 0, 1] rad/s;\n}", "v=vector L2 M T-1 A" },
    { "system s {\nm = [[1 0 0; 0 1 0; 0 0 1]] + diag(1, 2, 3);\n}", "m=matrix 1" },

    // built-in functions (§4.1)
    { "system s {\nunits: m;\np = mid((0, 0, 0), (0, 0, 1));\n}",  "p=point L" },
    { "system s {\nunits: m;\na = (0, 0, 0);\np = avg(a, a, a);\n}", "a=point L factor 1 ; p=point L" },
    { "system s {\nx = sin(90 deg);\n}",                           "x=scalar 1" },
    { "system s {\nunits: deg;\nx = sin(90);\n}",                  "x=scalar 1" },
    { "system s {\nx = 0.5 in * cos(1 rad);\n}",                   "x=scalar L" },
    { "system s {\nx = asin(0.5);\n}",                             "x=scalar A" },
    { "system s {\nx = atan2(1 m, 2 m);\n}",                       "x=scalar A" },
    { "system s {\nx = atan2(1, 2);\n}",                           "x=scalar A" },
    { "system s {\nx = sqrt(4);\n}",                               "x=scalar 1" },
    { "system s {\nx = sqrt(4) m;\n}",                             "x=scalar L factor 1" },
    { "system s {\nx = abs(-2 m);\n}",                             "x=scalar L" },
    { "system s {\nx = min(1 m, 2 m, 3 m);\n}",                    "x=scalar L" },
    { "system s {\nunits: m;\nx = max(2, 1 m);\n}",                "x=scalar L" },

    // properties against the catalog (§4)
    { "gravity: [0, 0, -9.81] m/s2;\nsystem s { }",                "" },
    { "system s {\nunits: m;\np = (0, 0, 0);\nj: revolute, at: p, axis: [0, 1, 0];\n}", "p=point L factor 1" },
    { "system s {\nunits: m;\np = (0, 0, 0);\nj: revolute { at: p; axis: [1, 0, 0]; }\n}", "p=point L factor 1" },
    { "system s {\nunits: m;\np = (0, 0, 0);\nq = (0, 0, 1);\nj: revolute, at: p, axis: q - p;\n}",
      "p=point L factor 1 ; q=point L factor 1" },
    { "system s {\nunits: m;\np = (0, 0, 0);\nv = [0, 1, 0];\nj: revolute, at: p, axis: v;\n}",
      "p=point L factor 1 ; v=list[3] 1" },
    { "system s {\nbodies { b { mass: 1 kg; cg: (0, 0, 0) m; inertia: [[1 0 0; 0 1 0; 0 0 1]] kg-m2; } }\n}", "" },
    { "system s {\nunits: m;\np = (0, 0, 0);\nc = TSDA, rate: 10 N/m, damping: 2 N-s/m, free: 0.3;\nk: c, p1: p, p2: p;\n}",
      "p=point L factor 1" },
    { "system s {\nunits: m;\nc = TSDA { rate: 10 N/m; free: 300 mm; }\n}", "" },

    // the pound, by the property that expects it (D145)
    { "system s {\nunits: in;\nbodies { b { mass: 12 lb; cg: (0, 0, 0); inertia: diag(1, 2, 3) lb-in2; } }\n}", "" },
    { "system s {\nunits: in;\np = (0, 0, 0);\nc: TSDA, rate: 250 lb/in, damping: 15 lb-s/in, free: 8, p1: p, p2: p;\n}",
      "p=point L factor 0.0254" },
};

static void test_typeCases( void )
{
    ut_beginModule( "Types" );
    for ( uint32_t i = 0 ; i < sizeof( typeCases ) / sizeof( typeCases[0] ) ; i++ )
    {
        TypeCase *kase = &typeCases[i];

        char shown[256];
        test_showText( shown, sizeof( shown ), kase->text );

        Unit unit;
        if ( !test_makeUnit( &unit, "a.ombd", kase->text ) )
        {
            ut_reportTestResult( false, "%s", shown );
            continue;
        }

        Model     model;
        LoadError error;
        bool      loaded  = tryLoadModel( &unit, 1, &model, &error );
        bool      checked = loaded && tryCheckTypes( &model, &error );
        char      summary[1024];
        summary[0] = '\0';
        if ( checked ) test_typeSummary( &model, summary, sizeof( summary ) );

        bool pass = checked && strcmp( summary, kase->summary ) == 0;
        ut_reportTestResult( pass, "%s", shown );
        if ( !checked )
        {
            printf( "        error at %u:%u: %s\n", error.line, error.col, error.message );
        }
        else if ( !pass )
        {
            printf( "        expected %s\n", kase->summary );
            printf( "        got      %s\n", summary );
        }

        if ( loaded ) Model_free( &model );
        test_freeUnit( &unit );
    }
    ut_endModule();
}

//////////////////////////////////////////////////////////////////////////////
// Models that don't
//////////////////////////////////////////////////////////////////////////////

struct TypeErrorCase
{
    const char *text;
    uint32_t    line;
    uint32_t    col;
    const char *fragment;
};

static TypeErrorCase typeErrorCases[] =
{
    // `units:` (D147)
    { "system s {\nunits: N;\n}",                                  2, 8,  "'N' is not a unit of length" },
    { "system s {\nunits: rpm;\n}",                                2, 8,  "'rpm' is not a unit of length" },
    { "system s {\nunits: x;\n}",                                  2, 8,  "'x' is not a unit of length" },
    { "system s {\nunits: m, mm;\n}",                              2, 11, "sets length more than once" },
    { "system s {\nunits: 3;\n}",                                  2, 8,  "is the name of a unit" },

    // a value with no unit (D147)
    { "system s {\np = (0, 0, 0);\n}",                             2, 5,  "no 'units:' default applies to dimension L" },
    { "system s {\nunits: kg;\np = (0, 0, 0);\n}",                 3, 5,  "no 'units:' default applies to dimension L" },
    { "system s {\nbodies { b { mass: 10; } }\n}",                 2, 20, "no 'units:' default applies to dimension M" },
    { "system s {\nunits: m, kg;\nbodies { b { inertia: diag(1, 2, 3); } }\n}", 3, 23, "must carry a unit" },
    { "gravity: [0, 0, -9.81];\nsystem s { }",                     1, 10, "must carry a unit" },
    { "system s {\nc = TSDA, rate: 10;\n}",                        2, 17, "must carry a unit" },

    // unit runs, reported where they go wrong
    { "system s {\nx = 5 furlong;\n}",                             2, 7,  "unknown unit 'furlong'" },
    { "system s {\nx = 5 m-x;\n}",                                 2, 9,  "unknown unit 'x'" },
    { "system s {\nx = 5 lb;\n}",                                  2, 7,  "needs an expected dimension" },
    { "system s {\nbodies { b { cg: (0, 0, 0) lb; } }\n}",         2, 28, "fits neither mass nor force" },

    // a point is a length
    { "system s {\np = (0, 0, 0) kg;\n}",                          2, 15, "a point is a length" },

    // the closure table (§3.4)
    { "system s {\na = (0, 0, 0) m;\nb = (0, 0, 1) m;\nc = a + b;\n}", 4, 7, "no operation '+' between point and point (§3.4; use avg or mid, D40)" },
    { "system s {\na = (0, 0, 0) m;\nc = 2 * a;\n}",               3, 7,  "no operation '*' between scalar and point (§3.4; use avg or mid, D40)" },
    { "system s {\na = (0, 0, 0) m;\nb = (0, 0, 1) m;\nc = a \\. b;\n}", 4, 7, "no operation '\\.' between point and point" },
    { "system s {\na = (0, 0, 0) m;\nb = (0, 0, 1) m;\nc = a \\x b;\n}", 4, 7, "no operation '\\x' between point and point" },
    { "system s {\na = (0, 0, 0) m;\nc = -a;\n}",                  3, 5,  "no operation '-' on a point" },
    { "system s {\nx = ^2;\n}",                                    2, 5,  "no operation '^' on a scalar" },
    { "system s {\nv = [1, 2] + [3, 4];\n}",                       2, 12, "no operation '+' between list[2] and list[2]" },

    // units that must agree
    { "system s {\nx = 1 m + 1 s;\n}",                             2, 9,  "must agree, and these are L and T" },
    { "system s {\nx = 1 m mod 1 s;\n}",                           2, 9,  "must agree, and these are L and T" },
    { "system s {\nx = 1 m + 1;\n}",                               2, 11, "no 'units:' default applies to dimension L" },
    { "system s {\nx = 1 N + 1;\n}",                               2, 11, "must carry a unit" },

    // a value's meaning is fixed where it's defined
    { "system s {\nx = 5;\nbodies { b { mass: x; } }\n}",          3, 20, "'mass' takes dimension M, and this has dimension 1" },

    // properties against the catalog (§4)
    { "system s {\nbodies { b { mass: 10 N; } }\n}",               2, 20, "'mass' takes dimension M, and this has dimension L M T-2" },
    { "system s {\nbodies { b { cg: [0, 0, 0] m; } }\n}",          2, 18, "'cg' takes a point, and this is a list[3]" },
    { "system s {\nunits: m;\nj: revolute, at: (0, 0, 0), axis: (0, 1, 0);\n}", 3, 35, "'axis' takes a vector, and this is a point" },
    { "system s {\nunits: m;\nj: revolute, at: (0, 0, 0), axis: [0, 1];\n}", 3, 35, "'axis' takes a vector, and this is a list[2]" },
    { "system s {\nbodies { b { inertia: [[1 0; 0 1]] kg-m2; } }\n}", 2, 23, "'inertia' takes a matrix, and this is a grid[2x2]" },

    // functions (§4.1)
    { "system s {\nx = sin(2 * pi);\n}",                           2, 9,  "'sin' takes dimension A here, and this has dimension 1" },
    { "system s {\nx = sin(1);\n}",                                2, 9,  "no 'units:' default applies to dimension A" },
    { "system s {\nx = sqrt(1 m);\n}",                             2, 10, "'sqrt' takes dimension 1 here, and this has dimension L" },
    { "system s {\nx = asin(1 m);\n}",                             2, 10, "'asin' takes dimension 1 here" },
    { "system s {\nx = sin([1, 2, 3] rad);\n}",                    2, 9,  "'sin' takes a scalar here, and this is a list[3]" },
    { "system s {\nx = mid(1);\n}",                                2, 5,  "'mid' takes 2 arguments, and this has 1" },
    { "system s {\nx = sin(1 rad, 2 rad);\n}",                     2, 5,  "'sin' takes 1 argument, and this has 2" },
    { "system s {\nx = min(1);\n}",                                2, 5,  "'min' takes at least 2 arguments, and this has 1" },
    { "system s {\nx = diag(1, 2);\n}",                            2, 5,  "'diag' takes 3 arguments, and this has 2" },
    { "system s {\nx = mid(1, 2);\n}",                             2, 9,  "'mid' takes a point here, and this is a scalar" },
    { "system s {\nx = min(1 m, 1 s);\n}",                         2, 14, "'min' takes dimension L here, and this has dimension T" },
    { "system s {\nx = atan2(1 m, 1 s);\n}",                       2, 16, "'atan2' takes dimension L here, and this has dimension T" },
    { "system s {\nx = abs(1 m) s;\n}",                            2, 14, "a unit can follow only a dimensionless value" },

    // cycles
    { "system s {\na = a;\n}",                                     2, 5,  "'a' is defined in terms of itself" },
    { "system s {\na = b;\nb = a;\n}",                             3, 5,  "'a' is defined in terms of itself" },
    { "system s {\na = b + 1;\nb = c * 2;\nc = -a;\n}",            4, 6,  "'a' is defined in terms of itself" },

    // bracket entries
    { "system s {\nx = (0, [1, 2, 3], 0);\n}",                     2, 9,  "the entries of a point are scalars" },
    { "system s {\nx = [ [1, 2, 3], 4];\n}",                       2, 7,  "the entries of a list are scalars" },

    // not implemented yet
    { "system s {\nunits: m;\ny = 1 m;\nx = (y, 0, 0);\n}",        4, 6,  "not implemented yet: an entry with a dimension" },
    { "system s {\nbodies { b { } }\nx = b;\n}",                   3, 5,  "not implemented yet: 'b' used as a value" },
    { "system s {\nx = world;\n}",                                 2, 5,  "not implemented yet: 'world' used as a value" },
    { "system s {\nbodies { b { } }\nx = b.cg;\n}",                3, 5,  "not implemented yet: the type of a ACCESS" },
    { "system s {\nx = <1>;\n}",                                   2, 5,  "not implemented yet: the type of a QUATERNION" },
    { "system s {\na = 1;\nj.angle.pos <- a;\nj: revolute, at: a, axis: a;\n}", 3, 1, "not implemented yet: the type of a driver" },
    { "system s {\na = 1;\nsignals { a; }\n}",                     3, 11, "not implemented yet: the type of a bare reference" },
};

static void test_typeErrorCases( void )
{
    ut_beginModule( "Type errors" );
    for ( uint32_t i = 0 ; i < sizeof( typeErrorCases ) / sizeof( typeErrorCases[0] ) ; i++ )
    {
        TypeErrorCase *kase = &typeErrorCases[i];

        char shown[256];
        test_showText( shown, sizeof( shown ), kase->text );

        Unit unit;
        if ( !test_makeUnit( &unit, "a.ombd", kase->text ) )
        {
            ut_reportTestResult( false, "%s", shown );
            continue;
        }

        Model     model;
        LoadError error;
        bool      loaded  = tryLoadModel( &unit, 1, &model, &error );
        bool      checked = loaded && tryCheckTypes( &model, &error );
        bool      pass    = loaded && !checked
                         && error.line == kase->line
                         && error.col  == kase->col
                         && strstr( error.message, kase->fragment ) != NULL;

        ut_reportTestResult( pass, "%s  at %u:%u", shown, kase->line, kase->col );
        if ( checked )    printf( "        checked without error\n" );
        else if ( !pass ) printf( "        %s %u:%u: %s\n", loaded ? "got" : "failed to load at", error.line, error.col, error.message );

        if ( loaded ) Model_free( &model );
        test_freeUnit( &unit );
    }
    ut_endModule();
}

//////////////////////////////////////////////////////////////////////////////
// Golden dump
//////////////////////////////////////////////////////////////////////////////

// Check `examplesDir/name.ombd` and compare its dump with `goldenDir/name.types`.
static void test_goldenTypes( const char *examplesDir, const char *goldenDir, const char *name )
{
    char modelPath[1024];
    char goldenPath[1024];
    snprintf( modelPath,  sizeof( modelPath ),  "%s/%s.ombd",  examplesDir, name );
    snprintf( goldenPath, sizeof( goldenPath ), "%s/%s.types", goldenDir,   name );

    Source source;
    Source golden;
    bool   haveModel  = tryReadFile( modelPath,  &source );
    bool   haveGolden = tryReadFile( goldenPath, &golden );
    bool   pass       = false;
    char  *dump       = NULL;

    if ( haveModel && haveGolden )
    {
        Unit unit;
        if ( test_makeUnit( &unit, modelPath, source.text ) )
        {
            Model     model;
            LoadError error;
            bool      loaded = tryLoadModel( &unit, 1, &model, &error );
            if ( loaded && tryCheckTypes( &model, &error ) )
            {
                uint32_t size = Model_formatTypes( &model, NULL, 0 );
                dump = (char *)malloc( (size_t)size + 1 );
                Model_formatTypes( &model, dump, size + 1 );
                pass = size == golden.size && memcmp( dump, golden.text, size ) == 0;
            }
            else
            {
                printf( "        error at %u:%u: %s\n", error.line, error.col, error.message );
            }
            if ( loaded ) Model_free( &model );
            test_freeUnit( &unit );
        }
    }
    ut_reportTestResult( pass, "%s.ombd matches %s.types", name, name );

    free( dump );
    if ( haveModel )  Source_free( &source );
    if ( haveGolden ) Source_free( &golden );
}

//////////////////////////////////////////////////////////////////////////////
// All type tests
//////////////////////////////////////////////////////////////////////////////

void types_ut( const char *examplesDir, const char *goldenDir )
{
    test_binaryClosure();
    test_typeCases();
    test_typeErrorCases();

    ut_beginModule( "Golden types" );
    test_goldenTypes( examplesDir, goldenDir, "Pendulum" );
    ut_endModule();
}

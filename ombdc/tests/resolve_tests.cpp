// resolve_tests.cpp -- table-driven tests of loading and name resolution.
//
// A case gives one or two texts. A successful load is compared as three
// strings:
//   root        the model's system, or NULL for a library text
//   symbols     every user symbol in the order declared, as KIND name@owner,
//               with :type added when the symbol has a catalog type
//   references  every resolved name in tree order, as name->KIND@owner
// where `owner` is the name of the system, body or namespace that holds the
// symbol, or `top`, or `builtin`.
//
// Failing loads give the position of the error and a fragment of its message.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/file.h"
#include "../src/lexer.h"
#include "../src/parser.h"
#include "../src/resolve.h"
#include "unit_test.h"

//////////////////////////////////////////////////////////////////////////////
// Helpers
//////////////////////////////////////////////////////////////////////////////

// Lex and parse `text` into a unit. A test's texts must be valid to this point.
static bool test_makeUnit( Unit *unit, const char *path, const char *text )
{
    LexError   lexError;
    ParseError parseError;

    Source_make( &unit->source, path, text, (uint32_t)strlen( text ) );
    unit->info = NULL;
    if ( !tryLex( &unit->source, &unit->tokens, &lexError ) )
    {
        printf( "        lexical error at %u:%u\n", lexError.line, lexError.col );
        Source_free( &unit->source );
        return false;
    }
    if ( !tryParse( &unit->source, &unit->tokens, &unit->ast, &parseError ) )
    {
        printf( "        parse error at %u:%u: %s\n", parseError.line, parseError.col, parseError.message );
        TokenArray_free( &unit->tokens );
        Source_free( &unit->source );
        return false;
    }
    return true;
}

static void test_freeUnit( Unit *unit )
{
    Ast_free( &unit->ast );
    TokenArray_free( &unit->tokens );
    Source_free( &unit->source );
}

// Make one or two units; `text2` may be NULL. Returns the count, or 0.
static uint32_t test_makeUnits( Unit *units, const char *text1, const char *text2 )
{
    if ( !test_makeUnit( &units[0], "a.ombd", text1 ) ) return 0;
    if ( text2 == NULL ) return 1;
    if ( !test_makeUnit( &units[1], "b.ombd", text2 ) )
    {
        test_freeUnit( &units[0] );
        return 0;
    }
    return 2;
}

// A type or assembly case is a model, which states its gravity (D184). Unless
// the case sets its own, the gravity is loaded as a second text, so that the
// case's positions stay as written. Returns the count, or 0.
static uint32_t test_makeModelUnits( Unit *units, const char *text )
{
    const char *gravity = strstr( text, "gravity:" ) != NULL ? NULL : "gravity: [0, 0, -1] m/s2;";
    return test_makeUnits( units, text, gravity );
}

static void test_freeUnits( Unit *units, uint32_t count )
{
    for ( uint32_t u = 0 ; u < count ; u++ ) test_freeUnit( &units[u] );
}

static void test_showText( char *out, uint32_t size, const char *text )
{
    uint32_t n = 0;
    for ( const char *s = text ; *s != '\0' && n + 3 < size ; s++ )
    {
        if ( *s == '\n' ) { out[n++] = '\\'; out[n++] = 'n'; }
        else              { out[n++] = *s; }
    }
    out[n] = '\0';
}

static void test_addWord( char *out, uint32_t size, uint32_t *used, const char *format, const char *a, int aLen,
                          const char *b, const char *c, int cLen )
{
    if ( *used > 0 && *used + 1 < size ) out[ ( *used )++ ] = ' ';
    if ( *used >= size ) return;
    int n = snprintf( out + *used, size - *used, format, aLen, a, b, cLen, c );
    if ( n > 0 ) *used += (uint32_t)n;
}

// The name of whatever holds this symbol.
static void test_ownerOf( const Model *m, const Symbol *s, const char **name, int *length )
{
    const Scope *scope = &m->scopes[ s->scope ];
    if ( scope->kind == SCOPE_BUILTIN ) { *name = "builtin"; *length = 7; return; }
    if ( scope->kind == SCOPE_TOP )     { *name = "top";     *length = 3; return; }
    if ( scope->kind == SCOPE_SIMULATION ) { *name = "simulation"; *length = 10; return; }
    const Symbol *owner = &m->symbols[ scope->owner ];
    *name   = owner->name;
    *length = (int)owner->length;
}

static void test_symbolSummary( const Model *m, char *out, uint32_t size )
{
    uint32_t used = 0;
    out[0] = '\0';
    for ( uint32_t i = 1 ; i < m->symbolCount ; i++ )
    {
        const Symbol *s = &m->symbols[i];
        if ( m->scopes[ s->scope ].kind == SCOPE_BUILTIN ) continue;

        const char *owner;
        int         ownerLength;
        test_ownerOf( m, s, &owner, &ownerLength );

        char word[128];
        if ( s->type != NULL )
        {
            snprintf( word, sizeof( word ), "%s %.*s@%.*s:%s", SymbolKind_name( s->kind ), (int)s->length, s->name,
                      ownerLength, owner, s->type->name );
        }
        else
        {
            snprintf( word, sizeof( word ), "%s %.*s@%.*s", SymbolKind_name( s->kind ), (int)s->length, s->name,
                      ownerLength, owner );
        }
        if ( used > 0 && used + 1 < size ) out[ used++ ] = ' ';
        int n = snprintf( out + used, size - used, "%s", word );
        if ( n > 0 ) used += (uint32_t)n;
    }
}

static void test_referenceWalk( const Model *m, uint32_t unit, uint32_t index, char *out, uint32_t size, uint32_t *used )
{
    const Unit     *u    = &m->units[ unit ];
    const NodeInfo *info = &u->info[ index ];
    bool isReference = info->role == ROLE_REFERENCE || info->role == ROLE_FUNCTION || info->role == ROLE_CONNECTOR
                    || info->role == ROLE_TYPE_HEAD || info->role == ROLE_PUBLISHED || info->role == ROLE_DEPENDENCY;
    if ( isReference )
    {
        const Symbol *s = &m->symbols[ info->symbol ];
        const char   *owner;
        int           ownerLength;
        test_ownerOf( m, s, &owner, &ownerLength );
        test_addWord( out, size, used, "%.*s->%s@%.*s", s->name, (int)s->length, SymbolKind_name( s->kind ), owner, ownerLength );
    }
    for ( uint32_t c = u->ast.nodes[ index ].child ; c != 0 ; c = u->ast.nodes[ c ].next )
    {
        test_referenceWalk( m, unit, c, out, size, used );
    }
}

static void test_referenceSummary( const Model *m, char *out, uint32_t size )
{
    uint32_t used = 0;
    out[0] = '\0';
    for ( uint32_t unit = 0 ; unit < m->unitCount ; unit++ ) test_referenceWalk( m, unit, 0, out, size, &used );
}

//////////////////////////////////////////////////////////////////////////////
// Successful loads
//////////////////////////////////////////////////////////////////////////////

// A model states its gravity (D184). A case that is a model starts with this,
// so that it shows only what it tests.
#define TEST_GRAVITY "gravity: [0, 0, -1] m/s2;\n"

struct LoadCase
{
    const char *text1;
    const char *text2;           // NULL for one text
    const char *root;            // NULL for a library text
    const char *symbols;
    const char *references;
};

static LoadCase loadCases[] =
{
    // the root (§3.1, D139)
    { "", NULL, NULL, "", "" },
    { TEST_GRAVITY "system s { }", NULL, "s", "SYSTEM s@top", "" },
    { TEST_GRAVITY "namespace { system s { } }", NULL, "s", "SYSTEM s@top", "" },
    { "namespace a { system s { } }", NULL, NULL, "NAMESPACE a@top SYSTEM s@a", "" },
    { TEST_GRAVITY "namespace a { system x { } }\nsystem s { }", NULL, "s", "NAMESPACE a@top SYSTEM x@a SYSTEM s@top", "" },

    // merging texts (§3.1, D11, D12)
    { "gravity: [0, 0, -1];", "system s { }", "s", "SYSTEM s@top", "" },
    { "namespace a { system x { } }", "namespace a { system y { } provides x; }", NULL,
      "NAMESPACE a@top SYSTEM x@a SYSTEM y@a", "x->SYSTEM@a" },
    { "namespace a { namespace b { system x { } } }", "namespace a.b { system y { } }", NULL,
      "NAMESPACE a@top NAMESPACE b@a SYSTEM x@b SYSTEM y@b", "" },
    { "namespace a.b { system x { } provides x; }", NULL, NULL,
      "NAMESPACE a@top NAMESPACE b@a SYSTEM x@b", "x->SYSTEM@b" },

    // sections are not scopes, and order does not matter (§0, §3.2, D22)
    { TEST_GRAVITY "system s {\npoints { p = (0, 0, 0); q = p; }\nsignals { r = q; }\n}", NULL, "s",
      "SYSTEM s@top VALUE p@s VALUE q@s VALUE r@s", "p->VALUE@s q->VALUE@s" },
    { TEST_GRAVITY "system s {\nsignals { r = q; }\npoints { q = 1; }\npoints { t = 2; }\n}", NULL, "s",
      "SYSTEM s@top VALUE r@s VALUE q@s VALUE t@s", "q->VALUE@s" },

    // bodies: lookup runs outward, and an inner name hides an outer one
    { TEST_GRAVITY "system s {\nbodies { b { cg: p; mass: 1; inertia: 1; } }\npoints { p = (0, 0, 0); }\n}", NULL, "s",
      "SYSTEM s@top BODY b@s VALUE p@s", "p->VALUE@s" },
    { TEST_GRAVITY "system s {\nbodies { b { points { q = (0, 0, 0); } cg: q; mass: 1; inertia: 1; } }\n}", NULL, "s",
      "SYSTEM s@top BODY b@s VALUE q@b", "q->VALUE@b" },
    { TEST_GRAVITY "system s {\np = 1;\nbodies { b { points { p = 2; } cg: p; mass: 1; inertia: 1; } }\n}", NULL, "s",
      "SYSTEM s@top VALUE p@s BODY b@s VALUE p@b", "p->VALUE@b" },
    { TEST_GRAVITY "system s {\nbodies { a { mass: 1; cg: (0, 0, 0); inertia: 1; } b { mass: 1; cg: (0, 0, 0); inertia: 1; } }\n}", NULL, "s", "SYSTEM s@top BODY a@s BODY b@s", "" },

    // properties of each element
    { TEST_GRAVITY "system s {\nunits: m, kg;\n}", NULL, "s", "SYSTEM s@top", "" },
    { TEST_GRAVITY "system s {\nbodies { b { mass: 10 kg; inertia: diag(1, 2, 3) kg-m2; cg: (0, 0, 0) m; } }\n}", NULL, "s",
      "SYSTEM s@top BODY b@s", "diag->BUILTIN_FUNCTION@builtin" },

    // instances of a built-in type, in line and block form (D152)
    { TEST_GRAVITY "system s {\np = (0, 0, 0);\nj: revolute, at: p, axis: [0, 1, 0];\n}", NULL, "s",
      "SYSTEM s@top VALUE p@s INSTANCE j@s:revolute", "revolute->BUILTIN_TYPE@builtin p->VALUE@s" },
    { TEST_GRAVITY "system s {\np = (0, 0, 0);\nj: revolute { at: p; axis: [0, 1, 0]; }\n}", NULL, "s",
      "SYSTEM s@top VALUE p@s INSTANCE j@s:revolute", "revolute->BUILTIN_TYPE@builtin p->VALUE@s" },
    { TEST_GRAVITY "system s {\np = 1;\nc: TSDA, rate: 1, free: 2, p1: p, p2: p;\n}", NULL, "s",
      "SYSTEM s@top VALUE p@s INSTANCE c@s:TSDA", "TSDA->BUILTIN_TYPE@builtin p->VALUE@s p->VALUE@s" },

    // shared definitions, in either order and either form (D142, D152)
    { TEST_GRAVITY "system s {\nc: coil, p1: a, p2: a;\ncoil = TSDA, rate: 1, free: 2;\na = 1;\n}", NULL, "s",
      "SYSTEM s@top INSTANCE c@s:TSDA DEFINITION coil@s:TSDA VALUE a@s",
      "coil->DEFINITION@s a->VALUE@s a->VALUE@s TSDA->BUILTIN_TYPE@builtin" },
    { TEST_GRAVITY "system s {\ncoil = TSDA { rate: 1; free: 2; }\na = 1;\nc: coil { p1: a; p2: a; }\n}", NULL, "s",
      "SYSTEM s@top DEFINITION coil@s:TSDA VALUE a@s INSTANCE c@s:TSDA",
      "TSDA->BUILTIN_TYPE@builtin coil->DEFINITION@s a->VALUE@s a->VALUE@s" },
    { TEST_GRAVITY "system s {\nd = TSDA, damping: 3;\n}", NULL, "s", "SYSTEM s@top DEFINITION d@s:TSDA", "TSDA->BUILTIN_TYPE@builtin" },

    // an instance or value may share a type's name (D144)
    { TEST_GRAVITY "system s {\na = 1;\nrevolute: revolute, at: a, axis: a;\n}", NULL, "s",
      "SYSTEM s@top VALUE a@s INSTANCE revolute@s:revolute", "revolute->BUILTIN_TYPE@builtin a->VALUE@s a->VALUE@s" },
    { TEST_GRAVITY "system s {\nTSDA = 1;\n}", NULL, "s", "SYSTEM s@top VALUE TSDA@s", "" },

    // nested systems (D138)
    { TEST_GRAVITY "system a {\nsystem b { p = 1; q = p; }\n}", NULL, "a",
      "SYSTEM a@top SYSTEM b@a VALUE p@b VALUE q@b", "p->VALUE@b" },

    // expressions look names up by position (§3.2)
    { TEST_GRAVITY "system s {\nx = sin(2 * pi * time) + y;\ny = 1;\n}", NULL, "s",
      "SYSTEM s@top VALUE x@s VALUE y@s",
      "sin->BUILTIN_FUNCTION@builtin pi->BUILTIN_VALUE@builtin time->BUILTIN_VALUE@builtin y->VALUE@s" },
    { TEST_GRAVITY "system s {\nx = 3 kg;\ny = [1 2 3] m/s2;\n}", NULL, "s", "SYSTEM s@top VALUE x@s VALUE y@s", "" },
    { TEST_GRAVITY "system s {\na = 1;\nb = 2;\nc = 3;\no = <| Z: a, Y': b, X'': c |>;\n}", NULL, "s",
      "SYSTEM s@top VALUE a@s VALUE b@s VALUE c@s VALUE o@s", "a->VALUE@s b->VALUE@s c->VALUE@s" },

    // only a path's first name is resolved so far
    { TEST_GRAVITY "system s {\nbodies { b { mass: 1; cg: (0, 0, 0); inertia: 1; } }\nx = b.cg.pos.x;\n}", NULL, "s", "SYSTEM s@top BODY b@s VALUE x@s", "b->BODY@s" },

    // drives, bare references and chains
    { TEST_GRAVITY "system s {\nj: revolute, at: a, axis: a;\na = 1;\nj.angle.pos <- a;\n}", NULL, "s",
      "SYSTEM s@top INSTANCE j@s:revolute VALUE a@s",
      "revolute->BUILTIN_TYPE@builtin a->VALUE@s a->VALUE@s j->INSTANCE@s a->VALUE@s" },
    { TEST_GRAVITY "system s {\noutputs_x = 1;\nsignals { outputs_x; }\n}", NULL, "s", "SYSTEM s@top VALUE outputs_x@s", "outputs_x->VALUE@s" },
    { TEST_GRAVITY "system s {\na = 1;\nj: revolute, at: a, axis: a;\nbodies { b { mass: 1; cg: (0, 0, 0); inertia: 1; } }\nassembly { world <j> b; }\n}", NULL, "s",
      "SYSTEM s@top VALUE a@s INSTANCE j@s:revolute BODY b@s",
      "revolute->BUILTIN_TYPE@builtin a->VALUE@s a->VALUE@s world->BUILTIN_VALUE@builtin j->INSTANCE@s b->BODY@s" },

    // a slot may hold a built-in connector type, written directly (D170)
    { TEST_GRAVITY "system s {\nbodies { b { mass: 1; cg: (0, 0, 0); inertia: 1; } }\nassembly { world <lock> b; }\n}", NULL, "s",
      "SYSTEM s@top BODY b@s", "world->BUILTIN_VALUE@builtin lock->BUILTIN_TYPE@builtin b->BODY@s" },

    // an assembly line reaches into nested systems by path, at any depth (D181, D182)
    { TEST_GRAVITY "system s {\nsystem t {\nbodies { u { mass: 1; cg: (0, 0, 0); inertia: 1; } }\nj: revolute, at: 1, axis: 1;\n}\n"
      "bodies { b { mass: 1; cg: (0, 0, 0); inertia: 1; } }\nassembly { b <t.j> t.u; }\n}", NULL, "s",
      "SYSTEM s@top SYSTEM t@s BODY b@s BODY u@t INSTANCE j@t:revolute",
      "revolute->BUILTIN_TYPE@builtin b->BODY@s j->INSTANCE@t t->SYSTEM@s u->BODY@t t->SYSTEM@s" },
    { TEST_GRAVITY "system s {\nsystem t {\nsystem v {\nj: revolute, at: 1, axis: 1;\n}\n}\nassembly { world <t.v.j> world; }\n}", NULL, "s",
      "SYSTEM s@top SYSTEM t@s SYSTEM v@t INSTANCE j@v:revolute",
      "revolute->BUILTIN_TYPE@builtin world->BUILTIN_VALUE@builtin j->INSTANCE@v t->SYSTEM@s v->SYSTEM@t world->BUILTIN_VALUE@builtin" },

    // a refinement takes its base's type, in either order and form, to any depth (D200)
    { TEST_GRAVITY "system s {\nstiff = coil, rate: 2;\ncoil = TSDA, rate: 1, free: 2;\na = 1;\nc: stiff, p1: a, p2: a;\n}", NULL, "s",
      "SYSTEM s@top DEFINITION stiff@s:TSDA DEFINITION coil@s:TSDA VALUE a@s INSTANCE c@s:TSDA",
      "coil->DEFINITION@s TSDA->BUILTIN_TYPE@builtin stiff->DEFINITION@s a->VALUE@s a->VALUE@s" },
    { TEST_GRAVITY "system s {\nc2 = c1 { damping: 3; }\nc1 = c0, rate: 2;\nc0 = TSDA, rate: 1;\n}", NULL, "s",
      "SYSTEM s@top DEFINITION c2@s:TSDA DEFINITION c1@s:TSDA DEFINITION c0@s:TSDA",
      "c1->DEFINITION@s c0->DEFINITION@s TSDA->BUILTIN_TYPE@builtin" },
    { TEST_GRAVITY "system s {\ncoil = TSDA, rate: 1;\nbodies { b { mass: 1; cg: 1; inertia: 1; points { soft = coil, rate: 0.5; } } }\n}", NULL, "s",
      "SYSTEM s@top DEFINITION coil@s:TSDA BODY b@s DEFINITION soft@b:TSDA", "TSDA->BUILTIN_TYPE@builtin coil->DEFINITION@s" },

    // an instance or a value may take a unit's name; only a definition may not (D177)
    { TEST_GRAVITY "system s {\ng = 9.81;\nm: TSDA, rate: 1, free: 2, p1: g, p2: g;\n}", NULL, "s",
      "SYSTEM s@top VALUE g@s INSTANCE m@s:TSDA", "TSDA->BUILTIN_TYPE@builtin g->VALUE@s g->VALUE@s" },

    // `simulation` is a scope under the root system, wherever it is written (D176)
    { TEST_GRAVITY "system s {\nx = 1;\n}\nsimulation {\nunits: in;\nhorizon: 1 s;\nsignals { y = x; }\noutputs { update: 1 ms; x; y; }\n}",
      NULL, "s", "SYSTEM s@top VALUE x@s VALUE y@simulation", "x->VALUE@s x->VALUE@s y->VALUE@simulation" },
    { "simulation { y = x; }", TEST_GRAVITY "system s { x = 1; }", "s", "SYSTEM s@top VALUE x@s VALUE y@simulation", "x->VALUE@s" },

    // every `use` names a loaded namespace or a provided system (D202)
    { "namespace lib { system x { } provides x; }", "namespace app { use lib; use lib.x; }", NULL,
      "NAMESPACE lib@top SYSTEM x@lib NAMESPACE app@top", "x->SYSTEM@lib lib->NAMESPACE@top x->SYSTEM@lib" },
    { "namespace a.b { }\nnamespace c { use a.b; }", NULL, NULL, "NAMESPACE a@top NAMESPACE b@a NAMESPACE c@top", "b->NAMESPACE@a" },
};

static void test_loadCases( void )
{
    ut_beginModule( "Loading" );
    for ( uint32_t i = 0 ; i < sizeof( loadCases ) / sizeof( loadCases[0] ) ; i++ )
    {
        LoadCase *kase = &loadCases[i];

        char shown[256];
        test_showText( shown, sizeof( shown ), kase->text1 );

        Unit     units[2];
        uint32_t count = test_makeUnits( units, kase->text1, kase->text2 );
        if ( count == 0 )
        {
            ut_reportTestResult( false, "%s", shown );
            continue;
        }

        Model     model;
        LoadError error;
        bool      loaded = tryLoadModel( units, count, &model, &error );
        bool      pass   = loaded;
        char      symbols[1024];
        char      references[1024];
        const char *root = "";
        char        rootName[64];
        symbols[0] = references[0] = '\0';

        if ( loaded )
        {
            test_symbolSummary( &model, symbols, sizeof( symbols ) );
            test_referenceSummary( &model, references, sizeof( references ) );

            bool rootMatches = kase->root == NULL && model.rootSystem == 0;
            if ( model.rootSystem != 0 )
            {
                const Symbol *r = &model.symbols[ model.rootSystem ];
                snprintf( rootName, sizeof( rootName ), "%.*s", (int)r->length, r->name );
                root = rootName;
                rootMatches = kase->root != NULL && strcmp( root, kase->root ) == 0;
            }
            pass = rootMatches
                && strcmp( symbols, kase->symbols ) == 0
                && strcmp( references, kase->references ) == 0;
        }

        ut_reportTestResult( pass, "%s", shown );
        if ( !loaded )
        {
            printf( "        load error at %u:%u: %s\n", error.line, error.col, error.message );
        }
        else if ( !pass )
        {
            printf( "        root:       expected %s, got %s\n", kase->root ? kase->root : "(none)", root );
            printf( "        symbols:    expected %s\n", kase->symbols );
            printf( "                    got      %s\n", symbols );
            printf( "        references: expected %s\n", kase->references );
            printf( "                    got      %s\n", references );
        }

        if ( loaded ) Model_free( &model );
        for ( uint32_t u = 0 ; u < count ; u++ ) test_freeUnit( &units[u] );
    }
    ut_endModule();
}

//////////////////////////////////////////////////////////////////////////////
// Load errors
//////////////////////////////////////////////////////////////////////////////

struct LoadErrorCase
{
    const char *text1;
    const char *text2;           // NULL for one text
    uint32_t    unit;            // which text the error is in, from 0
    uint32_t    line;
    uint32_t    col;
    const char *fragment;
};

static LoadErrorCase loadErrorCases[] =
{
    // the root and merging (§3.1, §4.11, D139)
    { "system a { }\nsystem b { }", NULL,            0, 2, 8,  "holds 2 systems, including 'a' and 'b'" },
    { "system a { }", "system b { }",                1, 1, 8,  "holds 2 systems, including 'a' and 'b'" },
    { "namespace { system a { } }\nsystem b { }", NULL, 0, 2, 8, "holds 2 systems" },
    { "namespace n { system a { } }", "namespace n { system a { } }", 1, 1, 22, "already declared in this scope, at a.ombd:1:22" },
    { "gravity: 1;\ngravity: 2;", NULL,              0, 2, 1,  "more than once" },
    { "gravity: 1;", "gravity: 2;",                  1, 1, 1,  "more than once" },
    { "simulation { }\nsimulation { }", NULL,        0, 2, 1,  "more than one 'simulation'" },
    { "simulation { }", NULL,                        0, 1, 1,  "a 'simulation' block needs a model, and the top level holds no system" },

    // a model states its gravity, after every other load error (D184)
    { "system s { }", NULL,                          0, 1, 8,  "the model states no 'gravity:'; state it once at the top level" },
    { "system s {\nbodies { b { } }\n}", NULL,      0, 2, 10, "'b' is missing the required property 'mass'" },

    // `simulation` is a child scope of the root system (D176)
    { "system s {\nx = 1;\n}\nsimulation {\nx = 2;\n}", NULL, 0, 5, 1, "'x' is already declared in the root system 's', at a.ombd:2:1; 'simulation' may not reuse its names (D176)" },
    { "system s {\nx = 1;\n}", "simulation {\nsignals { x = 2; }\n}", 1, 2, 11, "already declared in the root system 's', at a.ombd:2:1" },
    { "system s {\nbodies { b { points { q = 1; } } }\n}\nsimulation {\ny = q;\n}", NULL, 0, 5, 5, "unknown name 'q'" },
    { "system s {\nsystem t { p = 1; }\n}\nsimulation { y = p; }", NULL, 0, 4, 18, "unknown name 'p'" },
    { "system s { }\nsimulation {\nwidgets { }\n}", NULL, 0, 3, 1,  "'widgets' is not a section of 'simulation'" },
    { "system s { }\nsimulation {\noutputs { x = 1; }\n}", NULL, 0, 3, 11, "an 'outputs' block holds 'update:' and the paths to record" },
    { "system s { }\nsimulation {\noutputs { rate: 1; }\n}", NULL, 0, 3, 11, "an 'outputs' block holds 'update:'" },
    { "system s { }\nsimulation {\noutputs { update: 1 ms; update: 2 ms; }\n}", NULL, 0, 3, 25, "property 'update' is set twice" },
    { "system s { }\nsimulation {\nhorizon: 1 s;\nsignals { horizon: 2 s; }\n}", NULL, 0, 4, 11, "property 'horizon' is set twice" },
    { "system s { }\nsimulation {\nsystem t { }\n}", NULL, 0, 3, 1, "a system may not be declared inside 'simulation'" },
    { "system s { }\nsimulation {\nwidth: 2;\n}", NULL, 0, 3, 1, "'width' is not a property of the simulation" },

    // bare blocks (§3.2, D138)
    { "system s {\nwidgets { }\n}", NULL,            0, 2, 1,  "'widgets' is not a section of a system" },
    { "system s {\npoints { bodies { } }\n}", NULL,  0, 2, 10, "misplaced inside a section" },
    { "system s {\nbodies { b { joints { } } }\n}", NULL, 0, 2, 14, "'joints' is not a section of a body" },
    { "system s {\nbodies { b { system t { } } }\n}", NULL, 0, 2, 14, "may not be declared inside a body" },

    // uniqueness, across sections too (§3.2)
    { "system s {\na = 1;\na = 2;\n}", NULL,         0, 3, 1,  "already declared in this scope, at 2:1" },
    { "system s {\npoints { a = 1; }\nbodies { a { } }\n}", NULL, 0, 3, 10, "already declared in this scope, at 2:10" },
    { "system s {\na = 1;\na: revolute, at: a, axis: a;\n}", NULL, 0, 3, 1, "already declared" },
    { "system s {\ncoil = TSDA, rate: 1, free: 2;\ncoil: coil, p1: a, p2: a;\n}", NULL, 0, 3, 1, "already declared" },

    // no shadowing of built-ins (D141, D144)
    { "system s {\ntime = 1;\n}", NULL,              0, 2, 1,  "'time' is a built-in value" },
    { "system s {\nsin = 1;\n}", NULL,               0, 2, 1,  "'sin' is a built-in function" },
    { "system s {\nbodies { pi { } }\n}", NULL,      0, 2, 10, "'pi' is a built-in value" },
    { "system world { }", NULL,                      0, 1, 8,  "'world' is a built-in value" },
    { "system s {\nx: revolute, at: x, axis: x;\nmax: revolute, at: x, axis: x;\n}", NULL, 0, 3, 1, "'max' is a built-in function" },
    { "system s {\nrevolute = TSDA, rate: 1;\n}", NULL, 0, 2, 1, "'revolute' is a built-in type; a shared definition may not reuse its name" },

    // gravity belongs to the top level (D31)
    { "system s {\ngravity: [0, 0, -1];\n}", NULL,   0, 2, 1,  "gravity is set only at the top level" },

    // classifying `:` (§3.3, D151)
    { "system s {\nbodies { b { mas: 10 kg; } }\n}", NULL, 0, 2, 14, "'mas' is not a property of a body, and its head is not a type" },
    { "system s {\nx: 3;\n}", NULL,                  0, 2, 1,  "use '=' to name a value" },
    { "system s {\nx: (0, 0, 0);\n}", NULL,          0, 2, 1,  "use '=' to name a value" },
    { "system s {\np = 1;\nx: p;\n}", NULL,          0, 3, 1,  "its head 'p' is a value; use '=' to name a value" },
    { "system s {\nx: nosuch;\n}", NULL,             0, 2, 4,  "unknown type 'nosuch'" },
    { "system s {\nx: { }\n}", NULL,                 0, 2, 1,  "a bare block has no type" },
    { "system s {\nthrottle: 1, open;\n}", NULL,     0, 2, 1,  "not implemented yet: a typed open signal" },

    // classifying `=` (§3.3, D152)
    { "system s {\nhinge = revolute;\n}", NULL,      0, 2, 9,  "'revolute' has no law to share" },
    { "system s {\nc = TSDA;\n}", NULL,              0, 2, 1,  "must give at least one law property" },
    { "system s {\na = 1;\nc = TSDA, p1: a;\n}", NULL, 0, 3, 11, "'p1' is placement" },
    { "system s {\nc = nosuch { }\n}", NULL,         0, 2, 5,  "unknown type 'nosuch'" },
    { "system s {\nTSDA: revolute, at: TSDA, axis: TSDA;\ny = TSDA, rate: 1;\n}", NULL, 0, 3, 5, "'TSDA' is both a user name and a built-in type, so this '=' could copy the value or define a 'TSDA' (D199)" },
    { "system s {\na = b: 3;\n}", NULL,              0, 2, 5,  "the value, not a property" },
    { "system s {\nx = a, default: 0;\na = 1;\n}", NULL, 0, 2, 8, "not implemented yet: the properties of a value" },

    // refinement (D200)
    { "system s {\nd = TSDA, rate: 1;\nk = d;\n}", NULL, 0, 3, 1, "a refinement must set at least one law property; this would only rename 'd' (D200)" },
    { "system s {\nd = TSDA, rate: 1;\nk = d { }\n}", NULL, 0, 3, 1, "a refinement must set at least one law property" },
    { "system s {\na = b, rate: 1;\nb = a, rate: 2;\n}", NULL, 0, 3, 5, "these definitions refine each other in a cycle: b -> a -> b (D200)" },
    { "system s {\na = a, rate: 1;\n}", NULL,       0, 2, 5,  "refine each other in a cycle: a -> a" },
    { "system s {\nx = a, rate: 1;\na = b { rate: 1; }\nb = a { rate: 2; }\n}", NULL, 0, 4, 5, "in a cycle: b -> a -> b" },
    { "system s {\nc = TSDA, rate: 1;\nd = c, p1: 1;\n}", NULL, 0, 3, 8, "'p1' is placement; a definition holds law properties only" },
    { "system s {\nc = TSDA, rate: 1;\nd = c, damping: 2;\nj: d, rate: 3;\n}", NULL, 0, 4, 7, "must not restate it" },
    { "system s {\nc = TSDA, rate: 1;\nd = c, damping: 2;\na = 1;\nj: d, p1: a;\n}", NULL, 0, 5, 1, "'j' is missing the required property 'p2'" },
    { "system s {\nv = 1;\nd = v, rate: 1;\n}", NULL, 0, 3, 8, "not implemented yet: the properties of a value" },
    { "system s {\nc = TSDA, rate: 1;\nbodies { b { points { c = c, rate: 2; } } }\n}", NULL, 0, 3, 27, "refine each other in a cycle: c -> c" },

    // a definition may not take a unit's name (D177)
    { "system s {\nmm = TSDA, rate: 1;\n}", NULL,   0, 2, 1,  "'mm' is the name of a unit (§3.5), and a shared definition may not take one (D177)" },
    { "system s {\nlb = TSDA, rate: 1;\n}", NULL,   0, 2, 1,  "'lb' is the name of a unit" },
    { "system s {\nkN = TSDA { rate: 1; }\n}", NULL, 0, 2, 1, "'kN' is the name of a unit" },
    { "system s {\nc = TSDA, rate: 1;\ndeg = c, rate: 2;\n}", NULL, 0, 3, 1, "'deg' is the name of a unit" },

    // a connector is declared in the system, not in a body (D183)
    { "system s {\nbodies { b { j: revolute, at: 1, axis: 1; } }\n}", NULL, 0, 2, 14, "'j' is a revolute, a connector; a connector is declared in the system, not in a body (D183)" },
    { "system s {\nbodies { b { points { k: lock; } } }\n}", NULL, 0, 2, 23, "'k' is a lock, a connector" },

    // properties (§3.3, §4)
    { "system s {\na = 1;\nj: revolute, at: a, nosuch: a;\n}", NULL, 0, 3, 21, "'nosuch' is not a property of revolute" },
    { "system s {\na = 1;\nj: revolute, a;\n}", NULL, 0, 3, 14, "'revolute' takes no positional items" },
    { "system s {\na = 1;\nj: revolute, at: a, at: a;\n}", NULL, 0, 3, 21, "property 'at' is set twice" },
    { "system s {\nunits: m;\npoints { units: kg; }\n}", NULL, 0, 3, 10, "property 'units' is set twice; it was first set at 2:1" },
    { "system s {\nbodies { b { mass: 1; points { mass: 2; } } }\n}", NULL, 0, 2, 32, "property 'mass' is set twice" },
    { "system s {\nbodies { b { mass: 1, 2; } }\n}", NULL, 0, 2, 20, "property 'mass' takes one value" },
    { "system s {\nbodies { b { mass: { } } }\n}", NULL, 0, 2, 20, "takes a value, not a block" },
    { "system s {\na = 1;\nc = TSDA, rate: 1;\nj: c, rate: 2, p1: a;\n}", NULL, 0, 4, 7, "must not restate it" },
    { "system s {\na = 1;\nj: revolute { nosuch: a; }\n}", NULL, 0, 3, 15, "'nosuch' is not a property of revolute" },
    { "system s {\na = 1;\nj: revolute { p = 2; }\n}", NULL, 0, 3, 15, "not implemented yet: a statement other than" },

    // required properties, reported at the element's name after every other load error (§4, D161)
    { "system s {\nbodies { b { } }\n}", NULL,       0, 2, 10, "'b' is missing the required property 'mass'" },
    { "system s {\nbodies { b { mass: 1; inertia: 1; } }\n}", NULL, 0, 2, 10, "'b' is missing the required property 'cg'" },
    { "system s {\nbodies { b { mass: 1; points { cg: 0; } } }\n}", NULL, 0, 2, 10, "'b' is missing the required property 'inertia'" },
    { "system s {\na = 1;\nj: revolute, at: a;\n}", NULL, 0, 3, 1, "'j' is missing the required property 'axis'" },
    { "system s {\na = 1;\nj: revolute { axis: a; }\n}", NULL, 0, 3, 1, "'j' is missing the required property 'at'" },
    { "system s {\na = 1;\nc: TSDA, rate: 1, free: 2, p1: a;\n}", NULL, 0, 3, 1, "'c' is missing the required property 'p2'" },
    { "system s {\na = 1;\ncoil = TSDA, rate: 1, free: 2;\nc: coil, p1: a;\n}", NULL, 0, 4, 1, "'c' is missing the required property 'p2'" },
    { "system s {\nbodies { b { } }\nx = nosuch;\n}", NULL, 0, 3, 5, "unknown name 'nosuch'" },

    // lookup (§3.2, D2, D144)
    { "system s {\nx = nosuch;\n}", NULL,            0, 2, 5,  "unknown name 'nosuch'" },
    { "system s {\nbodies { b { cg: nosuch; } }\n}", NULL, 0, 2, 18, "unknown name 'nosuch'" },
    { "system s {\nx = nosuch(1);\n}", NULL,         0, 2, 5,  "unknown function 'nosuch'" },
    { "system s {\nf = 1;\nx = f(1);\n}", NULL,      0, 3, 5,  "unknown function 'f'" },
    { "system s {\nb = 1;\nx = b.c(1);\n}", NULL,    0, 3, 5,  "only a built-in function can be called" },
    { "system s {\nx = sin;\n}", NULL,               0, 2, 5,  "unknown name 'sin'" },
    { "system a {\np = 1;\nsystem b { q = p; }\n}", NULL, 0, 3, 16, "unknown name 'p'" },
    { "system a {\nsystem b { p = 1; }\nx = b.p;\n}", NULL, 0, 3, 5, "the system 'b' can't be read from outside it" },
    { "system a {\nsystem b { }\nx = b;\n}", NULL,   0, 3, 5,  "the system 'b' can't be read from outside it" },
    { "system a {\nx = a;\n}", NULL,                 0, 2, 5,  "unknown name 'a'" },
    { "gravity: g;\nsystem s { g = 1; }", NULL,      0, 1, 10, "unknown name 'g'" },

    // connector slots (§3.7)
    { "system s {\nbodies { b { } }\nassembly { world <nosuch> b; }\n}", NULL, 0, 3, 19, "'nosuch' is neither a declared connector nor a built-in connector type (§3.7, D170)" },
    { "system s {\np = 1;\nbodies { b { } }\nassembly { world <p> b; }\n}", NULL, 0, 4, 19, "'p' is a value; a slot holds a declared connector instance or a built-in connector type" },
    { "system s {\nd = TSDA, rate: 1;\nbodies { b { } }\nassembly { world <d> b; }\n}", NULL, 0, 4, 19, "'d' is a definition; declare an instance of it and wire that (D175)" },
    { "system s {\nlock: revolute, at: 1, axis: 1;\nbodies { b { } }\nassembly { world <lock> b; }\n}", NULL, 0, 4, 19, "'lock' is both a declared instance and a built-in connector type, so the slot can't tell which is meant (D175)" },

    // a slot path goes through nested systems to a connector (D181)
    { "system s {\nbodies { b { } }\nassembly { world <b.j> b; }\n}", NULL, 0, 3, 19, "'b' is a body, and no path reaches into a body; a connector is declared in the system (D181, D183)" },
    { "system s {\np = 1;\nassembly { world <p.j> world; }\n}", NULL, 0, 3, 19, "'p' is a value, not a nested system; a slot path names a connector of a nested system" },
    { "system s {\nassembly { world <q.j> world; }\n}", NULL, 0, 2, 19, "unknown name 'q'" },
    { "system s {\nsystem t { }\nassembly { world <t.j> world; }\n}", NULL, 0, 3, 21, "the system 't' declares no connector 'j' (D181)" },
    { "system s {\nsystem t { bodies { u { } } }\nassembly { world <t.u> world; }\n}", NULL, 0, 3, 21, "'u' is a body of the system 't', not a connector" },
    { "system s {\nsystem t { d = TSDA, rate: 1; }\nassembly { world <t.d> world; }\n}", NULL, 0, 3, 21, "'d' is a definition; declare an instance of it and wire that (D175)" },
    { "system s {\nsystem t { bodies { u { } } }\nassembly { world <t.u.j> world; }\n}", NULL, 0, 3, 21, "'u' is a body of the system 't'; a path goes on only through nested systems" },
    { "system s {\nsystem t { }\nassembly { world <t.v.j> world; }\n}", NULL, 0, 3, 21, "the system 't' declares no 'v'" },
    { "system a {\nsystem b { system c { j: lock; } }\nsystem d { assembly { world <b.c.j> world; } }\n}", NULL, 0, 3, 30, "unknown name 'b'" },

    // an endpoint path goes through nested systems to a body (D182)
    { "system s {\nsystem t { }\nassembly { world <lock> t.u; }\n}", NULL, 0, 3, 27, "the system 't' declares no 'u' (D182)" },
    { "system s {\nsystem t { }\nassembly { world <lock> t; }\n}", NULL, 0, 3, 25, "'t' is a nested system, not a body; name one of its bodies by path, as in t.bUCA (D182)" },
    { "system s {\nsystem t { system v { } }\nassembly { world <lock> t.v; }\n}", NULL, 0, 3, 27, "'v' is a nested system, not a body" },
    { "system s {\nsystem t { j: lock; }\nt.j.dof.pos <- 1;\n}", NULL, 0, 3, 1, "the system 't' can't be read from outside it; only an assembly line may name its members, by path (D2, D182)" },

    // every `use` must resolve (D202)
    { "namespace a { use b; }", NULL,                0, 1, 15, "'b' is not loaded, so this 'use' can't be met (D202)" },
    { "namespace a { use b.c; }", NULL,              0, 1, 15, "'b' is not loaded" },
    { "namespace b { }\nnamespace a { use b.c; }", NULL, 0, 2, 15, "'b.c' is not loaded" },
    { "namespace lib { system x { } }\nnamespace a { use lib.x; }", NULL, 0, 2, 15, "'lib.x' is not loaded: the system 'x' is private to the namespace 'lib', which does not provide it (D8, D202)" },
    { "namespace a { use lib.x; }", "namespace lib { system x { } }", 0, 1, 15, "private to the namespace 'lib'" },
    { "system s { }\nnamespace a { use s; }", NULL,  0, 2, 15, "'s' is not loaded" },
    { "system s {\nj: revolute, at: 1, axis: 1;\nassembly { nosuch <j> world; }\n}", NULL, 0, 3, 12, "unknown name 'nosuch'" },

    // `provides` (D8)
    { "namespace a { provides x; }", NULL,           0, 1, 24, "no system named 'x'" },

    // not implemented yet
    { "system s {\na[i] = 1 for i in 0..1;\n}", NULL, 0, 2, 1, "not implemented yet: a comprehension" },
    { "system s {\na: revolute, at: 1, axis: 1 for i in 0..1;\n}", NULL, 0, 2, 1, "not implemented yet: a comprehension" },
    { "system s {\nassembly { a <j> b for i in 0..1; }\n}", NULL, 0, 2, 20, "not implemented yet: a comprehension" },
    { "system s {\nx = f(map { });\n}", NULL,        0, 2, 5,  "unknown function 'f'" },
    { "system s {\nx = sin(map { });\n}", NULL,      0, 2, 9,  "not implemented yet: a typed block inside an expression" },
};

static void test_loadErrorCases( void )
{
    ut_beginModule( "Load errors" );
    for ( uint32_t i = 0 ; i < sizeof( loadErrorCases ) / sizeof( loadErrorCases[0] ) ; i++ )
    {
        LoadErrorCase *kase = &loadErrorCases[i];

        char shown[256];
        test_showText( shown, sizeof( shown ), kase->text1 );

        Unit     units[2];
        uint32_t count = test_makeUnits( units, kase->text1, kase->text2 );
        if ( count == 0 )
        {
            ut_reportTestResult( false, "%s", shown );
            continue;
        }

        Model     model;
        LoadError error;
        bool      loaded = tryLoadModel( units, count, &model, &error );
        bool      pass   = !loaded
                        && error.unit == kase->unit
                        && error.line == kase->line
                        && error.col  == kase->col
                        && strstr( error.message, kase->fragment ) != NULL;

        ut_reportTestResult( pass, "%s  at %u:%u", shown, kase->line, kase->col );
        if ( loaded )
        {
            printf( "        loaded without error\n" );
            Model_free( &model );
        }
        else if ( !pass )
        {
            printf( "        got unit %u, %u:%u: %s\n", error.unit, error.line, error.col, error.message );
        }
        for ( uint32_t u = 0 ; u < count ; u++ ) test_freeUnit( &units[u] );
    }
    ut_endModule();
}

//////////////////////////////////////////////////////////////////////////////
// Golden dump
//////////////////////////////////////////////////////////////////////////////

// Load `examplesDir/name.ombd` and compare its dump with `goldenDir/name.names`.
static void test_goldenNames( const char *examplesDir, const char *goldenDir, const char *name )
{
    char modelPath[1024];
    char goldenPath[1024];
    snprintf( modelPath,  sizeof( modelPath ),  "%s/%s.ombd",  examplesDir, name );
    snprintf( goldenPath, sizeof( goldenPath ), "%s/%s.names", goldenDir,   name );

    Source model;
    Source golden;
    bool   haveModel  = tryReadFile( modelPath,  &model );
    bool   haveGolden = tryReadFile( goldenPath, &golden );
    bool   pass       = false;
    char  *dump       = NULL;

    if ( haveModel && haveGolden )
    {
        Unit unit;
        if ( test_makeUnit( &unit, modelPath, model.text ) )
        {
            Model     loaded;
            LoadError error;
            if ( tryLoadModel( &unit, 1, &loaded, &error ) )
            {
                uint32_t size = Model_format( &loaded, NULL, 0 );
                dump = (char *)malloc( (size_t)size + 1 );
                Model_format( &loaded, dump, size + 1 );
                pass = size == golden.size && memcmp( dump, golden.text, size ) == 0;
                Model_free( &loaded );
            }
            else
            {
                printf( "        load error at %u:%u: %s\n", error.line, error.col, error.message );
            }
            test_freeUnit( &unit );
        }
    }
    ut_reportTestResult( pass, "%s.ombd matches %s.names", name, name );

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
// All loading tests
//////////////////////////////////////////////////////////////////////////////

void resolve_ut( const char *examplesDir, const char *goldenDir )
{
    test_loadCases();
    test_loadErrorCases();

    ut_beginModule( "Golden names" );
    test_goldenNames( examplesDir, goldenDir, "Pendulum" );
    ut_endModule();
}

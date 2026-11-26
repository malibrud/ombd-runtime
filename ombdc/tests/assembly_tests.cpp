// assembly_tests.cpp -- tests of connectors and assembly (Specification.md §3.7).
//
// Each case is a model run through loading, type checking and the assembly
// stage. A passing case is compared on a summary of its links, in order:
//     connector from->to ; connector from->to
// A failing case gives where the first error is, and a fragment of its message.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/assembly.h"
#include "../src/types.h"
#include "unit_test.h"

// Well-typed fillers, so that each case shows only its wiring.
#define TEST_BODY   "{ mass: 1 kg; cg: (0, 0, 0) m; inertia: diag(1, 1, 1) kg-m2; }"
#define TEST_JOINT  "revolute, at: (0, 0, 0) m, axis: [0, 1, 0]"
#define TEST_SPRING "TSDA, rate: 1 N/m, free: 1 m, p1: (0, 0, 0) m, p2: (0, 0, 0) m"

// The model's links, as above.
static void test_linkSummary( const Model *m, char *out, uint32_t size )
{
    uint32_t used = 0;
    out[0] = '\0';
    for ( uint32_t i = 0 ; i < m->linkCount ; i++ )
    {
        const Link   *link = &m->links[i];
        const Symbol *c    = &m->symbols[ link->connector ];
        const Symbol *from = &m->symbols[ link->from ];
        const Symbol *to   = &m->symbols[ link->to ];
        int n = snprintf( out + used, size - used, "%s%.*s %.*s->%.*s", i > 0 ? " ; " : "",
                          (int)c->length, c->name, (int)from->length, from->name, (int)to->length, to->name );
        if ( n > 0 && used + (uint32_t)n < size ) used += (uint32_t)n;
    }
}

// Run every stage through §3.7. `loaded` says whether the model needs freeing.
static bool test_checkAssembly( Unit *units, uint32_t count, Model *model, LoadError *error, bool *loaded )
{
    *loaded = tryLoadModel( units, count, model, error );
    return *loaded && tryCheckTypes( model, error ) && tryCheckAssembly( model, error );
}

//////////////////////////////////////////////////////////////////////////////
// Models that check
//////////////////////////////////////////////////////////////////////////////

struct AssemblyCase
{
    const char *text;
    const char *links;
};

static AssemblyCase assemblyCases[] =
{
    // no connectors, no links
    { "system s { units: m;\nbodies { a " TEST_BODY " }\n}", "" },

    // one link, in endpoint order (D159)
    { "system s { units: m;\nbodies { a " TEST_BODY " }\nj: " TEST_JOINT ";\nassembly { world <j> a; }\n}", "j world->a" },
    { "system s { units: m;\nbodies { a " TEST_BODY " }\nj: " TEST_JOINT ";\nassembly { a <j> world; }\n}", "j a->world" },

    // a chain line is a link per slot (D30)
    { "system s { units: m;\nbodies { a " TEST_BODY " b " TEST_BODY " }\nj: " TEST_JOINT ";\nk: " TEST_JOINT ";\n"
      "assembly { world <j> a <k> b; }\n}", "j world->a ; k a->b" },

    // a closed loop is lines meeting at the same bodies, and a TSDA joins two bodies (§4.4)
    { "system s { units: m;\nbodies { a " TEST_BODY " b " TEST_BODY " }\nj: " TEST_JOINT ";\nk: " TEST_JOINT ";\nc: " TEST_SPRING ";\n"
      "assembly { world <j> a; a <k> b; b <c> world; }\n}", "j world->a ; k a->b ; c b->world" },

    // an instance of a shared definition is a connector too (D152)
    { "system s { units: m;\nbodies { a " TEST_BODY " }\ncoil = TSDA, rate: 1 N/m, free: 1 m;\n"
      "c: coil, p1: (0, 0, 0) m, p2: (0, 0, 0) m;\nassembly { world <c> a; }\n}", "c world->a" },

    // sections carry no meaning, so links pool across `assembly` blocks, in any order (D22, §0)
    { "system s { units: m;\nassembly { a <k> b; }\nbodies { a " TEST_BODY " b " TEST_BODY " }\nj: " TEST_JOINT ";\nk: " TEST_JOINT ";\n"
      "assembly { world <j> a; }\n}", "k a->b ; j world->a" },

    // a nested system wires its own connectors
    { "system s { units: m;\nsystem t { units: m;\nbodies { a " TEST_BODY " }\nj: " TEST_JOINT ";\nassembly { world <j> a; }\n}\n}", "j world->a" },

    // or an enclosing system wires them, and its bodies, by path (D181, D182)
    { "system s { units: m;\nsystem t { units: m;\nj: " TEST_JOINT ";\n}\nbodies { b " TEST_BODY " }\nassembly { world <t.j> b; }\n}",
      "j world->b" },
    { "system s { units: m;\nsystem t { units: m;\nbodies { u " TEST_BODY " }\nj: " TEST_JOINT ";\n}\nbodies { b " TEST_BODY " }\n"
      "assembly { b <t.j> t.u; }\n}", "j b->u" },
    { "system s { units: m;\nsystem t { units: m;\nsystem v { units: m;\nbodies { u " TEST_BODY " }\nj: " TEST_JOINT ";\n}\n}\n"
      "assembly { world <t.v.j> t.v.u; }\n}", "j world->u" },

    // a built-in connector type with no properties, written in the slot, is a fresh instance at each use (D170)
    { "system s { units: m;\nbodies { a " TEST_BODY " b " TEST_BODY " }\nassembly { world <lock> a; a <lock> b; }\n}",
      "lock world->a ; lock a->b" },

    // each link is checked on its own, so a chain may close a loop (D179)
    { "system s { units: m;\nbodies { a " TEST_BODY " b " TEST_BODY " }\nj: " TEST_JOINT ";\nk: " TEST_JOINT ";\n"
      "assembly { world <lock> a; a <j> b <k> a; }\n}", "lock world->a ; j a->b ; k b->a" },
};

static void test_assemblyCases( void )
{
    ut_beginModule( "Assembly" );
    for ( uint32_t i = 0 ; i < sizeof( assemblyCases ) / sizeof( assemblyCases[0] ) ; i++ )
    {
        AssemblyCase *kase = &assemblyCases[i];

        char shown[512];
        test_showText( shown, sizeof( shown ), kase->text );

        Unit     units[2];
        uint32_t count = test_makeModelUnits( units, kase->text );
        if ( count == 0 )
        {
            ut_reportTestResult( false, "%s", shown );
            continue;
        }

        Model     model;
        LoadError error;
        bool      loaded;
        bool      checked = test_checkAssembly( units, count, &model, &error, &loaded );
        char      summary[512];
        summary[0] = '\0';
        if ( checked ) test_linkSummary( &model, summary, sizeof( summary ) );

        bool pass = checked && strcmp( summary, kase->links ) == 0;
        ut_reportTestResult( pass, "%s", shown );
        if ( !checked )
        {
            printf( "        error at %u:%u: %s\n", error.line, error.col, error.message );
        }
        else if ( !pass )
        {
            printf( "        expected %s\n", kase->links );
            printf( "        got      %s\n", summary );
        }

        if ( loaded ) Model_free( &model );
        test_freeUnits( units, count );
    }
    ut_endModule();
}

//////////////////////////////////////////////////////////////////////////////
// Models that don't
//////////////////////////////////////////////////////////////////////////////

struct AssemblyErrorCase
{
    const char *text;
    uint32_t    line;
    uint32_t    col;
    const char *fragment;
};

static AssemblyErrorCase assemblyErrorCases[] =
{
    // exactly one link (D162)
    { "system s { units: m;\nj: " TEST_JOINT ";\n}",                                                        2, 1,  "'j' is declared but never wired (D162)" },
    { "system s { units: m;\nc: " TEST_SPRING ";\n}",                                                       2, 1,  "'c' is declared but never wired" },
    { "system s { units: m;\nsystem t { units: m;\nj: " TEST_JOINT ";\n}\n}",                                         3, 1,  "'j' is declared but never wired" },
    { "system s { units: m;\nbodies { a " TEST_BODY " b " TEST_BODY " }\nj: " TEST_JOINT ";\nassembly { world <j> a <j> b; }\n}",
      4, 25, "'j' is already wired at 4:19; a connector is in exactly one link (D162)" },
    { "system s { units: m;\nbodies { a " TEST_BODY " }\nj: " TEST_JOINT ";\nassembly { world <j> a; }\nassembly { a <j> world; }\n}",
      5, 15, "'j' is already wired at 4:19" },

    // endpoints against the signature (§3.7, D159)
    { "system s { units: m;\np = 1;\nj: " TEST_JOINT ";\nassembly { world <j> p; }\n}",
      4, 22, "'p' is a value, but the second endpoint of the revolute 'j' must be a body or 'world'" },
    { "system s { units: m;\np = 1;\nbodies { a " TEST_BODY " }\nj: " TEST_JOINT ";\nassembly { p <j> a; }\n}",
      5, 12, "'p' is a value, but the first endpoint of the revolute 'j' must be a body or 'world'" },
    { "system s { units: m;\nj: " TEST_JOINT ";\nk: " TEST_JOINT ";\nassembly { world <j> k; }\n}",
      4, 22, "'k' is a revolute, but the second endpoint" },
    { "system s { units: m;\nj: " TEST_JOINT ";\nassembly { world <j> time; }\n}",
      3, 22, "'time' is a built-in value, but the second endpoint" },
    { "system s { units: m;\ncoil = TSDA, rate: 1 N/m, free: 1 m;\nc: coil, p1: (0, 0, 0) m, p2: (0, 0, 0) m;\nassembly { world <c> coil; }\n}",
      4, 22, "'coil' is a shared definition, but the second endpoint of the TSDA 'c' must be a body or 'world'" },

    // lookup stops at a nested system, so a bare name doesn't reach its connector; a path does (D169, D181)
    { "system s { units: m;\nsystem t { units: m;\nj: " TEST_JOINT ";\n}\nbodies { a " TEST_BODY " }\nassembly { world <j> a; }\n}",
      6, 19, "'j' is neither a declared connector nor a built-in connector type" },

    // exactly one link spans the system tree (D162, D181)
    { "system s { units: m;\nsystem t { units: m;\nbodies { u " TEST_BODY " }\nj: " TEST_JOINT ";\nassembly { world <j> u; }\n}\n"
      "assembly { world <t.j> t.u; }\n}", 7, 19, "'j' is already wired at 5:19" },

    // a path endpoint is checked against the signature like any other (D182)
    { "system s { units: m;\nsystem t { units: m;\nk: " TEST_JOINT ";\n}\nj: " TEST_JOINT ";\nassembly { world <j> t.k; }\n}",
      6, 22, "'k' is a revolute, but the second endpoint of the revolute 'j' must be a body or 'world'" },

    // a link's two endpoints differ (D179)
    { "system s { units: m;\nbodies { a " TEST_BODY " }\nj: " TEST_JOINT ";\nassembly { a <j> a; }\n}",
      4, 15, "'j' links 'a' to itself; a link's two endpoints must differ (D179)" },
    { "system s { units: m;\nj: " TEST_JOINT ";\nassembly { world <j> world; }\n}",
      3, 19, "'j' links 'world' to itself" },
    { "system s { units: m;\nsystem t { units: m;\nbodies { u " TEST_BODY " }\nj: " TEST_JOINT ";\n}\nassembly { t.u <t.j> t.u; }\n}",
      6, 17, "'j' links 'u' to itself" },

    // a built-in type in a slot has no properties, so it must need none (D170, §4)
    { "system s { units: m;\nbodies { a " TEST_BODY " }\nassembly { world <revolute> a; }\n}",
      3, 19, "a 'revolute' written in a slot is a fresh instance with no properties, and it requires 'at'" },

    // not implemented yet
    { "system s { units: m;\nbodies { a " TEST_BODY " }\nj: " TEST_JOINT ";\nassembly { world <j> a.flange; }\n}",
      4, 22, "not implemented yet: a port as an endpoint" },
};

static void test_assemblyErrorCases( void )
{
    ut_beginModule( "Assembly errors" );
    for ( uint32_t i = 0 ; i < sizeof( assemblyErrorCases ) / sizeof( assemblyErrorCases[0] ) ; i++ )
    {
        AssemblyErrorCase *kase = &assemblyErrorCases[i];

        char shown[512];
        test_showText( shown, sizeof( shown ), kase->text );

        Unit     units[2];
        uint32_t count = test_makeModelUnits( units, kase->text );
        if ( count == 0 )
        {
            ut_reportTestResult( false, "%s", shown );
            continue;
        }

        Model     model;
        LoadError error;
        bool      loaded;
        bool      checked = test_checkAssembly( units, count, &model, &error, &loaded );
        bool      pass    = !checked
                         && error.unit == 0
                         && error.line == kase->line
                         && error.col  == kase->col
                         && strstr( error.message, kase->fragment ) != NULL;

        ut_reportTestResult( pass, "%s  at %u:%u", shown, kase->line, kase->col );
        if ( checked )    printf( "        checked without error\n" );
        else if ( !pass ) printf( "        got %u:%u: %s\n", error.line, error.col, error.message );

        if ( loaded ) Model_free( &model );
        test_freeUnits( units, count );
    }
    ut_endModule();
}

//////////////////////////////////////////////////////////////////////////////
// Golden dump
//////////////////////////////////////////////////////////////////////////////

// Check `examplesDir/name.ombd` and compare its dump with `goldenDir/name.assembly`.
static void test_goldenAssembly( const char *examplesDir, const char *goldenDir, const char *name )
{
    char modelPath[1024];
    char goldenPath[1024];
    snprintf( modelPath,  sizeof( modelPath ),  "%s/%s.ombd",     examplesDir, name );
    snprintf( goldenPath, sizeof( goldenPath ), "%s/%s.assembly", goldenDir,   name );

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
            bool      loaded;
            if ( test_checkAssembly( &unit, 1, &model, &error, &loaded ) )
            {
                uint32_t size = Model_formatAssembly( &model, NULL, 0 );
                dump = (char *)malloc( (size_t)size + 1 );
                Model_formatAssembly( &model, dump, size + 1 );
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
    ut_reportTestResult( pass, "%s.ombd matches %s.assembly", name, name );

    free( dump );
    if ( haveModel )  Source_free( &source );
    if ( haveGolden ) Source_free( &golden );
}

//////////////////////////////////////////////////////////////////////////////
// All assembly tests
//////////////////////////////////////////////////////////////////////////////

void assembly_ut( const char *examplesDir, const char *goldenDir )
{
    test_assemblyCases();
    test_assemblyErrorCases();

    ut_beginModule( "Golden assembly" );
    test_goldenAssembly( examplesDir, goldenDir, "Pendulum" );
    ut_endModule();
}

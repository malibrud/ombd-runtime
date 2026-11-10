// units_tests.cpp -- table-driven tests of dimensions and unit runs.
//
// A case lexes a unit run and reads it. The expected dimension of a case, when
// it has one, is what settles a bare `lb`.

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../src/file.h"
#include "../src/lexer.h"
#include "../src/units.h"
#include "unit_test.h"

//////////////////////////////////////////////////////////////////////////////
// Helpers
//////////////////////////////////////////////////////////////////////////////

// Exponents of length, mass, time and angle, when `used` is true.
struct UnitsExpected
{
    bool used;
    int  length;
    int  mass;
    int  time;
    int  angle;
};

static const UnitsExpected NOTHING_EXPECTED = { false, 0, 0, 0, 0 };
static const UnitsExpected EXPECT_MASS      = { true,  0, 1,  0, 0 };
static const UnitsExpected EXPECT_FORCE     = { true,  1, 1, -2, 0 };
static const UnitsExpected EXPECT_INERTIA   = { true,  2, 1,  0, 0 };
static const UnitsExpected EXPECT_STIFFNESS = { true,  0, 1, -2, 0 };
static const UnitsExpected EXPECT_TORQUE    = { true,  2, 1, -2, 0 };
static const UnitsExpected EXPECT_LENGTH    = { true,  1, 0,  0, 0 };

static bool test_readRun( const char *text, UnitsExpected kase, Measure *measure, UnitError *error )
{
    Source     source;
    TokenArray tokens;
    LexError   lexError;
    Source_make( &source, "unit", text, (uint32_t)strlen( text ) );
    if ( !tryLex( &source, &tokens, &lexError ) )
    {
        snprintf( error->message, sizeof( error->message ), "lexical error" );
        error->token = 0;
        Source_free( &source );
        return false;
    }

    Dimension expected = Dimension_make( kase.length, kase.mass, kase.time, kase.angle );
    bool ok = tryReadUnitRun( tokens.data, 0, tokens.count - 2, source.text, kase.used ? &expected : NULL, measure, error );

    TokenArray_free( &tokens );
    Source_free( &source );
    return ok;
}

// Exact for the tables' own factors; the tolerance is for computed ones.
static bool test_nearlyEqual( double a, double b )
{
    return fabs( a - b ) <= 1e-13 * fabs( b );
}

//////////////////////////////////////////////////////////////////////////////
// Runs that read
//////////////////////////////////////////////////////////////////////////////

struct UnitRunCase
{
    const char   *text;
    UnitsExpected expected;
    const char   *dimension;
    double        factor;
};

static const double TEST_PI  = 3.14159265358979323846;
static const double TEST_LBF = 0.45359237 * 9.80665;

static UnitRunCase unitRunCases[] =
{
    // the SI units, unprefixed (D146)
    { "m",    NOTHING_EXPECTED, "L",        1.0 },
    { "g",    NOTHING_EXPECTED, "M",        1e-3 },
    { "s",    NOTHING_EXPECTED, "T",        1.0 },
    { "N",    NOTHING_EXPECTED, "L M T-2",  1.0 },
    { "Pa",   NOTHING_EXPECTED, "L-1 M T-2", 1.0 },
    { "Hz",   NOTHING_EXPECTED, "T-1",      1.0 },
    { "J",    NOTHING_EXPECTED, "L2 M T-2", 1.0 },
    { "W",    NOTHING_EXPECTED, "L2 M T-3", 1.0 },
    { "rad",  NOTHING_EXPECTED, "A",        1.0 },

    // every prefix
    { "nm",   NOTHING_EXPECTED, "L",        1e-9 },
    { "um",   NOTHING_EXPECTED, "L",        1e-6 },
    { "mm",   NOTHING_EXPECTED, "L",        1e-3 },
    { "cm",   NOTHING_EXPECTED, "L",        1e-2 },
    { "km",   NOTHING_EXPECTED, "L",        1e3 },
    { "MPa",  NOTHING_EXPECTED, "L-1 M T-2", 1e6 },
    { "GPa",  NOTHING_EXPECTED, "L-1 M T-2", 1e9 },
    { "kg",   NOTHING_EXPECTED, "M",        1.0 },
    { "mg",   NOTHING_EXPECTED, "M",        1e-6 },
    { "ng",   NOTHING_EXPECTED, "M",        1e-12 },
    { "Gg",   NOTHING_EXPECTED, "M",        1e6 },
    { "ms",   NOTHING_EXPECTED, "T",        1e-3 },
    { "kN",   NOTHING_EXPECTED, "L M T-2",  1e3 },
    { "kPa",  NOTHING_EXPECTED, "L-1 M T-2", 1e3 },
    { "kHz",  NOTHING_EXPECTED, "T-1",      1e3 },
    { "mrad", NOTHING_EXPECTED, "A",        1e-3 },

    // the non-SI units, by their exact factors
    { "in",     NOTHING_EXPECTED, "L",       0.0254 },
    { "ft",     NOTHING_EXPECTED, "L",       0.3048 },
    { "mi",     NOTHING_EXPECTED, "L",       1609.344 },
    { "lbm",    NOTHING_EXPECTED, "M",       0.45359237 },
    { "lbf",    NOTHING_EXPECTED, "L M T-2", TEST_LBF },
    { "slug",   NOTHING_EXPECTED, "M",       TEST_LBF / 0.3048 },
    { "slinch", NOTHING_EXPECTED, "M",       TEST_LBF / 0.0254 },
    { "psi",    NOTHING_EXPECTED, "L-1 M T-2", TEST_LBF / ( 0.0254 * 0.0254 ) },
    { "rpm",    NOTHING_EXPECTED, "T-1 A",   2.0 * TEST_PI / 60.0 },
    { "deg",    NOTHING_EXPECTED, "A",       TEST_PI / 180.0 },
    { "hr",     NOTHING_EXPECTED, "T",       3600.0 },
    { "min",    NOTHING_EXPECTED, "T",       60.0 },

    // the run grammar: `-` multiplies, and all after the first `/` divides (D125)
    { "kg-m2",     NOTHING_EXPECTED, "L2 M",     1.0 },
    { "m/s2",      NOTHING_EXPECTED, "L T-2",    1.0 },
    { "N-m",       NOTHING_EXPECTED, "L2 M T-2", 1.0 },
    { "N-m/rad",   NOTHING_EXPECTED, "L2 M T-2 A-1", 1.0 },
    { "N-m-s/rad", NOTHING_EXPECTED, "L2 M T-1 A-1", 1.0 },
    { "N/m-s",     NOTHING_EXPECTED, "M T-3",    1.0 },
    { "lbf/in",    NOTHING_EXPECTED, "M T-2",    TEST_LBF / 0.0254 },
    { "kg/m/s",    NOTHING_EXPECTED, "L-1 M T-1", 1.0 },
    { "in3",       NOTHING_EXPECTED, "L3",       0.0254 * 0.0254 * 0.0254 },
    { "m12",       NOTHING_EXPECTED, "L12",      1.0 },

    // `^` raises the one name before it, and takes an exact decimal (D126)
    { "m^3",       NOTHING_EXPECTED, "L3",       1.0 },
    { "m^3.1",     NOTHING_EXPECTED, "L(31/10)", 1.0 },
    { "N-m^2",     NOTHING_EXPECTED, "L3 M T-2", 1.0 },
    { "N^0.5-m^0.5", NOTHING_EXPECTED, "L M(1/2) T-1", 1.0 },
    { "kN/m^2.1",  NOTHING_EXPECTED, "L(-11/10) M T-2", 1e3 },
    { "kN/m^3.1",  NOTHING_EXPECTED, "L(-21/10) M T-2", 1e3 },
    { "rpm/N^0.5-m^0.5", NOTHING_EXPECTED, "L-1 M(-1/2) A", ( 2.0 * TEST_PI / 60.0 ) },
    { "cm^0.5",    NOTHING_EXPECTED, "L(1/2)",   0.1 },

    // the pound, by the dimension expected of the whole run (D145)
    { "lb",        EXPECT_MASS,      "M",        0.45359237 },
    { "lb",        EXPECT_FORCE,     "L M T-2",  TEST_LBF },
    { "lb-in2",    EXPECT_INERTIA,   "L2 M",     0.45359237 * 0.0254 * 0.0254 },
    { "lb/in",     EXPECT_STIFFNESS, "M T-2",    TEST_LBF / 0.0254 },
    { "lb-in",     EXPECT_TORQUE,    "L2 M T-2", TEST_LBF * 0.0254 },

    // `lbm` and `lbf` need nothing expected, and pay no attention to it
    { "lbm",       EXPECT_FORCE,     "M",        0.45359237 },
};

static void test_unitRunCases( void )
{
    ut_beginModule( "Unit runs" );
    for ( uint32_t i = 0 ; i < sizeof( unitRunCases ) / sizeof( unitRunCases[0] ) ; i++ )
    {
        UnitRunCase *kase = &unitRunCases[i];

        Measure   measure;
        UnitError error;
        char      dimension[ DIMENSION_TEXT_SIZE ] = "";
        bool      read = test_readRun( kase->text, kase->expected, &measure, &error );
        if ( read ) Dimension_format( &measure.dimension, dimension, sizeof( dimension ) );

        bool pass = read && strcmp( dimension, kase->dimension ) == 0 && test_nearlyEqual( measure.factor, kase->factor );
        ut_reportTestResult( pass, "%s  is  %s", kase->text, kase->dimension );
        if ( !read )
        {
            printf( "        error: %s\n", error.message );
        }
        else if ( !pass )
        {
            printf( "        got %s, factor %.17g (expected %.17g)\n", dimension, measure.factor, kase->factor );
        }
    }
    ut_endModule();
}

//////////////////////////////////////////////////////////////////////////////
// Runs that don't read
//////////////////////////////////////////////////////////////////////////////

struct UnitErrorCase
{
    const char   *text;
    UnitsExpected expected;
    uint32_t      token;
    const char   *fragment;
};

static UnitErrorCase unitErrorCases[] =
{
    { "x",         NOTHING_EXPECTED, 0, "unknown unit 'x'" },
    { "m-x",       NOTHING_EXPECTED, 2, "unknown unit 'x'" },
    { "kft",       NOTHING_EXPECTED, 0, "unknown unit 'kft'" },        // no prefix on non-SI
    { "kmin",      NOTHING_EXPECTED, 0, "unknown unit 'kmin'" },
    { "kkg",       NOTHING_EXPECTED, 0, "unknown unit 'kkg'" },        // one prefix only
    { "Kg",        NOTHING_EXPECTED, 0, "unknown unit 'Kg'" },         // case matters
    { "m2^3",      NOTHING_EXPECTED, 0, "already has an exponent" },
    { "m^1e2",     NOTHING_EXPECTED, 2, "plain decimal" },
    { "m^0.1234567891", NOTHING_EXPECTED, 2, "plain decimal" },
    { "km^400",    NOTHING_EXPECTED, 0, "too large or too small" },

    // the pound (D145)
    { "lb",        NOTHING_EXPECTED, 0, "needs an expected dimension" },
    { "lb",        EXPECT_LENGTH,    0, "fits neither mass nor force" },
    { "kg-lb",     EXPECT_LENGTH,    2, "fits neither mass nor force" },
};

static void test_unitErrorCases( void )
{
    ut_beginModule( "Unit run errors" );
    for ( uint32_t i = 0 ; i < sizeof( unitErrorCases ) / sizeof( unitErrorCases[0] ) ; i++ )
    {
        UnitErrorCase *kase = &unitErrorCases[i];

        Measure   measure;
        UnitError error;
        bool      read = test_readRun( kase->text, kase->expected, &measure, &error );
        bool      pass = !read && error.token == kase->token && strstr( error.message, kase->fragment ) != NULL;

        ut_reportTestResult( pass, "%s  is an error", kase->text );
        if ( read )       printf( "        read without error\n" );
        else if ( !pass ) printf( "        got token %u: %s\n", error.token, error.message );
    }
    ut_endModule();
}

//////////////////////////////////////////////////////////////////////////////
// `units:` names
//////////////////////////////////////////////////////////////////////////////

struct DefaultUnitCase
{
    const char   *name;
    bool          valid;
    BaseDimension base;
    double        factor;
};

static DefaultUnitCase defaultUnitCases[] =
{
    { "m",   true,  BASE_LENGTH, 1.0 },
    { "in",  true,  BASE_LENGTH, 0.0254 },
    { "kg",  true,  BASE_MASS,   1.0 },
    { "lb",  true,  BASE_MASS,   0.45359237 },         // the pound-mass (D147)
    { "lbm", true,  BASE_MASS,   0.45359237 },
    { "s",   true,  BASE_TIME,   1.0 },
    { "min", true,  BASE_TIME,   60.0 },
    { "deg", true,  BASE_ANGLE,  TEST_PI / 180.0 },

    { "N",   false, BASE_LENGTH, 0.0 },
    { "rpm", false, BASE_LENGTH, 0.0 },
    { "lbf", false, BASE_LENGTH, 0.0 },
    { "m2",  false, BASE_LENGTH, 0.0 },
    { "x",   false, BASE_LENGTH, 0.0 },
};

static void test_defaultUnitCases( void )
{
    ut_beginModule( "Default units" );
    for ( uint32_t i = 0 ; i < sizeof( defaultUnitCases ) / sizeof( defaultUnitCases[0] ) ; i++ )
    {
        DefaultUnitCase *kase = &defaultUnitCases[i];

        BaseDimension base = BASE_COUNT;
        Measure       measure;
        UnitError     error;
        bool          read = tryReadDefaultUnit( kase->name, (uint32_t)strlen( kase->name ), &base, &measure, &error );
        bool          pass = read == kase->valid
                          && ( !read || ( base == kase->base && test_nearlyEqual( measure.factor, kase->factor ) ) );

        ut_reportTestResult( pass, "units: %s;", kase->name );
    }
    ut_endModule();
}

//////////////////////////////////////////////////////////////////////////////
// Dimension arithmetic
//////////////////////////////////////////////////////////////////////////////

static void test_dimensionArithmetic( void )
{
    ut_beginModule( "Dimension arithmetic" );

    Dimension force  = Dimension_make( 1, 1, -2, 0 );
    Dimension length = Dimension_make( 1, 0, 0, 0 );
    Dimension none   = Dimension_none();
    BaseDimension base = BASE_COUNT;

    Dimension torque = Dimension_multiply( &force, &length );
    Dimension back   = Dimension_divide( &torque, &length );
    ut_reportTestResult( Dimension_equal( &back, &force ), "F * L / L is F" );
    ut_reportTestResult( !Dimension_equal( &torque, &force ), "F * L is not F" );
    ut_reportTestResult( Dimension_isNone( &none ), "none is dimensionless" );
    ut_reportTestResult( !Dimension_isNone( &length ), "L is not dimensionless" );
    ut_reportTestResult( Dimension_isBase( &length, &base ) && base == BASE_LENGTH, "L is a base dimension" );
    ut_reportTestResult( !Dimension_isBase( &force, &base ), "F is not a base dimension" );
    ut_reportTestResult( !Dimension_isBase( &none, &base ), "dimensionless is not a base dimension" );

    // Growing past 32 bits leaves the power invalid, and invalid equals nothing.
    Dimension huge = Dimension_make( 2000000000, 0, 0, 0 );
    Dimension over = Dimension_multiply( &huge, &huge );
    ut_reportTestResult( !Dimension_equal( &over, &over ), "an overflowed dimension equals nothing" );
    Dimension still = Dimension_divide( &over, &huge );
    ut_reportTestResult( !Dimension_equal( &still, &huge ), "an overflowed dimension stays overflowed" );

    ut_endModule();
}

//////////////////////////////////////////////////////////////////////////////
// All unit tests
//////////////////////////////////////////////////////////////////////////////

void units_ut( void )
{
    test_unitRunCases();
    test_unitErrorCases();
    test_defaultUnitCases();
    test_dimensionArithmetic();
}

// units.cpp -- dimensions and unit runs (Specification.md §3.5, D66, D125, D126, D145, D146).

#include "units.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define UNITS_COUNT( table ) ( sizeof( table ) / sizeof( table[0] ) )

//////////////////////////////////////////////////////////////////////////////
// Rationals
//////////////////////////////////////////////////////////////////////////////

static const Rational RATIONAL_INVALID = { 0, 0 };

static int64_t Rational_gcd( int64_t a, int64_t b )
{
    while ( b != 0 )
    {
        int64_t r = a % b;
        a = b;
        b = r;
    }
    return a < 0 ? -a : a;
}

static Rational Rational_make( int64_t num, int64_t den )
{
    if ( den == 0 ) return RATIONAL_INVALID;

    int64_t divisor = Rational_gcd( num, den );
    if ( den < 0 ) divisor = -divisor;
    num /= divisor;
    den /= divisor;

    if ( num > INT32_MAX || num < -INT32_MAX || den > INT32_MAX ) return RATIONAL_INVALID;

    Rational r = { (int32_t)num, (int32_t)den };
    return r;
}

// Two 32-bit products sum safely in 64 bits.
static Rational Rational_add( Rational a, Rational b )
{
    if ( a.den == 0 || b.den == 0 ) return RATIONAL_INVALID;
    return Rational_make( (int64_t)a.num * b.den + (int64_t)b.num * a.den, (int64_t)a.den * b.den );
}

static Rational Rational_subtract( Rational a, Rational b )
{
    if ( a.den == 0 || b.den == 0 ) return RATIONAL_INVALID;
    return Rational_make( (int64_t)a.num * b.den - (int64_t)b.num * a.den, (int64_t)a.den * b.den );
}

static Rational Rational_multiply( Rational a, Rational b )
{
    if ( a.den == 0 || b.den == 0 ) return RATIONAL_INVALID;
    return Rational_make( (int64_t)a.num * b.num, (int64_t)a.den * b.den );
}

static bool Rational_equal( Rational a, Rational b )
{
    return a.den != 0 && b.den != 0 && a.num == b.num && a.den == b.den;
}

//////////////////////////////////////////////////////////////////////////////
// Dimensions
//////////////////////////////////////////////////////////////////////////////

Dimension Dimension_make( int length, int mass, int time, int angle )
{
    Dimension d;
    d.power[ BASE_LENGTH ] = Rational_make( length, 1 );
    d.power[ BASE_MASS ]   = Rational_make( mass, 1 );
    d.power[ BASE_TIME ]   = Rational_make( time, 1 );
    d.power[ BASE_ANGLE ]  = Rational_make( angle, 1 );
    return d;
}

Dimension Dimension_none( void )
{
    return Dimension_make( 0, 0, 0, 0 );
}

bool Dimension_equal( const Dimension *a, const Dimension *b )
{
    for ( uint32_t i = 0 ; i < BASE_COUNT ; i++ )
    {
        if ( !Rational_equal( a->power[i], b->power[i] ) ) return false;
    }
    return true;
}

bool Dimension_isNone( const Dimension *d )
{
    Dimension none = Dimension_none();
    return Dimension_equal( d, &none );
}

Dimension Dimension_multiply( const Dimension *a, const Dimension *b )
{
    Dimension d;
    for ( uint32_t i = 0 ; i < BASE_COUNT ; i++ ) d.power[i] = Rational_add( a->power[i], b->power[i] );
    return d;
}

Dimension Dimension_divide( const Dimension *a, const Dimension *b )
{
    Dimension d;
    for ( uint32_t i = 0 ; i < BASE_COUNT ; i++ ) d.power[i] = Rational_subtract( a->power[i], b->power[i] );
    return d;
}

bool Dimension_isBase( const Dimension *d, BaseDimension *base )
{
    const Rational zero = { 0, 1 };
    const Rational one  = { 1, 1 };

    uint32_t found = BASE_COUNT;
    for ( uint32_t i = 0 ; i < BASE_COUNT ; i++ )
    {
        if ( Rational_equal( d->power[i], zero ) ) continue;
        if ( !Rational_equal( d->power[i], one ) || found != BASE_COUNT ) return false;
        found = i;
    }
    if ( found == BASE_COUNT ) return false;

    *base = (BaseDimension)found;
    return true;
}

// Append to `buffer`, keeping count of the whole size even when it doesn't fit.
static void Units_append( char *buffer, uint32_t size, uint32_t *used, const char *format, ... )
{
    char    *out  = NULL;
    uint32_t room = 0;
    if ( buffer != NULL && *used < size )
    {
        out  = buffer + *used;
        room = size - *used;
    }

    va_list args;
    va_start( args, format );
    int n = vsnprintf( out, room, format, args );
    va_end( args );

    if ( n > 0 ) *used += (uint32_t)n;
}

uint32_t Dimension_format( const Dimension *d, char *buffer, uint32_t size )
{
    static const char *const letters[ BASE_COUNT ] = { "L", "M", "T", "A" };

    uint32_t used = 0;
    bool     any  = false;
    for ( uint32_t i = 0 ; i < BASE_COUNT ; i++ )
    {
        Rational p = d->power[i];
        if ( p.den != 0 && p.num == 0 ) continue;

        const char *separator = any ? " " : "";
        any = true;

        if ( p.den == 0 )                     Units_append( buffer, size, &used, "%s%s(?)", separator, letters[i] );
        else if ( p.den == 1 && p.num == 1 )  Units_append( buffer, size, &used, "%s%s", separator, letters[i] );
        else if ( p.den == 1 )                Units_append( buffer, size, &used, "%s%s%d", separator, letters[i], p.num );
        else                                  Units_append( buffer, size, &used, "%s%s(%d/%d)", separator, letters[i], p.num, p.den );
    }
    if ( !any ) Units_append( buffer, size, &used, "1" );
    return used;
}

//////////////////////////////////////////////////////////////////////////////
// The vocabulary (D146)
//////////////////////////////////////////////////////////////////////////////

// Exponents of length, mass, time and angle.
typedef int UnitsPowers[ BASE_COUNT ];

// An SI unit, which can take a prefix. Its factor is a power of ten, kept as
// an exponent so that `kg` is exactly 1 and `mg` is exactly 1e-6.
struct UnitsSiRow
{
    const char *name;
    int         tenPower;
    UnitsPowers powers;
};

// A non-SI unit, which never takes a prefix.
struct UnitsOtherRow
{
    const char *name;
    double      factor;
    UnitsPowers powers;
};

struct UnitsPrefixRow
{
    char prefix;
    int  tenPower;
};

static const UnitsSiRow unitsSiRows[] =
{
    { "m",    0, {  1, 0,  0, 0 } },
    { "g",   -3, {  0, 1,  0, 0 } },
    { "s",    0, {  0, 0,  1, 0 } },
    { "N",    0, {  1, 1, -2, 0 } },
    { "Pa",   0, { -1, 1, -2, 0 } },
    { "Hz",   0, {  0, 0, -1, 0 } },
    { "J",    0, {  2, 1, -2, 0 } },
    { "W",    0, {  2, 1, -3, 0 } },
    { "rad",  0, {  0, 0,  0, 1 } },
};

static const UnitsPrefixRow unitsPrefixRows[] =
{
    { 'n', -9 }, { 'u', -6 }, { 'm', -3 }, { 'c', -2 }, { 'k', 3 }, { 'M', 6 }, { 'G', 9 },
};

// Every power of ten a prefixed SI unit can need: -9 - 3 up to 9.
static const int    UNITS_LOWEST_TEN_POWER = -12;
static const double unitsTenPowers[] =
{
    1e-12, 1e-11, 1e-10, 1e-9, 1e-8, 1e-7, 1e-6, 1e-5, 1e-4, 1e-3, 1e-2, 1e-1,
    1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9,
};

static const double UNITS_PI          = 3.14159265358979323846;
static const double UNITS_INCH        = 0.0254;
static const double UNITS_FOOT        = 0.3048;
static const double UNITS_POUND_FORCE = 0.45359237 * 9.80665;

static const UnitsOtherRow unitsOtherRows[] =
{
    { "in",     UNITS_INCH,                              {  1, 0,  0, 0 } },
    { "ft",     UNITS_FOOT,                              {  1, 0,  0, 0 } },
    { "mi",     1609.344,                                {  1, 0,  0, 0 } },
    { "lbm",    0.45359237,                              {  0, 1,  0, 0 } },
    { "lbf",    UNITS_POUND_FORCE,                       {  1, 1, -2, 0 } },
    { "slug",   UNITS_POUND_FORCE / UNITS_FOOT,          {  0, 1,  0, 0 } },
    { "slinch", UNITS_POUND_FORCE / UNITS_INCH,          {  0, 1,  0, 0 } },
    { "psi",    UNITS_POUND_FORCE / ( UNITS_INCH * UNITS_INCH ), { -1, 1, -2, 0 } },
    { "rpm",    2.0 * UNITS_PI / 60.0,                   {  0, 0, -1, 1 } },
    { "deg",    UNITS_PI / 180.0,                        {  0, 0,  0, 1 } },
    { "hr",     3600.0,                                  {  0, 0,  1, 0 } },
    { "min",    60.0,                                    {  0, 0,  1, 0 } },
};

static bool Units_isName( const char *word, const char *name, uint32_t length )
{
    return strlen( word ) == length && memcmp( word, name, length ) == 0;
}

static Measure Units_measure( const UnitsPowers powers, double factor )
{
    Measure m;
    m.dimension = Dimension_make( powers[0], powers[1], powers[2], powers[3] );
    m.factor    = factor;
    return m;
}

static double Units_tenPower( int power )
{
    return unitsTenPowers[ power - UNITS_LOWEST_TEN_POWER ];
}

static const UnitsSiRow *Units_findSi( const char *name, uint32_t length )
{
    for ( uint32_t i = 0 ; i < UNITS_COUNT( unitsSiRows ) ; i++ )
    {
        if ( Units_isName( unitsSiRows[i].name, name, length ) ) return &unitsSiRows[i];
    }
    return NULL;
}

// How a bare `lb` is being read.
enum PoundReading
{
    POUND_MASS,
    POUND_FORCE,
};

// Find a unit by name. The closed non-SI list comes first, so `min` is the
// minute and never a prefixed anything (D146).
static bool Units_lookup( const char *name, uint32_t length, PoundReading reading, Measure *measure, bool *isPound )
{
    *isPound = Units_isName( "lb", name, length );
    if ( *isPound )
    {
        name   = reading == POUND_MASS ? "lbm" : "lbf";
        length = 3;
    }

    for ( uint32_t i = 0 ; i < UNITS_COUNT( unitsOtherRows ) ; i++ )
    {
        const UnitsOtherRow *row = &unitsOtherRows[i];
        if ( Units_isName( row->name, name, length ) )
        {
            *measure = Units_measure( row->powers, row->factor );
            return true;
        }
    }

    const UnitsSiRow *si = Units_findSi( name, length );
    if ( si != NULL )
    {
        *measure = Units_measure( si->powers, Units_tenPower( si->tenPower ) );
        return true;
    }

    if ( length < 2 ) return false;
    si = Units_findSi( name + 1, length - 1 );
    if ( si == NULL ) return false;

    for ( uint32_t i = 0 ; i < UNITS_COUNT( unitsPrefixRows ) ; i++ )
    {
        if ( unitsPrefixRows[i].prefix == name[0] )
        {
            *measure = Units_measure( si->powers, Units_tenPower( si->tenPower + unitsPrefixRows[i].tenPower ) );
            return true;
        }
    }
    return false;
}

//////////////////////////////////////////////////////////////////////////////
// Unit runs
//////////////////////////////////////////////////////////////////////////////

static bool Units_failAt( UnitError *error, uint32_t token, const char *format, ... )
{
    error->token = token;

    va_list args;
    va_start( args, format );
    vsnprintf( error->message, sizeof( error->message ), format, args );
    va_end( args );
    return false;
}

static bool Units_isDigit( char c )
{
    return c >= '0' && c <= '9';
}

// `digits[.digits]`, read as an exact rational (D126). Nine digits always fit
// in 32 bits, and no real exponent needs more.
static bool tryReadDecimal( const char *text, uint32_t length, Rational *value )
{
    int64_t  num       = 0;
    int64_t  den       = 1;
    uint32_t digits    = 0;
    bool     afterDot  = false;

    for ( uint32_t i = 0 ; i < length ; i++ )
    {
        char c = text[i];
        if ( c == '.' && !afterDot )
        {
            afterDot = true;
            continue;
        }
        if ( !Units_isDigit( c ) || ++digits > 9 ) return false;

        num = num * 10 + ( c - '0' );
        if ( afterDot ) den *= 10;
    }

    *value = Rational_make( num, den );
    return true;
}

// `base` raised to a non-negative power. A whole power is multiplied out, so
// it's the same on every platform; only a fractional one needs pow().
static double Units_power( double base, Rational exponent )
{
    if ( exponent.den != 1 ) return pow( base, (double)exponent.num / (double)exponent.den );

    double   result = 1.0;
    double   square = base;
    uint32_t n      = (uint32_t)exponent.num;
    while ( n != 0 )
    {
        if ( ( n & 1 ) != 0 ) result *= square;
        square *= square;
        n >>= 1;
    }
    return result;
}

// Multiply or divide `total` by one unit raised to `exponent`.
static void Units_combine( Measure *total, const Measure *term, Rational exponent, bool inDenominator )
{
    const Rational sign = { inDenominator ? -1 : 1, 1 };
    Rational       signedExponent = Rational_multiply( exponent, sign );

    for ( uint32_t i = 0 ; i < BASE_COUNT ; i++ )
    {
        total->dimension.power[i] = Rational_add( total->dimension.power[i],
                                                  Rational_multiply( term->dimension.power[i], signedExponent ) );
    }

    double scale = Units_power( term->factor, exponent );
    if ( inDenominator ) total->factor /= scale;
    else                 total->factor *= scale;
}

// Read the whole run with bare `lb` taken one way. Says whether a bare `lb`
// was seen, and where.
static bool tryReadRunAs( const Token *tokens, uint32_t first, uint32_t last, const char *text, PoundReading reading,
                          Measure *measure, bool *sawPound, uint32_t *poundToken, UnitError *error )
{
    measure->dimension = Dimension_none();
    measure->factor    = 1.0;
    *sawPound          = false;
    *poundToken        = first;

    bool     inDenominator = false;
    uint32_t i             = first;
    for ( ;; )
    {
        const Token *t      = &tokens[i];
        const char  *name   = text + t->offset;
        uint32_t     length = t->len;

        // A trailing digit is an exponent on the name before it (D146).
        uint32_t nameLength = length;
        while ( nameLength > 0 && Units_isDigit( name[ nameLength - 1 ] ) ) nameLength--;

        Measure term;
        bool    isPound = false;
        if ( !Units_lookup( name, nameLength, reading, &term, &isPound ) )
        {
            return Units_failAt( error, i, "unknown unit '%.*s' (D146)", (int)nameLength, name );
        }
        if ( isPound && !*sawPound )
        {
            *sawPound   = true;
            *poundToken = i;
        }

        Rational exponent = { 1, 1 };
        if ( nameLength < length && !tryReadDecimal( name + nameLength, length - nameLength, &exponent ) )
        {
            return Units_failAt( error, i, "the exponent on '%.*s' is too long", (int)nameLength, name );
        }
        uint32_t nameToken = i;
        i++;

        if ( i <= last && tokens[i].kind == TOK_CARET )
        {
            if ( nameLength < length )
            {
                return Units_failAt( error, nameToken, "'%.*s' already has an exponent, so it can't take '^' as well",
                                     (int)length, name );
            }
            const Token *power = &tokens[ i + 1 ];
            if ( !tryReadDecimal( text + power->offset, power->len, &exponent ) )
            {
                return Units_failAt( error, i + 1, "the exponent after '^' must be a plain decimal of at most 9 digits (D126)" );
            }
            i += 2;
        }

        Units_combine( measure, &term, exponent, inDenominator );

        if ( i > last ) break;
        if ( tokens[i].kind == TOK_SLASH ) inDenominator = true;      // and it stays true (D125)
        i++;
    }

    if ( !isfinite( measure->factor ) || measure->factor <= 0.0 )
    {
        return Units_failAt( error, first, "this unit's scale is too large or too small to represent" );
    }
    return true;
}

bool tryReadUnitRun( const Token *tokens, uint32_t first, uint32_t last, const char *text,
                     const Dimension *expected, Measure *measure, UnitError *error )
{
    bool     sawPound   = false;
    uint32_t poundToken = first;
    if ( !tryReadRunAs( tokens, first, last, text, POUND_MASS, measure, &sawPound, &poundToken, error ) ) return false;
    if ( !sawPound ) return true;

    if ( expected == NULL )
    {
        return Units_failAt( error, poundToken, "a bare 'lb' needs an expected dimension, and there is none here; "
                                                "write 'lbm' or 'lbf' (D145)" );
    }

    Measure asForce;
    if ( !tryReadRunAs( tokens, first, last, text, POUND_FORCE, &asForce, &sawPound, &poundToken, error ) ) return false;

    bool massFits  = Dimension_equal( &measure->dimension, expected );
    bool forceFits = Dimension_equal( &asForce.dimension, expected );
    if ( massFits && forceFits )
    {
        return Units_failAt( error, poundToken, "'lb' fits both mass and force here; write 'lbm' or 'lbf' (D145)" );
    }
    if ( !massFits && !forceFits )
    {
        return Units_failAt( error, poundToken, "'lb' fits neither mass nor force here; write 'lbm' or 'lbf' (D145)" );
    }
    if ( forceFits ) *measure = asForce;
    return true;
}

bool tryReadDefaultUnit( const char *name, uint32_t length, BaseDimension *base, Measure *measure, UnitError *error )
{
    bool isPound = false;
    if ( !Units_lookup( name, length, POUND_MASS, measure, &isPound ) || !Dimension_isBase( &measure->dimension, base ) )
    {
        return Units_failAt( error, 0, "'%.*s' is not a unit of length, mass, time or angle (D147)", (int)length, name );
    }
    return true;
}

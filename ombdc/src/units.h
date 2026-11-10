// units.h -- dimensions and unit runs (Specification.md §3.5).
//
// This module knows nothing about the syntax tree. A dimension is a product
// of the four base dimensions raised to rational powers (D66). A Measure is
// what a unit means: a dimension, plus the factor that takes a number written
// in that unit to SI. (The name `Unit` already means one source text; see
// resolve.h.)

#pragma once
#include <stdint.h>
#include "token.h"

// Lowest terms, with den > 0. Arithmetic that doesn't fit in 32 bits gives
// den == 0, which stays invalid and never compares equal to anything.
struct Rational
{
    int32_t num;
    int32_t den;
};

enum BaseDimension
{
    BASE_LENGTH,
    BASE_MASS,
    BASE_TIME,
    BASE_ANGLE,

    BASE_COUNT                   // the number of base dimensions; not one
};

struct Dimension
{
    Rational power[ BASE_COUNT ];// all zero: dimensionless
};

struct Measure
{
    Dimension dimension;
    double    factor;            // a number in this unit, times factor, is in SI
};

struct UnitError
{
    uint32_t token;              // the token at fault: an index into the caller's array
    char     message[ 160 ];
};

// Room for any dimension Dimension_format can write.
#define DIMENSION_TEXT_SIZE 128

Dimension Dimension_none( void );
Dimension Dimension_make( int length, int mass, int time, int angle );
bool      Dimension_equal( const Dimension *a, const Dimension *b );
bool      Dimension_isNone( const Dimension *d );
Dimension Dimension_multiply( const Dimension *a, const Dimension *b );
Dimension Dimension_divide( const Dimension *a, const Dimension *b );

// Is `d` exactly one base dimension, to the first power? If so, says which.
bool Dimension_isBase( const Dimension *d, BaseDimension *base );

// Write `d` as "L T-2", "L2 M", "L(31/10)", or "1" when dimensionless.
// Like snprintf, and returns the size of the whole text, NUL excluded.
uint32_t Dimension_format( const Dimension *d, char *buffer, uint32_t size );

// Read the unit run spanning tokens[first] to tokens[last], inclusive, whose
// spelling is in `text`. `expected` settles a bare `lb` (D145). It may be NULL
// if nothing is expected, in which case a bare `lb` is an error.
bool tryReadUnitRun( const Token *tokens, uint32_t first, uint32_t last, const char *text,
                     const Dimension *expected, Measure *measure, UnitError *error );

// Read one name from a `units:` list. It must be a unit of one base dimension,
// and `lb` is the pound-mass (D147). `error->token` is left at 0.
bool tryReadDefaultUnit( const char *name, uint32_t length, BaseDimension *base, Measure *measure, UnitError *error );

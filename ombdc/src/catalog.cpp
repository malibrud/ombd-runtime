// catalog.cpp -- the vocabulary tables (Specification.md §4).

#include "catalog.h"
#include <string.h>

#define CATALOG_COUNT( table ) ( sizeof( table ) / sizeof( table[0] ) )

//////////////////////////////////////////////////////////////////////////////
// Elements and their properties
//////////////////////////////////////////////////////////////////////////////

static const PropertyInfo catalogTopLevelProperties[] =
{
    { "gravity", VALUE_EXPRESSION, false },              // §4.11
};

static const PropertyInfo catalogSystemProperties[] =
{
    { "units", VALUE_UNIT, false },                      // §3.5
};

static const PropertyInfo catalogBodyProperties[] =      // §4.2
{
    { "mass",    VALUE_EXPRESSION, false },
    { "cg",      VALUE_EXPRESSION, false },
    { "inertia", VALUE_EXPRESSION, false },
};

static const PropertyInfo catalogRevoluteProperties[] =  // §4.3: every joint property is placement
{
    { "at",   VALUE_EXPRESSION, false },
    { "axis", VALUE_EXPRESSION, false },
};

static const PropertyInfo catalogTsdaProperties[] =      // §4.4
{
    { "rate",    VALUE_EXPRESSION, true  },
    { "damping", VALUE_EXPRESSION, true  },
    { "free",    VALUE_EXPRESSION, true  },
    { "p1",      VALUE_EXPRESSION, false },
    { "p2",      VALUE_EXPRESSION, false },
};

static_assert( CATALOG_COUNT( catalogTopLevelProperties ) <= CATALOG_MAX_PROPERTIES, "raise CATALOG_MAX_PROPERTIES" );
static_assert( CATALOG_COUNT( catalogSystemProperties )   <= CATALOG_MAX_PROPERTIES, "raise CATALOG_MAX_PROPERTIES" );
static_assert( CATALOG_COUNT( catalogBodyProperties )     <= CATALOG_MAX_PROPERTIES, "raise CATALOG_MAX_PROPERTIES" );
static_assert( CATALOG_COUNT( catalogRevoluteProperties ) <= CATALOG_MAX_PROPERTIES, "raise CATALOG_MAX_PROPERTIES" );
static_assert( CATALOG_COUNT( catalogTsdaProperties )     <= CATALOG_MAX_PROPERTIES, "raise CATALOG_MAX_PROPERTIES" );

extern const ElementInfo Catalog_topLevel = { "the top level", catalogTopLevelProperties, CATALOG_COUNT( catalogTopLevelProperties ) };
extern const ElementInfo Catalog_system   = { "a system",      catalogSystemProperties,   CATALOG_COUNT( catalogSystemProperties ) };
extern const ElementInfo Catalog_body     = { "a body",        catalogBodyProperties,     CATALOG_COUNT( catalogBodyProperties ) };

static const ElementInfo catalogRevolute = { "revolute", catalogRevoluteProperties, CATALOG_COUNT( catalogRevoluteProperties ) };
static const ElementInfo catalogTsda     = { "TSDA",     catalogTsdaProperties,     CATALOG_COUNT( catalogTsdaProperties ) };

extern const ElementInfo *const Catalog_types[] =
{
    &catalogRevolute,
    &catalogTsda,
};
extern const uint32_t Catalog_typeCount = CATALOG_COUNT( Catalog_types );

//////////////////////////////////////////////////////////////////////////////
// Built-in names (§4.1, D144)
//////////////////////////////////////////////////////////////////////////////

extern const char *const Catalog_builtinValues[] = { "world", "time", "pi" };
extern const uint32_t    Catalog_builtinValueCount = CATALOG_COUNT( Catalog_builtinValues );

extern const char *const Catalog_builtinFunctions[] =
{
    "avg", "mid", "diag", "frame",
    "sin", "cos", "tan", "asin", "acos", "atan2", "sqrt", "exp", "log", "abs", "min", "max",
};
extern const uint32_t Catalog_builtinFunctionCount = CATALOG_COUNT( Catalog_builtinFunctions );

//////////////////////////////////////////////////////////////////////////////
// Sections (§3.2)
//////////////////////////////////////////////////////////////////////////////

static const char *const catalogSystemSections[] =
{
    "points", "frames", "bodies", "forces", "tires", "joints",
    "components", "maps", "controls", "signals", "drivers", "assembly",
};

static const char *const catalogBodySections[] = { "points", "frames" };

//////////////////////////////////////////////////////////////////////////////
// Lookups
//////////////////////////////////////////////////////////////////////////////

static bool catalogIsName( const char *word, const char *name, uint32_t length )
{
    return strlen( word ) == length && memcmp( word, name, length ) == 0;
}

const PropertyInfo *ElementInfo_findProperty( const ElementInfo *element, const char *name, uint32_t length )
{
    for ( uint32_t i = 0 ; i < element->propertyCount ; i++ )
    {
        if ( catalogIsName( element->properties[i].name, name, length ) ) return &element->properties[i];
    }
    return NULL;
}

bool ElementInfo_hasLaw( const ElementInfo *element )
{
    for ( uint32_t i = 0 ; i < element->propertyCount ; i++ )
    {
        if ( element->properties[i].law ) return true;
    }
    return false;
}

bool Catalog_isSystemSection( const char *name, uint32_t length )
{
    for ( uint32_t i = 0 ; i < CATALOG_COUNT( catalogSystemSections ) ; i++ )
    {
        if ( catalogIsName( catalogSystemSections[i], name, length ) ) return true;
    }
    return false;
}

bool Catalog_isBodySection( const char *name, uint32_t length )
{
    for ( uint32_t i = 0 ; i < CATALOG_COUNT( catalogBodySections ) ; i++ )
    {
        if ( catalogIsName( catalogBodySections[i], name, length ) ) return true;
    }
    return false;
}

// catalog.h -- what the loader knows about the language's vocabulary (Specification.md §4).
//
// The grammar is vocabulary-free, so the words `revolute` and `mass` first
// mean something here. The tables are flat, and they grow as models need
// them: today they hold what Pendulum uses, plus TSDA so that the law and
// placement rule has something to act on. Only §4.1's built-in names are
// complete, because the no-shadowing rule needs all of them.

#pragma once
#include <stdint.h>
#include "units.h"

// The space a property's value is looked up in (§3.2). Each new kind of
// property value (labels, state names) adds a member here.
enum ValueSpace
{
    VALUE_EXPRESSION,            // user values and elements, then built-in values
    VALUE_UNIT,                  // units (§3.5); `units: m, kg;`
};

// The value types (§3.4). The discrete types join when guards are checked.
enum ValueType
{
    TYPE_NONE,                   // not a value
    TYPE_SCALAR,
    TYPE_POINT,
    TYPE_VECTOR,
    TYPE_ORIENTATION,
    TYPE_MATRIX,
    TYPE_FRAME,
    TYPE_SPATIAL,
    TYPE_LIST,
    TYPE_GRID,
    TYPE_STRING,

    TYPE_COUNT                   // the number of types; not a type
};

// The dimension a property expects (§4).
enum ExpectedDimension
{
    DIM_NONE,                    // dimensionless
    DIM_ANY,                     // the catalog lists no dimension, as for `axis:`
    DIM_LENGTH,
    DIM_MASS,
    DIM_TIME,
    DIM_ACCELERATION,
    DIM_INERTIA,                 // mass·length²
    DIM_STIFFNESS,               // force/length
    DIM_DAMPING,                 // force·time/length

    DIM_COUNT                    // the number of entries; not an entry
};

struct PropertyInfo
{
    const char       *name;
    ValueSpace        space;
    bool              law;       // law (true) or placement (false), for a type that can be defined (D152)
    ValueType         type;      // TYPE_NONE when `space` is not VALUE_EXPRESSION
    ExpectedDimension dimension;
};

// A built-in value (§4.1). `world` is not a number of any kind: its type is TYPE_NONE.
struct BuiltinValue
{
    const char       *name;
    ValueType         type;
    ExpectedDimension dimension;
};

// Anything that has properties: a built-in type, or the top level, a system
// or a body, whose statements are checked the same way.
struct ElementInfo
{
    const char         *name;
    const PropertyInfo *properties;
    uint32_t            propertyCount;
};

// No element has more properties than this.
#define CATALOG_MAX_PROPERTIES 16

extern const ElementInfo Catalog_topLevel;
extern const ElementInfo Catalog_system;
extern const ElementInfo Catalog_body;

// The built-in names (§4.1, D144) and the built-in types (§4).
extern const BuiltinValue Catalog_builtinValues[];
extern const uint32_t    Catalog_builtinValueCount;
extern const char *const Catalog_builtinFunctions[];
extern const uint32_t    Catalog_builtinFunctionCount;
extern const ElementInfo *const Catalog_types[];
extern const uint32_t    Catalog_typeCount;

// The property of `element` with this name, or NULL.
const PropertyInfo *ElementInfo_findProperty( const ElementInfo *element, const char *name, uint32_t length );

// Does any property of `element` belong to its law? A type with none can't
// be a shared definition (D152).
bool ElementInfo_hasLaw( const ElementInfo *element );

// The built-in value with this name, or NULL.
const BuiltinValue *Catalog_findBuiltinValue( const char *name, uint32_t length );

// The dimension an entry stands for. DIM_ANY stands for none, so test for it first.
Dimension ExpectedDimension_value( ExpectedDimension expected );

// "scalar", "point", ...
const char *ValueType_name( ValueType type );

// The bare blocks recognized in a system and in a body (§3.2).
bool Catalog_isSystemSection( const char *name, uint32_t length );
bool Catalog_isBodySection( const char *name, uint32_t length );

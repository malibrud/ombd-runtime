// types.h -- value types and units (Specification.md §3.4, §3.5).
//
// The stage after loading. It works out the type and dimension of every value
// in a loaded model and checks them against the closure table and the catalog.
// As with name resolution, what it learns is kept in an array parallel to the
// syntax tree. It computes no values: a literal's numbers stay as written, and
// `factor` says how to take them to SI.

#pragma once
#include "catalog.h"
#include "resolve.h"
#include "units.h"

struct ValueInfo
{
    ValueType type;              // TYPE_NONE: the node is not a value
    Dimension dimension;
    double    factor;            // takes this literal's numbers to SI; 0 if it carries no unit of its own
    uint32_t  rows;              // a list's entries, or a grid's rows
    uint32_t  columns;           // a grid's columns
};

enum BinaryOp
{
    OP_ADD,
    OP_SUBTRACT,
    OP_MULTIPLY,
    OP_DIVIDE,
    OP_MOD,
    OP_DOT,                      // \.
    OP_CROSS,                    // \x
    OP_APPLY,                    // \>

    OP_COUNT                     // the number of operators; not one
};

enum UnaryOp
{
    UNARY_NEGATE,                // -
    UNARY_INVERSE,               // ^
};

// How an operator's result gets its dimension from its operands.
enum DimensionRule
{
    RULE_SAME,                   // the operands' units must agree (+ - mod)
    RULE_PRODUCT,
    RULE_QUOTIENT,
};

// Check the types of a loaded model. Fills `values` in every unit. On the
// first error, fills `error` and returns false; the model stays loaded either
// way, and Model_free releases what this made.
bool tryCheckTypes( Model *model, LoadError *error );

// The closure table (§3.4). Each returns false for a combination it doesn't list.
bool tryBinaryType( BinaryOp op, ValueType left, ValueType right, ValueType *result, DimensionRule *rule );
bool tryUnaryType( UnaryOp op, ValueType operand, ValueType *result );

// Write a value's type as "point", "list[3]" or "grid[2x3]". Like snprintf.
uint32_t ValueInfo_formatType( const ValueInfo *value, char *buffer, uint32_t size );

// Write the `ombdc --types` dump: one line per value, in tree order, as
//     line:col KIND type dimension [factor f]
// Like snprintf: writes at most `size` bytes and returns the size of the whole
// dump, NUL excluded.
uint32_t Model_formatTypes( const Model *model, char *buffer, uint32_t size );

// types.cpp -- value types and units (Specification.md §3.4, §3.5).
//
// Two passes over a loaded model, each stopping at the first error:
//
//   1. tryCollectDefaults   read every `units:` statement into a per-scope table
//   2. tryCheckBlock        check every value and every property
//
// The defaults come first because declarations are order-independent (§0).
// For the same reason a named value is checked the first time it is needed,
// and remembered.
//
// tryInfer pushes an expected dimension down an expression and records each
// node's type and dimension on the way back up. Only literals and sums look
// at what's expected: a bare literal takes its dimension from it (D147), and a
// bare `lb` is settled by it (D145).
//
// Not implemented yet, each with its own error: quaternion, orientation, frame
// and spatial literals, the later segments of a path, indexing, guards and
// discrete types, drivers and signal references, and dimensioned entries
// inside brackets.

#include "types.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TYPES_COUNT( table ) ( sizeof( table ) / sizeof( table[0] ) )

// The `units:` defaults set in one scope.
struct ScopeDefaults
{
    bool    set[ BASE_COUNT ];
    Measure measure[ BASE_COUNT ];
};

// How far a named value has got.
enum SymbolState
{
    SYMBOL_UNCHECKED,
    SYMBOL_CHECKING,
    SYMBOL_CHECKED,
};

// The checker's state. It lives on tryCheckTypes's stack.
struct Checker
{
    Model         *model;
    LoadError     *error;
    ScopeDefaults *defaults;     // parallel to model->scopes
    uint8_t       *symbolState;  // parallel to model->symbols: a SymbolState
};

// Where an expression was written: its text, and the scope its defaults come from.
struct Site
{
    uint32_t unit;
    uint32_t scope;
};

//////////////////////////////////////////////////////////////////////////////
// The closure table (§3.4)
//////////////////////////////////////////////////////////////////////////////

struct BinaryRow
{
    BinaryOp      op;
    ValueType     left;
    ValueType     right;
    ValueType     result;
    DimensionRule rule;
};

// An orientation and a frame are dimensionless, so a product also covers
// applying one: F \> P has the dimension of P.
static const BinaryRow binaryRows[] =
{
    { OP_ADD,      TYPE_SCALAR,      TYPE_SCALAR,      TYPE_SCALAR,      RULE_SAME },
    { OP_ADD,      TYPE_POINT,       TYPE_VECTOR,      TYPE_POINT,       RULE_SAME },
    { OP_ADD,      TYPE_VECTOR,      TYPE_POINT,       TYPE_POINT,       RULE_SAME },
    { OP_ADD,      TYPE_VECTOR,      TYPE_VECTOR,      TYPE_VECTOR,      RULE_SAME },
    { OP_ADD,      TYPE_MATRIX,      TYPE_MATRIX,      TYPE_MATRIX,      RULE_SAME },

    { OP_SUBTRACT, TYPE_SCALAR,      TYPE_SCALAR,      TYPE_SCALAR,      RULE_SAME },
    { OP_SUBTRACT, TYPE_POINT,       TYPE_POINT,       TYPE_VECTOR,      RULE_SAME },
    { OP_SUBTRACT, TYPE_POINT,       TYPE_VECTOR,      TYPE_POINT,       RULE_SAME },
    { OP_SUBTRACT, TYPE_VECTOR,      TYPE_VECTOR,      TYPE_VECTOR,      RULE_SAME },
    { OP_SUBTRACT, TYPE_MATRIX,      TYPE_MATRIX,      TYPE_MATRIX,      RULE_SAME },

    { OP_MULTIPLY, TYPE_SCALAR,      TYPE_SCALAR,      TYPE_SCALAR,      RULE_PRODUCT },
    { OP_MULTIPLY, TYPE_SCALAR,      TYPE_VECTOR,      TYPE_VECTOR,      RULE_PRODUCT },
    { OP_MULTIPLY, TYPE_VECTOR,      TYPE_SCALAR,      TYPE_VECTOR,      RULE_PRODUCT },
    { OP_MULTIPLY, TYPE_SCALAR,      TYPE_MATRIX,      TYPE_MATRIX,      RULE_PRODUCT },
    { OP_MULTIPLY, TYPE_MATRIX,      TYPE_SCALAR,      TYPE_MATRIX,      RULE_PRODUCT },
    { OP_MULTIPLY, TYPE_ORIENTATION, TYPE_ORIENTATION, TYPE_ORIENTATION, RULE_PRODUCT },
    { OP_MULTIPLY, TYPE_FRAME,       TYPE_FRAME,       TYPE_FRAME,       RULE_PRODUCT },
    { OP_MULTIPLY, TYPE_MATRIX,      TYPE_MATRIX,      TYPE_MATRIX,      RULE_PRODUCT },
    { OP_MULTIPLY, TYPE_ORIENTATION, TYPE_MATRIX,      TYPE_MATRIX,      RULE_PRODUCT },
    { OP_MULTIPLY, TYPE_MATRIX,      TYPE_ORIENTATION, TYPE_MATRIX,      RULE_PRODUCT },

    { OP_DIVIDE,   TYPE_SCALAR,      TYPE_SCALAR,      TYPE_SCALAR,      RULE_QUOTIENT },
    { OP_DIVIDE,   TYPE_VECTOR,      TYPE_SCALAR,      TYPE_VECTOR,      RULE_QUOTIENT },
    { OP_DIVIDE,   TYPE_MATRIX,      TYPE_SCALAR,      TYPE_MATRIX,      RULE_QUOTIENT },

    { OP_MOD,      TYPE_SCALAR,      TYPE_SCALAR,      TYPE_SCALAR,      RULE_SAME },       // D163

    { OP_DOT,      TYPE_VECTOR,      TYPE_VECTOR,      TYPE_SCALAR,      RULE_PRODUCT },
    { OP_CROSS,    TYPE_VECTOR,      TYPE_VECTOR,      TYPE_VECTOR,      RULE_PRODUCT },

    { OP_APPLY,    TYPE_ORIENTATION, TYPE_VECTOR,      TYPE_VECTOR,      RULE_PRODUCT },
    { OP_APPLY,    TYPE_MATRIX,      TYPE_VECTOR,      TYPE_VECTOR,      RULE_PRODUCT },
    { OP_APPLY,    TYPE_ORIENTATION, TYPE_MATRIX,      TYPE_MATRIX,      RULE_PRODUCT },
    { OP_APPLY,    TYPE_FRAME,       TYPE_POINT,       TYPE_POINT,       RULE_PRODUCT },
    { OP_APPLY,    TYPE_FRAME,       TYPE_VECTOR,      TYPE_VECTOR,      RULE_PRODUCT },
    { OP_APPLY,    TYPE_FRAME,       TYPE_MATRIX,      TYPE_MATRIX,      RULE_PRODUCT },
};

bool tryBinaryType( BinaryOp op, ValueType left, ValueType right, ValueType *result, DimensionRule *rule )
{
    for ( uint32_t i = 0 ; i < TYPES_COUNT( binaryRows ) ; i++ )
    {
        const BinaryRow *row = &binaryRows[i];
        if ( row->op == op && row->left == left && row->right == right )
        {
            *result = row->result;
            *rule   = row->rule;
            return true;
        }
    }
    return false;
}

bool tryUnaryType( UnaryOp op, ValueType operand, ValueType *result )
{
    bool legal = false;
    switch ( op )
    {
    case UNARY_NEGATE:
        legal = operand == TYPE_SCALAR || operand == TYPE_VECTOR || operand == TYPE_MATRIX;
        break;
    case UNARY_INVERSE:
        legal = operand == TYPE_ORIENTATION || operand == TYPE_MATRIX || operand == TYPE_FRAME;
        break;
    }
    if ( legal ) *result = operand;
    return legal;
}

uint32_t ValueInfo_formatType( const ValueInfo *value, char *buffer, uint32_t size )
{
    int n;
    if ( value->type == TYPE_LIST )
    {
        n = snprintf( buffer, size, "list[%u]", value->rows );
    }
    else if ( value->type == TYPE_GRID )
    {
        n = snprintf( buffer, size, "grid[%ux%u]", value->rows, value->columns );
    }
    else
    {
        n = snprintf( buffer, size, "%s", ValueType_name( value->type ) );
    }
    return n > 0 ? (uint32_t)n : 0;
}

//////////////////////////////////////////////////////////////////////////////
// Built-in functions (§4.1)
//////////////////////////////////////////////////////////////////////////////

enum FunctionRule
{
    FUNCTION_CENTROID,           // avg, mid: points to a point
    FUNCTION_DIAG,               // diag: three numbers to a matrix, a literal form
    FUNCTION_FRAME,              // frame: a point and an orientation
    FUNCTION_TRIG,               // an angle to a dimensionless scalar
    FUNCTION_INVERSE_TRIG,       // a dimensionless scalar to an angle
    FUNCTION_ATAN2,              // two scalars whose units agree, to an angle
    FUNCTION_PLAIN,              // a dimensionless scalar to a dimensionless scalar
    FUNCTION_ABS,                // keeps its argument's unit
    FUNCTION_MIN_MAX,            // two or more scalars whose units agree
};

struct FunctionRow
{
    const char  *name;
    FunctionRule rule;
    uint32_t     minArguments;
    uint32_t     maxArguments;   // 0: no limit
};

static const FunctionRow functionRows[] =
{
    { "avg",   FUNCTION_CENTROID,     1, 0 },
    { "mid",   FUNCTION_CENTROID,     2, 2 },
    { "diag",  FUNCTION_DIAG,         3, 3 },
    { "frame", FUNCTION_FRAME,        2, 2 },
    { "sin",   FUNCTION_TRIG,         1, 1 },
    { "cos",   FUNCTION_TRIG,         1, 1 },
    { "tan",   FUNCTION_TRIG,         1, 1 },
    { "asin",  FUNCTION_INVERSE_TRIG, 1, 1 },
    { "acos",  FUNCTION_INVERSE_TRIG, 1, 1 },
    { "atan2", FUNCTION_ATAN2,        2, 2 },
    { "sqrt",  FUNCTION_PLAIN,        1, 1 },
    { "exp",   FUNCTION_PLAIN,        1, 1 },
    { "log",   FUNCTION_PLAIN,        1, 1 },
    { "abs",   FUNCTION_ABS,          1, 1 },
    { "min",   FUNCTION_MIN_MAX,      2, 0 },
    { "max",   FUNCTION_MIN_MAX,      2, 0 },
};

//////////////////////////////////////////////////////////////////////////////
// Errors and tree access
//////////////////////////////////////////////////////////////////////////////

// Record an error at a token and return false, for `return Checker_failAt(...)`.
static bool Checker_failAt( Checker *c, uint32_t unit, uint32_t token, const char *format, ... )
{
    const Token *t = &c->model->units[ unit ].tokens.data[ token ];
    LoadError   *e = c->error;
    e->unit = unit;
    e->line = t->line;
    e->col  = t->col;

    va_list args;
    va_start( args, format );
    vsnprintf( e->message, sizeof( e->message ), format, args );
    va_end( args );
    return false;
}

static const Node *Checker_node( const Checker *c, uint32_t unit, uint32_t node )
{
    return &c->model->units[ unit ].ast.nodes[ node ];
}

static NodeKind Checker_kind( const Checker *c, uint32_t unit, uint32_t node )
{
    return Checker_node( c, unit, node )->kind;
}

static uint32_t Checker_child( const Checker *c, uint32_t unit, uint32_t node )
{
    return Checker_node( c, unit, node )->child;
}

static uint32_t Checker_next( const Checker *c, uint32_t unit, uint32_t node )
{
    return Checker_node( c, unit, node )->next;
}

// The node's first token, where an error about it is reported.
static uint32_t Checker_at( const Checker *c, uint32_t unit, uint32_t node )
{
    return Checker_node( c, unit, node )->firstToken;
}

static const NodeInfo *Checker_info( const Checker *c, uint32_t unit, uint32_t node )
{
    return &c->model->units[ unit ].info[ node ];
}

static ValueInfo *Checker_value( const Checker *c, uint32_t unit, uint32_t node )
{
    return &c->model->units[ unit ].values[ node ];
}

// A token's text, as a view of the source.
static const char *Checker_text( const Checker *c, uint32_t unit, uint32_t token, uint32_t *length )
{
    const Unit  *u = &c->model->units[ unit ];
    const Token *t = &u->tokens.data[ token ];
    *length = t->len;
    return u->source.text + t->offset;
}

static bool Checker_tokenIs( const Checker *c, uint32_t unit, uint32_t token, const char *word )
{
    uint32_t    length;
    const char *text = Checker_text( c, unit, token, &length );
    return strlen( word ) == length && memcmp( word, text, length ) == 0;
}

// Record a node's whole result: not a literal with a unit of its own.
static void Checker_setValue( Checker *c, uint32_t unit, uint32_t node, ValueType type, Dimension dimension )
{
    ValueInfo *v = Checker_value( c, unit, node );
    v->type      = type;
    v->dimension = dimension;
    v->factor    = 0.0;
    v->rows      = 0;
    v->columns   = 0;
}

// What the node's callee is, for a call to a built-in function.
static const FunctionRow *Checker_function( const Checker *c, uint32_t unit, uint32_t call )
{
    uint32_t token = Checker_node( c, unit, Checker_child( c, unit, call ) )->token;
    for ( uint32_t i = 0 ; i < TYPES_COUNT( functionRows ) ; i++ )
    {
        if ( Checker_tokenIs( c, unit, token, functionRows[i].name ) ) return &functionRows[i];
    }
    return NULL;
}

static bool Checker_isDiag( const Checker *c, uint32_t unit, uint32_t node )
{
    if ( Checker_kind( c, unit, node ) != NODE_CALL ) return false;
    const FunctionRow *f = Checker_function( c, unit, node );
    return f != NULL && f->rule == FUNCTION_DIAG;
}

// The token kind of a unary or binary node's operator.
static TokenKind Checker_operatorKind( const Checker *c, uint32_t unit, uint32_t node )
{
    return c->model->units[ unit ].tokens.data[ Checker_node( c, unit, node )->token ].kind;
}

// Will this expression take whatever dimension is expected of it? A point
// won't: it is always a length. Sums over such literals will.
static bool Checker_takesDimension( const Checker *c, uint32_t unit, uint32_t node )
{
    switch ( Checker_kind( c, unit, node ) )
    {
    case NODE_NUMBER:
    case NODE_LIST:
    case NODE_GRID:
        return true;

    case NODE_CALL:
        return Checker_isDiag( c, unit, node );

    case NODE_UNARY:
        return Checker_operatorKind( c, unit, node ) == TOK_MINUS
            && Checker_takesDimension( c, unit, Checker_child( c, unit, node ) );

    case NODE_BINARY:
    {
        TokenKind op = Checker_operatorKind( c, unit, node );
        if ( op != TOK_PLUS && op != TOK_MINUS ) return false;
        uint32_t left = Checker_child( c, unit, node );
        return Checker_takesDimension( c, unit, left )
            && Checker_takesDimension( c, unit, Checker_next( c, unit, left ) );
    }

    default:
        return false;
    }
}

// Three scalars are a vector wherever a vector is wanted (§2.9), and a 3×3
// grid is a matrix (D143). Operators and properties use this before comparing.
static void Checker_coerce( ValueInfo *v, ValueType wanted )
{
    if ( wanted == TYPE_VECTOR && v->type == TYPE_LIST && v->rows == 3 )
    {
        v->type = TYPE_VECTOR;
        v->rows = 0;
    }
    else if ( wanted == TYPE_MATRIX && v->type == TYPE_GRID && v->rows == 3 && v->columns == 3 )
    {
        v->type    = TYPE_MATRIX;
        v->rows    = 0;
        v->columns = 0;
    }
}

//////////////////////////////////////////////////////////////////////////////
// Literals
//////////////////////////////////////////////////////////////////////////////

static bool tryInfer( Checker *c, Site site, uint32_t node, const Dimension *expected );

// The entries inside brackets are scalars of no dimension of their own: the
// bracket's single unit covers them all (D63, D129).
static bool tryCheckEntries( Checker *c, Site site, uint32_t first, const char *what, uint32_t *count )
{
    *count = 0;
    for ( uint32_t e = first ; e != 0 ; e = Checker_next( c, site.unit, e ) )
    {
        if ( !tryInfer( c, site, e, NULL ) ) return false;

        const ValueInfo *v = Checker_value( c, site.unit, e );
        if ( v->type != TYPE_SCALAR )
        {
            char type[64];
            ValueInfo_formatType( v, type, sizeof( type ) );
            return Checker_failAt( c, site.unit, Checker_at( c, site.unit, e ), "the entries of %s are scalars, and this is a %s (§3.4)", what, type );
        }
        if ( !Dimension_isNone( &v->dimension ) )
        {
            return Checker_failAt( c, site.unit, Checker_at( c, site.unit, e ),
                                   "not implemented yet: an entry with a dimension of its own inside %s", what );
        }
        ( *count )++;
    }
    return true;
}

// Work out a literal's type from its form, and record it as dimensionless.
static bool tryShapeLiteral( Checker *c, Site site, uint32_t node )
{
    uint32_t count = 0;
    switch ( Checker_kind( c, site.unit, node ) )
    {
    case NODE_NUMBER:
        Checker_setValue( c, site.unit, node, TYPE_SCALAR, Dimension_none() );
        return true;

    case NODE_POINT:
        if ( !tryCheckEntries( c, site, Checker_child( c, site.unit, node ), "a point", &count ) ) return false;
        Checker_setValue( c, site.unit, node, TYPE_POINT, Dimension_none() );
        return true;

    case NODE_LIST:
        if ( !tryCheckEntries( c, site, Checker_child( c, site.unit, node ), "a list", &count ) ) return false;
        Checker_setValue( c, site.unit, node, TYPE_LIST, Dimension_none() );
        Checker_value( c, site.unit, node )->rows = count;
        return true;

    case NODE_GRID:
    {
        uint32_t rows    = 0;
        uint32_t columns = 0;
        for ( uint32_t row = Checker_child( c, site.unit, node ) ; row != 0 ; row = Checker_next( c, site.unit, row ) )
        {
            if ( !tryCheckEntries( c, site, Checker_child( c, site.unit, row ), "a grid", &count ) ) return false;
            columns = count;
            rows++;
        }
        Checker_setValue( c, site.unit, node, TYPE_GRID, Dimension_none() );
        Checker_value( c, site.unit, node )->rows    = rows;
        Checker_value( c, site.unit, node )->columns = columns;
        return true;
    }

    case NODE_CALL:                                               // diag
    {
        uint32_t first = Checker_next( c, site.unit, Checker_child( c, site.unit, node ) );
        if ( !tryCheckEntries( c, site, first, "'diag'", &count ) ) return false;
        if ( count != 3 )
        {
            return Checker_failAt( c, site.unit, Checker_at( c, site.unit, node ), "'diag' takes 3 arguments, and this has %u (§4.1)", count );
        }
        Checker_setValue( c, site.unit, node, TYPE_MATRIX, Dimension_none() );
        return true;
    }

    default:
        return Checker_failAt( c, site.unit, Checker_at( c, site.unit, node ), "not implemented yet: a unit on a %s", NodeKind_name( Checker_kind( c, site.unit, node ) ) );
    }
}

// The default unit for this base dimension, from the scope outward (D147).
static const Measure *Checker_findDefault( const Checker *c, uint32_t scope, BaseDimension base )
{
    for ( ; scope != 0 ; scope = c->model->scopes[ scope ].parent )
    {
        const ScopeDefaults *d = &c->defaults[ scope ];
        if ( d->set[ base ] ) return &d->measure[ base ];
    }
    return NULL;
}

// A literal with no unit takes what's expected of it, if it can (D147).
static bool tryApplyDefault( Checker *c, Site site, uint32_t node, const Dimension *expected )
{
    if ( expected == NULL || Dimension_isNone( expected ) ) return true;

    char text[ DIMENSION_TEXT_SIZE ];
    Dimension_format( expected, text, sizeof( text ) );

    BaseDimension base;
    if ( !Dimension_isBase( expected, &base ) )
    {
        return Checker_failAt( c, site.unit, Checker_at( c, site.unit, node ), "this value must carry a unit: "
                               "a value of dimension %s takes no default (D147)", text );
    }

    const Measure *measure = Checker_findDefault( c, site.scope, base );
    if ( measure == NULL )
    {
        return Checker_failAt( c, site.unit, Checker_at( c, site.unit, node ), "this value needs a unit: "
                               "no 'units:' default applies to dimension %s (D147)", text );
    }

    ValueInfo *v = Checker_value( c, site.unit, node );
    v->dimension = measure->dimension;
    v->factor    = measure->factor;
    return true;
}

// A literal written without a unit. A point is always a length, whatever else
// is expected of it.
static bool tryInferBareLiteral( Checker *c, Site site, uint32_t node, const Dimension *expected )
{
    if ( !tryShapeLiteral( c, site, node ) ) return false;

    Dimension length = ExpectedDimension_value( DIM_LENGTH );
    if ( Checker_kind( c, site.unit, node ) == NODE_POINT ) expected = &length;
    return tryApplyDefault( c, site, node, expected );
}

// `value unit`: a literal, or a call, and the unit run after it (D125).
static bool tryInferQuantity( Checker *c, Site site, uint32_t node, const Dimension *expected )
{
    uint32_t    inner    = Checker_child( c, site.unit, node );
    uint32_t    unitNode = Checker_next( c, site.unit, inner );
    NodeKind    kind     = Checker_kind( c, site.unit, inner );
    bool        isPoint  = kind == NODE_POINT;
    Dimension   length   = ExpectedDimension_value( DIM_LENGTH );
    const Unit *u        = &c->model->units[ site.unit ];
    const Node *run      = &u->ast.nodes[ unitNode ];

    Measure   measure;
    UnitError unitError;
    if ( !tryReadUnitRun( u->tokens.data, run->firstToken, run->lastToken, u->source.text,
                          isPoint ? &length : expected, &measure, &unitError ) )
    {
        return Checker_failAt( c, site.unit, unitError.token, "%s", unitError.message );
    }

    if ( kind == NODE_CALL && !Checker_isDiag( c, site.unit, inner ) )
    {
        if ( !tryInfer( c, site, inner, NULL ) ) return false;
        const ValueInfo *v = Checker_value( c, site.unit, inner );
        if ( !Dimension_isNone( &v->dimension ) )
        {
            char text[ DIMENSION_TEXT_SIZE ];
            Dimension_format( &v->dimension, text, sizeof( text ) );
            return Checker_failAt( c, site.unit, run->firstToken, "a unit can follow only a dimensionless value, and this has dimension %s (§3.5)", text );
        }
    }
    else if ( !tryShapeLiteral( c, site, inner ) )
    {
        return false;
    }

    if ( isPoint && !Dimension_equal( &measure.dimension, &length ) )
    {
        char text[ DIMENSION_TEXT_SIZE ];
        Dimension_format( &measure.dimension, text, sizeof( text ) );
        return Checker_failAt( c, site.unit, run->firstToken, "a point is a length, and this unit has dimension %s (§3.5)", text );
    }

    ValueInfo *v = Checker_value( c, site.unit, node );
    *v           = *Checker_value( c, site.unit, inner );
    v->dimension = measure.dimension;
    v->factor    = measure.factor;
    return true;
}

//////////////////////////////////////////////////////////////////////////////
// Names
//////////////////////////////////////////////////////////////////////////////

// Check the `=` value of symbol `s`, unless that's already done. It keeps the
// defaults of the scope that declares it, wherever it's first needed.
static bool tryCheckValueSymbol( Checker *c, uint32_t s, uint32_t useUnit, uint32_t useToken )
{
    const Symbol *symbol = &c->model->symbols[s];
    if ( c->symbolState[s] == SYMBOL_CHECKED ) return true;
    if ( c->symbolState[s] == SYMBOL_CHECKING )
    {
        return Checker_failAt( c, useUnit, useToken, "'%.*s' is defined in terms of itself", (int)symbol->length, symbol->name );
    }
    c->symbolState[s] = SYMBOL_CHECKING;

    Site     site      = { symbol->unit, symbol->scope };
    uint32_t statement = symbol->node;
    uint32_t key       = Checker_child( c, site.unit, statement );
    uint32_t items     = Checker_next( c, site.unit, key );
    uint32_t value     = Checker_child( c, site.unit, items );
    if ( !tryInfer( c, site, value, NULL ) ) return false;

    ValueInfo *named = Checker_value( c, site.unit, statement );
    *named           = *Checker_value( c, site.unit, value );
    named->factor    = 0.0;

    c->symbolState[s] = SYMBOL_CHECKED;
    return true;
}

static bool tryInferName( Checker *c, Site site, uint32_t node )
{
    const NodeInfo *info = Checker_info( c, site.unit, node );
    uint32_t        at   = Checker_at( c, site.unit, node );
    if ( info->role != ROLE_REFERENCE )
    {
        return Checker_failAt( c, site.unit, at, "not implemented yet: a name in this position" );
    }

    const Symbol *symbol = &c->model->symbols[ info->symbol ];
    switch ( symbol->kind )
    {
    case SYM_BUILTIN_VALUE:
    {
        const BuiltinValue *builtin = Catalog_findBuiltinValue( symbol->name, symbol->length );
        if ( builtin == NULL || builtin->type == TYPE_NONE ) break;
        Checker_setValue( c, site.unit, node, builtin->type, ExpectedDimension_value( builtin->dimension ) );
        return true;
    }

    case SYM_VALUE:
    {
        if ( !tryCheckValueSymbol( c, info->symbol, site.unit, at ) ) return false;
        ValueInfo *v = Checker_value( c, site.unit, node );
        *v = *Checker_value( c, symbol->unit, symbol->node );
        return true;
    }

    default:
        break;
    }
    return Checker_failAt( c, site.unit, at, "not implemented yet: '%.*s' used as a value", (int)symbol->length, symbol->name );
}

//////////////////////////////////////////////////////////////////////////////
// Operators
//////////////////////////////////////////////////////////////////////////////

// Siblings whose units must agree. One that has a dimension of its own goes
// first, so that bare literals beside it can take that dimension (§3.5).
static bool tryInferTogether( Checker *c, Site site, uint32_t first, const Dimension *expected )
{
    uint32_t anchor = first;
    for ( uint32_t n = first ; n != 0 ; n = Checker_next( c, site.unit, n ) )
    {
        if ( !Checker_takesDimension( c, site.unit, n ) )
        {
            anchor = n;
            break;
        }
    }

    if ( !tryInfer( c, site, anchor, expected ) ) return false;

    Dimension shared = Checker_value( c, site.unit, anchor )->dimension;
    for ( uint32_t n = first ; n != 0 ; n = Checker_next( c, site.unit, n ) )
    {
        if ( n != anchor && !tryInfer( c, site, n, &shared ) ) return false;
    }
    return true;
}

static bool tryBinaryOpOf( const Checker *c, uint32_t unit, uint32_t node, BinaryOp *op )
{
    uint32_t token = Checker_node( c, unit, node )->token;
    switch ( Checker_operatorKind( c, unit, node ) )
    {
    case TOK_PLUS:          *op = OP_ADD;      return true;
    case TOK_MINUS:         *op = OP_SUBTRACT; return true;
    case TOK_STAR:          *op = OP_MULTIPLY; return true;
    case TOK_SLASH:         *op = OP_DIVIDE;   return true;
    case TOK_DOT_PRODUCT:   *op = OP_DOT;      return true;
    case TOK_CROSS_PRODUCT: *op = OP_CROSS;    return true;
    case TOK_APPLY:         *op = OP_APPLY;    return true;
    default:
        if ( Checker_tokenIs( c, unit, token, "mod" ) )
        {
            *op = OP_MOD;
            return true;
        }
        return false;
    }
}

static bool tryInferBinary( Checker *c, Site site, uint32_t node, const Dimension *expected )
{
    uint32_t token = Checker_node( c, site.unit, node )->token;
    uint32_t left  = Checker_child( c, site.unit, node );
    uint32_t right = Checker_next( c, site.unit, left );

    uint32_t    length;
    const char *spelling = Checker_text( c, site.unit, token, &length );

    BinaryOp op;
    if ( !tryBinaryOpOf( c, site.unit, node, &op ) )
    {
        return Checker_failAt( c, site.unit, token, "not implemented yet: the operator '%.*s'", (int)length, spelling );
    }

    if ( op == OP_ADD || op == OP_SUBTRACT || op == OP_MOD )
    {
        if ( !tryInferTogether( c, site, left, expected ) ) return false;
    }
    else
    {
        if ( !tryInfer( c, site, left, NULL ) || !tryInfer( c, site, right, NULL ) ) return false;
    }

    ValueInfo *l = Checker_value( c, site.unit, left );
    ValueInfo *r = Checker_value( c, site.unit, right );
    Checker_coerce( l, TYPE_VECTOR );
    Checker_coerce( r, TYPE_VECTOR );
    Checker_coerce( l, TYPE_MATRIX );
    Checker_coerce( r, TYPE_MATRIX );

    ValueType     result;
    DimensionRule rule;
    if ( !tryBinaryType( op, l->type, r->type, &result, &rule ) )
    {
        char a[64];
        char b[64];
        ValueInfo_formatType( l, a, sizeof( a ) );
        ValueInfo_formatType( r, b, sizeof( b ) );

        bool affine = ( op == OP_ADD && l->type == TYPE_POINT && r->type == TYPE_POINT )
                   || ( op == OP_MULTIPLY && ( l->type == TYPE_POINT || r->type == TYPE_POINT ) );
        return Checker_failAt( c, site.unit, token, "there is no operation '%.*s' between %s and %s (§3.4%s)", (int)length, spelling,
                               a, b, affine ? "; use avg or mid, D40" : "" );
    }

    Dimension dimension = l->dimension;
    switch ( rule )
    {
    case RULE_SAME:
        if ( !Dimension_equal( &l->dimension, &r->dimension ) )
        {
            char a[ DIMENSION_TEXT_SIZE ];
            char b[ DIMENSION_TEXT_SIZE ];
            Dimension_format( &l->dimension, a, sizeof( a ) );
            Dimension_format( &r->dimension, b, sizeof( b ) );
            return Checker_failAt( c, site.unit, token, "the units of '%.*s' must agree, and these are %s and %s (§3.4)", (int)length, spelling, a, b );
        }
        break;
    case RULE_PRODUCT:
        dimension = Dimension_multiply( &l->dimension, &r->dimension );
        break;
    case RULE_QUOTIENT:
        dimension = Dimension_divide( &l->dimension, &r->dimension );
        break;
    }

    Checker_setValue( c, site.unit, node, result, dimension );
    return true;
}

static bool tryInferUnary( Checker *c, Site site, uint32_t node, const Dimension *expected )
{
    uint32_t  token   = Checker_node( c, site.unit, node )->token;
    uint32_t  operand = Checker_child( c, site.unit, node );
    TokenKind kind    = Checker_operatorKind( c, site.unit, node );

    uint32_t    length;
    const char *spelling = Checker_text( c, site.unit, token, &length );

    UnaryOp op;
    if ( kind == TOK_MINUS )
    {
        op = UNARY_NEGATE;
        if ( !tryInfer( c, site, operand, expected ) ) return false;
    }
    else if ( kind == TOK_CARET )
    {
        op = UNARY_INVERSE;
        if ( !tryInfer( c, site, operand, NULL ) ) return false;
    }
    else
    {
        return Checker_failAt( c, site.unit, token, "not implemented yet: the operator '%.*s'", (int)length, spelling );
    }

    ValueInfo *v = Checker_value( c, site.unit, operand );
    Checker_coerce( v, TYPE_VECTOR );
    Checker_coerce( v, TYPE_MATRIX );

    ValueType result;
    if ( !tryUnaryType( op, v->type, &result ) )
    {
        char type[64];
        ValueInfo_formatType( v, type, sizeof( type ) );
        return Checker_failAt( c, site.unit, token, "there is no operation '%.*s' on a %s (§3.4)", (int)length, spelling, type );
    }

    Dimension dimension = v->dimension;
    if ( op == UNARY_INVERSE )
    {
        Dimension none = Dimension_none();
        dimension = Dimension_divide( &none, &v->dimension );
    }
    Checker_setValue( c, site.unit, node, result, dimension );
    return true;
}

//////////////////////////////////////////////////////////////////////////////
// Calls
//////////////////////////////////////////////////////////////////////////////

// Checked one way for every kind of argument: its type, then its dimension if
// `dimension` is not NULL.
static bool tryRequireValue( Checker *c, Site site, uint32_t node, const char *function, ValueType type, const Dimension *dimension )
{
    const ValueInfo *v  = Checker_value( c, site.unit, node );
    uint32_t         at = Checker_at( c, site.unit, node );
    if ( v->type != type )
    {
        char got[64];
        ValueInfo_formatType( v, got, sizeof( got ) );
        return Checker_failAt( c, site.unit, at, "'%s' takes a %s here, and this is a %s (§4.1)", function, ValueType_name( type ), got );
    }
    if ( dimension != NULL && !Dimension_equal( &v->dimension, dimension ) )
    {
        char want[ DIMENSION_TEXT_SIZE ];
        char got[ DIMENSION_TEXT_SIZE ];
        Dimension_format( dimension, want, sizeof( want ) );
        Dimension_format( &v->dimension, got, sizeof( got ) );
        return Checker_failAt( c, site.unit, at, "'%s' takes dimension %s here, and this has dimension %s (§4.1)", function, want, got );
    }
    return true;
}

// Every argument in the list has `type` and, when given, `dimension`.
static bool tryRequireAll( Checker *c, Site site, uint32_t first, const char *function, ValueType type, const Dimension *dimension )
{
    for ( uint32_t a = first ; a != 0 ; a = Checker_next( c, site.unit, a ) )
    {
        if ( !tryRequireValue( c, site, a, function, type, dimension ) ) return false;
    }
    return true;
}

static bool tryInferCall( Checker *c, Site site, uint32_t node, const Dimension *expected )
{
    uint32_t callee = Checker_child( c, site.unit, node );
    uint32_t first  = Checker_next( c, site.unit, callee );
    uint32_t at     = Checker_at( c, site.unit, node );

    const FunctionRow *f = Checker_function( c, site.unit, node );
    if ( f == NULL )
    {
        return Checker_failAt( c, site.unit, at, "not implemented yet: this function" );
    }

    uint32_t count = 0;
    for ( uint32_t a = first ; a != 0 ; a = Checker_next( c, site.unit, a ) ) count++;
    if ( count < f->minArguments || ( f->maxArguments != 0 && count > f->maxArguments ) )
    {
        if ( f->maxArguments == f->minArguments )
        {
            return Checker_failAt( c, site.unit, at, "'%s' takes %u argument%s, and this has %u (§4.1)", f->name,
                                   f->minArguments, f->minArguments == 1 ? "" : "s", count );
        }
        return Checker_failAt( c, site.unit, at, "'%s' takes at least %u argument%s, and this has %u (§4.1)", f->name,
                               f->minArguments, f->minArguments == 1 ? "" : "s", count );
    }

    Dimension none   = Dimension_none();
    Dimension angle  = Dimension_make( 0, 0, 0, 1 );
    Dimension length = ExpectedDimension_value( DIM_LENGTH );

    switch ( f->rule )
    {
    case FUNCTION_DIAG:                                           // handled as a literal
        break;

    case FUNCTION_CENTROID:
        for ( uint32_t a = first ; a != 0 ; a = Checker_next( c, site.unit, a ) )
        {
            if ( !tryInfer( c, site, a, NULL ) ) return false;
        }
        if ( !tryRequireAll( c, site, first, f->name, TYPE_POINT, &length ) ) return false;
        Checker_setValue( c, site.unit, node, TYPE_POINT, length );
        return true;

    case FUNCTION_FRAME:
    {
        uint32_t orientation = Checker_next( c, site.unit, first );
        if ( !tryInfer( c, site, first, NULL ) || !tryInfer( c, site, orientation, NULL ) ) return false;
        if ( !tryRequireValue( c, site, first, f->name, TYPE_POINT, &length ) ) return false;
        if ( !tryRequireValue( c, site, orientation, f->name, TYPE_ORIENTATION, NULL ) ) return false;
        Checker_setValue( c, site.unit, node, TYPE_FRAME, none );
        return true;
    }

    case FUNCTION_TRIG:
        if ( !tryInfer( c, site, first, &angle ) ) return false;
        if ( !tryRequireValue( c, site, first, f->name, TYPE_SCALAR, &angle ) ) return false;
        Checker_setValue( c, site.unit, node, TYPE_SCALAR, none );
        return true;

    case FUNCTION_INVERSE_TRIG:
        if ( !tryInfer( c, site, first, &none ) ) return false;
        if ( !tryRequireValue( c, site, first, f->name, TYPE_SCALAR, &none ) ) return false;
        Checker_setValue( c, site.unit, node, TYPE_SCALAR, angle );
        return true;

    case FUNCTION_ATAN2:
    {
        if ( !tryInferTogether( c, site, first, NULL ) ) return false;
        Dimension shared = Checker_value( c, site.unit, first )->dimension;
        if ( !tryRequireAll( c, site, first, f->name, TYPE_SCALAR, &shared ) ) return false;
        Checker_setValue( c, site.unit, node, TYPE_SCALAR, angle );
        return true;
    }

    case FUNCTION_PLAIN:
        if ( !tryInfer( c, site, first, &none ) ) return false;
        if ( !tryRequireValue( c, site, first, f->name, TYPE_SCALAR, &none ) ) return false;
        Checker_setValue( c, site.unit, node, TYPE_SCALAR, none );
        return true;

    case FUNCTION_ABS:
        if ( !tryInfer( c, site, first, expected ) ) return false;
        if ( !tryRequireValue( c, site, first, f->name, TYPE_SCALAR, NULL ) ) return false;
        Checker_setValue( c, site.unit, node, TYPE_SCALAR, Checker_value( c, site.unit, first )->dimension );
        return true;

    case FUNCTION_MIN_MAX:
    {
        if ( !tryInferTogether( c, site, first, expected ) ) return false;
        Dimension shared = Checker_value( c, site.unit, first )->dimension;
        if ( !tryRequireAll( c, site, first, f->name, TYPE_SCALAR, &shared ) ) return false;
        Checker_setValue( c, site.unit, node, TYPE_SCALAR, shared );
        return true;
    }
    }
    return Checker_failAt( c, site.unit, at, "not implemented yet: this function" );
}

//////////////////////////////////////////////////////////////////////////////
// Expressions
//////////////////////////////////////////////////////////////////////////////

// Work out the type and dimension of an expression and record them. `expected`
// is the dimension wanted of it, or NULL when nothing is.
static bool tryInfer( Checker *c, Site site, uint32_t node, const Dimension *expected )
{
    NodeKind kind = Checker_kind( c, site.unit, node );
    switch ( kind )
    {
    case NODE_NUMBER:
    case NODE_POINT:
    case NODE_LIST:
    case NODE_GRID:
        return tryInferBareLiteral( c, site, node, expected );

    case NODE_STRING:
        Checker_setValue( c, site.unit, node, TYPE_STRING, Dimension_none() );
        return true;

    case NODE_QUANTITY:
        return tryInferQuantity( c, site, node, expected );

    case NODE_NAME:
        return tryInferName( c, site, node );

    case NODE_UNARY:
        return tryInferUnary( c, site, node, expected );

    case NODE_BINARY:
        return tryInferBinary( c, site, node, expected );

    case NODE_CALL:
        if ( Checker_isDiag( c, site.unit, node ) ) return tryInferBareLiteral( c, site, node, expected );
        return tryInferCall( c, site, node, expected );

    default:
        return Checker_failAt( c, site.unit, Checker_at( c, site.unit, node ), "not implemented yet: the type of a %s", NodeKind_name( kind ) );
    }
}

//////////////////////////////////////////////////////////////////////////////
// Properties and statements
//////////////////////////////////////////////////////////////////////////////

// Check a value against what the catalog says of its property (§4).
static bool tryCheckProperty( Checker *c, Site site, uint32_t value, const PropertyInfo *property )
{
    if ( property->space != VALUE_EXPRESSION ) return true;

    bool      any    = property->dimension == DIM_ANY;
    Dimension wanted = ExpectedDimension_value( property->dimension );
    if ( !tryInfer( c, site, value, any ? NULL : &wanted ) ) return false;

    ValueInfo *v  = Checker_value( c, site.unit, value );
    uint32_t   at = Checker_at( c, site.unit, value );

    Checker_coerce( v, property->type );
    if ( v->type != property->type )
    {
        char type[64];
        ValueInfo_formatType( v, type, sizeof( type ) );
        return Checker_failAt( c, site.unit, at, "'%s' takes a %s, and this is a %s (§4)", property->name, ValueType_name( property->type ), type );
    }

    if ( !any && !Dimension_equal( &v->dimension, &wanted ) )
    {
        char want[ DIMENSION_TEXT_SIZE ];
        char got[ DIMENSION_TEXT_SIZE ];
        Dimension_format( &wanted, want, sizeof( want ) );
        Dimension_format( &v->dimension, got, sizeof( got ) );
        return Checker_failAt( c, site.unit, at, "'%s' takes dimension %s, and this has dimension %s (§3.5)", property->name, want, got );
    }
    return true;
}

// `key: value;`
static bool tryCheckPropertyStatement( Checker *c, Site site, uint32_t statement )
{
    uint32_t key   = Checker_child( c, site.unit, statement );
    uint32_t items = Checker_next( c, site.unit, key );
    return tryCheckProperty( c, site, Checker_child( c, site.unit, items ), Checker_info( c, site.unit, statement )->property );
}

// What follows an element's head, in either shape (D152).
static bool tryCheckElementValue( Checker *c, Site site, uint32_t value )
{
    uint32_t head = Checker_child( c, site.unit, value );
    uint32_t rest = Checker_next( c, site.unit, head );

    if ( Checker_kind( c, site.unit, value ) == NODE_TYPED_BLOCK )
    {
        for ( uint32_t s = Checker_child( c, site.unit, rest ) ; s != 0 ; s = Checker_next( c, site.unit, s ) )
        {
            if ( !tryCheckPropertyStatement( c, site, s ) ) return false;
        }
        return true;
    }

    for ( uint32_t item = rest ; item != 0 ; item = Checker_next( c, site.unit, item ) )
    {
        uint32_t key       = Checker_child( c, site.unit, item );
        uint32_t itemValue = Checker_next( c, site.unit, key );
        if ( !tryCheckProperty( c, site, itemValue, Checker_info( c, site.unit, item )->property ) ) return false;
    }
    return true;
}

// Does this statement hold a block? If so, says which, and the scope inside it.
static bool Checker_innerBlock( const Checker *c, uint32_t unit, uint32_t statement, uint32_t scope, uint32_t *block, uint32_t *inner )
{
    const NodeInfo *info  = Checker_info( c, unit, statement );
    uint32_t        first = Checker_child( c, unit, statement );

    switch ( Checker_kind( c, unit, statement ) )
    {
    case NODE_NAMESPACE:
    {
        bool named = Checker_kind( c, unit, first ) != NODE_BLOCK;
        *block = named ? Checker_next( c, unit, first ) : first;
        *inner = named ? c->model->symbols[ info->symbol ].ownScope : scope;
        return true;
    }

    case NODE_SYSTEM:
        *block = Checker_next( c, unit, first );
        *inner = c->model->symbols[ info->symbol ].ownScope;
        return true;

    case NODE_SCOPE:
        *block = Checker_next( c, unit, first );
        *inner = info->role == ROLE_BODY ? c->model->symbols[ info->symbol ].ownScope : scope;
        return true;

    default:
        return false;
    }
}

static bool tryCheckBlock( Checker *c, uint32_t unit, uint32_t block, uint32_t scope );

static bool tryCheckStatement( Checker *c, uint32_t unit, uint32_t statement, uint32_t scope )
{
    uint32_t block = 0;
    uint32_t inner = 0;
    if ( Checker_innerBlock( c, unit, statement, scope, &block, &inner ) ) return tryCheckBlock( c, unit, block, inner );

    Site            site  = { unit, scope };
    const NodeInfo *info  = Checker_info( c, unit, statement );
    uint32_t        first = Checker_child( c, unit, statement );
    uint32_t        value = first != 0 ? Checker_next( c, unit, first ) : 0;

    switch ( Checker_kind( c, unit, statement ) )
    {
    case NODE_DEFINITION:
        if ( info->role == ROLE_VALUE ) return tryCheckValueSymbol( c, info->symbol, unit, Checker_at( c, unit, statement ) );
        return tryCheckElementValue( c, site, value );

    case NODE_DECLARATION:
        if ( info->role == ROLE_PROPERTY ) return tryCheckPropertyStatement( c, site, statement );
        return tryCheckElementValue( c, site, value );

    case NODE_DRIVE:
        return Checker_failAt( c, unit, Checker_at( c, unit, statement ), "not implemented yet: the type of a driver" );

    case NODE_REFERENCE:
        return Checker_failAt( c, unit, Checker_at( c, unit, statement ), "not implemented yet: the type of a bare reference" );

    default:
        return true;                                              // wiring is §3.7's; `use` and `provides` have no values
    }
}

static bool tryCheckBlock( Checker *c, uint32_t unit, uint32_t block, uint32_t scope )
{
    for ( uint32_t s = Checker_child( c, unit, block ) ; s != 0 ; s = Checker_next( c, unit, s ) )
    {
        if ( !tryCheckStatement( c, unit, s, scope ) ) return false;
    }
    return true;
}

//////////////////////////////////////////////////////////////////////////////
// `units:` (D147)
//////////////////////////////////////////////////////////////////////////////

static bool tryReadDefaults( Checker *c, uint32_t unit, uint32_t statement, uint32_t scope )
{
    static const char *const baseNames[ BASE_COUNT ] = { "length", "mass", "time", "angle" };

    uint32_t       key   = Checker_child( c, unit, statement );
    uint32_t       items = Checker_next( c, unit, key );
    ScopeDefaults *d     = &c->defaults[ scope ];

    for ( uint32_t item = Checker_child( c, unit, items ) ; item != 0 ; item = Checker_next( c, unit, item ) )
    {
        uint32_t at = Checker_at( c, unit, item );
        if ( Checker_kind( c, unit, item ) != NODE_NAME )
        {
            return Checker_failAt( c, unit, at, "each entry of 'units:' is the name of a unit (D147)" );
        }

        uint32_t      length;
        const char   *name = Checker_text( c, unit, Checker_node( c, unit, item )->token, &length );
        BaseDimension base;
        Measure       measure;
        UnitError     unitError;
        if ( !tryReadDefaultUnit( name, length, &base, &measure, &unitError ) )
        {
            return Checker_failAt( c, unit, at, "%s", unitError.message );
        }
        if ( d->set[ base ] )
        {
            return Checker_failAt( c, unit, at, "'units:' sets %s more than once (D147)", baseNames[ base ] );
        }
        d->set[ base ]     = true;
        d->measure[ base ] = measure;
    }
    return true;
}

static bool tryCollectDefaults( Checker *c, uint32_t unit, uint32_t block, uint32_t scope )
{
    for ( uint32_t s = Checker_child( c, unit, block ) ; s != 0 ; s = Checker_next( c, unit, s ) )
    {
        uint32_t inner = 0;
        uint32_t held  = 0;
        if ( Checker_innerBlock( c, unit, s, scope, &held, &inner ) )
        {
            if ( !tryCollectDefaults( c, unit, held, inner ) ) return false;
            continue;
        }

        const NodeInfo *info = Checker_info( c, unit, s );
        if ( Checker_kind( c, unit, s ) == NODE_DECLARATION && info->role == ROLE_PROPERTY && info->property->space == VALUE_UNIT )
        {
            if ( !tryReadDefaults( c, unit, s, scope ) ) return false;
        }
    }
    return true;
}

//////////////////////////////////////////////////////////////////////////////
// Public API
//////////////////////////////////////////////////////////////////////////////

static void *Checker_allocate( uint32_t count, size_t size )
{
    void *memory = calloc( count, size );
    if ( memory == NULL )
    {
        fprintf( stderr, "ombdc: out of memory\n" );
        exit( 1 );
    }
    return memory;
}

bool tryCheckTypes( Model *model, LoadError *error )
{
    Checker checker;
    checker.model       = model;
    checker.error       = error;
    checker.defaults    = (ScopeDefaults *)Checker_allocate( model->scopeCount, sizeof( ScopeDefaults ) );
    checker.symbolState = (uint8_t *)Checker_allocate( model->symbolCount, sizeof( uint8_t ) );

    for ( uint32_t u = 0 ; u < model->unitCount ; u++ )
    {
        model->units[u].values = (ValueInfo *)Checker_allocate( model->units[u].ast.count, sizeof( ValueInfo ) );
    }

    bool ok = true;
    for ( uint32_t u = 0 ; ok && u < model->unitCount ; u++ ) ok = tryCollectDefaults( &checker, u, 0, SCOPE_INDEX_TOP );
    for ( uint32_t u = 0 ; ok && u < model->unitCount ; u++ ) ok = tryCheckBlock( &checker, u, 0, SCOPE_INDEX_TOP );

    free( checker.defaults );
    free( checker.symbolState );
    return ok;
}

//////////////////////////////////////////////////////////////////////////////
// The dump
//////////////////////////////////////////////////////////////////////////////

static void Checker_append( char *buffer, uint32_t size, uint32_t *used, const char *format, ... )
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

static void Checker_formatNode( const Model *m, uint32_t unit, uint32_t index, char *buffer, uint32_t size, uint32_t *used )
{
    const Unit      *u     = &m->units[ unit ];
    const Node      *node  = &u->ast.nodes[ index ];
    const ValueInfo *value = &u->values[ index ];

    if ( value->type != TYPE_NONE )
    {
        const Token *at = &u->tokens.data[ node->firstToken ];
        char type[64];
        char dimension[ DIMENSION_TEXT_SIZE ];
        ValueInfo_formatType( value, type, sizeof( type ) );
        Dimension_format( &value->dimension, dimension, sizeof( dimension ) );

        Checker_append( buffer, size, used, "%u:%u %s %s %s", at->line, at->col, NodeKind_name( node->kind ), type, dimension );
        if ( value->factor != 0.0 ) Checker_append( buffer, size, used, " factor %.17g", value->factor );
        Checker_append( buffer, size, used, "\n" );
    }

    for ( uint32_t c = node->child ; c != 0 ; c = u->ast.nodes[ c ].next )
    {
        Checker_formatNode( m, unit, c, buffer, size, used );
    }
}

uint32_t Model_formatTypes( const Model *m, char *buffer, uint32_t size )
{
    uint32_t used = 0;
    for ( uint32_t unit = 0 ; unit < m->unitCount ; unit++ )
    {
        Checker_append( buffer, size, &used, "unit %u\n", unit + 1 );
        Checker_formatNode( m, unit, 0, buffer, size, &used );
    }
    return used;
}

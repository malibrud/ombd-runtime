// resolve.cpp -- loading: merging texts and resolving names (Specification.md §3.1–§3.3).
//
// Four passes over the parsed units, each stopping at the first error:
//
//   1. tryMerge     unite namespaces, take `gravity` and `simulation`, find the root
//   2. tryDeclare   walk the systems: flatten sections, enter every name in its scope
//   3. tryClassify  with every name known, say what each `=` and `:` statement is
//   4. tryResolve   look up every remaining name in the space its position selects
//
// Declarations are order-independent (§0), which is why declaring comes first.
//
// Not implemented yet, each with its own error: comprehensions, typed open
// signals and other unit bases, the properties of a value (`default:`), the
// `simulation` layer, and the later segments of a path. Only a path's first
// name is resolved.

#include "resolve.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Lookup spaces (§3.2, D144) are sets of symbol kinds.
#define SYM_BIT( kind ) ( 1u << (kind) )

static const uint32_t SPACE_USER = SYM_BIT( SYM_VALUE ) | SYM_BIT( SYM_DEFINITION ) | SYM_BIT( SYM_INSTANCE )
                                 | SYM_BIT( SYM_BODY ) | SYM_BIT( SYM_SYSTEM );

// A system is included only so that reading one can be reported (D2).
static const uint32_t SPACE_EXPRESSION = SPACE_USER | SYM_BIT( SYM_BUILTIN_VALUE );

static const uint32_t SPACE_HEAD     = SYM_BIT( SYM_BUILTIN_TYPE ) | SYM_BIT( SYM_DEFINITION );
static const uint32_t SPACE_FUNCTION = SYM_BIT( SYM_BUILTIN_FUNCTION );
static const uint32_t SPACE_SLOT     = SYM_BIT( SYM_INSTANCE );                // §3.7

// The loader's state. It lives on tryLoadModel's stack.
struct Loader
{
    Model     *model;
    LoadError *error;
};

// What a bare block means depends on where it stands (§2.3, §3.2).
enum BlockContext
{
    CONTEXT_SYSTEM,              // directly in a system: a section
    CONTEXT_SECTION,             // in a section: no bare blocks
    CONTEXT_BODIES,              // in `bodies`: a body
    CONTEXT_BODY,                // directly in a body: `points` or `frames`
};

// How strictly an element's properties are checked (D152).
enum BindMode
{
    BIND_ANY,                    // an instance of a built-in type, or a scope's own properties
    BIND_DEFINITION,             // a shared definition: law only
    BIND_OF_DEFINITION,          // an instance of a definition: placement only
};

// Which of an element's properties have been set: the setting node of each.
struct SeenProperties
{
    uint32_t node[ CATALOG_MAX_PROPERTIES ];
};

//////////////////////////////////////////////////////////////////////////////
// Errors
//////////////////////////////////////////////////////////////////////////////

// Record an error at a token and return false, for `return Loader_failAt(...)`.
static bool Loader_failAt( Loader *l, uint32_t unit, uint32_t token, const char *format, ... )
{
    const Token *t = &l->model->units[ unit ].tokens.data[ token ];
    LoadError   *e = l->error;
    e->unit = unit;
    e->line = t->line;
    e->col  = t->col;

    va_list args;
    va_start( args, format );
    vsnprintf( e->message, sizeof( e->message ), format, args );
    va_end( args );
    return false;
}

//////////////////////////////////////////////////////////////////////////////
// Tree access
//////////////////////////////////////////////////////////////////////////////

static const Node *Loader_node( const Loader *l, uint32_t unit, uint32_t node )
{
    return &l->model->units[ unit ].ast.nodes[ node ];
}

static NodeKind Loader_kind( const Loader *l, uint32_t unit, uint32_t node )
{
    return Loader_node( l, unit, node )->kind;
}

static uint32_t Loader_child( const Loader *l, uint32_t unit, uint32_t node )
{
    return Loader_node( l, unit, node )->child;
}

static uint32_t Loader_next( const Loader *l, uint32_t unit, uint32_t node )
{
    return Loader_node( l, unit, node )->next;
}

// The node's first token, where an error about it is reported.
static uint32_t Loader_at( const Loader *l, uint32_t unit, uint32_t node )
{
    return Loader_node( l, unit, node )->firstToken;
}

static NodeInfo *Loader_info( Loader *l, uint32_t unit, uint32_t node )
{
    return &l->model->units[ unit ].info[ node ];
}

// A token's text, as a view of the source.
static const char *Loader_text( const Loader *l, uint32_t unit, uint32_t token, uint32_t *length )
{
    const Unit  *u = &l->model->units[ unit ];
    const Token *t = &u->tokens.data[ token ];
    *length = t->len;
    return u->source.text + t->offset;
}

// The last child of `node`, or 0.
static uint32_t Loader_lastChild( const Loader *l, uint32_t unit, uint32_t node )
{
    uint32_t last = 0;
    for ( uint32_t c = Loader_child( l, unit, node ) ; c != 0 ; c = Loader_next( l, unit, c ) ) last = c;
    return last;
}

//////////////////////////////////////////////////////////////////////////////
// Scopes and symbols
//////////////////////////////////////////////////////////////////////////////

static uint32_t Model_addScope( Model *m, ScopeKind kind, uint32_t parent, uint32_t owner, const ElementInfo *element )
{
    if ( m->scopeCount == m->scopeCapacity )
    {
        uint32_t capacity = m->scopeCapacity * 2;
        Scope   *scopes   = (Scope *)realloc( m->scopes, sizeof( Scope ) * capacity );
        if ( scopes == NULL )
        {
            fprintf( stderr, "ombdc: out of memory\n" );
            exit( 1 );
        }
        m->scopes        = scopes;
        m->scopeCapacity = capacity;
    }

    uint32_t index = m->scopeCount++;
    Scope   *s     = &m->scopes[ index ];
    s->kind        = kind;
    s->parent      = parent;
    s->firstSymbol = 0;
    s->lastSymbol  = 0;
    s->owner       = owner;
    s->element     = element;
    return index;
}

// Add a symbol at the end of its scope and return its index. Indices stay
// valid as the array grows; pointers into it do not.
static uint32_t Model_addSymbol( Model *m, SymbolKind kind, const char *name, uint32_t length,
                                 uint32_t unit, uint32_t token, uint32_t node, uint32_t scope )
{
    if ( m->symbolCount == m->symbolCapacity )
    {
        uint32_t capacity = m->symbolCapacity * 2;
        Symbol  *symbols  = (Symbol *)realloc( m->symbols, sizeof( Symbol ) * capacity );
        if ( symbols == NULL )
        {
            fprintf( stderr, "ombdc: out of memory\n" );
            exit( 1 );
        }
        m->symbols        = symbols;
        m->symbolCapacity = capacity;
    }

    uint32_t index = m->symbolCount++;
    Symbol  *s     = &m->symbols[ index ];
    s->kind     = kind;
    s->name     = name;
    s->length   = length;
    s->unit     = unit;
    s->token    = token;
    s->node     = node;
    s->scope    = scope;
    s->next     = 0;
    s->ownScope = 0;
    s->type     = NULL;

    Scope *owner = &m->scopes[ scope ];
    if ( owner->lastSymbol == 0 ) owner->firstSymbol = index;
    else                          m->symbols[ owner->lastSymbol ].next = index;
    owner->lastSymbol = index;
    return index;
}

static bool Symbol_isNamed( const Symbol *s, const char *name, uint32_t length )
{
    return s->length == length && memcmp( s->name, name, length ) == 0;
}

// The symbol with this name and one of the kinds in `space`, in this one scope.
static uint32_t Model_findInScope( const Model *m, uint32_t scope, const char *name, uint32_t length, uint32_t space )
{
    for ( uint32_t s = m->scopes[ scope ].firstSymbol ; s != 0 ; s = m->symbols[ s ].next )
    {
        const Symbol *symbol = &m->symbols[ s ];
        if ( ( SYM_BIT( symbol->kind ) & space ) != 0 && Symbol_isNamed( symbol, name, length ) ) return s;
    }
    return 0;
}

// Look a name up from `scope` outward. A system is self-contained, so the
// search goes from a system straight to the built-ins (D1, D2).
static uint32_t Model_lookup( const Model *m, uint32_t scope, const char *name, uint32_t length, uint32_t space )
{
    while ( scope != 0 )
    {
        uint32_t found = Model_findInScope( m, scope, name, length, space );
        if ( found != 0 ) return found;

        const Scope *s = &m->scopes[ scope ];
        scope = s->kind == SCOPE_SYSTEM ? (uint32_t)SCOPE_INDEX_BUILTIN : s->parent;
    }
    return 0;
}

// Enter a user name in `scope`: it must not reuse a built-in value or
// function (D141, D144), and it must be new in that scope.
static bool tryDeclareName( Loader *l, uint32_t unit, uint32_t token, uint32_t node, SymbolKind kind, uint32_t scope, uint32_t *symbol )
{
    Model      *m = l->model;
    uint32_t    length;
    const char *name = Loader_text( l, unit, token, &length );

    uint32_t builtin = Model_findInScope( m, SCOPE_INDEX_BUILTIN, name, length,
                                          SYM_BIT( SYM_BUILTIN_VALUE ) | SYM_BIT( SYM_BUILTIN_FUNCTION ) );
    if ( builtin != 0 )
    {
        return Loader_failAt( l, unit, token, "'%.*s' is a built-in %s; a user name may not reuse it (D141, D144)",
                              (int)length, name,
                              m->symbols[ builtin ].kind == SYM_BUILTIN_VALUE ? "value" : "function" );
    }

    uint32_t earlier = Model_findInScope( m, scope, name, length, SPACE_USER | SYM_BIT( SYM_NAMESPACE ) );
    if ( earlier != 0 )
    {
        const Symbol *e     = &m->symbols[ earlier ];
        const Token  *first = &m->units[ e->unit ].tokens.data[ e->token ];
        if ( e->unit == unit )
        {
            return Loader_failAt( l, unit, token, "'%.*s' is already declared in this scope, at %u:%u (§3.2)",
                                  (int)length, name, first->line, first->col );
        }
        return Loader_failAt( l, unit, token, "'%.*s' is already declared in this scope, at %s:%u:%u (§3.2)",
                              (int)length, name, m->units[ e->unit ].source.path, first->line, first->col );
    }

    *symbol = Model_addSymbol( m, kind, name, length, unit, token, node, scope );
    return true;
}

//////////////////////////////////////////////////////////////////////////////
// Properties
//////////////////////////////////////////////////////////////////////////////

// `item` sets the property named by `keyToken` on `element`: check that it
// exists, that the mode allows it and that it is set only once (§3.3).
static bool tryBindProperty( Loader *l, uint32_t unit, uint32_t item, uint32_t keyToken, const ElementInfo *element,
                             BindMode mode, SeenProperties *seen, uint32_t *lawCount )
{
    uint32_t    length;
    const char *key      = Loader_text( l, unit, keyToken, &length );
    const PropertyInfo *property = ElementInfo_findProperty( element, key, length );
    if ( property == NULL )
    {
        return Loader_failAt( l, unit, keyToken, "'%.*s' is not a property of %s (§4)", (int)length, key, element->name );
    }
    if ( mode == BIND_DEFINITION && !property->law )
    {
        return Loader_failAt( l, unit, keyToken, "'%.*s' is placement; a definition holds law properties only (D152)",
                              (int)length, key );
    }
    if ( mode == BIND_OF_DEFINITION && property->law )
    {
        return Loader_failAt( l, unit, keyToken, "'%.*s' belongs to the definition's law; "
                                                 "an instance of a definition must not restate it (D152)",
                              (int)length, key );
    }

    uint32_t index = (uint32_t)( property - element->properties );
    if ( seen->node[ index ] != 0 )
    {
        const Token *first = &l->model->units[ unit ].tokens.data[ Loader_at( l, unit, seen->node[ index ] ) ];
        return Loader_failAt( l, unit, keyToken, "property '%.*s' is set twice; it was first set at %u:%u (§3.3)",
                              (int)length, key, first->line, first->col );
    }
    seen->node[ index ] = item;

    NodeInfo *info = Loader_info( l, unit, item );
    info->role     = ROLE_PROPERTY;
    info->property = property;

    if ( property->law && lawCount != NULL ) ( *lawCount )++;
    return true;
}

// A property statement `key: value;` has a value list, no `for`, and, unless
// its value is a list of units, exactly one plain value.
static bool tryCheckPropertyShape( Loader *l, uint32_t unit, uint32_t statement, const PropertyInfo *property )
{
    uint32_t key   = Loader_child( l, unit, statement );
    uint32_t value = Loader_next( l, unit, key );

    if ( Loader_kind( l, unit, value ) != NODE_ITEMS )
    {
        return Loader_failAt( l, unit, Loader_at( l, unit, value ), "property '%s' takes a value, not a block", property->name );
    }
    if ( Loader_next( l, unit, value ) != 0 )
    {
        return Loader_failAt( l, unit, Loader_at( l, unit, statement ), "not implemented yet: a comprehension ('for')" );
    }
    if ( property->space == VALUE_EXPRESSION )
    {
        uint32_t first = Loader_child( l, unit, value );
        if ( Loader_next( l, unit, first ) != 0 || Loader_kind( l, unit, first ) == NODE_PROPERTY )
        {
            return Loader_failAt( l, unit, Loader_at( l, unit, value ), "property '%s' takes one value", property->name );
        }
    }
    return true;
}

//////////////////////////////////////////////////////////////////////////////
// Pass 1: merge (§3.1)
//////////////////////////////////////////////////////////////////////////////

static bool tryDeclareSystemSymbol( Loader *l, uint32_t unit, uint32_t statement, uint32_t scope )
{
    uint32_t nameNode = Loader_child( l, unit, statement );
    uint32_t symbol   = 0;
    if ( !tryDeclareName( l, unit, Loader_node( l, unit, nameNode )->token, statement, SYM_SYSTEM, scope, &symbol ) ) return false;

    uint32_t own = Model_addScope( l->model, SCOPE_SYSTEM, scope, symbol, &Catalog_system );
    l->model->symbols[ symbol ].ownScope = own;

    NodeInfo *info = Loader_info( l, unit, statement );
    info->role   = ROLE_SYSTEM;
    info->symbol = symbol;
    return true;
}

// Find or open the namespace named by the NAME at `token` inside `parent`.
static bool tryEnterNamespaceName( Loader *l, uint32_t unit, uint32_t token, uint32_t statement, uint32_t parent, uint32_t *scope )
{
    Model      *m = l->model;
    uint32_t    length;
    const char *name = Loader_text( l, unit, token, &length );

    uint32_t symbol = Model_findInScope( m, parent, name, length, SYM_BIT( SYM_NAMESPACE ) );
    if ( symbol == 0 )
    {
        if ( !tryDeclareName( l, unit, token, statement, SYM_NAMESPACE, parent, &symbol ) ) return false;
        uint32_t own = Model_addScope( m, SCOPE_NAMESPACE, parent, symbol, NULL );
        m->symbols[ symbol ].ownScope = own;
    }
    *scope = m->symbols[ symbol ].ownScope;
    return true;
}

// Walk a namespace's dotted name, opening each segment (D11).
static bool tryEnterNamespacePath( Loader *l, uint32_t unit, uint32_t path, uint32_t statement, uint32_t parent, uint32_t *scope )
{
    if ( Loader_kind( l, unit, path ) == NODE_ACCESS )
    {
        uint32_t base = Loader_child( l, unit, path );
        uint32_t name = Loader_next( l, unit, base );
        uint32_t outer = 0;
        if ( !tryEnterNamespacePath( l, unit, base, statement, parent, &outer ) ) return false;
        return tryEnterNamespaceName( l, unit, Loader_node( l, unit, name )->token, statement, outer, scope );
    }
    return tryEnterNamespaceName( l, unit, Loader_node( l, unit, path )->token, statement, parent, scope );
}

static bool tryMergeGravity( Loader *l, uint32_t unit, uint32_t statement )
{
    uint32_t key   = Loader_child( l, unit, statement );
    uint32_t token = Loader_node( l, unit, key )->token;
    if ( l->model->gravityNode != 0 )
    {
        return Loader_failAt( l, unit, token, "gravity is set more than once (§4.11)" );
    }
    l->model->gravityNode = statement;

    SeenProperties seen = {};
    if ( !tryBindProperty( l, unit, statement, token, &Catalog_topLevel, BIND_ANY, &seen, NULL ) ) return false;
    return tryCheckPropertyShape( l, unit, statement, Loader_info( l, unit, statement )->property );
}

static bool tryMergeStatement( Loader *l, uint32_t unit, uint32_t statement, uint32_t scope )
{
    switch ( Loader_kind( l, unit, statement ) )
    {
    case NODE_DECLARATION:
        return tryMergeGravity( l, unit, statement );            // the parser allows no other here

    case NODE_SYSTEM:
        return tryDeclareSystemSymbol( l, unit, statement, scope );

    case NODE_SIMULATION:
        if ( l->model->simulationNode != 0 )
        {
            return Loader_failAt( l, unit, Loader_at( l, unit, statement ), "more than one 'simulation' block (D139)" );
        }
        l->model->simulationUnit = unit;
        l->model->simulationNode = statement;
        return true;

    case NODE_NAMESPACE:
    {
        uint32_t first = Loader_child( l, unit, statement );
        uint32_t block = first;
        uint32_t inner = scope;
        if ( Loader_kind( l, unit, first ) != NODE_BLOCK )       // a named namespace
        {
            if ( !tryEnterNamespacePath( l, unit, first, statement, scope, &inner ) ) return false;
            block = Loader_next( l, unit, first );

            NodeInfo *info = Loader_info( l, unit, statement );
            info->role   = ROLE_NAMESPACE;
            info->symbol = l->model->scopes[ inner ].owner;
        }
        for ( uint32_t s = Loader_child( l, unit, block ) ; s != 0 ; s = Loader_next( l, unit, s ) )
        {
            if ( !tryMergeStatement( l, unit, s, inner ) ) return false;
        }
        return true;
    }

    default:
        return true;                                             // `use` and `provides` wait for pass 4
    }
}

// After merging, the top level holds exactly one system (D139).
static bool tryFindRoot( Loader *l )
{
    Model   *m      = l->model;
    uint32_t count  = 0;
    uint32_t first  = 0;
    uint32_t second = 0;
    for ( uint32_t s = m->scopes[ SCOPE_INDEX_TOP ].firstSymbol ; s != 0 ; s = m->symbols[ s ].next )
    {
        if ( m->symbols[ s ].kind != SYM_SYSTEM ) continue;
        count++;
        if ( first == 0 )       first  = s;
        else if ( second == 0 ) second = s;
    }

    if ( count >= 2 )
    {
        const Symbol *a = &m->symbols[ first ];
        const Symbol *b = &m->symbols[ second ];
        return Loader_failAt( l, b->unit, b->token, "the top level holds %u systems, including '%.*s' and '%.*s'; "
                                                    "it must hold exactly one (D139)",
                              count, (int)a->length, a->name, (int)b->length, b->name );
    }
    m->rootSystem = first;
    return true;
}

static bool tryMerge( Loader *l )
{
    Model *m = l->model;
    for ( uint32_t unit = 0 ; unit < m->unitCount ; unit++ )
    {
        for ( uint32_t s = Loader_child( l, unit, 0 ) ; s != 0 ; s = Loader_next( l, unit, s ) )
        {
            if ( !tryMergeStatement( l, unit, s, SCOPE_INDEX_TOP ) ) return false;
        }
    }
    return tryFindRoot( l );
}

//////////////////////////////////////////////////////////////////////////////
// Pass 2: declare (§3.2)
//////////////////////////////////////////////////////////////////////////////

static bool tryDeclareBlock( Loader *l, uint32_t unit, uint32_t block, uint32_t scope, BlockContext context, SeenProperties *seen );

static bool tryDeclareBareBlock( Loader *l, uint32_t unit, uint32_t statement, uint32_t scope, BlockContext context, SeenProperties *seen )
{
    uint32_t    nameNode = Loader_child( l, unit, statement );
    uint32_t    block    = Loader_next( l, unit, nameNode );
    uint32_t    token    = Loader_node( l, unit, nameNode )->token;
    uint32_t    length;
    const char *name     = Loader_text( l, unit, token, &length );
    NodeInfo   *info     = Loader_info( l, unit, statement );

    switch ( context )
    {
    case CONTEXT_BODIES:
    {
        uint32_t symbol = 0;
        if ( !tryDeclareName( l, unit, token, statement, SYM_BODY, scope, &symbol ) ) return false;
        uint32_t own = Model_addScope( l->model, SCOPE_BODY, scope, symbol, &Catalog_body );
        l->model->symbols[ symbol ].ownScope = own;

        info = Loader_info( l, unit, statement );
        info->role   = ROLE_BODY;
        info->symbol = symbol;

        SeenProperties bodySeen = {};
        return tryDeclareBlock( l, unit, block, own, CONTEXT_BODY, &bodySeen );
    }

    case CONTEXT_SYSTEM:
    {
        if ( !Catalog_isSystemSection( name, length ) )
        {
            return Loader_failAt( l, unit, token, "'%.*s' is not a section of a system (§3.2)", (int)length, name );
        }
        info->role = ROLE_SECTION;
        bool isBodies = length == 6 && memcmp( name, "bodies", 6 ) == 0;
        return tryDeclareBlock( l, unit, block, scope, isBodies ? CONTEXT_BODIES : CONTEXT_SECTION, seen );
    }

    case CONTEXT_BODY:
        if ( !Catalog_isBodySection( name, length ) )
        {
            return Loader_failAt( l, unit, token, "'%.*s' is not a section of a body; "
                                                  "a body may hold 'points' and 'frames' (§3.2)", (int)length, name );
        }
        info->role = ROLE_SECTION;
        return tryDeclareBlock( l, unit, block, scope, CONTEXT_SECTION, seen );

    case CONTEXT_SECTION:
        break;
    }
    return Loader_failAt( l, unit, token, "a bare block '%.*s' is misplaced inside a section (§3.2, D138)", (int)length, name );
}

// `name = …;`. The statement is a value until pass 3 finds a type at its head.
static bool tryDeclareDefinition( Loader *l, uint32_t unit, uint32_t statement, uint32_t scope )
{
    uint32_t key = Loader_child( l, unit, statement );
    if ( Loader_kind( l, unit, key ) != NODE_NAME || Loader_kind( l, unit, Loader_lastChild( l, unit, statement ) ) == NODE_FOR )
    {
        return Loader_failAt( l, unit, Loader_at( l, unit, statement ), "not implemented yet: a comprehension ('for')" );
    }

    uint32_t symbol = 0;
    if ( !tryDeclareName( l, unit, Loader_node( l, unit, key )->token, statement, SYM_VALUE, scope, &symbol ) ) return false;
    Loader_info( l, unit, statement )->symbol = symbol;
    return true;
}

// `key: …;`. A key that is a property of the enclosing element sets it (§3.3).
// Any other declares a name, and pass 3 looks at its head.
static bool tryDeclareDeclaration( Loader *l, uint32_t unit, uint32_t statement, uint32_t scope, SeenProperties *seen )
{
    uint32_t key = Loader_child( l, unit, statement );
    if ( Loader_kind( l, unit, key ) != NODE_NAME )
    {
        return Loader_failAt( l, unit, Loader_at( l, unit, statement ), "not implemented yet: an indexed or numbered key" );
    }

    uint32_t           token   = Loader_node( l, unit, key )->token;
    const ElementInfo *element = l->model->scopes[ scope ].element;
    uint32_t           length;
    const char        *name    = Loader_text( l, unit, token, &length );

    const PropertyInfo *property = ElementInfo_findProperty( element, name, length );
    if ( property != NULL )
    {
        if ( !tryBindProperty( l, unit, statement, token, element, BIND_ANY, seen, NULL ) ) return false;
        return tryCheckPropertyShape( l, unit, statement, property );
    }

    if ( length == 7 && memcmp( name, "gravity", 7 ) == 0 )
    {
        return Loader_failAt( l, unit, token, "gravity is set only at the top level (D31)" );
    }
    if ( Loader_kind( l, unit, Loader_lastChild( l, unit, statement ) ) == NODE_FOR )
    {
        return Loader_failAt( l, unit, Loader_at( l, unit, statement ), "not implemented yet: a comprehension ('for')" );
    }

    uint32_t symbol = 0;
    if ( !tryDeclareName( l, unit, token, statement, SYM_INSTANCE, scope, &symbol ) ) return false;
    Loader_info( l, unit, statement )->symbol = symbol;
    return true;
}

// Enter the names of a block in `scope`. Sections are not scopes (D22), so
// their contents land in the same scope, and `seen` runs across them.
static bool tryDeclareBlock( Loader *l, uint32_t unit, uint32_t block, uint32_t scope, BlockContext context, SeenProperties *seen )
{
    for ( uint32_t s = Loader_child( l, unit, block ) ; s != 0 ; s = Loader_next( l, unit, s ) )
    {
        bool declared = true;
        switch ( Loader_kind( l, unit, s ) )
        {
        case NODE_SYSTEM:
            if ( context == CONTEXT_BODY )
            {
                return Loader_failAt( l, unit, Loader_at( l, unit, s ), "a system may not be declared inside a body (D138)" );
            }
            declared = tryDeclareSystemSymbol( l, unit, s, scope );
            break;

        case NODE_SCOPE:
            declared = tryDeclareBareBlock( l, unit, s, scope, context, seen );
            break;

        case NODE_DEFINITION:
            declared = tryDeclareDefinition( l, unit, s, scope );
            break;

        case NODE_DECLARATION:
            declared = tryDeclareDeclaration( l, unit, s, scope, seen );
            break;

        default:
            break;                                               // drives, references and chains declare nothing
        }
        if ( !declared ) return false;
    }
    return true;
}

static bool tryDeclare( Loader *l )
{
    Model *m = l->model;
    if ( m->simulationNode != 0 )
    {
        return Loader_failAt( l, m->simulationUnit, Loader_at( l, m->simulationUnit, m->simulationNode ),
                              "not implemented yet: a 'simulation' block" );
    }

    // Declaring a system can add nested systems, which this loop then reaches.
    for ( uint32_t s = 1 ; s < m->symbolCount ; s++ )
    {
        if ( m->symbols[ s ].kind != SYM_SYSTEM ) continue;

        uint32_t unit  = m->symbols[ s ].unit;
        uint32_t scope = m->symbols[ s ].ownScope;
        uint32_t name  = Loader_child( l, unit, m->symbols[ s ].node );
        uint32_t block = Loader_next( l, unit, name );

        SeenProperties seen = {};
        if ( !tryDeclareBlock( l, unit, block, scope, CONTEXT_SYSTEM, &seen ) ) return false;
    }
    return true;
}

//////////////////////////////////////////////////////////////////////////////
// Pass 3: classify (§3.3)
//////////////////////////////////////////////////////////////////////////////

// Bind the properties of a typed block: each statement is `key: value;`.
static bool tryBindBlock( Loader *l, uint32_t unit, uint32_t block, const ElementInfo *type, BindMode mode,
                          SeenProperties *seen, uint32_t *lawCount )
{
    for ( uint32_t s = Loader_child( l, unit, block ) ; s != 0 ; s = Loader_next( l, unit, s ) )
    {
        uint32_t key = Loader_child( l, unit, s );
        if ( Loader_kind( l, unit, s ) != NODE_DECLARATION || Loader_kind( l, unit, key ) != NODE_NAME )
        {
            return Loader_failAt( l, unit, Loader_at( l, unit, s ), "not implemented yet: a statement other than "
                                                                    "'key: value;' in the block of '%s'", type->name );
        }
        if ( !tryBindProperty( l, unit, s, Loader_node( l, unit, key )->token, type, mode, seen, lawCount ) ) return false;
        if ( !tryCheckPropertyShape( l, unit, s, Loader_info( l, unit, s )->property ) ) return false;
    }
    return true;
}

// Bind the items after an element's head: each is `name: value`.
static bool tryBindItems( Loader *l, uint32_t unit, uint32_t firstItem, const ElementInfo *type, BindMode mode,
                          SeenProperties *seen, uint32_t *lawCount )
{
    for ( uint32_t item = firstItem ; item != 0 ; item = Loader_next( l, unit, item ) )
    {
        if ( Loader_kind( l, unit, item ) != NODE_PROPERTY )
        {
            return Loader_failAt( l, unit, Loader_at( l, unit, item ), "'%s' takes no positional items (§4)", type->name );
        }
        uint32_t key = Loader_child( l, unit, item );
        if ( !tryBindProperty( l, unit, item, Loader_node( l, unit, key )->token, type, mode, seen, lawCount ) ) return false;
    }
    return true;
}

// A statement's value is a typed block or an item list: find its head NAME
// node, or 0 if the head is something else.
static uint32_t Loader_headName( const Loader *l, uint32_t unit, uint32_t value )
{
    uint32_t first = Loader_child( l, unit, value );            // the type NAME, or the first item
    return Loader_kind( l, unit, first ) == NODE_NAME ? first : 0;
}

// Bind whatever follows the head, in either shape (D152).
static bool tryBindValue( Loader *l, uint32_t unit, uint32_t value, const ElementInfo *type, BindMode mode, uint32_t *lawCount )
{
    SeenProperties seen = {};
    uint32_t       head = Loader_child( l, unit, value );
    uint32_t       rest = Loader_next( l, unit, head );
    if ( Loader_kind( l, unit, value ) == NODE_TYPED_BLOCK ) return tryBindBlock( l, unit, rest, type, mode, &seen, lawCount );
    return tryBindItems( l, unit, rest, type, mode, &seen, lawCount );
}

static void Loader_markHead( Loader *l, uint32_t unit, uint32_t head, uint32_t symbol )
{
    NodeInfo *info = Loader_info( l, unit, head );
    info->role   = ROLE_TYPE_HEAD;
    info->symbol = symbol;
}

// `name = …;`: a shared definition when its head is a built-in type, and
// otherwise a value (§3.3).
static bool tryClassifyEquals( Loader *l, uint32_t s )
{
    Model      *m         = l->model;
    uint32_t    unit      = m->symbols[ s ].unit;
    uint32_t    statement = m->symbols[ s ].node;
    uint32_t    scope     = m->symbols[ s ].scope;
    uint32_t    key       = Loader_child( l, unit, statement );
    uint32_t    value     = Loader_next( l, unit, key );
    uint32_t    head      = Loader_headName( l, unit, value );
    bool        isBlock   = Loader_kind( l, unit, value ) == NODE_TYPED_BLOCK;

    uint32_t type = 0;
    if ( head != 0 )
    {
        uint32_t    length;
        const char *name = Loader_text( l, unit, Loader_node( l, unit, head )->token, &length );
        type = Model_findInScope( m, SCOPE_INDEX_BUILTIN, name, length, SYM_BIT( SYM_BUILTIN_TYPE ) );

        if ( type != 0 )
        {
            uint32_t user = Model_lookup( m, scope, name, length, SPACE_USER );
            if ( user != 0 && user != s )
            {
                return Loader_failAt( l, unit, Loader_at( l, unit, head ), "'%.*s' is both a user name and a built-in type, "
                                      "and the specification does not say which this '=' means", (int)length, name );
            }
        }
        else if ( isBlock )
        {
            return Loader_failAt( l, unit, Loader_at( l, unit, head ), "unknown type '%.*s' (§4)", (int)length, name );
        }
    }

    NodeInfo *info = Loader_info( l, unit, statement );
    if ( type == 0 )
    {
        info->role = ROLE_VALUE;

        uint32_t first = Loader_child( l, unit, value );
        if ( Loader_kind( l, unit, first ) == NODE_PROPERTY )
        {
            return Loader_failAt( l, unit, Loader_at( l, unit, first ), "the first item after '=' is the value, not a property" );
        }
        if ( Loader_next( l, unit, first ) != 0 )
        {
            return Loader_failAt( l, unit, Loader_at( l, unit, Loader_next( l, unit, first ) ),
                                  "not implemented yet: the properties of a value" );
        }
        return true;
    }

    // A shared definition.
    const ElementInfo *element = m->symbols[ type ].type;
    info->role = ROLE_DEFINITION;
    m->symbols[ s ].kind = SYM_DEFINITION;
    m->symbols[ s ].type = element;
    Loader_markHead( l, unit, head, type );

    uint32_t    length;
    const char *own = Loader_text( l, unit, Loader_node( l, unit, key )->token, &length );
    if ( Model_findInScope( m, SCOPE_INDEX_BUILTIN, own, length, SYM_BIT( SYM_BUILTIN_TYPE ) ) != 0 )
    {
        return Loader_failAt( l, unit, Loader_at( l, unit, key ), "'%.*s' is a built-in type; "
                                                                  "a shared definition may not reuse its name (D144)", (int)length, own );
    }
    if ( !ElementInfo_hasLaw( element ) )
    {
        return Loader_failAt( l, unit, Loader_at( l, unit, head ), "'%s' has no law to share, so it can't be a shared definition (D152)",
                              element->name );
    }

    uint32_t lawCount = 0;
    if ( !tryBindValue( l, unit, value, element, BIND_DEFINITION, &lawCount ) ) return false;
    if ( lawCount == 0 )
    {
        return Loader_failAt( l, unit, Loader_at( l, unit, key ), "a shared definition must give at least one law property (D152)" );
    }
    return true;
}

// `key: head, …;` where `key` is no property: its head must be a built-in
// type, a user definition or a unit basis (§3.3).
static bool tryClassifyColon( Loader *l, uint32_t s )
{
    Model      *m         = l->model;
    uint32_t    unit      = m->symbols[ s ].unit;
    uint32_t    statement = m->symbols[ s ].node;
    uint32_t    scope     = m->symbols[ s ].scope;
    uint32_t    key       = Loader_child( l, unit, statement );
    uint32_t    value     = Loader_next( l, unit, key );
    const char *elementName = m->scopes[ scope ].element->name;

    uint32_t    keyLength;
    const char *keyText = Loader_text( l, unit, Loader_node( l, unit, key )->token, &keyLength );

    if ( Loader_kind( l, unit, value ) == NODE_BLOCK )
    {
        return Loader_failAt( l, unit, Loader_at( l, unit, key ), "'%.*s' is not a property of %s, and a bare block has no type (§3.3)",
                              (int)keyLength, keyText, elementName );
    }

    uint32_t head = Loader_headName( l, unit, value );
    if ( head == 0 )
    {
        uint32_t first = Loader_child( l, unit, value );
        if ( Loader_kind( l, unit, first ) == NODE_NUMBER )
        {
            uint32_t    length;
            const char *number = Loader_text( l, unit, Loader_node( l, unit, first )->token, &length );
            if ( length == 1 && number[0] == '1' )
            {
                return Loader_failAt( l, unit, Loader_at( l, unit, key ), "not implemented yet: a typed open signal or port" );
            }
        }
        return Loader_failAt( l, unit, Loader_at( l, unit, key ), "'%.*s' is not a property of %s, and its head is not a type, "
                              "a definition or a unit basis; use '=' to name a value (§3.3)", (int)keyLength, keyText, elementName );
    }

    uint32_t    length;
    const char *name = Loader_text( l, unit, Loader_node( l, unit, head )->token, &length );
    uint32_t    type = Model_lookup( m, scope, name, length, SPACE_HEAD );
    if ( type == 0 )
    {
        if ( Model_lookup( m, scope, name, length, SPACE_EXPRESSION ) != 0 )
        {
            return Loader_failAt( l, unit, Loader_at( l, unit, key ), "'%.*s' is not a property of %s, and its head '%.*s' is a value; "
                                  "use '=' to name a value (§3.3)", (int)keyLength, keyText, elementName, (int)length, name );
        }
        return Loader_failAt( l, unit, Loader_at( l, unit, head ), "unknown type '%.*s' (§3.3; unit bases are not implemented yet)",
                              (int)length, name );
    }

    const ElementInfo *element = m->symbols[ type ].type;
    BindMode           mode    = m->symbols[ type ].kind == SYM_DEFINITION ? BIND_OF_DEFINITION : BIND_ANY;

    m->symbols[ s ].type = element;
    Loader_info( l, unit, statement )->role = ROLE_INSTANCE;
    Loader_markHead( l, unit, head, type );

    return tryBindValue( l, unit, value, element, mode, NULL );
}

// A value's head may name a user definition: `x = coil, …;`. Whether that is
// legal is an open question (§3.2 allows it, D152 keeps aliases out).
static bool tryRejectDefinitionHead( Loader *l, uint32_t s )
{
    Model   *m         = l->model;
    uint32_t unit      = m->symbols[ s ].unit;
    uint32_t statement = m->symbols[ s ].node;
    uint32_t key       = Loader_child( l, unit, statement );
    uint32_t value     = Loader_next( l, unit, key );
    uint32_t head      = Loader_headName( l, unit, value );
    if ( head == 0 ) return true;

    uint32_t    length;
    const char *name = Loader_text( l, unit, Loader_node( l, unit, head )->token, &length );
    if ( Model_lookup( m, m->symbols[ s ].scope, name, length, SYM_BIT( SYM_DEFINITION ) ) != 0 )
    {
        return Loader_failAt( l, unit, Loader_at( l, unit, head ), "not implemented yet: '=' headed by the definition '%.*s'",
                              (int)length, name );
    }
    return true;
}

static bool tryClassify( Loader *l )
{
    Model *m = l->model;

    // Every `=` first, so that every definition is known when the heads of
    // `:` statements are looked up, whatever the order in the text.
    for ( uint32_t s = 1 ; s < m->symbolCount ; s++ )
    {
        if ( m->symbols[ s ].kind == SYM_VALUE && !tryClassifyEquals( l, s ) ) return false;
    }
    for ( uint32_t s = 1 ; s < m->symbolCount ; s++ )
    {
        if ( m->symbols[ s ].kind == SYM_VALUE && !tryRejectDefinitionHead( l, s ) ) return false;
        if ( m->symbols[ s ].kind == SYM_INSTANCE && !tryClassifyColon( l, s ) ) return false;
    }
    return true;
}

//////////////////////////////////////////////////////////////////////////////
// Pass 4: resolve (§3.2)
//////////////////////////////////////////////////////////////////////////////

static bool tryResolveExpression( Loader *l, uint32_t unit, uint32_t node, uint32_t scope );
static bool tryResolveBlock( Loader *l, uint32_t unit, uint32_t block, uint32_t scope );

static bool tryResolveName( Loader *l, uint32_t unit, uint32_t node, uint32_t scope )
{
    Model      *m     = l->model;
    uint32_t    token = Loader_node( l, unit, node )->token;
    uint32_t    length;
    const char *name  = Loader_text( l, unit, token, &length );

    uint32_t symbol = Model_lookup( m, scope, name, length, SPACE_EXPRESSION );
    if ( symbol == 0 ) return Loader_failAt( l, unit, token, "unknown name '%.*s' (§3.2)", (int)length, name );
    if ( m->symbols[ symbol ].kind == SYM_SYSTEM )
    {
        return Loader_failAt( l, unit, token, "the system '%.*s' can't be read from outside it (D2)", (int)length, name );
    }

    NodeInfo *info = Loader_info( l, unit, node );
    info->role   = ROLE_REFERENCE;
    info->symbol = symbol;
    return true;
}

static bool tryResolveCall( Loader *l, uint32_t unit, uint32_t call, uint32_t scope )
{
    uint32_t callee = Loader_child( l, unit, call );
    if ( Loader_kind( l, unit, callee ) != NODE_NAME )
    {
        return Loader_failAt( l, unit, Loader_at( l, unit, callee ), "only a built-in function can be called (D144)" );
    }

    uint32_t    token = Loader_node( l, unit, callee )->token;
    uint32_t    length;
    const char *name  = Loader_text( l, unit, token, &length );
    uint32_t    symbol = Model_findInScope( l->model, SCOPE_INDEX_BUILTIN, name, length, SPACE_FUNCTION );
    if ( symbol == 0 ) return Loader_failAt( l, unit, token, "unknown function '%.*s' (D144)", (int)length, name );

    NodeInfo *info = Loader_info( l, unit, callee );
    info->role   = ROLE_FUNCTION;
    info->symbol = symbol;

    for ( uint32_t a = Loader_next( l, unit, callee ) ; a != 0 ; a = Loader_next( l, unit, a ) )
    {
        if ( !tryResolveExpression( l, unit, a, scope ) ) return false;
    }
    return true;
}

static bool tryResolveExpression( Loader *l, uint32_t unit, uint32_t node, uint32_t scope )
{
    switch ( Loader_kind( l, unit, node ) )
    {
    case NODE_NAME:
        return tryResolveName( l, unit, node, scope );

    case NODE_NUMBER:
    case NODE_STRING:
    case NODE_UNIT:
        return true;

    case NODE_CALL:
        return tryResolveCall( l, unit, node, scope );

    case NODE_ACCESS:
        // Only the path's first name, for now. Later segments are members
        // and ports of what it names (§4.8).
        return tryResolveExpression( l, unit, Loader_child( l, unit, node ), scope );

    case NODE_PROPERTY:
        // An Euler label: the label is no name, only its value is.
        return tryResolveExpression( l, unit, Loader_next( l, unit, Loader_child( l, unit, node ) ), scope );

    case NODE_TYPED_BLOCK:
        return Loader_failAt( l, unit, Loader_at( l, unit, node ), "not implemented yet: a typed block inside an expression" );

    default:
        for ( uint32_t c = Loader_child( l, unit, node ) ; c != 0 ; c = Loader_next( l, unit, c ) )
        {
            if ( !tryResolveExpression( l, unit, c, scope ) ) return false;
        }
        return true;
    }
}

// A property's value is looked up in the space the catalog gives it.
static bool tryResolvePropertyValue( Loader *l, uint32_t unit, uint32_t value, const PropertyInfo *property, uint32_t scope )
{
    if ( property->space == VALUE_UNIT ) return true;            // units are checked with §3.5
    return tryResolveExpression( l, unit, value, scope );
}

// The value of an element statement: what follows the head.
static bool tryResolveElementValue( Loader *l, uint32_t unit, uint32_t value, uint32_t scope )
{
    uint32_t head = Loader_child( l, unit, value );
    uint32_t rest = Loader_next( l, unit, head );
    if ( Loader_kind( l, unit, value ) == NODE_TYPED_BLOCK ) return tryResolveBlock( l, unit, rest, scope );

    for ( uint32_t item = rest ; item != 0 ; item = Loader_next( l, unit, item ) )
    {
        uint32_t itemValue = Loader_next( l, unit, Loader_child( l, unit, item ) );
        if ( !tryResolvePropertyValue( l, unit, itemValue, Loader_info( l, unit, item )->property, scope ) ) return false;
    }
    return true;
}

// assembly: endpoint <connector> endpoint …
static bool tryResolveChain( Loader *l, uint32_t unit, uint32_t chain, uint32_t scope )
{
    Model   *m    = l->model;
    uint32_t last = Loader_lastChild( l, unit, chain );
    if ( Loader_kind( l, unit, last ) == NODE_FOR )
    {
        return Loader_failAt( l, unit, Loader_at( l, unit, last ), "not implemented yet: a comprehension ('for')" );
    }

    for ( uint32_t c = Loader_child( l, unit, chain ) ; c != 0 ; c = Loader_next( l, unit, c ) )
    {
        if ( Loader_kind( l, unit, c ) != NODE_SLOT )
        {
            if ( !tryResolveExpression( l, unit, c, scope ) ) return false;
            continue;
        }

        uint32_t connector = Loader_child( l, unit, c );
        if ( Loader_kind( l, unit, connector ) != NODE_NAME )
        {
            return Loader_failAt( l, unit, Loader_at( l, unit, connector ), "not implemented yet: an indexed connector" );
        }
        uint32_t    token = Loader_node( l, unit, connector )->token;
        uint32_t    length;
        const char *name  = Loader_text( l, unit, token, &length );
        uint32_t    symbol = Model_lookup( m, scope, name, length, SPACE_SLOT );
        if ( symbol == 0 )
        {
            return Loader_failAt( l, unit, token, "'%.*s' is not a declared connector (§3.7; "
                                                  "built-in connector types are not implemented yet)", (int)length, name );
        }

        NodeInfo *info = Loader_info( l, unit, connector );
        info->role   = ROLE_CONNECTOR;
        info->symbol = symbol;
    }
    return true;
}

// `provides a, b;`: each must be a system of this namespace (D8).
static bool tryResolveProvides( Loader *l, uint32_t unit, uint32_t statement, uint32_t scope )
{
    for ( uint32_t c = Loader_child( l, unit, statement ) ; c != 0 ; c = Loader_next( l, unit, c ) )
    {
        uint32_t    token = Loader_node( l, unit, c )->token;
        uint32_t    length;
        const char *name  = Loader_text( l, unit, token, &length );
        uint32_t    symbol = Model_findInScope( l->model, scope, name, length, SYM_BIT( SYM_SYSTEM ) );
        if ( symbol == 0 )
        {
            return Loader_failAt( l, unit, token, "this namespace has no system named '%.*s' to provide (D8)", (int)length, name );
        }

        NodeInfo *info = Loader_info( l, unit, c );
        info->role   = ROLE_PUBLISHED;
        info->symbol = symbol;
    }
    return true;
}

static bool tryResolveStatement( Loader *l, uint32_t unit, uint32_t statement, uint32_t scope )
{
    const NodeInfo *info  = Loader_info( l, unit, statement );
    uint32_t        first = Loader_child( l, unit, statement );
    uint32_t        second = first != 0 ? Loader_next( l, unit, first ) : 0;

    switch ( Loader_kind( l, unit, statement ) )
    {
    case NODE_NAMESPACE:
    {
        bool     named = Loader_kind( l, unit, first ) != NODE_BLOCK;
        uint32_t inner = named ? l->model->symbols[ info->symbol ].ownScope : scope;
        return tryResolveBlock( l, unit, named ? second : first, inner );
    }

    case NODE_SYSTEM:
        return tryResolveBlock( l, unit, second, l->model->symbols[ info->symbol ].ownScope );

    case NODE_SCOPE:
    {
        uint32_t inner = info->role == ROLE_BODY ? l->model->symbols[ info->symbol ].ownScope : scope;
        return tryResolveBlock( l, unit, second, inner );
    }

    case NODE_PROVIDES:
        return tryResolveProvides( l, unit, statement, scope );

    case NODE_DEFINITION:
        if ( info->role == ROLE_VALUE ) return tryResolveExpression( l, unit, Loader_child( l, unit, second ), scope );
        return tryResolveElementValue( l, unit, second, scope );

    case NODE_DECLARATION:
        if ( info->role == ROLE_PROPERTY )
        {
            for ( uint32_t item = Loader_child( l, unit, second ) ; item != 0 ; item = Loader_next( l, unit, item ) )
            {
                if ( !tryResolvePropertyValue( l, unit, item, info->property, scope ) ) return false;
            }
            return true;
        }
        return tryResolveElementValue( l, unit, second, scope );

    case NODE_DRIVE:
        if ( !tryResolveExpression( l, unit, first, scope ) ) return false;
        return tryResolveExpression( l, unit, second, scope );

    case NODE_REFERENCE:
        return tryResolveExpression( l, unit, first, scope );

    case NODE_CHAIN:
        return tryResolveChain( l, unit, statement, scope );

    default:
        return true;                                             // `use` has no effect yet
    }
}

static bool tryResolveBlock( Loader *l, uint32_t unit, uint32_t block, uint32_t scope )
{
    for ( uint32_t s = Loader_child( l, unit, block ) ; s != 0 ; s = Loader_next( l, unit, s ) )
    {
        if ( !tryResolveStatement( l, unit, s, scope ) ) return false;
    }
    return true;
}

static bool tryResolve( Loader *l )
{
    for ( uint32_t unit = 0 ; unit < l->model->unitCount ; unit++ )
    {
        if ( !tryResolveBlock( l, unit, 0, SCOPE_INDEX_TOP ) ) return false;
    }
    return true;
}

//////////////////////////////////////////////////////////////////////////////
// Public API
//////////////////////////////////////////////////////////////////////////////

// Make the model's two standing scopes, and enter the built-ins.
static void Model_init( Model *m, Unit *units, uint32_t unitCount )
{
    m->units     = units;
    m->unitCount = unitCount;

    m->symbolCapacity = 256;
    m->symbolCount    = 1;                                       // symbols[0] is "none"
    m->symbols        = (Symbol *)calloc( m->symbolCapacity, sizeof( Symbol ) );
    m->scopeCapacity  = 64;
    m->scopeCount     = 1;                                       // scopes[0] is "none"
    m->scopes         = (Scope *)calloc( m->scopeCapacity, sizeof( Scope ) );
    if ( m->symbols == NULL || m->scopes == NULL )
    {
        fprintf( stderr, "ombdc: out of memory\n" );
        exit( 1 );
    }

    m->rootSystem     = 0;
    m->gravityNode    = 0;
    m->simulationUnit = 0;
    m->simulationNode = 0;

    Model_addScope( m, SCOPE_BUILTIN, 0, 0, NULL );              // SCOPE_INDEX_BUILTIN
    Model_addScope( m, SCOPE_TOP, SCOPE_INDEX_BUILTIN, 0, &Catalog_topLevel );   // SCOPE_INDEX_TOP

    for ( uint32_t i = 0 ; i < Catalog_builtinValueCount ; i++ )
    {
        const char *name = Catalog_builtinValues[i].name;
        Model_addSymbol( m, SYM_BUILTIN_VALUE, name, (uint32_t)strlen( name ), 0, 0, 0, SCOPE_INDEX_BUILTIN );
    }
    for ( uint32_t i = 0 ; i < Catalog_builtinFunctionCount ; i++ )
    {
        const char *name = Catalog_builtinFunctions[i];
        Model_addSymbol( m, SYM_BUILTIN_FUNCTION, name, (uint32_t)strlen( name ), 0, 0, 0, SCOPE_INDEX_BUILTIN );
    }
    for ( uint32_t i = 0 ; i < Catalog_typeCount ; i++ )
    {
        const char *name   = Catalog_types[i]->name;
        uint32_t    symbol = Model_addSymbol( m, SYM_BUILTIN_TYPE, name, (uint32_t)strlen( name ), 0, 0, 0, SCOPE_INDEX_BUILTIN );
        m->symbols[ symbol ].type = Catalog_types[i];
    }

    for ( uint32_t u = 0 ; u < unitCount ; u++ )
    {
        units[u].values = NULL;
        units[u].info   = (NodeInfo *)calloc( units[u].ast.count, sizeof( NodeInfo ) );
        if ( units[u].info == NULL )
        {
            fprintf( stderr, "ombdc: out of memory\n" );
            exit( 1 );
        }
    }
}

bool tryLoadModel( Unit *units, uint32_t unitCount, Model *model, LoadError *error )
{
    Model_init( model, units, unitCount );

    Loader loader;
    loader.model = model;
    loader.error = error;

    if ( !tryMerge( &loader ) || !tryDeclare( &loader ) || !tryClassify( &loader ) || !tryResolve( &loader ) )
    {
        Model_free( model );
        return false;
    }
    return true;
}

void Model_free( Model *model )
{
    for ( uint32_t u = 0 ; u < model->unitCount ; u++ )
    {
        free( model->units[u].info );
        free( model->units[u].values );
        model->units[u].info   = NULL;
        model->units[u].values = NULL;
    }
    free( model->symbols );
    free( model->scopes );
    model->symbols     = NULL;
    model->scopes      = NULL;
    model->symbolCount = 0;
    model->scopeCount  = 0;
}

void LoadError_print( const Unit *units, const LoadError *error )
{
    fprintf( stderr, "%s:%u:%u: error: %s\n", units[ error->unit ].source.path, error->line, error->col, error->message );
}

//////////////////////////////////////////////////////////////////////////////
// The dump
//////////////////////////////////////////////////////////////////////////////

static const char *symbolKindNames[] =
{
    "NONE", "BUILTIN_VALUE", "BUILTIN_FUNCTION", "BUILTIN_TYPE",
    "VALUE", "DEFINITION", "INSTANCE", "BODY", "SYSTEM", "NAMESPACE",
};
static_assert( sizeof( symbolKindNames ) / sizeof( symbolKindNames[0] ) == SYM_KIND_COUNT,
               "symbolKindNames must have one entry per SymbolKind" );

static const char *nodeRoleNames[] =
{
    "NONE", "SYSTEM", "NAMESPACE", "SECTION", "BODY", "VALUE", "DEFINITION", "INSTANCE",
    "PROPERTY", "TYPE", "REFERENCE", "FUNCTION", "CONNECTOR", "PUBLISHED",
};
static_assert( sizeof( nodeRoleNames ) / sizeof( nodeRoleNames[0] ) == ROLE_COUNT,
               "nodeRoleNames must have one entry per NodeRole" );

const char *SymbolKind_name( SymbolKind kind )
{
    if ( kind < 0 || kind >= SYM_KIND_COUNT ) return "?";
    return symbolKindNames[ kind ];
}

static void Model_append( char *buffer, uint32_t size, uint32_t *used, const char *format, ... )
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

// One line per node that has a role, in tree order:
//     line:col ROLE name [: type] [-> KIND line:col]
static void Model_formatNode( const Model *m, uint32_t unit, uint32_t index, char *buffer, uint32_t size, uint32_t *used )
{
    const Unit     *u    = &m->units[ unit ];
    const Node     *node = &u->ast.nodes[ index ];
    const NodeInfo *info = &u->info[ index ];

    if ( info->role != ROLE_NONE )
    {
        const Token *at = &u->tokens.data[ node->firstToken ];
        Model_append( buffer, size, used, "%u:%u %s", at->line, at->col, nodeRoleNames[ info->role ] );

        const Symbol *symbol = info->symbol != 0 ? &m->symbols[ info->symbol ] : NULL;
        if ( info->role == ROLE_PROPERTY )
        {
            Model_append( buffer, size, used, " %s", info->property->name );
        }
        else if ( info->role == ROLE_SECTION )
        {
            const Token *t = &u->tokens.data[ node->token ];
            Model_append( buffer, size, used, " %.*s", (int)t->len, u->source.text + t->offset );
        }
        else if ( symbol != NULL )
        {
            Model_append( buffer, size, used, " %.*s", (int)symbol->length, symbol->name );
        }

        bool declares = info->role == ROLE_VALUE || info->role == ROLE_DEFINITION || info->role == ROLE_INSTANCE;
        if ( declares && symbol != NULL && symbol->type != NULL )
        {
            Model_append( buffer, size, used, " : %s", symbol->type->name );
        }

        bool refers = info->role == ROLE_REFERENCE || info->role == ROLE_CONNECTOR
                   || info->role == ROLE_TYPE_HEAD || info->role == ROLE_FUNCTION || info->role == ROLE_PUBLISHED;
        if ( refers && symbol != NULL )
        {
            Model_append( buffer, size, used, " -> %s", SymbolKind_name( symbol->kind ) );
            if ( symbol->scope != SCOPE_INDEX_BUILTIN )
            {
                const Token *target = &m->units[ symbol->unit ].tokens.data[ symbol->token ];
                if ( symbol->unit != unit ) Model_append( buffer, size, used, " unit %u", symbol->unit + 1 );
                Model_append( buffer, size, used, " %u:%u", target->line, target->col );
            }
        }
        Model_append( buffer, size, used, "\n" );
    }

    for ( uint32_t c = node->child ; c != 0 ; c = u->ast.nodes[ c ].next )
    {
        Model_formatNode( m, unit, c, buffer, size, used );
    }
}

uint32_t Model_format( const Model *m, char *buffer, uint32_t size )
{
    static const char *scopeNames[] = { "NONE", "BUILTIN", "TOP", "NAMESPACE", "SYSTEM", "BODY" };

    uint32_t used = 0;

    if ( m->rootSystem != 0 )
    {
        const Symbol *root = &m->symbols[ m->rootSystem ];
        Model_append( buffer, size, &used, "root %.*s\n", (int)root->length, root->name );
    }
    else
    {
        Model_append( buffer, size, &used, "no root: a library\n" );
    }

    // Every scope but the built-ins, with its names.
    for ( uint32_t i = SCOPE_INDEX_TOP ; i < m->scopeCount ; i++ )
    {
        const Scope *scope = &m->scopes[ i ];
        Model_append( buffer, size, &used, "scope %s", scopeNames[ scope->kind ] );
        if ( scope->owner != 0 )
        {
            const Symbol *owner = &m->symbols[ scope->owner ];
            Model_append( buffer, size, &used, " %.*s", (int)owner->length, owner->name );
        }
        Model_append( buffer, size, &used, ":" );
        for ( uint32_t s = scope->firstSymbol ; s != 0 ; s = m->symbols[ s ].next )
        {
            Model_append( buffer, size, &used, " %.*s", (int)m->symbols[ s ].length, m->symbols[ s ].name );
        }
        Model_append( buffer, size, &used, "\n" );
    }

    for ( uint32_t unit = 0 ; unit < m->unitCount ; unit++ )
    {
        Model_append( buffer, size, &used, "unit %u\n", unit + 1 );
        Model_formatNode( m, unit, 0, buffer, size, &used );
    }
    return used;
}

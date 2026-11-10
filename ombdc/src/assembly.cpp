// assembly.cpp -- connectors and assembly (Specification.md §3.7).
//
// Name resolution has already found what every endpoint and every connector
// slot names, so this stage only reads the tree and the symbols. It walks the
// chain lines in tree order, unit by unit, and stops at the first error.

#include "assembly.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Where a connector instance was first wired.
struct Wiring
{
    bool     wired;
    uint32_t token;              // its name in the slot
};

// The stage's state. It lives on tryCheckAssembly's stack.
struct Assembler
{
    Model     *model;
    LoadError *error;
    Wiring    *wiring;           // one per symbol
};

//////////////////////////////////////////////////////////////////////////////
// Errors and tree access
//////////////////////////////////////////////////////////////////////////////

// Record an error at a token and return false, for `return Assembly_failAt(...)`.
static bool Assembly_failAt( Assembler *a, uint32_t unit, uint32_t token, const char *format, ... )
{
    const Token *t = &a->model->units[ unit ].tokens.data[ token ];
    LoadError   *e = a->error;
    e->unit = unit;
    e->line = t->line;
    e->col  = t->col;

    va_list args;
    va_start( args, format );
    vsnprintf( e->message, sizeof( e->message ), format, args );
    va_end( args );
    return false;
}

static const Node *Assembly_node( const Assembler *a, uint32_t unit, uint32_t node )
{
    return &a->model->units[ unit ].ast.nodes[ node ];
}

static const NodeInfo *Assembly_info( const Assembler *a, uint32_t unit, uint32_t node )
{
    return &a->model->units[ unit ].info[ node ];
}

//////////////////////////////////////////////////////////////////////////////
// Endpoints
//////////////////////////////////////////////////////////////////////////////

static bool Assembly_isWorld( const Symbol *symbol )
{
    return symbol->kind == SYM_BUILTIN_VALUE && symbol->length == 5 && memcmp( symbol->name, "world", 5 ) == 0;
}

// What a symbol is, for a message: "a body", "a revolute", "a value".
static void Assembly_describe( const Symbol *symbol, char *buffer, uint32_t size )
{
    const char *what = "a name";
    switch ( symbol->kind )
    {
    case SYM_BODY:          what = "a body";             break;
    case SYM_VALUE:         what = "a value";            break;
    case SYM_DEFINITION:    what = "a shared definition"; break;
    case SYM_SYSTEM:        what = "a system";           break;
    case SYM_BUILTIN_VALUE: what = Assembly_isWorld( symbol ) ? "the ground" : "a built-in value"; break;
    case SYM_INSTANCE:
        if ( symbol->type != NULL )
        {
            snprintf( buffer, size, "a %s", symbol->type->name );
            return;
        }
        what = "an element";
        break;
    default:
        break;
    }
    snprintf( buffer, size, "%s", what );
}

static bool Assembly_fits( const Symbol *symbol, EndpointKind kind )
{
    switch ( kind )
    {
    case ENDPOINT_BODY: return symbol->kind == SYM_BODY || Assembly_isWorld( symbol );
    default:            return false;
    }
}

static const char *EndpointKind_wanted( EndpointKind kind )
{
    switch ( kind )
    {
    case ENDPOINT_BODY: return "a body or 'world'";
    default:            return "?";
    }
}

// The symbol an endpoint names, checked against the connector's signature at
// `position` (0 for the first attachment, 1 for the second).
static bool tryReadEndpoint( Assembler *a, uint32_t unit, uint32_t endpoint, uint32_t connector, uint32_t position, uint32_t *symbol )
{
    const Node *node = Assembly_node( a, unit, endpoint );
    switch ( node->kind )
    {
    case NODE_NAME:
        break;
    case NODE_ACCESS:
        return Assembly_failAt( a, unit, node->firstToken, "not implemented yet: a port as an endpoint" );
    case NODE_INDEX:
        return Assembly_failAt( a, unit, node->firstToken, "not implemented yet: an indexed endpoint" );
    default:
        return Assembly_failAt( a, unit, node->firstToken, "not implemented yet: this endpoint" );
    }

    const Model  *m     = a->model;
    uint32_t      named = Assembly_info( a, unit, endpoint )->symbol;
    const Symbol *s     = &m->symbols[ named ];
    const Symbol *c     = &m->symbols[ connector ];
    EndpointKind  kind  = c->type->endpoints[ position ];
    if ( !Assembly_fits( s, kind ) )
    {
        char what[64];
        Assembly_describe( s, what, sizeof( what ) );
        return Assembly_failAt( a, unit, node->firstToken, "'%.*s' is %s, but the %s endpoint of the %s '%.*s' must be %s (§3.7, D159)",
                                (int)s->length, s->name, what, position == 0 ? "first" : "second",
                                c->type->name, (int)c->length, c->name, EndpointKind_wanted( kind ) );
    }

    *symbol = named;
    return true;
}

//////////////////////////////////////////////////////////////////////////////
// Chain lines
//////////////////////////////////////////////////////////////////////////////

// The connector named in a slot: an instance of a connector type, not yet
// wired anywhere else (D162).
static bool tryReadConnector( Assembler *a, uint32_t unit, uint32_t slot, uint32_t *connector )
{
    Model      *m     = a->model;
    uint32_t    name  = Assembly_node( a, unit, slot )->child;
    uint32_t    token = Assembly_node( a, unit, name )->token;
    uint32_t    found = Assembly_info( a, unit, name )->symbol;
    Symbol     *s     = &m->symbols[ found ];

    if ( s->type == NULL || s->type->endpointCount == 0 )
    {
        char what[64];
        Assembly_describe( s, what, sizeof( what ) );
        return Assembly_failAt( a, unit, token, "'%.*s' is %s, not a connector (§3.7)", (int)s->length, s->name, what );
    }

    // A connector is wired only from its own system, which is one text, so
    // both uses are in this unit.
    Wiring *w = &a->wiring[ found ];
    if ( w->wired )
    {
        const Token *first = &m->units[ unit ].tokens.data[ w->token ];
        return Assembly_failAt( a, unit, token, "'%.*s' is already wired at %u:%u; a connector is in exactly one link (D162)",
                                (int)s->length, s->name, first->line, first->col );
    }
    w->wired = true;
    w->token = token;

    *connector = found;
    return true;
}

// `A <c1> B <c2> C;` is the links A–B through c1 and B–C through c2. The
// parser guarantees that endpoints and slots alternate.
static bool tryCheckChain( Assembler *a, uint32_t unit, uint32_t chain )
{
    Model   *m    = a->model;
    uint32_t from = Assembly_node( a, unit, chain )->child;

    while ( from != 0 )
    {
        uint32_t slot = Assembly_node( a, unit, from )->next;
        if ( slot == 0 || Assembly_node( a, unit, slot )->kind != NODE_SLOT ) break;
        uint32_t to = Assembly_node( a, unit, slot )->next;

        Link link = {};
        link.unit = unit;
        link.node = from;
        if ( !tryReadConnector( a, unit, slot, &link.connector ) )               return false;
        if ( !tryReadEndpoint( a, unit, from, link.connector, 0, &link.from ) ) return false;
        if ( !tryReadEndpoint( a, unit, to,   link.connector, 1, &link.to ) )   return false;

        m->links[ m->linkCount++ ] = link;
        from = to;
    }
    return true;
}

// Chain lines stand in `assembly` sections, which stand in systems, which
// stand at the top level or in namespaces.
static bool tryCheckNode( Assembler *a, uint32_t unit, uint32_t node )
{
    const Node *n = Assembly_node( a, unit, node );
    switch ( n->kind )
    {
    case NODE_CHAIN:
        return tryCheckChain( a, unit, node );

    case NODE_SOURCE:
    case NODE_NAMESPACE:
    case NODE_SYSTEM:
    case NODE_SCOPE:
    case NODE_BLOCK:
        for ( uint32_t c = n->child ; c != 0 ; c = Assembly_node( a, unit, c )->next )
        {
            if ( !tryCheckNode( a, unit, c ) ) return false;
        }
        return true;

    default:
        return true;
    }
}

// Every declared connector instance is wired (D162). A connector inside a
// nested system must be wired there, since lookup stops at the system.
static bool tryCheckAllWired( Assembler *a )
{
    const Model *m = a->model;
    for ( uint32_t s = 1 ; s < m->symbolCount ; s++ )
    {
        const Symbol *symbol = &m->symbols[ s ];
        if ( symbol->kind != SYM_INSTANCE || symbol->type == NULL || symbol->type->endpointCount == 0 ) continue;
        if ( a->wiring[ s ].wired ) continue;

        return Assembly_failAt( a, symbol->unit, symbol->token, "'%.*s' is declared but never wired (D162)",
                                (int)symbol->length, symbol->name );
    }
    return true;
}

//////////////////////////////////////////////////////////////////////////////
// Public API
//////////////////////////////////////////////////////////////////////////////

// A chain line has a link per slot, so the slots bound the links.
static uint32_t Model_countSlots( const Model *m )
{
    uint32_t count = 0;
    for ( uint32_t u = 0 ; u < m->unitCount ; u++ )
    {
        const Ast *ast = &m->units[u].ast;
        for ( uint32_t i = 0 ; i < ast->count ; i++ )
        {
            if ( ast->nodes[i].kind == NODE_SLOT ) count++;
        }
    }
    return count;
}

bool tryCheckAssembly( Model *model, LoadError *error )
{
    Assembler assembler;
    assembler.model  = model;
    assembler.error  = error;
    assembler.wiring = (Wiring *)calloc( model->symbolCount, sizeof( Wiring ) );

    uint32_t capacity = Model_countSlots( model );
    model->linkCount  = 0;
    model->links      = capacity > 0 ? (Link *)calloc( capacity, sizeof( Link ) ) : NULL;
    if ( assembler.wiring == NULL || ( capacity > 0 && model->links == NULL ) )
    {
        fprintf( stderr, "ombdc: out of memory\n" );
        exit( 1 );
    }

    bool ok = true;
    for ( uint32_t u = 0 ; ok && u < model->unitCount ; u++ ) ok = tryCheckNode( &assembler, u, 0 );
    if ( ok ) ok = tryCheckAllWired( &assembler );

    free( assembler.wiring );
    return ok;
}

//////////////////////////////////////////////////////////////////////////////
// The dump
//////////////////////////////////////////////////////////////////////////////

static void Assembly_append( char *buffer, uint32_t size, uint32_t *used, const char *format, ... )
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

uint32_t Model_formatAssembly( const Model *m, char *buffer, uint32_t size )
{
    uint32_t used = 0;
    for ( uint32_t unit = 0 ; unit < m->unitCount ; unit++ )
    {
        Assembly_append( buffer, size, &used, "unit %u\n", unit + 1 );

        const Unit *u = &m->units[ unit ];
        for ( uint32_t i = 0 ; i < m->linkCount ; i++ )
        {
            const Link *link = &m->links[i];
            if ( link->unit != unit ) continue;

            const Token  *at   = &u->tokens.data[ u->ast.nodes[ link->node ].firstToken ];
            const Symbol *c    = &m->symbols[ link->connector ];
            const Symbol *from = &m->symbols[ link->from ];
            const Symbol *to   = &m->symbols[ link->to ];
            Assembly_append( buffer, size, &used, "%u:%u %.*s %s %.*s -> %.*s\n", at->line, at->col,
                             (int)c->length, c->name, c->type->name,
                             (int)from->length, from->name, (int)to->length, to->name );
        }
    }
    return used;
}

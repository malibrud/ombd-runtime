// ast.cpp -- the node array, kind names and the tree dump.

#include "ast.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

//////////////////////////////////////////////////////////////////////////////
// Ast
//////////////////////////////////////////////////////////////////////////////

void Ast_make( Ast *ast, uint32_t capacity )
{
    if ( capacity == 0 ) capacity = 1;

    ast->nodes = (Node *)malloc( sizeof( Node ) * capacity );
    if ( ast->nodes == NULL )
    {
        fprintf( stderr, "ombdc: out of memory\n" );
        exit( 1 );
    }
    ast->count    = 0;
    ast->capacity = capacity;
}

void Ast_free( Ast *ast )
{
    free( ast->nodes );
    ast->nodes    = NULL;
    ast->count    = 0;
    ast->capacity = 0;
}

uint32_t Ast_add( Ast *ast, NodeKind kind, uint32_t token )
{
    if ( ast->count == ast->capacity )
    {
        uint32_t capacity = ast->capacity * 2;
        Node    *nodes    = (Node *)realloc( ast->nodes, sizeof( Node ) * capacity );
        if ( nodes == NULL )
        {
            fprintf( stderr, "ombdc: out of memory\n" );
            exit( 1 );
        }
        ast->nodes    = nodes;
        ast->capacity = capacity;
    }

    uint32_t index = ast->count;
    Node    *node  = &ast->nodes[ index ];
    node->kind       = kind;
    node->token      = token;
    node->firstToken = token;
    node->lastToken  = token;
    node->child      = 0;
    node->next       = 0;
    ast->count++;
    return index;
}

//////////////////////////////////////////////////////////////////////////////
// NodeKind
//////////////////////////////////////////////////////////////////////////////

// In the order of `enum NodeKind`.
static const char *nodeKindNames[] =
{
    "SOURCE",
    "NAMESPACE",
    "USE",
    "PROVIDES",
    "SYSTEM",
    "SIMULATION",
    "BLOCK",
    "SCOPE",
    "DEFINITION",
    "DECLARATION",
    "DRIVE",
    "REFERENCE",
    "CHAIN",
    "SLOT",
    "AXIS",
    "WHEN",
    "ITEMS",
    "PROPERTY",
    "FOR",
    "NAME",
    "NUMBER",
    "STRING",
    "UNARY",
    "BINARY",
    "CALL",
    "INDEX",
    "ACCESS",
    "POINT",
    "LIST",
    "GRID",
    "ROW",
    "SPATIAL",
    "HALF",
    "QUATERNION",
    "ORIENTATION",
    "FRAME",
    "TYPED_BLOCK",
    "QUANTITY",
    "UNIT",
};
static_assert( sizeof( nodeKindNames ) / sizeof( nodeKindNames[0] ) == NODE_KIND_COUNT,
               "nodeKindNames must have one entry per NodeKind" );

const char *NodeKind_name( NodeKind kind )
{
    if ( kind < 0 || kind >= NODE_KIND_COUNT ) return "?";
    return nodeKindNames[ kind ];
}

//////////////////////////////////////////////////////////////////////////////
// The tree dump
//////////////////////////////////////////////////////////////////////////////

// Append formatted text at `*used`, snprintf-style: once the buffer is full,
// keep counting without writing.
static void appendFormat( char *buffer, uint32_t size, uint32_t *used, const char *format, ... )
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

// Does the dump show the node's token text? Only where the text says more
// than the kind: a name, a literal, an operator, a unit, `in` or `out`.
static bool showsText( NodeKind kind )
{
    switch ( kind )
    {
    case NODE_NAME: case NODE_NUMBER: case NODE_STRING:
    case NODE_UNARY: case NODE_BINARY: case NODE_UNIT: case NODE_AXIS:
        return true;
    default:
        return false;
    }
}

static void formatNode( const Ast *ast, const TokenArray *tokens, const char *text,
                        uint32_t index, uint32_t depth, char *buffer, uint32_t size, uint32_t *used )
{
    const Node  *node  = &ast->nodes[ index ];
    const Token *first = &tokens->data[ node->firstToken ];

    // The position is where the node's span starts.
    appendFormat( buffer, size, used, "%*s%u:%u %s",
                  (int)( depth * 2 ), "", first->line, first->col, NodeKind_name( node->kind ) );

    if ( showsText( node->kind ) )
    {
        // A unit shows its whole run; every other node, its own token.
        const Token *token = &tokens->data[ node->token ];
        uint32_t     count = token->len;
        if ( node->kind == NODE_UNIT )
        {
            const Token *last = &tokens->data[ node->lastToken ];
            count = last->offset + last->len - token->offset;
        }
        appendFormat( buffer, size, used, " '%.*s'", (int)count, text + token->offset );
    }
    appendFormat( buffer, size, used, "\n" );

    for ( uint32_t child = node->child ; child != 0 ; child = ast->nodes[ child ].next )
    {
        formatNode( ast, tokens, text, child, depth + 1, buffer, size, used );
    }
}

uint32_t Ast_format( const Ast *ast, const TokenArray *tokens, const char *text, char *buffer, uint32_t size )
{
    uint32_t used = 0;
    if ( ast->count > 0 ) formatNode( ast, tokens, text, 0, 0, buffer, size, &used );
    return used;
}

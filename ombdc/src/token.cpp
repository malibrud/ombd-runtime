// token.cpp -- token arrays, kind names and the token dump.

#include "token.h"
#include <stdio.h>
#include <stdlib.h>

//////////////////////////////////////////////////////////////////////////////
// TokenArray
//////////////////////////////////////////////////////////////////////////////

void TokenArray_make( TokenArray *tokens, uint32_t capacity )
{
    if ( capacity == 0 ) capacity = 1;

    tokens->data = (Token *)malloc( sizeof( Token ) * capacity );
    if ( tokens->data == NULL )
    {
        fprintf( stderr, "ombdc: out of memory\n" );
        exit( 1 );
    }
    tokens->count    = 0;
    tokens->capacity = capacity;
}

void TokenArray_append( TokenArray *tokens, Token token )
{
    if ( tokens->count == tokens->capacity )
    {
        uint32_t capacity = tokens->capacity * 2;
        Token   *data     = (Token *)realloc( tokens->data, sizeof( Token ) * capacity );
        if ( data == NULL )
        {
            fprintf( stderr, "ombdc: out of memory\n" );
            exit( 1 );
        }
        tokens->data     = data;
        tokens->capacity = capacity;
    }
    tokens->data[ tokens->count ] = token;
    tokens->count++;
}

void TokenArray_free( TokenArray *tokens )
{
    free( tokens->data );
    tokens->data     = NULL;
    tokens->count    = 0;
    tokens->capacity = 0;
}

//////////////////////////////////////////////////////////////////////////////
// TokenKind
//////////////////////////////////////////////////////////////////////////////

// Both tables follow the order of `enum TokenKind`.
static const char *tokenKindNames[] =
{
    "EOF",
    "IDENT",
    "KEYWORD",
    "NUMBER",
    "STRING",
    "LPAREN",
    "RPAREN",
    "LBRACKET",
    "RBRACKET",
    "LBRACE",
    "RBRACE",
    "LANGLE",
    "RANGLE",
    "COMMA",
    "SEMICOLON",
    "COLON",
    "DOT",
    "EQUAL",
    "PLUS",
    "MINUS",
    "STAR",
    "SLASH",
    "CARET",
    "AT",
    "LBRACKET_LBRACKET",
    "RBRACKET_RBRACKET",
    "LBRACKET_BAR",
    "BAR_RBRACKET",
    "LANGLE_BAR",
    "BAR_RANGLE",
    "LANGLE_LBRACKET",
    "RBRACKET_RANGLE",
    "LANGLE_MINUS_RANGLE",
    "LANGLE_MINUS",
    "MINUS_RANGLE",
    "DOT_DOT",
    "LANGLE_EQUAL",
    "RANGLE_EQUAL",
    "EQUAL_EQUAL",
    "BANG_EQUAL",
    "DOT_PRODUCT",
    "CROSS_PRODUCT",
    "APPLY",
};
static_assert( sizeof( tokenKindNames ) / sizeof( tokenKindNames[0] ) == TOK_KIND_COUNT,
               "tokenKindNames must have one entry per TokenKind" );

static const char *tokenKindSpellings[] =
{
    "", "", "", "", "",                                   // EOF IDENT KEYWORD NUMBER STRING
    "(", ")", "[", "]", "{", "}", "<", ">",
    ",", ";", ":", ".", "=", "+", "-", "*", "/", "^",
    "@",
    "[[", "]]", "[|", "|]", "<|", "|>", "<[", "]>",
    "<->", "<-", "->", "..",
    "<=", ">=", "==", "!=",
    "\\.", "\\x", "\\>",
};
static_assert( sizeof( tokenKindSpellings ) / sizeof( tokenKindSpellings[0] ) == TOK_KIND_COUNT,
               "tokenKindSpellings must have one entry per TokenKind" );

const char *TokenKind_name( TokenKind kind )
{
    if ( kind < 0 || kind >= TOK_KIND_COUNT ) return "?";
    return tokenKindNames[ kind ];
}

const char *TokenKind_spelling( TokenKind kind )
{
    if ( kind < 0 || kind >= TOK_KIND_COUNT ) return "?";
    return tokenKindSpellings[ kind ];
}

//////////////////////////////////////////////////////////////////////////////
// The token dump
//////////////////////////////////////////////////////////////////////////////

uint32_t TokenArray_format( const TokenArray *tokens, const char *text, char *buffer, uint32_t size )
{
    uint32_t used = 0;
    for ( uint32_t i = 0 ; i < tokens->count ; i++ )
    {
        const Token *token = &tokens->data[ i ];

        // Once the buffer is full, keep counting without writing.
        char    *out  = NULL;
        uint32_t room = 0;
        if ( buffer != NULL && used < size )
        {
            out  = buffer + used;
            room = size - used;
        }

        int n = snprintf(
            out, room,
            "%u:%u %s '%.*s' %c%c\n",
            token->line,
            token->col,
            TokenKind_name( token->kind ),
            (int)token->len, text + token->offset,
            ( token->flags & SPACE_BEFORE ) ? 'S' : '-',
            ( token->flags & LINE_BEFORE )  ? 'L' : '-'
        );
        used += (uint32_t)n;
    }
    return used;
}

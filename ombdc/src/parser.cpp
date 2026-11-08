// parser.cpp -- the OMBD parser (Specification.md §2, Appendix A).
//
// A hand-written recursive-descent parser over the token array. One function
// per grammar rule, named for the rule: tryParseSystem, tryParseSum, ...
// Each returns false on the first error, which it records in ParseError, and
// otherwise returns the index of the node it built through its last argument.
//
// Two rules need more than the grammar shows:
//
// - Compound closers (§2.2, D168). The lexer never forms `]]`, `|]`, `|>` or
//   `]>`: each is two single tokens, and the parser matches the pair where it
//   expects one, with the second half tight against the first.
//
// - Bracket context (§2.6, §2.7). Inside a space-separated bracket (a list,
//   a grid row, a spatial-vector half, a quaternion's x y z), whitespace can
//   end an entry, and an IDENT after an entry starts a new entry instead of a
//   unit. Two flags carry that context; every bracket sets them for its
//   contents and restores them after.

#include "parser.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// How a block's statements are read (§2.4). The mode comes from the block's
// name, so the parser needs no other state.
enum BlockMode
{
    MODE_GENERAL,                // the statement forms of §2.3
    MODE_ASSEMBLY,               // `assembly { }`: chain lines only
    MODE_MAP,                    // `map { }`: `in` and `out` axis lines, and properties
    MODE_STATES,                 // `states { }`: states only, `name { … }`
    MODE_STATE,                  // one state: `when` transitions, and outputs
};

struct Parser
{
    const char  *text;           // the source text the tokens point into
    const Token *tokens;
    uint32_t     count;          // tokens, the final EOF included
    uint32_t     pos;            // index of the current token
    uint32_t     lastConsumed;   // index of the last token taken

    // Bracket context (§2.6, §2.7).
    bool         inSpaceList;    // at the top level of a space-separated bracket
    bool         unitsAllowed;   // a spaced IDENT after a value literal is its unit

    Ast         *ast;
    ParseError  *error;
};

// The saved bracket context, restored when a bracket closes.
struct Context
{
    bool inSpaceList;
    bool unitsAllowed;
};

// A sibling chain under construction: children are linked as they are
// parsed, and the parent node is made once they are all known.
struct Chain
{
    uint32_t first;
    uint32_t last;
    uint32_t count;
};

static bool tryParseTopStatement( Parser *p, uint32_t *node );
static bool tryParseBlock( Parser *p, BlockMode mode, uint32_t *node );
static bool tryParseStatement( Parser *p, BlockMode mode, uint32_t *node );
static bool tryParseGuard( Parser *p, uint32_t *node );
static bool tryParseExpr( Parser *p, uint32_t *node );
static bool tryParseRange( Parser *p, uint32_t *node );
static bool tryParseSum( Parser *p, uint32_t *node );

//////////////////////////////////////////////////////////////////////////////
// Tokens
//////////////////////////////////////////////////////////////////////////////

// The current token.
static const Token *Parser_peek( const Parser *p )
{
    return &p->tokens[ p->pos ];
}

// The token after the current one. EOF repeats at the end.
static const Token *Parser_peekNext( const Parser *p )
{
    uint32_t i = p->pos + 1;
    if ( i >= p->count ) i = p->count - 1;
    return &p->tokens[ i ];
}

// The index of the current token, for a node's token.
static uint32_t Parser_index( const Parser *p )
{
    return p->pos;
}

// Take the current token and return its index. EOF is never passed.
static uint32_t Parser_advance( Parser *p )
{
    uint32_t i = p->pos;
    if ( p->tokens[ i ].kind != TOK_EOF ) p->pos++;
    p->lastConsumed = i;
    return i;
}

static bool isSpaced( const Token *t )
{
    return ( t->flags & SPACE_BEFORE ) != 0;
}

static bool Parser_isText( const Parser *p, const Token *t, const char *s )
{
    size_t n = strlen( s );
    return t->len == n && memcmp( p->text + t->offset, s, n ) == 0;
}

// An IDENT that the grammar recognizes by position: `system`, `when`, `in`, ...
static bool Parser_isWord( const Parser *p, const Token *t, const char *word )
{
    return t->kind == TOK_IDENT && Parser_isText( p, t, word );
}

// One of the five reserved words.
static bool Parser_isReserved( const Parser *p, const Token *t, const char *word )
{
    return t->kind == TOK_KEYWORD && Parser_isText( p, t, word );
}

static bool Parser_isPrimed( const Parser *p, const Token *t )
{
    return t->kind == TOK_IDENT && p->text[ t->offset + t->len - 1 ] == '\'';
}

static bool isComparison( const Token *t )
{
    switch ( t->kind )
    {
    case TOK_LANGLE: case TOK_RANGLE: case TOK_LANGLE_EQUAL:
    case TOK_RANGLE_EQUAL: case TOK_EQUAL_EQUAL: case TOK_BANG_EQUAL:
        return true;
    default:
        return false;
    }
}

// Can this token begin an expression (§2.7 rule 5)? `not` is left out: it
// only begins a guard.
static bool canStartExpression( const Token *t )
{
    switch ( t->kind )
    {
    case TOK_NUMBER: case TOK_STRING: case TOK_IDENT:
    case TOK_LPAREN: case TOK_LBRACKET: case TOK_LBRACKET_LBRACKET: case TOK_LBRACKET_BAR:
    case TOK_LANGLE: case TOK_LANGLE_BAR: case TOK_LANGLE_LBRACKET:
    case TOK_MINUS: case TOK_CARET:
        return true;
    default:
        return false;
    }
}

//////////////////////////////////////////////////////////////////////////////
// Errors
//////////////////////////////////////////////////////////////////////////////

// Record an error at token `t` and return false, for `return Parser_failAt(...)`.
static bool Parser_failAt( Parser *p, const Token *t, const char *format, ... )
{
    ParseError *e = p->error;
    e->offset = t->offset;
    e->line   = t->line;
    e->col    = t->col;

    va_list args;
    va_start( args, format );
    vsnprintf( e->message, sizeof( e->message ), format, args );
    va_end( args );
    return false;
}

// Describe a token for a message: 'text', or "the end of the file".
static void Parser_describe( const Parser *p, const Token *t, char *out, size_t size )
{
    if ( t->kind == TOK_EOF ) snprintf( out, size, "the end of the file" );
    else                      snprintf( out, size, "'%.*s'", (int)t->len, p->text + t->offset );
}

// "expected X, found Y" at the current token. A few tokens are never legal,
// and get their own message instead.
static bool Parser_failExpected( Parser *p, const char *expected )
{
    const Token *t = Parser_peek( p );
    if ( t->kind == TOK_AT )
    {
        return Parser_failAt( p, t, "'@' is reserved (D141)" );
    }
    if ( t->kind == TOK_LANGLE_MINUS_RANGLE )
    {
        return Parser_failAt( p, t, "'<->' has no statement form yet (Appendix B)" );
    }
    if ( t->kind == TOK_BAR )
    {
        return Parser_failAt( p, t, "'|' is legal only in the closers '|]' and '|>' (D168)" );
    }
    char found[64];
    Parser_describe( p, t, found, sizeof( found ) );
    return Parser_failAt( p, t, "expected %s, found %s", expected, found );
}

// Take a token of `kind`, or fail with "expected <expected>".
static bool Parser_tryExpect( Parser *p, TokenKind kind, const char *expected )
{
    if ( Parser_peek( p )->kind != kind ) return Parser_failExpected( p, expected );
    Parser_advance( p );
    return true;
}

// Take a compound closer, `]]`, `|]`, `|>` or `]>`: the token `first`, then
// the token `second` tight against it (§2.2, D168). `closer` is its spelling.
static bool Parser_tryExpectCloser( Parser *p, TokenKind first, TokenKind second, const char *closer )
{
    char expected[16];
    snprintf( expected, sizeof( expected ), "'%s'", closer );
    if ( Parser_peek( p )->kind != first ) return Parser_failExpected( p, expected );

    const Token *next = Parser_peekNext( p );
    if ( next->kind == second && isSpaced( next ) )
    {
        return Parser_failAt( p, next, "the two halves of '%s' must be adjacent, "
                                       "with nothing between them (D168)", closer );
    }
    Parser_advance( p );
    if ( next->kind != second )
    {
        char found[64];
        Parser_describe( p, next, found, sizeof( found ) );
        return Parser_failAt( p, next, "expected '%c' to complete '%s', found %s", closer[1], closer, found );
    }
    Parser_advance( p );
    return true;
}

// Is the current token `closer`?
static bool Parser_isAtCloser( const Parser *p, TokenKind closer )
{
    return Parser_peek( p )->kind == closer;
}

// Take the `;` that ends a statement.
static bool Parser_tryExpectSemicolon( Parser *p )
{
    const Token *t = Parser_peek( p );
    if ( t->kind == TOK_SEMICOLON )
    {
        Parser_advance( p );
        return true;
    }
    if ( ( t->flags & LINE_BEFORE ) != 0 )
    {
        return Parser_failAt( p, t, "expected ';' at the end of the previous line" );
    }
    if ( isSpaced( t ) && canStartExpression( t ) )
    {
        char found[64];
        Parser_describe( p, t, found, sizeof( found ) );
        return Parser_failAt( p, t, "expected ';' to end the statement, or ',' between items, found %s", found );
    }
    return Parser_failExpected( p, "';'" );
}

// A statement that ends in `}` takes no `;` (D135).
static bool Parser_tryCheckNoSemicolon( Parser *p )
{
    const Token *t = Parser_peek( p );
    if ( p->tokens[ p->lastConsumed ].kind == TOK_RBRACE && t->kind == TOK_SEMICOLON )
    {
        return Parser_failAt( p, t, "a statement that ends in '}' takes no ';' (D135)" );
    }
    return true;
}

//////////////////////////////////////////////////////////////////////////////
// Nodes
//////////////////////////////////////////////////////////////////////////////

static Node *Parser_node( Parser *p, uint32_t index )
{
    return &p->ast->nodes[ index ];
}

// A new node whose span starts at `firstToken` and, for now, ends at the
// last token taken.
static uint32_t Parser_newNode( Parser *p, NodeKind kind, uint32_t token, uint32_t firstToken )
{
    uint32_t index = Ast_add( p->ast, kind, token );
    Parser_node( p, index )->firstToken = firstToken;
    Parser_node( p, index )->lastToken  = p->lastConsumed;
    return index;
}

static void Chain_append( Parser *p, Chain *chain, uint32_t node )
{
    if ( chain->count == 0 ) chain->first = node;
    else                     Parser_node( p, chain->last )->next = node;
    chain->last = node;
    chain->count++;
}

// Make the parent of a finished chain. Its span ends at the last token taken.
static uint32_t Parser_makeParent( Parser *p, NodeKind kind, uint32_t token, uint32_t firstToken, const Chain *children )
{
    uint32_t parent = Parser_newNode( p, kind, token, firstToken );
    Parser_node( p, parent )->child = children->count > 0 ? children->first : 0;
    return parent;
}

// A node over two finished nodes, such as a binary operation.
static uint32_t Parser_makePair( Parser *p, NodeKind kind, uint32_t token, uint32_t a, uint32_t b )
{
    Chain children = {};
    Chain_append( p, &children, a );
    Chain_append( p, &children, b );
    return Parser_makeParent( p, kind, token, Parser_node( p, a )->firstToken, &children );
}

// A node over one finished node, such as a prefix operation.
static uint32_t Parser_makeSingle( Parser *p, NodeKind kind, uint32_t token, uint32_t firstToken, uint32_t a )
{
    Chain children = {};
    Chain_append( p, &children, a );
    return Parser_makeParent( p, kind, token, firstToken, &children );
}

static Context Parser_enterContext( Parser *p, bool inSpaceList, bool unitsAllowed )
{
    Context saved = { p->inSpaceList, p->unitsAllowed };
    p->inSpaceList  = inSpaceList;
    p->unitsAllowed = unitsAllowed;
    return saved;
}

static void Parser_restoreContext( Parser *p, Context saved )
{
    p->inSpaceList  = saved.inSpaceList;
    p->unitsAllowed = saved.unitsAllowed;
}

//////////////////////////////////////////////////////////////////////////////
// Names and paths
//////////////////////////////////////////////////////////////////////////////

// NAME = IDENT, never primed outside an Euler label (§1.4).
static bool tryParseName( Parser *p, uint32_t *node )
{
    const Token *t = Parser_peek( p );
    if ( t->kind != TOK_IDENT ) return Parser_failExpected( p, "a name" );
    if ( Parser_isPrimed( p, t ) )
    {
        return Parser_failAt( p, t, "a primed name is legal only as an Euler axis label, "
                                    "as in <| Z: a, Y': b, X'': c |> (D120)" );
    }
    uint32_t i = Parser_advance( p );
    *node = Parser_newNode( p, NODE_NAME, i, i );
    return true;
}

// path = IDENT { "." IDENT }
static bool tryParsePath( Parser *p, uint32_t *node )
{
    if ( !tryParseName( p, node ) ) return false;
    while ( Parser_peek( p )->kind == TOK_DOT )
    {
        uint32_t dot = Parser_advance( p );
        uint32_t name = 0;
        if ( !tryParseName( p, &name ) ) return false;
        *node = Parser_makePair( p, NODE_ACCESS, dot, *node, name );
    }
    return true;
}

// The `[ expr ]` of an index, at a tight `[`.
static bool tryParseIndexSuffix( Parser *p, uint32_t *node )
{
    uint32_t open  = Parser_advance( p );
    Context  saved = Parser_enterContext( p, false, true );
    uint32_t index = 0;
    if ( !tryParseExpr( p, &index ) ) return false;
    Parser_restoreContext( p, saved );
    if ( !Parser_tryExpect( p, TOK_RBRACKET, "']'" ) ) return false;
    *node = Parser_makePair( p, NODE_INDEX, open, *node, index );
    return true;
}

// lhs = IDENT [ tight( "[" ) expr "]" ]
static bool tryParseLhs( Parser *p, uint32_t *node )
{
    if ( !tryParseName( p, node ) ) return false;
    const Token *t = Parser_peek( p );
    if ( t->kind == TOK_LBRACKET && !isSpaced( t ) ) return tryParseIndexSuffix( p, node );
    return true;
}

//////////////////////////////////////////////////////////////////////////////
// Units (§2.6)
//////////////////////////////////////////////////////////////////////////////

// unit_run = IDENT { tight( "-" | "/" ) tight( IDENT ) | tight( "^" ) tight( NUMBER ) }
// The caller has checked that the current token is an IDENT.
static bool tryParseUnit( Parser *p, uint32_t *node )
{
    const Token *t = Parser_peek( p );
    if ( Parser_isPrimed( p, t ) ) return Parser_failAt( p, t, "a unit name is never primed (D120)" );
    uint32_t first = Parser_advance( p );

    for ( ;; )
    {
        const Token *joiner = Parser_peek( p );
        const Token *after  = Parser_peekNext( p );
        bool         tight  = !isSpaced( joiner ) && !isSpaced( after );

        if ( tight && ( joiner->kind == TOK_MINUS || joiner->kind == TOK_SLASH ) )
        {
            if ( after->kind != TOK_IDENT )
            {
                return Parser_failAt( p, after, "after '%.*s' in a unit comes a unit name",
                                      (int)joiner->len, p->text + joiner->offset );
            }
            if ( Parser_isPrimed( p, after ) ) return Parser_failAt( p, after, "a unit name is never primed (D120)" );
            Parser_advance( p );
            Parser_advance( p );
        }
        else if ( tight && joiner->kind == TOK_CARET )
        {
            if ( after->kind != TOK_NUMBER ) return Parser_failAt( p, after, "after '^' in a unit comes a number" );
            Parser_advance( p );
            Parser_advance( p );
        }
        else
        {
            break;
        }
    }

    *node = Parser_newNode( p, NODE_UNIT, first, first );
    return true;
}

// After a value literal: its unit run, if one follows and the context
// allows it. The result is the value itself, or a QUANTITY over value and unit.
static bool tryParseUnitAfter( Parser *p, uint32_t value, uint32_t *node )
{
    *node = value;
    const Token *t = Parser_peek( p );
    if ( !p->unitsAllowed || t->kind != TOK_IDENT || !isSpaced( t ) ) return true;

    uint32_t unit = 0;
    if ( !tryParseUnit( p, &unit ) ) return false;
    *node = Parser_makePair( p, NODE_QUANTITY, Parser_node( p, value )->token, value, unit );
    return true;
}

//////////////////////////////////////////////////////////////////////////////
// Bracket entries (§2.7)
//////////////////////////////////////////////////////////////////////////////

// entries = entry { [ "," ] entry }, in a space-separated bracket. Stops in
// front of `closer` or `closer2` (TOK_EOF when there is no second closer),
// or after `max` entries when `max` is not 0. Quaternion components are
// sums (§2.8), so with `sumsOnly` each entry is parsed as a sum.
static bool tryParseEntries( Parser *p, Chain *entries, TokenKind closer, TokenKind closer2,
                             uint32_t max, bool sumsOnly )
{
    if ( Parser_isAtCloser( p, closer ) || Parser_peek( p )->kind == closer2 ) return true;

    for ( ;; )
    {
        uint32_t entry = 0;
        bool     parsed = sumsOnly ? tryParseSum( p, &entry ) : tryParseExpr( p, &entry );
        if ( !parsed ) return false;
        Chain_append( p, entries, entry );
        if ( max != 0 && entries->count == max ) return true;

        // Rule 1: a separator or a closer ends the entry.
        const Token *t = Parser_peek( p );
        if ( t->kind == TOK_COMMA )
        {
            Parser_advance( p );
            if ( Parser_isAtCloser( p, closer ) || Parser_peek( p )->kind == closer2 )
            {
                return Parser_failAt( p, Parser_peek( p ), "a ',' in a list must be followed by an entry" );
            }
            continue;
        }
        if ( Parser_isAtCloser( p, closer ) || t->kind == closer2 ) return true;

        // Rules 2 and 4 continue the entry, and the expression parser has
        // already taken every operator it can. A comparison is what is left.
        if ( isComparison( t ) || Parser_isReserved( p, t, "and" ) || Parser_isReserved( p, t, "or" ) )
        {
            return Parser_failAt( p, t, "a comparison is legal only in a 'when' guard%s",
                                  t->kind == TOK_LANGLE ? "; separate quaternions in a list with commas" : "" );
        }

        // Rule 3 for '+': a sign, but there is no prefix '+' (Appendix A).
        if ( t->kind == TOK_PLUS ) return Parser_failAt( p, t, "there is no prefix '+'" );

        // Rule 5: a spaced token that can start an expression starts the next entry.
        if ( isSpaced( t ) && canStartExpression( t ) ) continue;

        // Rule 6.
        return Parser_failExpected( p, "',' or the end of the bracket" );
    }
}

// Take a list's closer: `closer` alone when `closer2` is TOK_EOF, or the
// compound closer `closer` `closer2`, spelled `closerText` (D168).
static bool Parser_tryTakeCloser( Parser *p, TokenKind closer, TokenKind closer2, const char *closerText )
{
    if ( closer2 == TOK_EOF )
    {
        Parser_advance( p );
        return true;
    }
    return Parser_tryExpectCloser( p, closer, closer2, closerText );
}

// A comma-only list (§2.7): parentheses, call arguments, `<| |>`, `<[ ]>`.
// Takes the closer too: `closer`, then `closer2` for a compound closer, or
// TOK_EOF for none. With `labels`, an entry may be `IDENT: expr`, an Euler
// label, whose IDENT may be primed (D140).
static bool tryParseCommaList( Parser *p, Chain *entries, TokenKind closer, TokenKind closer2,
                               const char *closerText, bool labels )
{
    if ( Parser_peek( p )->kind == closer ) return Parser_tryTakeCloser( p, closer, closer2, closerText );

    for ( ;; )
    {
        uint32_t     entry = 0;
        const Token *t = Parser_peek( p );
        if ( labels && t->kind == TOK_IDENT && Parser_peekNext( p )->kind == TOK_COLON )
        {
            uint32_t label = Parser_advance( p );
            uint32_t name  = Parser_newNode( p, NODE_NAME, label, label );
            Parser_advance( p );                                   // the ':'
            uint32_t value = 0;
            if ( !tryParseExpr( p, &value ) ) return false;
            entry = Parser_makePair( p, NODE_PROPERTY, label, name, value );
        }
        else if ( !tryParseExpr( p, &entry ) )
        {
            return false;
        }
        Chain_append( p, entries, entry );

        t = Parser_peek( p );
        if ( t->kind == TOK_COMMA )
        {
            Parser_advance( p );
            continue;
        }
        if ( t->kind == closer ) return Parser_tryTakeCloser( p, closer, closer2, closerText );
        if ( isSpaced( t ) && canStartExpression( t ) )
        {
            return Parser_failAt( p, t, "entries before '%s' are separated by commas, as in diag(1, 2, 3) (D149)",
                                  closerText );
        }
        char expected[32];
        snprintf( expected, sizeof( expected ), "',' or '%s'", closerText );
        return Parser_failExpected( p, expected );
    }
}

//////////////////////////////////////////////////////////////////////////////
// Literals (§2.9)
//////////////////////////////////////////////////////////////////////////////

// "(" args ")": one entry is grouping, three are a point (D132, D164).
static bool tryParseParens( Parser *p, uint32_t *node )
{
    uint32_t open  = Parser_advance( p );
    Chain    entries = {};
    Context  saved = Parser_enterContext( p, false, true );
    if ( !tryParseCommaList( p, &entries, TOK_RPAREN, TOK_EOF, ")", false ) ) return false;
    Parser_restoreContext( p, saved );

    if ( entries.count == 1 )
    {
        const Token *t = Parser_peek( p );
        if ( p->unitsAllowed && t->kind == TOK_IDENT && isSpaced( t ) )
        {
            return Parser_failAt( p, t, "a unit can't follow grouping parentheses; "
                                        "'( )' with one entry is grouping (D132)" );
        }
        *node = entries.first;
        return true;
    }
    if ( entries.count != 3 )
    {
        return Parser_failAt( p, &p->tokens[ open ], "a point has exactly three entries, found %u (D132)",
                              entries.count );
    }
    uint32_t point = Parser_makeParent( p, NODE_POINT, open, open, &entries );
    return tryParseUnitAfter( p, point, node );
}

// "[" [ entries ] "]" [ unit_run ]
static bool tryParseList( Parser *p, uint32_t *node )
{
    uint32_t open    = Parser_advance( p );
    Chain    entries = {};
    Context  saved   = Parser_enterContext( p, true, false );
    if ( !tryParseEntries( p, &entries, TOK_RBRACKET, TOK_EOF, 0, false ) ) return false;
    Parser_restoreContext( p, saved );
    if ( !Parser_tryExpect( p, TOK_RBRACKET, "']'" ) ) return false;

    uint32_t list = Parser_makeParent( p, NODE_LIST, open, open, &entries );
    return tryParseUnitAfter( p, list, node );
}

// "[[" row { ";" row } "]]" [ unit_run ]: at least two rows of one length (D143).
static bool tryParseGrid( Parser *p, uint32_t *node )
{
    const Token *opener = Parser_peek( p );
    const Token *after  = Parser_peekNext( p );
    if ( after->kind == TOK_LBRACKET && !isSpaced( after ) )
    {
        return Parser_failAt( p, opener, "'[[[' is not an opener; write a list of matrices "
                                         "as '[ [[ … ]], [[ … ]] ]' (D114)" );
    }

    uint32_t open      = Parser_advance( p );
    Chain    rows      = {};
    uint32_t rowLength = 0;                                        // the first row's entry count
    Context  saved     = Parser_enterContext( p, true, false );
    for ( ;; )
    {
        uint32_t rowStart = Parser_index( p );
        Chain    entries  = {};
        if ( !tryParseEntries( p, &entries, TOK_RBRACKET, TOK_SEMICOLON, 0, false ) ) return false;
        if ( entries.count == 0 ) return Parser_failAt( p, Parser_peek( p ), "a grid row needs at least one entry" );

        if ( rows.count == 0 ) rowLength = entries.count;
        if ( entries.count != rowLength )
        {
            return Parser_failAt( p, &p->tokens[ rowStart ], "every row of a grid has the same number of entries: "
                                  "this row has %u, the first has %u (D143)", entries.count, rowLength );
        }
        Chain_append( p, &rows, Parser_makeParent( p, NODE_ROW, rowStart, rowStart, &entries ) );

        if ( Parser_peek( p )->kind == TOK_SEMICOLON )
        {
            Parser_advance( p );
            continue;
        }
        if ( !Parser_tryExpectCloser( p, TOK_RBRACKET, TOK_RBRACKET, "]]" ) ) return false;
        break;
    }
    Parser_restoreContext( p, saved );

    if ( rows.count < 2 )
    {
        return Parser_failAt( p, &p->tokens[ open ], "a grid has at least two rows; "
                                                     "for a diagonal matrix write diag(a, b, c) (D143)" );
    }
    uint32_t grid = Parser_makeParent( p, NODE_GRID, open, open, &rows );
    return tryParseUnitAfter( p, grid, node );
}

// half = three entries, then the half's unit run (D148).
static bool tryParseHalf( Parser *p, uint32_t *node )
{
    uint32_t start   = Parser_index( p );
    Chain    entries = {};
    if ( !tryParseEntries( p, &entries, TOK_BAR, TOK_SEMICOLON, 3, false ) ) return false;
    if ( entries.count != 3 )
    {
        return Parser_failAt( p, &p->tokens[ start ], "a spatial-vector half has exactly three entries, "
                                                      "then its unit (D64, D148)" );
    }
    if ( Parser_peek( p )->kind != TOK_IDENT ) return Parser_failExpected( p, "the half's unit (D148)" );

    uint32_t unit = 0;
    if ( !tryParseUnit( p, &unit ) ) return false;
    Chain_append( p, &entries, unit );
    *node = Parser_makeParent( p, NODE_HALF, start, start, &entries );
    return true;
}

// "[|" half ";" half "|]"
static bool tryParseSpatial( Parser *p, uint32_t *node )
{
    uint32_t open   = Parser_advance( p );
    Chain    halves = {};
    Context  saved  = Parser_enterContext( p, true, false );

    uint32_t half = 0;
    if ( !tryParseHalf( p, &half ) ) return false;
    Chain_append( p, &halves, half );
    if ( !Parser_tryExpect( p, TOK_SEMICOLON, "';' between the halves" ) ) return false;
    if ( !tryParseHalf( p, &half ) ) return false;
    Chain_append( p, &halves, half );
    if ( !Parser_tryExpectCloser( p, TOK_BAR, TOK_RBRACKET, "|]" ) ) return false;

    Parser_restoreContext( p, saved );
    *node = Parser_makeParent( p, NODE_SPATIAL, open, open, &halves );
    return true;
}

// "<" sum [ ";" entries ] ">" [ unit_run ]: <w> or <w; x y z> (D128, D131).
static bool tryParseQuaternion( Parser *p, uint32_t *node )
{
    uint32_t open  = Parser_advance( p );
    Chain    parts = {};
    Context  saved = Parser_enterContext( p, false, false );

    uint32_t w = 0;
    if ( !tryParseSum( p, &w ) ) return false;
    Chain_append( p, &parts, w );

    if ( Parser_peek( p )->kind == TOK_SEMICOLON )
    {
        Parser_advance( p );
        Parser_enterContext( p, true, false );
        if ( !tryParseEntries( p, &parts, TOK_RANGLE, TOK_EOF, 0, true ) ) return false;
        if ( parts.count != 4 )
        {
            return Parser_failAt( p, &p->tokens[ open ], "a quaternion is <w> or <w; x y z>, "
                                                         "with three entries after ';' (D128)" );
        }
    }
    if ( !Parser_tryExpect( p, TOK_RANGLE, "'>' to close the quaternion (or ';' after w)" ) ) return false;

    Parser_restoreContext( p, saved );
    uint32_t quaternion = Parser_makeParent( p, NODE_QUATERNION, open, open, &parts );
    return tryParseUnitAfter( p, quaternion, node );
}

// Check the Euler form of an orientation (D140). Any labeled entry makes it
// Euler: then all three entries are labeled X, Y or Z, with no primes or with
// 0, 1 and 2 primes in order.
static bool Parser_tryCheckEuler( Parser *p, uint32_t orientation, bool *isEuler )
{
    uint32_t count   = 0;
    uint32_t labeled = 0;
    for ( uint32_t c = Parser_node( p, orientation )->child ; c != 0 ; c = Parser_node( p, c )->next )
    {
        count++;
        if ( Parser_node( p, c )->kind == NODE_PROPERTY ) labeled++;
    }

    *isEuler = labeled > 0;
    if ( labeled == 0 ) return true;

    const Token *open = &p->tokens[ Parser_node( p, orientation )->token ];
    if ( labeled != 3 || count != 3 )
    {
        return Parser_failAt( p, open, "Euler angles are three labeled entries, "
                                       "as in <| Z: a, Y': b, X'': c |> (D140)" );
    }

    bool     unprimed = true;
    bool     ordered  = true;
    uint32_t i        = 0;
    for ( uint32_t c = Parser_node( p, orientation )->child ; c != 0 ; c = Parser_node( p, c )->next, i++ )
    {
        const Token *label  = &p->tokens[ Parser_node( p, c )->token ];
        const char  *s      = p->text + label->offset;
        uint32_t     primes = 0;
        while ( primes < label->len && s[ label->len - 1 - primes ] == '\'' ) primes++;

        if ( label->len - primes != 1 || ( s[0] != 'X' && s[0] != 'Y' && s[0] != 'Z' ) )
        {
            return Parser_failAt( p, label, "an Euler label is X, Y or Z (D140)" );
        }
        if ( primes != 0 ) unprimed = false;
        if ( primes != i ) ordered  = false;
    }
    if ( !unprimed && !ordered )
    {
        return Parser_failAt( p, &p->tokens[ Parser_node( p, Parser_node( p, orientation )->child )->token ],
                              "Euler labels carry no primes, or 0, 1 and 2 primes in order (D140)" );
    }
    return true;
}

// "<|" clist "|>" [ unit_run ]: a unit only after Euler angles.
static bool tryParseOrientation( Parser *p, uint32_t *node )
{
    uint32_t open    = Parser_advance( p );
    Chain    entries = {};
    Context  saved   = Parser_enterContext( p, false, true );
    if ( !tryParseCommaList( p, &entries, TOK_BAR, TOK_RANGLE, "|>", true ) ) return false;
    Parser_restoreContext( p, saved );

    uint32_t orientation = Parser_makeParent( p, NODE_ORIENTATION, open, open, &entries );
    bool     isEuler;
    if ( !Parser_tryCheckEuler( p, orientation, &isEuler ) ) return false;
    if ( isEuler ) return tryParseUnitAfter( p, orientation, node );

    const Token *t = Parser_peek( p );
    if ( p->unitsAllowed && t->kind == TOK_IDENT && isSpaced( t ) )
    {
        return Parser_failAt( p, t, "a unit after '|>' is legal only for Euler angles (§2.9)" );
    }
    *node = orientation;
    return true;
}

// "<[" clist "]>": a point and an orientation, and no unit after (D148, D149).
static bool tryParseFrame( Parser *p, uint32_t *node )
{
    uint32_t open    = Parser_advance( p );
    Chain    entries = {};
    Context  saved   = Parser_enterContext( p, false, true );
    if ( !tryParseCommaList( p, &entries, TOK_RBRACKET, TOK_RANGLE, "]>", true ) ) return false;
    Parser_restoreContext( p, saved );

    // Appendix A lets a frame entry carry a label, but a label here is not
    // an Euler label, so it may not be primed (§1.4).
    for ( uint32_t c = entries.first ; entries.count > 0 && c != 0 ; c = Parser_node( p, c )->next )
    {
        const Token *label = &p->tokens[ Parser_node( p, c )->token ];
        if ( Parser_node( p, c )->kind == NODE_PROPERTY && Parser_isPrimed( p, label ) )
        {
            return Parser_failAt( p, label, "a primed name is legal only as an Euler axis label, "
                                            "as in <| Z: a, Y': b, X'': c |> (D120)" );
        }
    }

    if ( entries.count != 2 )
    {
        return Parser_failAt( p, &p->tokens[ open ], "a frame is <[ point, orientation ]>, found %u entries (D149)",
                              entries.count );
    }
    const Token *t = Parser_peek( p );
    if ( p->unitsAllowed && t->kind == TOK_IDENT && isSpaced( t ) )
    {
        return Parser_failAt( p, t, "a frame takes no unit; put the unit on its point, "
                                    "as in <[ (1, 2, 3) in, q ]> (D148)" );
    }
    *node = Parser_makeParent( p, NODE_FRAME, open, open, &entries );
    return true;
}

// The block mode a typed block or scope gives its statements (§2.4).
static BlockMode Parser_modeFor( const Parser *p, const Token *name, bool typed )
{
    if ( typed )
    {
        if ( Parser_isText( p, name, "map" ) ) return MODE_MAP;
        return MODE_GENERAL;
    }
    if ( Parser_isText( p, name, "assembly" ) ) return MODE_ASSEMBLY;
    if ( Parser_isText( p, name, "states" ) )   return MODE_STATES;
    return MODE_GENERAL;
}

// typed_block = IDENT block
static bool tryParseTypedBlock( Parser *p, uint32_t *node )
{
    BlockMode mode = Parser_modeFor( p, Parser_peek( p ), true );
    uint32_t  name = 0;
    if ( !tryParseName( p, &name ) ) return false;
    uint32_t block = 0;
    if ( !tryParseBlock( p, mode, &block ) ) return false;
    uint32_t token = Parser_node( p, name )->token;
    *node = Parser_makePair( p, NODE_TYPED_BLOCK, token, name, block );
    return true;
}

//////////////////////////////////////////////////////////////////////////////
// Expressions (§2.8)
//////////////////////////////////////////////////////////////////////////////

// primary = a literal, a path's first name, or a typed block.
static bool tryParsePrimary( Parser *p, uint32_t *node )
{
    const Token *t = Parser_peek( p );
    switch ( t->kind )
    {
    case TOK_NUMBER:
    {
        uint32_t     i    = Parser_advance( p );
        const Token *next = Parser_peek( p );
        if ( next->kind == TOK_IDENT && !isSpaced( next ) )
        {
            return Parser_failAt( p, next, "write a space between a number and its unit: '%.*s %.*s' (§2.6)",
                                  (int)t->len, p->text + t->offset, (int)next->len, p->text + next->offset );
        }
        uint32_t number = Parser_newNode( p, NODE_NUMBER, i, i );
        return tryParseUnitAfter( p, number, node );
    }

    case TOK_STRING:
    {
        uint32_t i = Parser_advance( p );
        *node = Parser_newNode( p, NODE_STRING, i, i );
        return true;
    }

    case TOK_IDENT:
        if ( Parser_peekNext( p )->kind == TOK_LBRACE ) return tryParseTypedBlock( p, node );
        return tryParseName( p, node );

    case TOK_LPAREN:             return tryParseParens( p, node );
    case TOK_LBRACKET:           return tryParseList( p, node );
    case TOK_LBRACKET_LBRACKET:  return tryParseGrid( p, node );
    case TOK_LBRACKET_BAR:       return tryParseSpatial( p, node );
    case TOK_LANGLE:             return tryParseQuaternion( p, node );
    case TOK_LANGLE_BAR:         return tryParseOrientation( p, node );
    case TOK_LANGLE_LBRACKET:    return tryParseFrame( p, node );

    case TOK_LANGLE_MINUS:
        return Parser_failAt( p, t, "'<-' is the drive arrow; a quaternion with a negative scalar "
                                    "is written '< -w; x y z>' (D155)" );

    case TOK_PLUS:
        return Parser_failAt( p, t, "there is no prefix '+'" );

    case TOK_KEYWORD:
        if ( Parser_isText( p, t, "not" ) || Parser_isText( p, t, "and" ) || Parser_isText( p, t, "or" ) )
        {
            return Parser_failAt( p, t, "'%.*s' is legal only in a 'when' guard (D133)",
                                  (int)t->len, p->text + t->offset );
        }
        return Parser_failExpected( p, "a value" );

    default:
        return Parser_failExpected( p, "a value" );
    }
}

// postfix = primary { call | index | access }. A call's `(` and an index's
// `[` must be tight (D133); `.` may be spaced (D153).
static bool tryParsePostfix( Parser *p, uint32_t *node )
{
    if ( !tryParsePrimary( p, node ) ) return false;

    for ( ;; )
    {
        const Token *t = Parser_peek( p );
        if ( t->kind == TOK_LPAREN && !isSpaced( t ) )
        {
            uint32_t open  = Parser_advance( p );
            Chain    parts = {};
            Chain_append( p, &parts, *node );
            Context  saved = Parser_enterContext( p, false, true );
            if ( !tryParseCommaList( p, &parts, TOK_RPAREN, TOK_EOF, ")", false ) ) return false;
            Parser_restoreContext( p, saved );
            uint32_t call = Parser_makeParent( p, NODE_CALL, open, Parser_node( p, *node )->firstToken, &parts );
            if ( !tryParseUnitAfter( p, call, node ) ) return false;
        }
        else if ( t->kind == TOK_LBRACKET && !isSpaced( t ) )
        {
            if ( !tryParseIndexSuffix( p, node ) ) return false;
        }
        else if ( t->kind == TOK_DOT )
        {
            uint32_t dot = Parser_advance( p );
            uint32_t name = 0;
            if ( !tryParseName( p, &name ) ) return false;
            *node = Parser_makePair( p, NODE_ACCESS, dot, *node, name );
        }
        else
        {
            return true;
        }
    }
}

// prefix = ( "-" | "^" ) prefix | postfix
static bool tryParsePrefix( Parser *p, uint32_t *node )
{
    const Token *t = Parser_peek( p );
    if ( t->kind == TOK_MINUS || t->kind == TOK_CARET )
    {
        uint32_t op = Parser_advance( p );
        uint32_t operand = 0;
        if ( !tryParsePrefix( p, &operand ) ) return false;
        *node = Parser_makeSingle( p, NODE_UNARY, op, op, operand );
        return true;
    }
    return tryParsePostfix( p, node );
}

// apply = prefix [ "\>" apply ]   (right-associative)
static bool tryParseApply( Parser *p, uint32_t *node )
{
    if ( !tryParsePrefix( p, node ) ) return false;
    if ( Parser_peek( p )->kind != TOK_APPLY ) return true;

    uint32_t op = Parser_advance( p );
    uint32_t right = 0;
    if ( !tryParseApply( p, &right ) ) return false;
    *node = Parser_makePair( p, NODE_BINARY, op, *node, right );
    return true;
}

// product = apply { ( "*" | "/" | "mod" | "\." | "\x" ) apply }
static bool tryParseProduct( Parser *p, uint32_t *node )
{
    if ( !tryParseApply( p, node ) ) return false;
    for ( ;; )
    {
        const Token *t = Parser_peek( p );
        bool isProductOp = t->kind == TOK_STAR || t->kind == TOK_SLASH
                        || t->kind == TOK_DOT_PRODUCT || t->kind == TOK_CROSS_PRODUCT
                        || Parser_isReserved( p, t, "mod" );
        if ( !isProductOp ) return true;

        uint32_t op = Parser_advance( p );
        uint32_t right = 0;
        if ( !tryParseApply( p, &right ) ) return false;
        *node = Parser_makePair( p, NODE_BINARY, op, *node, right );
    }
}

// sum = product { ( "+" | "-" ) product }
// At the top level of a space-separated bracket, a spaced sign with a tight
// operand starts the next entry instead (§2.7 rule 3): [0 -1 0].
static bool tryParseSum( Parser *p, uint32_t *node )
{
    if ( !tryParseProduct( p, node ) ) return false;
    for ( ;; )
    {
        const Token *t = Parser_peek( p );
        if ( t->kind != TOK_PLUS && t->kind != TOK_MINUS ) return true;
        if ( p->inSpaceList && isSpaced( t ) && !isSpaced( Parser_peekNext( p ) ) ) return true;

        uint32_t op = Parser_advance( p );
        uint32_t right = 0;
        if ( !tryParseProduct( p, &right ) ) return false;
        *node = Parser_makePair( p, NODE_BINARY, op, *node, right );
    }
}

// range = sum [ ".." sum ]   (non-associative)
static bool tryParseRange( Parser *p, uint32_t *node )
{
    if ( !tryParseSum( p, node ) ) return false;
    if ( Parser_peek( p )->kind != TOK_DOT_DOT ) return true;

    uint32_t op = Parser_advance( p );
    uint32_t right = 0;
    if ( !tryParseSum( p, &right ) ) return false;
    *node = Parser_makePair( p, NODE_BINARY, op, *node, right );

    if ( Parser_peek( p )->kind == TOK_DOT_DOT )
    {
        return Parser_failAt( p, Parser_peek( p ), "'..' does not chain; a range is 'a..b' (D133)" );
    }
    return true;
}

// expr = range, outside a guard: a comparison or `and`/`or` here is an error.
static bool tryParseExpr( Parser *p, uint32_t *node )
{
    if ( !tryParseRange( p, node ) ) return false;

    const Token *t = Parser_peek( p );
    if ( isComparison( t ) || Parser_isReserved( p, t, "and" ) || Parser_isReserved( p, t, "or" ) )
    {
        // In a list, a spaced '<' after an entry continues it as a
        // comparison (§2.7 rule 4), so quaternions there need commas.
        bool inList = p->inSpaceList && t->kind == TOK_LANGLE;
        return Parser_failAt( p, t, "'%.*s' is legal only in a 'when' guard (D133)%s",
                              (int)t->len, p->text + t->offset,
                              inList ? "; separate quaternions in a list with commas" : "" );
    }
    return true;
}

// cmp_expr = expr [ ( "<" | ">" | "<=" | ">=" | "==" | "!=" ) expr ]   (non-associative)
static bool tryParseComparison( Parser *p, uint32_t *node )
{
    if ( !tryParseRange( p, node ) ) return false;
    if ( !isComparison( Parser_peek( p ) ) ) return true;

    uint32_t op = Parser_advance( p );
    uint32_t right = 0;
    if ( !tryParseRange( p, &right ) ) return false;
    *node = Parser_makePair( p, NODE_BINARY, op, *node, right );

    if ( isComparison( Parser_peek( p ) ) )
    {
        return Parser_failAt( p, Parser_peek( p ), "comparisons do not chain; write 'a < b and b < c' (D133)" );
    }
    return true;
}

// not_expr = "not" not_expr | cmp_expr
static bool tryParseNot( Parser *p, uint32_t *node )
{
    if ( !Parser_isReserved( p, Parser_peek( p ), "not" ) ) return tryParseComparison( p, node );

    uint32_t op = Parser_advance( p );
    uint32_t operand = 0;
    if ( !tryParseNot( p, &operand ) ) return false;
    *node = Parser_makeSingle( p, NODE_UNARY, op, op, operand );
    return true;
}

// and_expr = not_expr { "and" not_expr }
static bool tryParseAnd( Parser *p, uint32_t *node )
{
    if ( !tryParseNot( p, node ) ) return false;
    while ( Parser_isReserved( p, Parser_peek( p ), "and" ) )
    {
        uint32_t op = Parser_advance( p );
        uint32_t right = 0;
        if ( !tryParseNot( p, &right ) ) return false;
        *node = Parser_makePair( p, NODE_BINARY, op, *node, right );
    }
    return true;
}

// guard = or_expr = and_expr { "or" and_expr }
static bool tryParseGuard( Parser *p, uint32_t *node )
{
    if ( !tryParseAnd( p, node ) ) return false;
    while ( Parser_isReserved( p, Parser_peek( p ), "or" ) )
    {
        uint32_t op = Parser_advance( p );
        uint32_t right = 0;
        if ( !tryParseAnd( p, &right ) ) return false;
        *node = Parser_makePair( p, NODE_BINARY, op, *node, right );
    }
    return true;
}

//////////////////////////////////////////////////////////////////////////////
// Statements (§2.3, §2.4)
//////////////////////////////////////////////////////////////////////////////

// items = item { "," item };  item = IDENT ":" expr | expr
static bool tryParseItems( Parser *p, uint32_t *node )
{
    uint32_t first = Parser_index( p );
    Chain    items = {};
    for ( ;; )
    {
        uint32_t     item = 0;
        const Token *t = Parser_peek( p );
        if ( t->kind == TOK_IDENT && Parser_peekNext( p )->kind == TOK_COLON )
        {
            uint32_t name = 0;
            if ( !tryParseName( p, &name ) ) return false;
            Parser_advance( p );                                   // the ':'
            uint32_t value = 0;
            if ( !tryParseExpr( p, &value ) ) return false;
            item = Parser_makePair( p, NODE_PROPERTY, Parser_node( p, name )->token, name, value );
        }
        else if ( !tryParseExpr( p, &item ) )
        {
            return false;
        }
        Chain_append( p, &items, item );

        if ( Parser_peek( p )->kind != TOK_COMMA ) break;
        Parser_advance( p );
    }
    *node = Parser_makeParent( p, NODE_ITEMS, first, first, &items );
    return true;
}

// for_clause = "for" IDENT "in" expr   (§2.5)
static bool tryParseFor( Parser *p, uint32_t *node )
{
    uint32_t keyword = Parser_advance( p );
    uint32_t name = 0;
    if ( !tryParseName( p, &name ) ) return false;
    if ( !Parser_isWord( p, Parser_peek( p ), "in" ) ) return Parser_failExpected( p, "'in'" );
    Parser_advance( p );
    uint32_t set = 0;
    if ( !tryParseExpr( p, &set ) ) return false;

    Chain parts = {};
    Chain_append( p, &parts, name );
    Chain_append( p, &parts, set );
    *node = Parser_makeParent( p, NODE_FOR, keyword, keyword, &parts );
    return true;
}

// The rest of a line statement: items [ for_clause ] ";"
static bool tryParseLineRest( Parser *p, Chain *parts )
{
    uint32_t items = 0;
    if ( !tryParseItems( p, &items ) ) return false;
    Chain_append( p, parts, items );

    if ( Parser_isReserved( p, Parser_peek( p ), "for" ) )
    {
        uint32_t forClause = 0;
        if ( !tryParseFor( p, &forClause ) ) return false;
        Chain_append( p, parts, forClause );
    }
    return Parser_tryExpectSemicolon( p );
}

// declaration = key ":" ( block | typed_block | items [ for_clause ] ";" )
static bool tryParseDeclarationRest( Parser *p, uint32_t key, uint32_t *node )
{
    uint32_t colon = Parser_advance( p );
    Chain    parts = {};
    Chain_append( p, &parts, key );

    const Token *t = Parser_peek( p );
    if ( t->kind == TOK_LBRACE )
    {
        uint32_t block = 0;
        if ( !tryParseBlock( p, MODE_GENERAL, &block ) ) return false;
        Chain_append( p, &parts, block );
    }
    else if ( t->kind == TOK_IDENT && Parser_peekNext( p )->kind == TOK_LBRACE )
    {
        uint32_t typed = 0;
        if ( !tryParseTypedBlock( p, &typed ) ) return false;
        Chain_append( p, &parts, typed );
    }
    else if ( !tryParseLineRest( p, &parts ) )
    {
        return false;
    }
    *node = Parser_makeParent( p, NODE_DECLARATION, colon, Parser_node( p, key )->firstToken, &parts );
    return true;
}

// definition = lhs "=" ( typed_block | items [ for_clause ] ";" )
static bool tryParseDefinitionRest( Parser *p, uint32_t key, uint32_t *node )
{
    uint32_t equal = Parser_advance( p );
    Chain    parts = {};
    Chain_append( p, &parts, key );

    const Token *t = Parser_peek( p );
    if ( t->kind == TOK_IDENT && Parser_peekNext( p )->kind == TOK_LBRACE )
    {
        uint32_t typed = 0;
        if ( !tryParseTypedBlock( p, &typed ) ) return false;
        Chain_append( p, &parts, typed );
    }
    else if ( !tryParseLineRest( p, &parts ) )
    {
        return false;
    }
    *node = Parser_makeParent( p, NODE_DEFINITION, equal, Parser_node( p, key )->firstToken, &parts );
    return true;
}

// scope = IDENT block   (a section, a body, a state or a sub-block, D138)
static bool tryParseScope( Parser *p, BlockMode mode, uint32_t *node )
{
    uint32_t name = 0;
    if ( !tryParseName( p, &name ) ) return false;
    uint32_t block = 0;
    if ( !tryParseBlock( p, mode, &block ) ) return false;
    uint32_t token = Parser_node( p, name )->token;
    *node = Parser_makePair( p, NODE_SCOPE, token, name, block );
    return true;
}

// system = "system" IDENT block
static bool tryParseSystem( Parser *p, uint32_t *node )
{
    uint32_t keyword = Parser_advance( p );
    Chain    parts   = {};
    uint32_t name = 0;
    if ( !tryParseName( p, &name ) ) return false;
    Chain_append( p, &parts, name );
    uint32_t block = 0;
    if ( !tryParseBlock( p, MODE_GENERAL, &block ) ) return false;
    Chain_append( p, &parts, block );
    *node = Parser_makeParent( p, NODE_SYSTEM, keyword, keyword, &parts );
    return true;
}

// endpoint = lhs { "." IDENT }
static bool tryParseEndpoint( Parser *p, uint32_t *node )
{
    if ( !tryParseLhs( p, node ) ) return false;
    while ( Parser_peek( p )->kind == TOK_DOT )
    {
        uint32_t dot = Parser_advance( p );
        uint32_t name = 0;
        if ( !tryParseName( p, &name ) ) return false;
        *node = Parser_makePair( p, NODE_ACCESS, dot, *node, name );
    }
    return true;
}

// chain_stmt = endpoint "<" lhs ">" endpoint { "<" lhs ">" endpoint } [ for_clause ] ";"
static bool tryParseChain( Parser *p, uint32_t *node )
{
    uint32_t first = Parser_index( p );
    Chain    parts = {};
    uint32_t endpoint = 0;
    if ( !tryParseEndpoint( p, &endpoint ) ) return false;
    Chain_append( p, &parts, endpoint );

    if ( Parser_peek( p )->kind != TOK_LANGLE )
    {
        if ( Parser_peek( p )->kind == TOK_LANGLE_MINUS_RANGLE ) return Parser_failExpected( p, "'<'" );
        return Parser_failExpected( p, "'<connector>': an assembly line is 'a <connector> b;' (D30)" );
    }
    while ( Parser_peek( p )->kind == TOK_LANGLE )
    {
        uint32_t open = Parser_advance( p );
        uint32_t connector = 0;
        if ( !tryParseLhs( p, &connector ) ) return false;
        if ( !Parser_tryExpect( p, TOK_RANGLE, "'>' to close the connector slot" ) ) return false;
        Chain_append( p, &parts, Parser_makeSingle( p, NODE_SLOT, open, open, connector ) );

        if ( !tryParseEndpoint( p, &endpoint ) ) return false;
        Chain_append( p, &parts, endpoint );
    }

    if ( Parser_isReserved( p, Parser_peek( p ), "for" ) )
    {
        uint32_t forClause = 0;
        if ( !tryParseFor( p, &forClause ) ) return false;
        Chain_append( p, &parts, forClause );
    }
    if ( !Parser_tryExpectSemicolon( p ) ) return false;
    *node = Parser_makeParent( p, NODE_CHAIN, first, first, &parts );
    return true;
}

// axis_stmt = ( "in" | "out" ) IDENT ":" items ";"   (in a map, D55, D160)
static bool tryParseAxis( Parser *p, uint32_t *node )
{
    uint32_t keyword = Parser_advance( p );
    Chain    parts   = {};
    uint32_t name = 0;
    if ( !tryParseName( p, &name ) ) return false;
    Chain_append( p, &parts, name );
    if ( !Parser_tryExpect( p, TOK_COLON, "':'" ) ) return false;
    uint32_t items = 0;
    if ( !tryParseItems( p, &items ) ) return false;
    Chain_append( p, &parts, items );
    if ( !Parser_tryExpectSemicolon( p ) ) return false;
    *node = Parser_makeParent( p, NODE_AXIS, keyword, keyword, &parts );
    return true;
}

// when_stmt = "when" guard "->" IDENT ";"   (in a state, D78)
static bool tryParseWhen( Parser *p, uint32_t *node )
{
    uint32_t keyword = Parser_advance( p );
    Chain    parts   = {};
    uint32_t guard = 0;
    if ( !tryParseGuard( p, &guard ) ) return false;
    Chain_append( p, &parts, guard );
    if ( !Parser_tryExpect( p, TOK_MINUS_RANGLE, "'->' and the next state" ) ) return false;
    uint32_t target = 0;
    if ( !tryParseName( p, &target ) ) return false;
    Chain_append( p, &parts, target );
    if ( !Parser_tryExpectSemicolon( p ) ) return false;
    *node = Parser_makeParent( p, NODE_WHEN, keyword, keyword, &parts );
    return true;
}

// The statement forms of §2.3, chosen by their first tokens:
//   system NAME { … }      a nested system
//   NAME { … }             a scope
//   key : …                a declaration (the key may be a NUMBER)
//   key = …                a definition
//   path <- expr ;         a drive
//   path ;                 a bare reference
static bool tryParseGeneralStatement( Parser *p, uint32_t *node )
{
    const Token *t    = Parser_peek( p );
    const Token *next = Parser_peekNext( p );

    if ( Parser_isWord( p, t, "system" ) && next->kind == TOK_IDENT ) return tryParseSystem( p, node );
    if ( t->kind == TOK_IDENT && next->kind == TOK_LBRACE )
    {
        return tryParseScope( p, Parser_modeFor( p, t, false ), node );
    }
    if ( t->kind == TOK_NUMBER )
    {
        uint32_t i   = Parser_advance( p );
        uint32_t key = Parser_newNode( p, NODE_NUMBER, i, i );
        if ( Parser_peek( p )->kind != TOK_COLON ) return Parser_failExpected( p, "':' after a number key (D134)" );
        return tryParseDeclarationRest( p, key, node );
    }
    if ( t->kind != TOK_IDENT ) return Parser_failExpected( p, "a statement" );

    uint32_t key = 0;
    if ( !tryParseLhs( p, &key ) ) return false;

    t = Parser_peek( p );
    if ( t->kind == TOK_COLON ) return tryParseDeclarationRest( p, key, node );
    if ( t->kind == TOK_EQUAL ) return tryParseDefinitionRest( p, key, node );

    // A drive or a bare reference: the key was the first name of a path.
    bool isPathStart = Parser_node( p, key )->kind == NODE_NAME;
    if ( isPathStart && ( t->kind == TOK_DOT || t->kind == TOK_LANGLE_MINUS || t->kind == TOK_SEMICOLON ) )
    {
        uint32_t path = key;
        while ( Parser_peek( p )->kind == TOK_DOT )
        {
            uint32_t dot = Parser_advance( p );
            uint32_t name = 0;
            if ( !tryParseName( p, &name ) ) return false;
            path = Parser_makePair( p, NODE_ACCESS, dot, path, name );
        }
        uint32_t first = Parser_node( p, path )->firstToken;

        if ( Parser_peek( p )->kind == TOK_LANGLE_MINUS )
        {
            uint32_t arrow = Parser_advance( p );
            uint32_t value = 0;
            if ( !tryParseExpr( p, &value ) ) return false;
            if ( !Parser_tryExpectSemicolon( p ) ) return false;
            Chain parts = {};
            Chain_append( p, &parts, path );
            Chain_append( p, &parts, value );
            *node = Parser_makeParent( p, NODE_DRIVE, arrow, first, &parts );
            return true;
        }
        if ( Parser_peek( p )->kind == TOK_SEMICOLON )
        {
            Parser_advance( p );
            *node = Parser_makeSingle( p, NODE_REFERENCE, first, first, path );
            return true;
        }
        return Parser_failExpected( p, "'<-' or ';'" );
    }
    return Parser_failExpected( p, "':', '=', '<-' or ';'" );
}

// One statement of a block, in the block's mode (§2.4).
static bool tryParseStatement( Parser *p, BlockMode mode, uint32_t *node )
{
    const Token *t    = Parser_peek( p );
    const Token *next = Parser_peekNext( p );

    switch ( mode )
    {
    case MODE_ASSEMBLY:
        return tryParseChain( p, node );

    case MODE_STATES:
        if ( t->kind == TOK_IDENT && next->kind == TOK_LBRACE ) return tryParseScope( p, MODE_STATE, node );
        return Parser_failAt( p, t, "a 'states' block holds only states, written 'name { … }' (D78)" );

    case MODE_STATE:
        if ( Parser_isWord( p, t, "when" ) ) return tryParseWhen( p, node );
        break;

    case MODE_MAP:
        if ( ( Parser_isWord( p, t, "in" ) || Parser_isWord( p, t, "out" ) ) && next->kind == TOK_IDENT )
        {
            return tryParseAxis( p, node );
        }
        break;

    case MODE_GENERAL:
        break;
    }
    return tryParseGeneralStatement( p, node );
}

// block = "{" { stmt } "}"
static bool tryParseBlock( Parser *p, BlockMode mode, uint32_t *node )
{
    if ( Parser_peek( p )->kind != TOK_LBRACE ) return Parser_failExpected( p, "'{'" );
    uint32_t open       = Parser_advance( p );
    Chain    statements = {};
    Context  saved      = Parser_enterContext( p, false, true );

    for ( ;; )
    {
        const Token *t = Parser_peek( p );
        if ( t->kind == TOK_RBRACE ) break;
        if ( t->kind == TOK_EOF )
        {
            const Token *brace = &p->tokens[ open ];
            return Parser_failAt( p, t, "expected '}' to close the block opened at %u:%u", brace->line, brace->col );
        }
        uint32_t statement = 0;
        if ( !tryParseStatement( p, mode, &statement ) ) return false;
        Chain_append( p, &statements, statement );
        if ( !Parser_tryCheckNoSemicolon( p ) ) return false;
    }
    Parser_advance( p );                                           // the '}'

    Parser_restoreContext( p, saved );
    *node = Parser_makeParent( p, NODE_BLOCK, open, open, &statements );
    return true;
}

//////////////////////////////////////////////////////////////////////////////
// The top level and namespaces (§2.1)
//////////////////////////////////////////////////////////////////////////////

// "gravity" ":" expr ";"   (D31)
static bool tryParseGravity( Parser *p, uint32_t *node )
{
    uint32_t key = 0;
    if ( !tryParseName( p, &key ) ) return false;
    uint32_t colon = Parser_advance( p );

    uint32_t value = 0;
    if ( !tryParseExpr( p, &value ) ) return false;
    if ( Parser_peek( p )->kind == TOK_COMMA ) return Parser_failAt( p, Parser_peek( p ), "gravity takes one value (D31)" );
    if ( !Parser_tryExpectSemicolon( p ) ) return false;

    uint32_t items = Parser_makeSingle( p, NODE_ITEMS, Parser_node( p, value )->firstToken,
                                        Parser_node( p, value )->firstToken, value );
    *node = Parser_makePair( p, NODE_DECLARATION, colon, key, items );
    return true;
}

// The braces of a namespace: "{" { statement } "}", where each statement is
// a top statement (anonymous) or a namespace statement (named).
static bool tryParseNamespaceBlock( Parser *p, bool named, uint32_t *node );

// namespace = "namespace" qname "{" { ns_stmt } "}"  or, at the top level,
//             "namespace" "{" { top_stmt } "}"   (D9, D11, D166)
static bool tryParseNamespace( Parser *p, bool atTopLevel, uint32_t *node )
{
    uint32_t keyword = Parser_advance( p );
    Chain    parts   = {};

    bool named = Parser_peek( p )->kind != TOK_LBRACE;
    if ( !named && !atTopLevel )
    {
        return Parser_failAt( p, &p->tokens[ keyword ], "an anonymous namespace is legal only at the top level (D166)" );
    }
    if ( named )
    {
        uint32_t name = 0;
        if ( !tryParsePath( p, &name ) ) return false;
        Chain_append( p, &parts, name );
    }
    uint32_t block = 0;
    if ( !tryParseNamespaceBlock( p, named, &block ) ) return false;
    Chain_append( p, &parts, block );
    *node = Parser_makeParent( p, NODE_NAMESPACE, keyword, keyword, &parts );
    return true;
}

// ns_stmt = "use" qname ";" | "provides" IDENT { "," IDENT } ";" | namespace | system
static bool tryParseNamespaceStatement( Parser *p, uint32_t *node )
{
    const Token *t    = Parser_peek( p );
    const Token *next = Parser_peekNext( p );

    if ( Parser_isWord( p, t, "use" ) && next->kind == TOK_IDENT )
    {
        uint32_t keyword = Parser_advance( p );
        uint32_t name = 0;
        if ( !tryParsePath( p, &name ) ) return false;
        if ( !Parser_tryExpectSemicolon( p ) ) return false;
        *node = Parser_makeSingle( p, NODE_USE, keyword, keyword, name );
        return true;
    }
    if ( Parser_isWord( p, t, "provides" ) && next->kind == TOK_IDENT )
    {
        uint32_t keyword = Parser_advance( p );
        Chain    names   = {};
        for ( ;; )
        {
            uint32_t name = 0;
            if ( !tryParseName( p, &name ) ) return false;
            Chain_append( p, &names, name );
            if ( Parser_peek( p )->kind != TOK_COMMA ) break;
            Parser_advance( p );
        }
        if ( !Parser_tryExpectSemicolon( p ) ) return false;
        *node = Parser_makeParent( p, NODE_PROVIDES, keyword, keyword, &names );
        return true;
    }
    if ( Parser_isWord( p, t, "namespace" ) ) return tryParseNamespace( p, false, node );
    if ( Parser_isWord( p, t, "system" ) && next->kind == TOK_IDENT ) return tryParseSystem( p, node );

    char found[64];
    Parser_describe( p, t, found, sizeof( found ) );
    return Parser_failAt( p, t, "expected 'use', 'provides', 'namespace' or 'system' in a namespace, found %s", found );
}

static bool tryParseNamespaceBlock( Parser *p, bool named, uint32_t *node )
{
    if ( Parser_peek( p )->kind != TOK_LBRACE ) return Parser_failExpected( p, "'{'" );
    uint32_t open       = Parser_advance( p );
    Chain    statements = {};

    for ( ;; )
    {
        const Token *t = Parser_peek( p );
        if ( t->kind == TOK_RBRACE ) break;
        if ( t->kind == TOK_EOF )
        {
            const Token *brace = &p->tokens[ open ];
            return Parser_failAt( p, t, "expected '}' to close the namespace opened at %u:%u", brace->line, brace->col );
        }
        uint32_t statement = 0;
        bool     parsed = named ? tryParseNamespaceStatement( p, &statement ) : tryParseTopStatement( p, &statement );
        if ( !parsed ) return false;
        Chain_append( p, &statements, statement );
        if ( !Parser_tryCheckNoSemicolon( p ) ) return false;
    }
    Parser_advance( p );                                           // the '}'

    *node = Parser_makeParent( p, NODE_BLOCK, open, open, &statements );
    return true;
}

// top_stmt = gravity | anonymous namespace | namespace | system | "simulation" block
static bool tryParseTopStatement( Parser *p, uint32_t *node )
{
    const Token *t    = Parser_peek( p );
    const Token *next = Parser_peekNext( p );

    if ( Parser_isWord( p, t, "gravity" ) && next->kind == TOK_COLON ) return tryParseGravity( p, node );
    if ( Parser_isWord( p, t, "namespace" ) ) return tryParseNamespace( p, true, node );
    if ( Parser_isWord( p, t, "system" ) && next->kind == TOK_IDENT ) return tryParseSystem( p, node );
    if ( Parser_isWord( p, t, "simulation" ) && next->kind == TOK_LBRACE )
    {
        uint32_t keyword = Parser_advance( p );
        uint32_t block = 0;
        if ( !tryParseBlock( p, MODE_GENERAL, &block ) ) return false;
        *node = Parser_makeSingle( p, NODE_SIMULATION, keyword, keyword, block );
        return true;
    }
    if ( Parser_isWord( p, t, "use" ) || Parser_isWord( p, t, "provides" ) )
    {
        return Parser_failAt( p, t, "'%.*s' is legal only inside a named namespace (D8, D10, D13)",
                              (int)t->len, p->text + t->offset );
    }

    char found[64];
    Parser_describe( p, t, found, sizeof( found ) );
    return Parser_failAt( p, t, "expected 'gravity:', 'namespace', 'system' or 'simulation' at the top level, found %s",
                          found );
}

// source = { top_stmt } EOF
static bool tryParseSource( Parser *p )
{
    uint32_t root       = Ast_add( p->ast, NODE_SOURCE, 0 );       // always nodes[0]
    Chain    statements = {};

    while ( Parser_peek( p )->kind != TOK_EOF )
    {
        uint32_t statement = 0;
        if ( !tryParseTopStatement( p, &statement ) ) return false;
        Chain_append( p, &statements, statement );
        if ( !Parser_tryCheckNoSemicolon( p ) ) return false;
    }

    Node *node = Parser_node( p, root );
    node->child     = statements.count > 0 ? statements.first : 0;
    node->lastToken = p->count - 1;                                // the EOF
    return true;
}

//////////////////////////////////////////////////////////////////////////////
// Public API
//////////////////////////////////////////////////////////////////////////////

bool tryParse( const Source *source, const TokenArray *tokens, Ast *ast, ParseError *error )
{
    Parser p;
    p.text         = source->text;
    p.tokens       = tokens->data;
    p.count        = tokens->count;
    p.pos          = 0;
    p.lastConsumed = 0;
    p.inSpaceList  = false;
    p.unitsAllowed = true;
    p.ast          = ast;
    p.error        = error;

    // Most tokens become one node.
    Ast_make( ast, tokens->count + 16 );

    if ( !tryParseSource( &p ) )
    {
        Ast_free( ast );
        return false;
    }
    return true;
}

void ParseError_print( const Source *source, const ParseError *error )
{
    fprintf( stderr, "%s:%u:%u: error: %s\n", source->path, error->line, error->col, error->message );
}

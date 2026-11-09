// resolve.h -- loading: merging texts and resolving names (Specification.md §3.1–§3.3).
//
// Everything here is flat and linked by index, like the syntax tree, and
// index 0 always means "none". What resolution learns about a syntax node is
// kept in an array parallel to that tree, so the tree stays purely syntactic.

#pragma once
#include "ast.h"
#include "catalog.h"
#include "file.h"
#include "token.h"

// What a node turned out to be.
enum NodeRole
{
    ROLE_NONE,                   // no role: most expression nodes
    ROLE_SYSTEM,                 // `system s { }`
    ROLE_NAMESPACE,              // `namespace a.b { }`
    ROLE_SECTION,                // `points { }`: organizational, not a scope (D22)
    ROLE_BODY,                   // `bRod { }` in `bodies`
    ROLE_VALUE,                  // `x = expr;`
    ROLE_DEFINITION,             // `coil = TSDA, ...;`: a shared definition
    ROLE_INSTANCE,               // `jAxle: revolute, ...;`
    ROLE_PROPERTY,               // `mass: 10 kg;` or the item `at: pivot`
    ROLE_TYPE_HEAD,              // the `revolute` of a declaration
    ROLE_REFERENCE,              // a name in an expression
    ROLE_FUNCTION,               // the callee of a call
    ROLE_CONNECTOR,              // the name in a connector slot
    ROLE_PUBLISHED,              // a name in `provides`

    ROLE_COUNT                   // the number of roles; not a role
};

struct NodeInfo
{
    NodeRole            role;
    uint32_t            symbol;  // what the node declares or refers to, or 0
    const PropertyInfo *property;// for ROLE_PROPERTY: the catalog's entry
};

// One text given to the loader.
struct Unit
{
    Source     source;
    TokenArray tokens;
    Ast        ast;
    NodeInfo  *info;             // parallel to ast.nodes; made by tryLoadModel
};

enum SymbolKind
{
    SYM_NONE,
    SYM_BUILTIN_VALUE,
    SYM_BUILTIN_FUNCTION,
    SYM_BUILTIN_TYPE,
    SYM_VALUE,
    SYM_DEFINITION,
    SYM_INSTANCE,
    SYM_BODY,
    SYM_SYSTEM,
    SYM_NAMESPACE,

    SYM_KIND_COUNT               // the number of kinds; not a kind
};

struct Symbol
{
    SymbolKind         kind;
    const char        *name;     // a view of the source text, or a catalog literal
    uint32_t           length;   // bytes in `name`
    uint32_t           unit;     // where it is declared; a built-in has no position
    uint32_t           token;    // its name's token in that unit
    uint32_t           node;     // its declaring statement in that unit
    uint32_t           scope;    // the scope that holds it
    uint32_t           next;     // the next symbol in that scope
    uint32_t           ownScope; // the scope it opens, or 0
    const ElementInfo *type;     // an element's or definition's catalog entry, or NULL
};

enum ScopeKind
{
    SCOPE_NONE,
    SCOPE_BUILTIN,               // the built-in names; outermost
    SCOPE_TOP,                   // the top level, the anonymous namespace (D13)
    SCOPE_NAMESPACE,
    SCOPE_SYSTEM,
    SCOPE_BODY,
};

struct Scope
{
    ScopeKind          kind;
    uint32_t           parent;
    uint32_t           firstSymbol;
    uint32_t           lastSymbol;
    uint32_t           owner;    // the symbol that opened it, or 0
    const ElementInfo *element;  // whose properties a `key:` here is checked against
};

struct Model
{
    Unit    *units;              // not owned
    uint32_t unitCount;

    Symbol  *symbols;            // symbols[0] is unused
    uint32_t symbolCount;
    uint32_t symbolCapacity;

    Scope   *scopes;             // scopes[0] is unused
    uint32_t scopeCount;
    uint32_t scopeCapacity;

    uint32_t rootSystem;         // the model's system (D139), or 0 for a library text

    uint32_t gravityNode;        // the `gravity:` statement, or 0
    uint32_t simulationUnit;     // the `simulation { }` block, or node 0
    uint32_t simulationNode;
};

// Where loading stopped, and why.
struct LoadError
{
    uint32_t unit;               // index into the units given to tryLoadModel
    uint32_t line;
    uint32_t col;
    char     message[320];
};

// Merge the parsed `units` and resolve their names (§3.1–§3.3). On the first
// error, fills `error`, frees what it made and returns false. The units must
// outlive the model.
bool tryLoadModel( Unit *units, uint32_t unitCount, Model *model, LoadError *error );

// Free the symbols, the scopes and every unit's `info`. The units stay.
void Model_free( Model *model );

// Print `path:line:col: error: message` to stderr.
void LoadError_print( const Unit *units, const LoadError *error );

// Write the `ombdc --names` dump. Like snprintf: writes at most `size` bytes
// and returns the size of the whole dump, NUL excluded.
uint32_t Model_format( const Model *model, char *buffer, uint32_t size );

const char *SymbolKind_name( SymbolKind kind );

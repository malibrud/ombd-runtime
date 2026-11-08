// ast.h -- the syntax tree of an OMBD source (Specification.md §2, Appendix A).
//
// The tree is flat: every node lives in one array, `Ast.nodes`, and nodes
// link to each other by index, not by pointer. A node has a first child and
// a next sibling, so any node can have any number of children, and one free
// releases the whole tree. Index 0 is always the SOURCE root, and because
// the root is never anyone's child or sibling, 0 also means "no node".
//
// The tree is vocabulary-free, like the grammar (§2): `revolute`, `mass` and
// `points` are NAME nodes, and §3 gives them meaning. Each kind's children
// come in a fixed order, listed below; [x] is optional and {x} repeats.
//
// Nodes refer to tokens by index into the TokenArray the tree was parsed
// from, so the token array must outlive the tree.

#pragma once
#include <stdint.h>
#include "token.h"

enum NodeKind
{
    // ---- structure -------------------------------------------------------
    NODE_SOURCE,         // { top statement }                       the root, always nodes[0]
    NODE_NAMESPACE,      // [name] BLOCK                            `namespace a.b { }`; no name if anonymous (D166)
    NODE_USE,            // name                                    `use a.b;` (D10)
    NODE_PROVIDES,       // NAME { NAME }                           `provides x, y;` (D8)
    NODE_SYSTEM,         // NAME BLOCK                              `system p { }` (D138)
    NODE_SIMULATION,     // BLOCK                                   `simulation { }` (D36)
    NODE_BLOCK,          // { statement }                           `{ … }`; token is the `{`

    // ---- statements (§2.3, §2.4) ------------------------------------------
    NODE_SCOPE,          // NAME BLOCK                              `points { }`, `bRod { }`, a state
    NODE_DEFINITION,     // key ( TYPED_BLOCK | ITEMS [FOR] )       `k = 1200 N-m/rad;`; token is the `=`
    NODE_DECLARATION,    // key ( BLOCK | TYPED_BLOCK | ITEMS [FOR] )  `mass: 10 kg;`; token is the `:`
    NODE_DRIVE,          // path expr                               `j.angle.pos <- theta;`; token is the `<-`
    NODE_REFERENCE,      // path                                    `engine_rpm;` in inputs/outputs
    NODE_CHAIN,          // endpoint SLOT endpoint { SLOT endpoint } [FOR]   an assembly line (D30)
    NODE_SLOT,           // lhs                                     `<jAxle>`; token is the `<`
    NODE_AXIS,           // NAME ITEMS                              `in rpm: [...] rpm;` in a map; token is `in` or `out`
    NODE_WHEN,           // guard NAME                              `when sr > 0.92 -> locked;` in a state
    NODE_ITEMS,          // item { item }                           the comma list after `:` or `=`
    NODE_PROPERTY,       // NAME expr                               `at: pivot` as an item, or an Euler label `Z: 6`
    NODE_FOR,            // NAME expr                               `for i in 0..n-1`; token is the `for`

    // ---- expressions (§2.8, §2.9) -----------------------------------------
    NODE_NAME,           //                                         an IDENT
    NODE_NUMBER,         //                                         spelling kept, as lexed
    NODE_STRING,         //                                         `"OZX"`
    NODE_UNARY,          // expr                                    `-x`, `^F`, `not g`; token is the operator
    NODE_BINARY,         // expr expr                               token is the operator: + - * / mod \. \x \> .. and, in guards, < > <= >= == != and or
    NODE_CALL,           // expr { expr }                           callee, then arguments; token is the `(`
    NODE_INDEX,          // expr expr                               `pin[i]`; token is the `[`
    NODE_ACCESS,         // expr NAME                               `bRod.cg`; token is the `.`
    NODE_POINT,          // expr expr expr                          `(0, 0, -1)` (D132, D164)
    NODE_LIST,           // { expr }                                `[0, 1, 0]`, `[1 0 0]`
    NODE_GRID,           // ROW ROW { ROW }                         `[[ … ; … ]]` (D143)
    NODE_ROW,            // expr { expr }                           one grid row
    NODE_SPATIAL,        // HALF HALF                               `[| … ; … |]` (D64, D148)
    NODE_HALF,           // expr expr expr UNIT                     one spatial-vector half, with its unit
    NODE_QUATERNION,     // expr [ expr expr expr ]                 `<1>`, `<w; x y z>` (D38, D128)
    NODE_ORIENTATION,    // expr { expr }                           `<| … |>`; Euler entries are PROPERTY nodes (D140)
    NODE_FRAME,          // expr expr                               `<[ p, o ]>` (D149)
    NODE_TYPED_BLOCK,    // NAME BLOCK                              `map { }`, `differential { }` as a value
    NODE_QUANTITY,       // expr UNIT                               a value literal and its unit run
    NODE_UNIT,           //                                         a unit run (§2.6), kept as its token span

    NODE_KIND_COUNT      // the number of kinds; not a node
};

struct Node
{
    NodeKind kind;
    uint32_t token;          // the node's own token: its name, literal, operator or opener
    uint32_t firstToken;     // the node's span in the token array, first to last, inclusive
    uint32_t lastToken;
    uint32_t child;          // the first child, or 0 for none
    uint32_t next;           // the next sibling, or 0 for none
};

struct Ast
{
    Node    *nodes;          // nodes[0] is the SOURCE root
    uint32_t count;          // nodes in use
    uint32_t capacity;       // nodes allocated
};

// Allocate room for `capacity` nodes. The tree starts empty (count 0).
void Ast_make( Ast *ast, uint32_t capacity );

// Free the nodes and leave the tree empty.
void Ast_free( Ast *ast );

// Add a node with no children or siblings and return its index. Indices stay
// valid as the array grows; pointers into it do not.
uint32_t Ast_add( Ast *ast, NodeKind kind, uint32_t token );

// The kind's name as `ombdc --ast` prints it: "DEFINITION", "BINARY", ...
const char *NodeKind_name( NodeKind kind );

// Write the `ombdc --ast` dump: one node per line, indented two spaces per
// level, as `line:col KIND`, where line:col is the start of the node's span.
// Names, literals, operators, units and axis lines add their text: `'kg'`.
// Like snprintf: writes at most `size` bytes and returns the size of the
// whole dump, NUL excluded.
uint32_t Ast_format( const Ast *ast, const TokenArray *tokens, const char *text, char *buffer, uint32_t size );

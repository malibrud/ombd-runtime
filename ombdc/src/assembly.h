// assembly.h -- connectors and assembly (Specification.md §3.7).
//
// The stage after type checking. It walks every chain line, checks each link
// against its connector's endpoint signature (D50, D52, D159), checks that
// every declared connector is wired exactly once (D162) and that no link joins
// an endpoint to itself (D179), and keeps the links on the model as a flat
// array: the topology the IR is lowered from. The links of every `assembly`
// block pool (D180), and a parent may wire a nested system's connector and
// bodies by path (D181, D182).
//
// Not implemented yet, each with its own error: ports and indexed names as
// endpoints. Drivers (`.pos <-`, `.force <-`) come with the first model that
// has one.

#pragma once
#include "resolve.h"

// One link of a chain line: `from <connector> to`.
struct Link
{
    uint32_t unit;               // the unit holding the chain line
    uint32_t node;               // the first endpoint's node, where the link starts in the text
    uint32_t connector;          // the connector instance's symbol, or a built-in connector type's for a fresh one (D170)
    uint32_t from;               // the endpoint taking the connector's first attachment: a body's symbol, or `world`'s
    uint32_t to;                 // the endpoint taking its second
};

// Check the model's assembly and fill `model->links`. On the first error,
// fills `error` and returns false; `model->links` is freed with the model
// either way.
bool tryCheckAssembly( Model *model, LoadError *error );

// Write the `ombdc --assembly` dump: per unit, one line per link,
//     line:col connector type from -> to
// Like snprintf: writes at most `size` bytes and returns the size of the
// whole dump, NUL excluded.
uint32_t Model_formatAssembly( const Model *model, char *buffer, uint32_t size );

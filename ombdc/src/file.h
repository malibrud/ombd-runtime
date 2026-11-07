// file.h -- a source text read whole into memory.

#pragma once
#include <stdint.h>

// The bytes of one source file. `text` holds `size` bytes and then a NUL, so
// a scanner can look one byte ahead anywhere without a bounds check. A NUL
// before `text[size]` is a byte of the file, not the end.
struct Source
{
    const char *path;            // as given; diagnostics print it (not owned)
    char       *text;            // size bytes, then a NUL (owned)
    uint32_t    size;            // bytes in the file, NUL excluded
};

// Copy `size` bytes into a new NUL-terminated source.
void Source_make( Source *source, const char *path, const char *bytes, uint32_t size );

// Free the text and leave the source empty.
void Source_free( Source *source );

// Read the whole file at `path`. On failure, print `path: error: ...` and
// return false.
bool tryReadFile( const char *path, Source *source );

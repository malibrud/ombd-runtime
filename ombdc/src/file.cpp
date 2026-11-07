// file.cpp -- reading a source file whole.

#include "file.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void Source_make( Source *source, const char *path, const char *bytes, uint32_t size )
{
    source->path = path;
    source->text = (char *)malloc( (size_t)size + 1 );
    if ( source->text == NULL )
    {
        fprintf( stderr, "ombdc: out of memory\n" );
        exit( 1 );
    }
    if ( size > 0 ) memcpy( source->text, bytes, size );
    source->text[ size ] = '\0';
    source->size = size;
}

void Source_free( Source *source )
{
    free( source->text );
    source->text = NULL;
    source->size = 0;
}

bool tryReadFile( const char *path, Source *source )
{
    // Open the file.
    FILE *f = fopen( path, "rb" );
    if ( f == NULL )
    {
        fprintf( stderr, "%s: error: cannot open file\n", path );
        return false;
    }

    // Get its size. Offsets are 32-bit, and ftell's long is 32-bit on
    // Windows, so a source must be under 2 GiB.
    long fileSize = -1;
    if ( fseek( f, 0, SEEK_END ) != 0 ) goto FAIL_READ;
    fileSize = ftell( f );
    if ( fileSize < 0 ) goto FAIL_READ;
    if ( fseek( f, 0, SEEK_SET ) != 0 ) goto FAIL_READ;

    // Read the bytes and terminate them.
    source->path = path;
    source->size = (uint32_t)fileSize;
    source->text = (char *)malloc( (size_t)source->size + 1 );
    if ( source->text == NULL )
    {
        fprintf( stderr, "ombdc: out of memory\n" );
        exit( 1 );
    }
    if ( fread( source->text, 1, source->size, f ) != source->size )
    {
        Source_free( source );
        goto FAIL_READ;
    }
    source->text[ source->size ] = '\0';

    fclose( f );
    return true;

FAIL_READ:
    fprintf( stderr, "%s: error: cannot read file\n", path );
    fclose( f );
    return false;
}

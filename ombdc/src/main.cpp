// main.cpp -- the ombdc driver.
//
//     ombdc --tokens file.ombd    lex the file and print one token per line
//
// Exit status: 0 on success, 1 on an error in the file, 2 on bad arguments.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "file.h"
#include "lexer.h"

static void printUsage( void )
{
    fprintf( stderr, "usage: ombdc --tokens file.ombd\n" );
}

int main( int argc, char *argv[] )
{
    if ( argc != 3 || strcmp( argv[1], "--tokens" ) != 0 )
    {
        printUsage();
        return 2;
    }

    Source source;
    if ( !tryReadFile( argv[2], &source ) ) return 1;

    TokenArray tokens;
    LexError   error;
    if ( !tryLex( &source, &tokens, &error ) )
    {
        LexError_print( &source, &error );
        Source_free( &source );
        return 1;
    }

    uint32_t size = TokenArray_format( &tokens, source.text, NULL, 0 );
    char    *dump = (char *)malloc( (size_t)size + 1 );
    if ( dump == NULL )
    {
        fprintf( stderr, "ombdc: out of memory\n" );
        return 1;
    }
    TokenArray_format( &tokens, source.text, dump, size + 1 );

#ifdef _WIN32
    // Write \n, not \r\n, so a redirected dump matches the golden files byte
    // for byte.
    _setmode( _fileno( stdout ), _O_BINARY );
#endif
    fwrite( dump, 1, size, stdout );

    free( dump );
    TokenArray_free( &tokens );
    Source_free( &source );
    return 0;
}

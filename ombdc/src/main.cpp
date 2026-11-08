// main.cpp -- the ombdc driver.
//
//     ombdc --tokens file.ombd    lex the file and print one token per line
//     ombdc --ast file.ombd       parse the file and print its syntax tree
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
#include "parser.h"

static void printUsage( void )
{
    fprintf( stderr, "usage: ombdc --tokens file.ombd\n"
                     "       ombdc --ast file.ombd\n" );
}

// Write a dump to stdout as-is.
static void writeDump( const char *dump, uint32_t size )
{
#ifdef _WIN32
    // Write \n, not \r\n, so a redirected dump matches the golden files byte
    // for byte.
    _setmode( _fileno( stdout ), _O_BINARY );
#endif
    fwrite( dump, 1, size, stdout );
}

static char *allocateDump( uint32_t size )
{
    char *dump = (char *)malloc( (size_t)size + 1 );
    if ( dump == NULL )
    {
        fprintf( stderr, "ombdc: out of memory\n" );
        exit( 1 );
    }
    return dump;
}

int main( int argc, char *argv[] )
{
    bool printTokens = argc == 3 && strcmp( argv[1], "--tokens" ) == 0;
    bool printAst    = argc == 3 && strcmp( argv[1], "--ast" ) == 0;
    if ( !printTokens && !printAst )
    {
        printUsage();
        return 2;
    }

    Source source;
    if ( !tryReadFile( argv[2], &source ) ) return 1;

    TokenArray tokens;
    LexError   lexError;
    if ( !tryLex( &source, &tokens, &lexError ) )
    {
        LexError_print( &source, &lexError );
        Source_free( &source );
        return 1;
    }

    if ( printTokens )
    {
        uint32_t size = TokenArray_format( &tokens, source.text, NULL, 0 );
        char    *dump = allocateDump( size );
        TokenArray_format( &tokens, source.text, dump, size + 1 );
        writeDump( dump, size );
        free( dump );
    }
    else
    {
        Ast        ast;
        ParseError parseError;
        if ( !tryParse( &source, &tokens, &ast, &parseError ) )
        {
            ParseError_print( &source, &parseError );
            TokenArray_free( &tokens );
            Source_free( &source );
            return 1;
        }
        uint32_t size = Ast_format( &ast, &tokens, source.text, NULL, 0 );
        char    *dump = allocateDump( size );
        Ast_format( &ast, &tokens, source.text, dump, size + 1 );
        writeDump( dump, size );
        free( dump );
        Ast_free( &ast );
    }

    TokenArray_free( &tokens );
    Source_free( &source );
    return 0;
}

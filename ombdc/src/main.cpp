// main.cpp -- the ombdc driver.
//
//     ombdc --tokens file.ombd       lex the file and print one token per line
//     ombdc --ast file.ombd          parse the file and print its syntax tree
//     ombdc --names file.ombd ...    load the files as one model and print its names
//     ombdc --types file.ombd ...    load and type-check them, and print every value's type
//
// Exit status: 0 on success, 1 on an error in a file, 2 on bad arguments.

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
#include "resolve.h"
#include "types.h"

static void printUsage( void )
{
    fprintf( stderr, "usage: ombdc --tokens file.ombd\n"
                     "       ombdc --ast file.ombd\n"
                     "       ombdc --names file.ombd ...\n"
                     "       ombdc --types file.ombd ...\n" );
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

// Read and lex one file.
static bool tryLexFile( const char *path, Source *source, TokenArray *tokens )
{
    if ( !tryReadFile( path, source ) ) return false;

    LexError error;
    if ( !tryLex( source, tokens, &error ) )
    {
        LexError_print( source, &error );
        Source_free( source );
        return false;
    }
    return true;
}

// Read, lex and parse one file.
static bool tryParseFile( const char *path, Source *source, TokenArray *tokens, Ast *ast )
{
    if ( !tryLexFile( path, source, tokens ) ) return false;

    ParseError error;
    if ( !tryParse( source, tokens, ast, &error ) )
    {
        ParseError_print( source, &error );
        TokenArray_free( tokens );
        Source_free( source );
        return false;
    }
    return true;
}

static int printTokens( const char *path )
{
    Source     source;
    TokenArray tokens;
    if ( !tryLexFile( path, &source, &tokens ) ) return 1;

    uint32_t size = TokenArray_format( &tokens, source.text, NULL, 0 );
    char    *dump = allocateDump( size );
    TokenArray_format( &tokens, source.text, dump, size + 1 );
    writeDump( dump, size );

    free( dump );
    TokenArray_free( &tokens );
    Source_free( &source );
    return 0;
}

static int printAst( const char *path )
{
    Source     source;
    TokenArray tokens;
    Ast        ast;
    if ( !tryParseFile( path, &source, &tokens, &ast ) ) return 1;

    uint32_t size = Ast_format( &ast, &tokens, source.text, NULL, 0 );
    char    *dump = allocateDump( size );
    Ast_format( &ast, &tokens, source.text, dump, size + 1 );
    writeDump( dump, size );

    free( dump );
    Ast_free( &ast );
    TokenArray_free( &tokens );
    Source_free( &source );
    return 0;
}

// Load the files as one model, then type-check it if `checkTypes`, and print
// the dump of the last stage that ran.
static int printModel( char **paths, uint32_t count, bool checkTypes )
{
    Unit *units = (Unit *)calloc( count, sizeof( Unit ) );
    if ( units == NULL )
    {
        fprintf( stderr, "ombdc: out of memory\n" );
        return 1;
    }

    for ( uint32_t i = 0 ; i < count ; i++ )
    {
        if ( !tryParseFile( paths[i], &units[i].source, &units[i].tokens, &units[i].ast ) ) return 1;
    }

    Model     model;
    LoadError error;
    if ( !tryLoadModel( units, count, &model, &error ) )
    {
        LoadError_print( units, &error );
        return 1;
    }
    if ( checkTypes && !tryCheckTypes( &model, &error ) )
    {
        LoadError_print( units, &error );
        return 1;
    }

    uint32_t size = checkTypes ? Model_formatTypes( &model, NULL, 0 ) : Model_format( &model, NULL, 0 );
    char    *dump = allocateDump( size );
    if ( checkTypes ) Model_formatTypes( &model, dump, size + 1 );
    else              Model_format( &model, dump, size + 1 );
    writeDump( dump, size );

    free( dump );
    Model_free( &model );
    for ( uint32_t i = 0 ; i < count ; i++ )
    {
        Ast_free( &units[i].ast );
        TokenArray_free( &units[i].tokens );
        Source_free( &units[i].source );
    }
    free( units );
    return 0;
}

int main( int argc, char *argv[] )
{
    if ( argc == 3 && strcmp( argv[1], "--tokens" ) == 0 ) return printTokens( argv[2] );
    if ( argc == 3 && strcmp( argv[1], "--ast" ) == 0 )    return printAst( argv[2] );
    if ( argc >= 3 && strcmp( argv[1], "--names" ) == 0 )  return printModel( &argv[2], (uint32_t)( argc - 2 ), false );
    if ( argc >= 3 && strcmp( argv[1], "--types" ) == 0 )  return printModel( &argv[2], (uint32_t)( argc - 2 ), true );

    printUsage();
    return 2;
}

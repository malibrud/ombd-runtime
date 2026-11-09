// test_main.cpp -- the ombdc unit tests.
//
//     ombdc_tests <examples-dir> <golden-dir>
//
// <examples-dir> holds Pendulum.ombd and SliderCrank.ombd (the specification
// repo's Examples); <golden-dir> holds their reviewed dumps (*.tokens, *.ast, *.names).
// Exit status: the number of failed tests, or 2 on bad arguments.

#include <stdio.h>
#include "unit_test.h"

void lexer_ut( const char *examplesDir, const char *goldenDir );
void parser_ut( const char *examplesDir, const char *goldenDir );
void resolve_ut( const char *examplesDir, const char *goldenDir );

int main( int argc, char *argv[] )
{
    if ( argc != 3 )
    {
        fprintf( stderr, "usage: ombdc_tests <examples-dir> <golden-dir>\n" );
        return 2;
    }

    lexer_ut( argv[1], argv[2] );
    parser_ut( argv[1], argv[2] );
    resolve_ut( argv[1], argv[2] );

    return ut_reportTotals();
}

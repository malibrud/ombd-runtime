// unit_test.cpp -- the test reporter.

#include "unit_test.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>

static int         ut_nTests;
static int         ut_nPassed;
static int         ut_nModules;
static int         ut_nModuleTests;
static int         ut_nModulePassed;
static const char *ut_moduleName;
static bool        ut_isInModule;

void ut_beginModule( const char *name )
{
    assert( !ut_isInModule );

    ut_nModules++;
    ut_moduleName    = name;
    ut_isInModule    = true;
    ut_nModuleTests  = 0;
    ut_nModulePassed = 0;
    printf( "Begin testing module: %s\n", name );
}

void ut_endModule( void )
{
    assert( ut_isInModule );

    ut_isInModule = false;
    printf( "End testing module: %s. %d of %d tests passed.\n\n",
            ut_moduleName, ut_nModulePassed, ut_nModuleTests );
}

void ut_reportTestResult( bool passed, const char *format, ... )
{
    ut_nTests++;
    ut_nModuleTests++;
    ut_nPassed       += passed;
    ut_nModulePassed += passed;

    printf( "  %3d. ", ut_nModuleTests );

    va_list args;
    va_start( args, format );
    vprintf( format, args );
    va_end( args );

    printf( " ...%s\n", passed ? "pass" : "FAIL" );
}

int ut_reportTotals( void )
{
    int nFailed = ut_nTests - ut_nPassed;

    printf( "*** Testing results ***\n" );
    printf( "  %d modules tested.\n", ut_nModules );
    printf( "  %d tests executed.\n", ut_nTests );
    printf( "  %d tests passed.\n", ut_nPassed );
    printf( "  %d tests failed.\n", nFailed );
    return nFailed;
}

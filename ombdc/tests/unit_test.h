// unit_test.h -- a small test reporter (after compilerExperiments).
//
// Tests are grouped in modules. Each test reports pass or fail with a
// printf-style description; the totals give the failure count.

#pragma once

void ut_beginModule( const char *name );
void ut_endModule( void );
void ut_reportTestResult( bool passed, const char *format, ... );

// Print the totals and return the number of failed tests.
int ut_reportTotals( void );

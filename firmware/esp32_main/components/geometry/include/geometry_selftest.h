#pragma once

#include <stddef.h>

/* Run the geometry self-test and write a JSON result into `out`.
 *
 * This runs on the board, with the real compiler and the real maths
 * library, so it checks the code that will actually run. The same assertions
 * exist as a host test in test/test_geometry.c for anyone with gcc; the
 * expected numbers are the SPEC and the two are kept in step. */
void geometry_selftest(char *out, size_t cap);

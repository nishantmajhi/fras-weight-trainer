/*
 * Declarations for the failing allocator in json_test.c.
 *
 * The test build force-includes this into src/json.c (-include) alongside
 * -DJSON_MALLOC=json_test_malloc and friends, so the parser's allocation
 * sites route here and can be made to fail one at a time. Production
 * builds never see this file and use plain malloc/calloc/realloc.
 */
#ifndef FRAS_TEST_ALLOC_HOOKS_H
#define FRAS_TEST_ALLOC_HOOKS_H

#include <stddef.h>

void *json_test_malloc(size_t n);
void *json_test_calloc(size_t n, size_t sz);
void *json_test_realloc(void *p, size_t n);

#endif

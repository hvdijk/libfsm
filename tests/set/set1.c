/*
 * Copyright 2017 Ed Kellett
 *
 * See LICENCE for the full copyright terms.
 */

#include <assert.h>
#include <stdlib.h>

#include <adt/set.h>

typedef int item_t;

#include "set.inc"

static int
cmp_int(const void *a, const void *b)
{
	const int *pa = * (const int * const *) a;
	const int *pb = * (const int * const *) b;

	if (*pa > *pb)      return +1;
	else if (*pa < *pb) return -1;
	else                return  0;
}

int main(void) {
	struct set *s = set_create(NULL, cmp_int);
	int a[3] = {1, 2, 3};
	assert(set_add(s, &a[0]));
	assert(set_add(s, &a[1]));
	assert(set_add(s, &a[2]));
	assert(set_contains(s, &a[0]));
	assert(set_contains(s, &a[1]));
	assert(set_contains(s, &a[2]));
	return 0;
}

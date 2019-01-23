/*
 * Copyright 2004, Axel Dörfler, axeld@pinc-software.de. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#undef NDEBUG
	// just in case

#include <OS.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>


extern char* __progname;


void
__assert_fail(const char *assertion, const char* file, unsigned int line,
	const char* function)
{
	fprintf(stderr, "%s: %s:%d:%s: %s\n", __progname, file, line, function,
		assertion);
	abort();
}


void
__assert_perror_fail(int error, const char* file, unsigned int line,
	const char* function)
{
	fprintf(stderr, "%s: %s:%d:%s: %s\n", __progname, file, line, function,
		strerror(error));
	abort();
}

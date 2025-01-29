/*
 * Copyright 2008, Axel Dörfler, axeld@pinc-software.de.
 * Distributed under the terms of the MIT license.
 */


#include <stdbool.h>
#include <string.h>
#include <SupportDefs.h>


#define LACKS_ZERO_BYTE(value) \
	(((value - 0x01010101) & ~value & 0x80808080) == 0)

int
strcmp(char const *a, char const *b)
{
	if ((((addr_t)a) & 3) == 0 && (((addr_t)b) & 3) == 0) {
		uint32* a32 = (uint32*)a;
		uint32* b32 = (uint32*)b;

		while (*a32 == *b32 && LACKS_ZERO_BYTE((*a32))) {
			a32++;
			b32++;
		}
		a = (const char *)a32;
		b = (const char *)b32;
	}

	while (true) {
		int cmp = (unsigned char)*a - (unsigned char)*b++;
		if (cmp != 0 || *a++ == '\0')
			return cmp;
	}
}

// Copyright 2014-2015 Ben Trask
// MIT licensed (see LICENSE for details)

#ifndef COMMON_H
#define COMMON_H

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "util/strext.h"

typedef unsigned char byte_t;
typedef char str_t;
typedef str_t const *strarg_t; // A string that belongs to someone else.

#define numberof(x) (sizeof(x) / sizeof(*(x)))

#define STR_LEN(str) (str), (sizeof(str)-1)
#define UV_BUF_STATIC(lit) uv_buf_init((char *)STR_LEN(lit))

#define MIN(a, b) ({ \
	__typeof__(a) const __a = (a); \
	__typeof__(b) const __b = (b); \
	__a < __b ? __a : __b; \
})
#define MAX(a, b) ({ \
	__typeof__(a) const __a = (a); \
	__typeof__(b) const __b = (b); \
	__a > __b ? __a : __b; \
})

#ifdef NDEBUG
#define assertf(x, fmt, ...) (void)0
#define assert_zeroed(buf, type) (void)0
#else
#define assertf(x, fmt, ...) do { \
	if(0 == (x)) { \
		fprintf(stderr, "%s:%d %s: assertion '%s' failed\n", \
			__FILE__, __LINE__, __PRETTY_FUNCTION__, #x); \
		fprintf(stderr, fmt, ##__VA_ARGS__); \
		fprintf(stderr, "\n"); \
		abort(); \
	} \
} while(0)
#define assert_zeroed(ptr, count) do { \
	void const *const __p = (ptr); \
	size_t const __c = (count); \
	for(size_t __i = 0; __i < sizeof(*(ptr)) * __c; __i++) { \
		if(0 == ((unsigned char const *)__p)[__i]) continue; \
		fprintf(stderr, "%s:%d Buffer at %p not zeroed (%zu of %zu * %zu)\n", \
			__FILE__, __LINE__, __p, __i, sizeof(*(ptr)), __c); \
		abort(); \
	} \
} while(0)
#endif

#define HERE() fprintf(stderr, "%s:%d\n", __FILE__, __LINE__)

#define UNUSED(x) ((void)(x))

#define FREE(ptrptr) do { \
	__typeof__(ptrptr) const __x = (ptrptr); \
	free(*__x); *__x = NULL; \
} while(0)

// Compares nul-terminated string `a` with substring of `blen` at `b`.
static bool substr(strarg_t const a, strarg_t const b, size_t const blen) {
	size_t i = 0;
	for(; i < blen; ++i) {
		if(a[i] != b[i]) return false;
		if(!a[i]) return false; // Terminated early.
	}
	if(a[i]) return false; // Terminated late.
	return true;
}
// Returns strlen(a) if `b` starts with `a`, otherwise 0.
static size_t prefix(strarg_t const a, strarg_t const b) {
	for(size_t i = 0; ; ++i) {
		if(!a[i]) return i;
		if(a[i] != b[i]) return 0;
	}
}


// TODO: Hex should be nul-terminated
// TODO: Clean these up
static uint8_t hexchar(char const c) {
	if(c >= '0' && c <= '9') return c - '0';
	if(c >= 'a' && c <= 'f') return c - 'a' + 10;
	if(c >= 'A' && c <= 'F') return c - 'A' + 10;
//	assert(!"hex char"); // TODO
	return 0;
}
static void tobin(uint8_t *const bin, char const *const hex, size_t const hexlen) {
//	assert(0 == hexlen % 2); // TODO
	for(size_t i = 0; i < hexlen/2; i++) {
		bin[i] = hexchar(hex[i*2+0]) << 4 | hexchar(hex[i*2+1]) << 0;
	}
}
static void tohex(char *const hex, uint8_t const *const bin, size_t const binlen) {
	char const *const map = "0123456789abcdef";
	for(size_t i = 0; i < binlen; i++) {
		hex[i*2+0] = map[0xf & (bin[i] >> 4)];
		hex[i*2+1] = map[0xf & (bin[i] >> 0)];
	}
}

static str_t *tohexstr(byte_t const *const buf, size_t const len) {
	str_t *const hex = malloc(len*2+1);
	if(!hex) return NULL;
	tohex(hex, buf, len);
	hex[len*2] = '\0';
	return hex;
}

#endif

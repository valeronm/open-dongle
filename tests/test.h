#ifndef TEST_H
#define TEST_H

#include <stdio.h>
#include <string.h>

static int test_failures;

#define CHECK(cond)                                                                                \
	do {                                                                                       \
		if (!(cond)) {                                                                     \
			printf("%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);            \
			test_failures++;                                                           \
		}                                                                                  \
	} while (0)

#define CHECK_EQ(a, b)                                                                             \
	do {                                                                                       \
		long long a_ = (long long)(a), b_ = (long long)(b);                                \
		if (a_ != b_) {                                                                    \
			printf("%s:%d: %s == %s failed (%lld != %lld)\n", __FILE__, __LINE__, #a, #b,  \
			       a_, b_);                                                            \
			test_failures++;                                                           \
		}                                                                                  \
	} while (0)

#define CHECK_MEM(a, b, n) CHECK(memcmp((a), (b), (n)) == 0)

#define RUN(fn)                                                                                    \
	do {                                                                                       \
		int failures_before = test_failures;                                               \
		fn();                                                                              \
		printf("%s %s\n", test_failures == failures_before ? "ok  " : "FAIL", #fn);        \
	} while (0)

#define TEST_EXIT() (test_failures == 0 ? 0 : 1)

#endif

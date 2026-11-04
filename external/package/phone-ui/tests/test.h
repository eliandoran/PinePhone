/*
 * Unit tests on the host (`just test`). A test file includes this once and has
 * TEST(name) { CHECK(...); } functions; they register themselves and main()
 * runs each in its own process (forked): the static state of the code under test
 * starts fresh, and a crash fails that test only.
 *
 * ./test_x [filter]: only the tests whose name contains `filter`.
 *
 * Checks report and go on; the test fails if any did. A file can define
 * test_setup() (TEST_SETUP), run in each test's process before it.
 */

#ifndef PHONE_UI_TEST_H
#define PHONE_UI_TEST_H

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define TEST_MAX 256

static struct {
	const char *name;
	void (*fn)(void);
} test_list[TEST_MAX];
static unsigned test_count;
static unsigned test_failures; /* checks failed in this test */
static void (*test_setup_fn)(void);

#ifdef TEST_COVERAGE
void __gcov_dump(void);
#endif

#define TEST(name)                                                                \
	static void test_##name(void);                                            \
	__attribute__((constructor)) static void test_register_##name(void)       \
	{                                                                         \
		if (test_count < TEST_MAX)                                        \
			test_list[test_count++] = (typeof(test_list[0])){ #name, test_##name }; \
	}                                                                         \
	static void test_##name(void)

#define TEST_SETUP                                                                \
	static void test_setup(void);                                             \
	__attribute__((constructor)) static void test_register_setup(void)        \
	{                                                                         \
		test_setup_fn = test_setup;                                       \
	}                                                                         \
	static void test_setup(void)

#define TEST_FAIL(...)                                                            \
	do {                                                                      \
		fprintf(stderr, "  %s:%d: ", __FILE__, __LINE__);                 \
		fprintf(stderr, __VA_ARGS__);                                     \
		fputc('\n', stderr);                                              \
		test_failures++;                                                  \
	} while (0)

#define CHECK(cond)                                                               \
	do {                                                                      \
		if (!(cond))                                                      \
			TEST_FAIL("CHECK(%s)", #cond);                            \
	} while (0)

#define CHECK_INT(a, b)                                                           \
	do {                                                                      \
		long long a_ = (a), b_ = (b);                                     \
		if (a_ != b_)                                                     \
			TEST_FAIL("%s is %lld, expected %lld", #a, a_, b_);       \
	} while (0)

#define CHECK_DBL(a, b)                                                           \
	do {                                                                      \
		double a_ = (a), b_ = (b);                                        \
		if (!(fabs(a_ - b_) <= 1e-9 * fmax(1, fabs(b_))))                 \
			TEST_FAIL("%s is %.17g, expected %.17g", #a, a_, b_);     \
	} while (0)

/* NULL is a value: CHECK_STR(x, NULL) checks that x is NULL */
#define CHECK_STR(a, b)                                                           \
	do {                                                                      \
		const char *a_ = (a), *b_ = (b);                                  \
		if (a_ != b_ && (!a_ || !b_ || strcmp(a_, b_) != 0))             \
			TEST_FAIL("%s is %s%s%s, expected %s%s%s", #a,            \
			          a_ ? "\"" : "", a_ ? a_ : "NULL", a_ ? "\"" : "", \
			          b_ ? "\"" : "", b_ ? b_ : "NULL", b_ ? "\"" : ""); \
	} while (0)

int main(int argc, char **argv)
{
	const char *filter = argc > 1 ? argv[1] : NULL;
	unsigned run = 0, failed = 0;

	for (unsigned i = 0; i < test_count; i++) {
		if (filter && !strstr(test_list[i].name, filter))
			continue;
		run++;
		fflush(NULL);
		pid_t pid = fork();
		if (pid == 0) {
			if (test_setup_fn)
				test_setup_fn();
			test_list[i].fn();
			fflush(NULL);
#ifdef TEST_COVERAGE
			__gcov_dump(); /* _exit() does not write the counts */
#endif
			_exit(test_failures ? EXIT_FAILURE : EXIT_SUCCESS);
		}
		int status;
		waitpid(pid, &status, 0);
		if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
			continue;
		failed++;
		if (WIFSIGNALED(status))
			fprintf(stderr, "FAIL %s (%s)\n", test_list[i].name, strsignal(WTERMSIG(status)));
		else
			fprintf(stderr, "FAIL %s\n", test_list[i].name);
	}
	printf("%s: %u tests, %u failed\n", argv[0], run, failed);
	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}

#endif

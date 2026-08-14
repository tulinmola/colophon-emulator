/*
 * test.h — assertions for the hand-written test suites.
 *
 * Failures report and keep going, so one run finds everything wrong rather
 * than the first thing wrong. Tests are void functions taking no arguments.
 */
#ifndef COLOPHON_TEST_H
#define COLOPHON_TEST_H

#include <stdio.h>

static int test_failures;
static int test_count;
static const char *test_current = "";

#define TEST_CHECK(condition)                                                                      \
  do {                                                                                             \
    if (!(condition)) {                                                                            \
      printf("  %s: %s (line %d)\n", test_current, #condition, __LINE__);                          \
      test_failures++;                                                                             \
    }                                                                                              \
  } while (0)

#define TEST_EQUAL(actual, expected)                                                               \
  do {                                                                                             \
    const long test_got = (long)(actual);                                                          \
    const long test_want = (long)(expected);                                                       \
    if (test_got != test_want) {                                                                   \
      printf("  %s: %s is %ld, expected %ld (line %d)\n", test_current, #actual, test_got,         \
             test_want, __LINE__);                                                                 \
      test_failures++;                                                                             \
    }                                                                                              \
  } while (0)

#define TEST_FAIL(...)                                                                             \
  do {                                                                                             \
    printf("  %s: ", test_current);                                                                \
    printf(__VA_ARGS__);                                                                           \
    printf("\n");                                                                                  \
    test_failures++;                                                                               \
  } while (0)

#define TEST_RUN(test)                                                                             \
  do {                                                                                             \
    test_current = #test;                                                                          \
    test_count++;                                                                                  \
    test();                                                                                        \
  } while (0)

#define TEST_REPORT(suite)                                                                         \
  (printf("%s: %d tests, %d failures\n", suite, test_count, test_failures), test_failures != 0)

#endif

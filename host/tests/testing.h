// Tiny unit-test harness (no external dependencies, works on host only).
#pragma once
#include <stdio.h>
#include <string.h>
#include <vector>

struct TestCase { const char* name; void (*fn)(); };
std::vector<TestCase>& testRegistry();
extern int g_failures;
extern int g_checks;

struct TestReg { TestReg(const char* n, void (*f)()) { testRegistry().push_back({n, f}); } };

#define TEST(name) \
    static void test_##name(); \
    static TestReg reg_##name(#name, test_##name); \
    static void test_##name()

#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failures++; \
    printf("    CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { g_checks++; long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { g_failures++; printf("    CHECK_EQ FAILED %s:%d: %s (%lld) != %s (%lld)\n", \
    __FILE__, __LINE__, #a, _a, #b, _b); } } while (0)
#define CHECK_STR(a, b) do { g_checks++; if (strcmp((a), (b)) != 0) { g_failures++; \
    printf("    CHECK_STR FAILED %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); } } while (0)

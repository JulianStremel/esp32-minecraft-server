#include "testing.h"

std::vector<TestCase>& testRegistry() { static std::vector<TestCase> r; return r; }
int g_failures = 0;
int g_checks = 0;

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int ran = 0, failedTests = 0;
    for (auto& t : testRegistry()) {
        if (filter && !strstr(t.name, filter)) continue;
        int before = g_failures;
        printf("[ RUN  ] %s\n", t.name);
        fflush(stdout);
        t.fn();
        ran++;
        if (g_failures != before) { failedTests++; printf("[ FAIL ] %s\n", t.name); }
        else printf("[  OK  ] %s\n", t.name);
    }
    printf("\n%d tests, %d checks, %d failed tests (%d failed checks)\n", ran, g_checks, failedTests, g_failures);
    return failedTests ? 1 : 0;
}

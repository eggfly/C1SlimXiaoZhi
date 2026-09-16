#include "test_framework.h"

#include <cstdio>

namespace c1xz_test {
namespace {

int g_failures = 0;
bool g_current_failed = false;

}  // namespace

std::vector<Case>& Registry() {
    static std::vector<Case> registry;
    return registry;
}

void Fail(const char* file, int line, const std::string& message) {
    printf("    %s:%d: %s\n", file, line, message.c_str());
    g_current_failed = true;
}

int RunAll() {
    int passed = 0;
    for (auto& test : Registry()) {
        g_current_failed = false;
        test.body();
        if (g_current_failed) {
            printf("[FAIL] %s\n", test.name.c_str());
            ++g_failures;
        } else {
            printf("[ ok ] %s\n", test.name.c_str());
            ++passed;
        }
    }
    printf("\n%d passed, %d failed\n", passed, g_failures);
    return g_failures == 0 ? 0 : 1;
}

}  // namespace c1xz_test

int main() { return c1xz_test::RunAll(); }

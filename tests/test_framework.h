#ifndef C1XZ_TESTS_TEST_FRAMEWORK_H
#define C1XZ_TESTS_TEST_FRAMEWORK_H

// A test harness small enough to not be worth a dependency.

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace c1xz_test {

struct Case {
    std::string name;
    std::function<void()> body;
};

std::vector<Case>& Registry();
void Fail(const char* file, int line, const std::string& message);
int RunAll();

struct Registrar {
    Registrar(const char* name, std::function<void()> body) {
        Registry().push_back({name, std::move(body)});
    }
};

}  // namespace c1xz_test

#define TEST(name)                                                                \
    static void name();                                                           \
    static ::c1xz_test::Registrar registrar_##name(#name, name);                  \
    static void name()

#define CHECK(condition)                                                          \
    do {                                                                          \
        if (!(condition)) {                                                       \
            ::c1xz_test::Fail(__FILE__, __LINE__, "CHECK failed: " #condition);   \
            return;                                                               \
        }                                                                         \
    } while (0)

#define CHECK_EQ(actual, expected)                                                \
    do {                                                                          \
        auto actual_value = (actual);                                             \
        auto expected_value = (expected);                                         \
        if (!(actual_value == expected_value)) {                                  \
            ::c1xz_test::Fail(__FILE__, __LINE__,                                 \
                              std::string("CHECK_EQ failed: " #actual " == " #expected)); \
            return;                                                               \
        }                                                                         \
    } while (0)

#define CHECK_STREQ(actual, expected)                                             \
    do {                                                                          \
        std::string actual_value = (actual);                                      \
        std::string expected_value = (expected);                                  \
        if (actual_value != expected_value) {                                     \
            ::c1xz_test::Fail(__FILE__, __LINE__,                                 \
                              "CHECK_STREQ failed: got \"" + actual_value +       \
                                  "\", want \"" + expected_value + "\"");         \
            return;                                                               \
        }                                                                         \
    } while (0)

#endif  // C1XZ_TESTS_TEST_FRAMEWORK_H

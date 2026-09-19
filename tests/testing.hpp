// Minimal in-tree test framework. No external dependencies, so a fresh clone
// builds and tests without network access.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <exception>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace hgmtest {

class Failure : public std::exception {
 public:
  explicit Failure(std::string message) : message_(std::move(message)) {}
  const char* what() const noexcept override { return message_.c_str(); }

 private:
  std::string message_;
};

struct TestCase {
  std::string suite;
  std::string name;
  std::function<void()> body;
};

std::vector<TestCase>& registry();

struct Registrar {
  Registrar(const char* suite, const char* name, std::function<void()> body);
};

void fail(const std::string& message, const char* file, int line);
void check(bool condition, const char* expression, const char* file, int line);

// Renders a value for failure messages without requiring operator<<.
template <class T>
std::string describe(const T& value) {
  if constexpr (std::is_enum_v<T>) {
    return std::to_string(static_cast<long long>(value));
  } else if constexpr (std::is_same_v<T, bool>) {
    return value ? "true" : "false";
  } else if constexpr (std::is_arithmetic_v<T>) {
    return std::to_string(value);
  } else if constexpr (std::is_convertible_v<const T&, std::string_view>) {
    return std::string(std::string_view(value));
  } else {
    return std::string("<value>");
  }
}

template <class A, class B>
void check_equal(const A& actual, const B& expected, const char* text, const char* file, int line) {
  if (!(actual == expected)) {
    fail(std::string("expected ") + text + " to equal " + describe(expected) + " but got " +
             describe(actual),
         file, line);
  }
}

template <class A, class B>
void check_not_equal(const A& actual, const B& unexpected, const char* text, const char* file,
                     int line) {
  if (actual == unexpected) {
    fail(std::string("expected ") + text + " to differ from " + describe(unexpected), file, line);
  }
}

// Runs every registered test, or only those in the named suite.
int run_all(const std::string& suite_filter);

}  // namespace hgmtest

#define HGM_TEST(suite_name, test_name)                                                     \
  static void suite_name##_##test_name##_body();                                            \
  static const ::hgmtest::Registrar suite_name##_##test_name##_registrar(                   \
      #suite_name, #test_name, &suite_name##_##test_name##_body);                           \
  static void suite_name##_##test_name##_body()

#define HGM_CHECK(expression) ::hgmtest::check((expression), #expression, __FILE__, __LINE__)

#define HGM_CHECK_EQ(actual, expected)   ::hgmtest::check_equal((actual), (expected), #actual, __FILE__, __LINE__)

#define HGM_CHECK_NE(actual, unexpected)   ::hgmtest::check_not_equal((actual), (unexpected), #actual, __FILE__, __LINE__)

#define HGM_FAIL(message) ::hgmtest::fail((message), __FILE__, __LINE__)

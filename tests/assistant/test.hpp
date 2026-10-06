#pragma once
// Tiny unit test framework for the assistant tests.
//
//   TEST(name) { CHECK(cond); CHECK_EQ(a, b); }
//   TEST_LIVE(name) { ... }   // only runs with --live
//
// CHECK/CHECK_EQ report file:line on failure and continue; REQUIRE aborts the current test.
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>


namespace test {


struct TestCase {
	const char* name;
	void (*fn)();
	bool live;
};


struct State {
	std::vector<TestCase> tests;
	int checks = 0;
	int failedChecks = 0;
	/** Failed checks of the currently running test */
	int currentFailures = 0;
};


inline State& state() {
	static State s;
	return s;
}


struct Registrar {
	Registrar(const char* name, void (*fn)(), bool live) {
		TestCase t = {name, fn, live};
		state().tests.push_back(t);
	}
};


template <typename T>
std::string toString(const T& v) {
	std::ostringstream ss;
	ss << v;
	return ss.str();
}

inline std::string toString(bool v) {
	return v ? "true" : "false";
}

inline std::string toString(const std::string& v) {
	return "\"" + v + "\"";
}

inline std::string toString(const char* v) {
	return std::string("\"") + (v ? v : "(null)") + "\"";
}

inline std::string toString(char* v) {
	return toString((const char*) v);
}


inline void fail(const char* file, int line, const std::string& msg) {
	State& s = state();
	s.failedChecks++;
	s.currentFailures++;
	std::fprintf(stderr, "    FAIL %s:%d: %s\n", file, line, msg.c_str());
}


inline bool check(bool ok, const char* file, int line, const char* expr) {
	state().checks++;
	if (!ok)
		fail(file, line, std::string("CHECK(") + expr + ")");
	return ok;
}


template <typename A, typename B>
bool checkEq(const A& a, const B& b, const char* file, int line, const char* exprA, const char* exprB) {
	state().checks++;
	bool ok = (a == b);
	if (!ok)
		fail(file, line, std::string("CHECK_EQ(") + exprA + ", " + exprB + "): " + toString(a) + " != " + toString(b));
	return ok;
}


/** Runs all registered tests. Returns the process exit code. */
inline int runAll(bool live, const char* filter = NULL) {
	State& s = state();
	int run = 0, passed = 0, skipped = 0;
	std::vector<std::string> failedNames;
	for (const TestCase& t : s.tests) {
		if (filter && !std::strstr(t.name, filter)) {
			skipped++;
			continue;
		}
		if (t.live && !live) {
			skipped++;
			continue;
		}
		std::printf("[ RUN  ] %s\n", t.name);
		std::fflush(stdout);
		s.currentFailures = 0;
		try {
			t.fn();
		}
		catch (std::exception& e) {
			fail(t.name, 0, std::string("unexpected exception: ") + e.what());
		}
		catch (...) {
			fail(t.name, 0, "unexpected unknown exception");
		}
		run++;
		if (s.currentFailures == 0) {
			passed++;
			std::printf("[  OK  ] %s\n", t.name);
		}
		else {
			failedNames.push_back(t.name);
			std::printf("[ FAIL ] %s\n", t.name);
		}
		std::fflush(stdout);
	}
	std::printf("\n%d tests run, %d passed, %d failed, %d skipped; %d checks, %d failed checks\n",
		run, passed, run - passed, skipped, s.checks, s.failedChecks);
	for (const std::string& n : failedNames)
		std::printf("  failed: %s\n", n.c_str());
	return (run - passed) == 0 ? 0 : 1;
}


} // namespace test


#define TEST_IMPL(name, live) \
	static void test_##name(); \
	static test::Registrar registrar_##name(#name, test_##name, live); \
	static void test_##name()

#define TEST(name) TEST_IMPL(name, false)
#define TEST_LIVE(name) TEST_IMPL(name, true)

#define CHECK(cond) test::check(!!(cond), __FILE__, __LINE__, #cond)
#define CHECK_EQ(a, b) test::checkEq((a), (b), __FILE__, __LINE__, #a, #b)
/** Aborts the current test (void functions only) if the condition is false. */
#define REQUIRE(cond) do { if (!test::check(!!(cond), __FILE__, __LINE__, #cond)) return; } while (0)

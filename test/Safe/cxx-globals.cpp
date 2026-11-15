// RFC 0046 §4 (D2): a variable with static storage that is not
// `thread_local` is immutable or of shareable type (`thread-share`); one of
// shareable type is used in place.
// RUN: %weavec_cxx -std=c++20 -fsyntax-only -fweavec-safe -w -ferror-limit=0 -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

static std::vector<std::string> Log; // expected-error {{every thread can reach the global 'Log', and 'std::vector<std::string>' is not safe to share [weavec-safe:thread-share]}}
static int nextId; // expected-error {{every thread can reach the global 'nextId', and 'int' is not safe to share [weavec-safe:thread-share]}}
const std::vector<int> Primes{2, 3, 5};
constexpr int Limit = 10;
const char *const Labels[] = {"a", "b"};
const std::string_view Name = "weavec";
static std::atomic<int> Count{0};
thread_local std::vector<int> Scratch;
static std::mutex Mu;

class Registry {
  std::mutex mu_;
  std::map<int, std::string> names_;

public:
  void add(int key, std::string value) {
    const std::lock_guard lock(mu_);
    names_[key] = std::move(value);
  }
};
Registry Names;

struct Config {
  static int instances;
  static const int max = 3;
};
int Config::instances = 0; // expected-error {{every thread can reach the global 'instances', and 'int' is not safe to share [weavec-safe:thread-share]}}

void addName() { Names.add(1, "x"); }
int next() { return ++Count + Limit + Primes[0]; }
void scratch() { Scratch.push_back(1); } // expected-error {{'Scratch' is a global, which every function can reach, and 'push_back' may change what it owns [weavec-safe:owner-path]}}
void scratchInPlace() {
  std::vector<int> local = std::move(Scratch);
  local.push_back(1);
  Scratch = std::move(local);
}
extern int elsewhere;
int *const toElsewhere = &elsewhere; // expected-error {{every thread can reach the global 'elsewhere', and 'int' is not safe to share [weavec-safe:thread-share]}}
extern const int limit;
const int *const toLimit = &limit;
struct Hits {
  mutable int hits = 0;
};
struct Counted : Hits {};
const Counted Counts{}; // expected-error {{every thread can reach the global 'Counts', and 'Counted' is not safe to share [weavec-safe:thread-share]}}
int counter() {
  static int calls = 0; // expected-error {{every thread can reach the global 'calls', and 'int' is not safe to share [weavec-safe:thread-share]}}
  return ++calls;
}
int table(int i) {
  static const int values[] = {1, 2, 3};
  return values[i % 3];
}

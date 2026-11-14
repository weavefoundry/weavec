// RFC 0046 §2, §3: what a thread start gives the thread by value is
// sendable (`thread-send`), with accepted and rejected forms.
// RUN: %weavec_cxx -std=c++20 -fsyntax-only -fweavec-safe -w -ferror-limit=0 -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
#include <atomic>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct State {
  std::map<int, std::string> names;
};
struct Stats {
  std::atomic<long> hits{0};
  const std::string name;
};
class Counter {
  mutable std::mutex mu_;
  int value_ = 0;

public:
  void add(int n) {
    const std::lock_guard lock(mu_);
    value_ += n;
  }
};
struct Job {
  std::string input;
  std::vector<int> data;
};
struct Node {
  std::unique_ptr<Node> next;
  int value = 0;
};
struct Peek {
  int *slot;
};

void sharedMutable() {
  auto state = std::make_shared<State>();
  std::jthread a([state] { state->names[1] = "a"; }); // expected-error {{'state' is copied into a thread, and 'State' can be changed by two threads at once [weavec-safe:thread-send]}}
}
void sharedConst() {
  auto state = std::make_shared<const State>();
  std::jthread a([state] { (void)state->names.size(); });
}
void sharedLocked() {
  auto counter = std::make_shared<Counter>();
  std::jthread a([counter] { counter->add(1); });
  auto stats = std::make_shared<Stats>();
  std::jthread b([stats] { ++stats->hits; });
}
void ownedInput() {
  Job job{.input = "x", .data = {1, 2}};
  std::jthread a([job = std::move(job)] { (void)job.data.size(); });
  auto node = std::make_unique<Node>();
  std::jthread b([node = std::move(node)] { (void)node->value; });
}
void futures() {
  auto f = std::async(
      std::launch::async, [](std::vector<int> v) { return v.size(); },
      std::vector<int>{1, 2});
  std::future<std::size_t> g = std::async([] { return std::size_t{3}; });
  std::jthread t([g = std::move(g)]() mutable { (void)g.get(); });
  (void)f.get();
}
void rawPointerMember(int &x) {
  Peek peek{&x};
  std::jthread a([peek] { ++*peek.slot; }); // expected-error {{'peek' is copied into a thread, and 'Peek' can be changed by two threads at once [weavec-safe:thread-send]}}
}
void erasedCallable(std::function<void()> task) {
  std::jthread a(task); // expected-error {{'task' is a 'std::function<void ()>', which does not say what it captured, and is handed to a thread [weavec-safe:thread-send]}}
}
void copiedArgument() {
  std::vector<std::string> names{"a"};
  std::thread t([](const std::vector<std::string> &v) { (void)v.size(); },
                names);
  t.join();
}
void sharedArgument() {
  auto state = std::make_shared<State>();
  std::thread t([](const std::shared_ptr<State> &s) { (void)s; }, state); // expected-error {{'state' is copied into a thread, and 'State' can be changed by two threads at once [weavec-safe:thread-send]}}
  t.join();
}
void plainFunction(int n);
void functionPointer() {
  std::jthread a(plainFunction, 3);
  std::jthread b(+[] {});
}
class Holder {
  std::vector<int> items_;

public:
  void copyOfThis() {
    std::jthread a([*this] { (void)items_.size(); });
  }
};

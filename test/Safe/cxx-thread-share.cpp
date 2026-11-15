// RFC 0046 §2, §3, §9: what a thread start gives the thread by reference is
// shareable, immutable, or a lent local (`thread-share`); no
// `std::shared_ptr<const T>` is made from a `std::shared_ptr<T>`; no
// function that keeps state of its own is called.
// RUN: %weavec_cxx -std=c++20 %cxx_threads -fsyntax-only -fweavec-safe -w -ferror-limit=0 -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
#include <atomic>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

struct Plain {
  int count = 0;
  std::vector<int> items;
};
class Locked {
  std::mutex mu_;
  std::vector<int> items_;

public:
  void add(int v) {
    const std::lock_guard lock(mu_);
    items_.push_back(v);
  }
};
class Worker {
  std::vector<std::string> names_;

public:
  void start() {
    std::jthread t([this] { names_.push_back("x"); }); // expected-error {{'this' is shared with a thread, and 'Worker' is not safe to share [weavec-safe:thread-share]}}
  }
};
class Tally {
  std::atomic<int> hits_{0};
  const std::string name_ = "t";

public:
  void start() {
    std::jthread t([this] { ++hits_; });
  }
};
void readParameter(const std::vector<int> &data) {
  std::jthread t([&data] { (void)data.size(); }); // expected-error {{'data' is shared with a thread, and 'std::vector<int>' is not safe to share [weavec-safe:thread-share]}}
}
void shareLocked(Locked &locked) {
  std::jthread t([&locked] { locked.add(1); });
}
void atomicLocal() {
  std::atomic<int> n{0};
  std::jthread a([&n] { ++n; });
  std::jthread b([&n] { ++n; });
}
void constLocal() {
  const std::vector<int> data{1, 2, 3};
  std::jthread a([&data] { (void)data.size(); });
  std::jthread b([&data] { (void)data[0]; });
}
void pointerFromMemory(Plain **slot) {
  std::jthread a([p = *slot] { ++p->count; }); // expected-error {{'p' is shared with a thread, and 'Plain' is not safe to share [weavec-safe:thread-share]}}
}
void viewOfParameter(std::string_view text) {
  std::jthread a([text] { (void)text.size(); }); // expected-error {{'text' is shared with a thread, and 'std::string_view' is not safe to share [weavec-safe:thread-share]}}
}
void snapshots() {
  auto config = std::make_shared<Plain>();
  std::shared_ptr<const Plain> snapshot = config; // expected-error {{a 'std::shared_ptr<const Plain>' is made from one that can still change the object [weavec-safe:thread-share]}}
  auto frozen = std::make_shared<const Plain>();
  std::shared_ptr<const Plain> again = frozen;
  std::jthread t([again] { (void)again->items.size(); });
}
void stateOfItsOwn() {
  std::time_t now = 0;
  (void)std::localtime(&now); // expected-error {{'localtime' keeps state of its own that every thread shares [weavec-safe:thread-share]}}
  std::tm parts{};
  (void)localtime_r(&now, &parts);
  (void)std::rand(); // expected-error {{'rand' keeps state of its own that every thread shares [weavec-safe:thread-share]}}
}

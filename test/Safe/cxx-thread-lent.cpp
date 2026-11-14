// RFC 0046 §5: a local lent to a thread is not used against the loan while
// the thread object holds it (`thread-lent`); a thread object ends before
// what it borrowed (`stale-borrow`) and is not detached with a borrow
// (`escape`).
// RUN: %weavec_cxx -std=c++20 -fsyntax-only -fweavec-safe -w -ferror-limit=0 -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
#include <atomic>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

void lentTwice() {
  std::vector<int> v;
  std::jthread a([&] { v.push_back(1); });
  std::jthread b([&] { v.push_back(2); }); // expected-error {{'v' is used while the thread 'a' may be changing it [weavec-safe:thread-lent]}}
}
void usedWhileLent() {
  std::vector<int> v;
  std::jthread t([&v] { v.push_back(1); });
  v.push_back(2); // expected-error {{'v' is used while the thread 't' may be changing it [weavec-safe:thread-lent]}}
}
void readers() {
  std::vector<int> v{1, 2};
  {
    std::jthread a([&v] { (void)v.size(); });
    std::jthread b([&v] {
      for (const int x : v)
        (void)x;
    });
    (void)v.front();
  }
  v.push_back(3);
}
void writtenWhileRead() {
  std::vector<int> v{1};
  std::jthread a([&v] { (void)v.size(); });
  v.push_back(2); // expected-error {{'v' is changed while the thread 'a' reads it [weavec-safe:thread-lent]}}
}
int joined() {
  int total = 0;
  std::thread t([&total] { total = 42; });
  t.join();
  return total + 1;
}
std::size_t scopedWorker(const std::vector<std::string> &lines) {
  std::size_t total = 0;
  {
    std::jthread worker([&total, &lines] { // expected-error {{'lines' is shared with a thread, and 'std::vector<std::string>' is not safe to share [weavec-safe:thread-share]}}
      for (const std::string &line : lines)
        total += line.size();
    });
  }
  return total;
}
std::size_t ownWorker() {
  std::vector<std::string> lines{"a", "bb"};
  std::size_t total = 0;
  {
    std::jthread worker([&total, &lines] {
      for (const std::string &line : lines)
        total += line.size();
    });
  }
  return total + lines.size();
}
// Each iteration lends the element at its own index.
int slots() {
  std::vector<int> out(4);
  std::vector<std::jthread> pool;
  for (int i = 0; i < 4; ++i)
    pool.emplace_back([&out, i] { out[i] = i * i; });
  for (auto &worker : pool)
    worker.join();
  return out[3];
}
void slotsRead() {
  std::vector<int> out(4);
  std::vector<std::jthread> pool;
  for (int i = 0; i < 4; ++i)
    pool.emplace_back([&out, i] { out[i] = i; });
  (void)out.size(); // expected-error {{'out' is used while the thread 'pool' may be changing it [weavec-safe:thread-lent]}}
}
void slotsWholeVector() {
  std::vector<int> out(4);
  std::vector<std::jthread> pool;
  for (int i = 0; i < 4; ++i)
    pool.emplace_back([&out, i] { out[i] = static_cast<int>(out.size()); }); // expected-error {{'out' is used while the thread 'pool' may be changing it [weavec-safe:thread-lent]}}
}
void slotsTwoStarts() {
  std::vector<int> out(4);
  std::vector<std::jthread> pool;
  for (int i = 0; i < 4; ++i) {
    pool.emplace_back([&out, i] { out[i] = 1; }); // expected-error {{'out' is used while the thread 'pool' may be changing it [weavec-safe:thread-lent]}}
    pool.emplace_back([&out, i] { out[i] = 2; }); // expected-error {{'out' is used while the thread 'pool' may be changing it [weavec-safe:thread-lent]}}
  }
}
void slotsStepped() {
  std::vector<int> out(8);
  std::vector<std::jthread> pool;
  for (int i = 0; i < 4; ++i) {
    pool.emplace_back([&out, i] { out[i] = 1; }); // expected-error {{'out' is used while the thread 'pool' may be changing it [weavec-safe:thread-lent]}}
    i = i % 2;
  }
}
void slotsInner() {
  std::vector<int> out(4);
  std::vector<std::jthread> pool;
  for (int i = 0; i < 4; ++i)
    for (int k = 0; k < 2; ++k)
      pool.emplace_back([&out, i] { out[i] += 1; }); // expected-error {{'out' is used while the thread 'pool' may be changing it [weavec-safe:thread-lent]}}
}
void slotsOfBits() {
  std::vector<bool> marks(4);
  std::vector<std::jthread> pool;
  for (int i = 0; i < 4; ++i)
    pool.emplace_back([&marks, i] { marks[i] = true; }); // expected-error {{'marks' is used while the thread 'pool' may be changing it [weavec-safe:thread-lent]}}
}
int resultsByFuture() {
  std::vector<std::future<int>> parts;
  for (int i = 0; i < 4; ++i)
    parts.push_back(std::async(std::launch::async, [i] { return i * i; }));
  int sum = 0;
  for (auto &part : parts)
    sum += part.get();
  return sum;
}
void asyncLent() {
  std::vector<int> data{1, 2, 3};
  auto f = std::async(std::launch::async, [&data] { data.push_back(4); });
  data.push_back(5); // expected-error {{'data' is used while the thread 'f' may be changing it [weavec-safe:thread-lent]}}
}
void asyncWaited() {
  std::vector<int> data{1, 2, 3};
  auto f = std::async(std::launch::async, [&data] { data.push_back(4); });
  f.get();
  data.push_back(5);
}
void throughReference() {
  std::vector<int> v;
  auto &r = v;
  std::jthread t([&v] { v.push_back(1); });
  r.push_back(2); // expected-error {{'v' is used while the thread 't' may be changing it [weavec-safe:thread-lent]}}
}
void throughRef() {
  std::vector<int> v;
  std::thread t([](std::vector<int> &w) { w.push_back(1); }, std::ref(v));
  v.push_back(2); // expected-error {{'v' is used while the thread 't' may be changing it [weavec-safe:thread-lent]}}
  t.join(); // expected-error {{'t' points into 'v', which 'push_back' may have changed [weavec-safe:stale-borrow]}}
}
void outlives() {
  std::jthread t;
  {
    std::string message = "hi";
    t = std::jthread([&message] { (void)message.size(); });
  }
} // expected-error {{the thread 't' is still running when 'message', which it uses, ends [weavec-safe:stale-borrow]}}
void detached() {
  std::string message = "x";
  std::thread t([&message] { (void)message.size(); });
  t.detach(); // expected-error {{the thread 't' is detached while it holds a reference to 'message' [weavec-safe:escape]}}
}
void detachedByValue() {
  std::string message = "x";
  std::thread t([message] { (void)message.size(); });
  t.detach();
}

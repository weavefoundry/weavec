// RFC 0046 §6: a locked member is used only under a guard on its lock, and
// locks are taken by guard objects (`lock-held`); a borrow of a locked
// member ends with its guard (`stale-borrow`) and does not leave it
// (`escape`).
// RUN: %weavec_cxx -std=c++20 -fsyntax-only -fweavec-safe -w -ferror-limit=0 -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <utility>

class Queue {
  std::mutex mu_;
  std::condition_variable ready_;
  std::deque<std::string> items_;
  bool closed_ = false;

public:
  Queue() { items_.clear(); }
  void push(std::string s) {
    {
      const std::lock_guard lock(mu_);
      items_.push_back(std::move(s));
    }
    ready_.notify_one();
  }
  std::optional<std::string> pop() {
    std::unique_lock lock(mu_);
    ready_.wait(lock, [this] { return closed_ || !items_.empty(); });
    if (items_.empty())
      return std::nullopt;
    std::string s = std::move(items_.front());
    items_.pop_front();
    return s;
  }
  void close() {
    {
      const std::lock_guard lock(mu_);
      closed_ = true;
    }
    ready_.notify_all();
  }
  std::size_t drain() {
    const std::size_t n = items_.size(); // expected-error {{'items_' is used without a lock on 'mu_' [weavec-safe:lock-held]}}
    items_.clear(); // expected-error {{'items_' is used without a lock on 'mu_' [weavec-safe:lock-held]}}
    return n;
  }
  const std::string &front() {
    const std::lock_guard lock(mu_);
    return items_.front(); // expected-error {{a reference into 'items_' is returned past the lock that guards it [weavec-safe:escape]}}
  }
  std::string firstCopy() {
    const std::lock_guard lock(mu_);
    return items_.front();
  }
  void byHand() {
    mu_.lock(); // expected-error {{'lock' takes or releases a lock by hand [weavec-safe:lock-held]}}
    mu_.unlock(); // expected-error {{'unlock' takes or releases a lock by hand [weavec-safe:lock-held]}}
  }
  std::size_t staleAcrossWait() {
    std::unique_lock lock(mu_);
    const std::string &first = items_.front();
    ready_.wait(lock);
    return first.size(); // expected-error {{'first' was taken under the lock 'lock', which was released here [weavec-safe:stale-borrow]}}
  }
  std::size_t released() {
    std::unique_lock lock(mu_);
    const std::string &first = items_.front();
    lock.unlock();
    return first.size(); // expected-error {{'first' was taken under the lock 'lock', which was released here [weavec-safe:stale-borrow]}}
  }
  void deferred() {
    std::unique_lock lock(mu_, std::defer_lock); // expected-error {{a guard constructed with a tag or from another guard is not followed by the rules [weavec-safe:not-modeled]}}
  }
  void otherQueue(Queue &other) {
    const std::lock_guard lock(other.mu_);
    other.items_.clear();
  }
  void wrongLock(Queue &other) {
    const std::lock_guard lock(mu_);
    other.items_.clear(); // expected-error {{'items_' is used without a lock on 'mu_' [weavec-safe:lock-held]}}
  }
  void both(Queue &other) {
    const std::scoped_lock locks(mu_, other.mu_);
    items_.swap(other.items_);
  }
  void helper(std::unique_lock<std::mutex> &lock) {
    items_.clear();
    (void)lock;
  }
  void callsHelper() {
    std::unique_lock lock(mu_);
    helper(lock);
  }
  void helperOnAnother(Queue &other) {
    std::unique_lock lock(mu_);
    other.helper(lock); // expected-error {{the guard passed to 'helper' is not held on the lock of the object it is called on [weavec-safe:lock-held]}}
  }
};

class Table {
  mutable std::shared_mutex mu_;
  std::map<int, std::string> rows_;

public:
  std::size_t size() const {
    const std::shared_lock lock(mu_);
    return rows_.size();
  }
  void put(int key) {
    const std::shared_lock lock(mu_);
    rows_[key] = "x"; // expected-error {{'rows_' is changed under a shared lock on 'mu_', which other readers hold too [weavec-safe:lock-held]}}
  }
  void putExclusive(int key) {
    const std::unique_lock lock(mu_);
    rows_[key] = "x";
  }
};

template <class T> class Locked {
  mutable std::mutex mu_;
  T value_;

public:
  explicit Locked(T value) : value_(std::move(value)) {}
  template <class F> auto with(F f) {
    const std::lock_guard lock(mu_);
    return f(value_);
  }
};
std::size_t useLocked(Locked<std::string> &name) {
  name.with([](std::string &s) { s += "x"; return 0; });
  return name.with([](const std::string &s) { return s.size(); });
}

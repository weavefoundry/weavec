// RFC 0046 §7: a class that owns the thread that uses `this` is shareable,
// `final`, neither copyable nor movable, and declares the thread member
// last (`escape`, `thread-share`).
// RUN: %weavec_cxx -std=c++20 -fsyntax-only -fweavec-safe -w -ferror-limit=0 -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class Good final {
  std::mutex mu_;
  std::vector<std::string> names_;
  std::jthread thread_;

public:
  Good() : thread_([this] { run(); }) {}
  Good(const Good &) = delete;
  Good &operator=(const Good &) = delete;
  ~Good() { thread_.request_stop(); }
  void run() {
    const std::lock_guard lock(mu_);
    names_.push_back("x");
  }
  void stop() {
    const std::lock_guard lock(mu_);
    thread_.request_stop();
  }
  void stopUnlocked() {
    thread_.request_stop(); // expected-error {{'thread_' is used without a lock on 'mu_' [weavec-safe:lock-held]}}
  }
};

class Early final {
  std::mutex mu_;
  std::jthread thread_;
  std::vector<std::string> names_;

public:
  Early() : thread_([this] { run(); }) {} // expected-error {{the thread in 'thread_' uses 'this', and 'names_' is destroyed before it is joined [weavec-safe:escape]}}
  Early(const Early &) = delete;
  Early &operator=(const Early &) = delete;
  void run() {
    const std::lock_guard lock(mu_);
    names_.push_back("x");
  }
};

class Open {
  std::atomic<int> hits_{0};
  std::jthread thread_;

public:
  Open() : thread_([this] { tick(); }) {} // expected-error {{the thread in 'thread_' uses 'this', and a class derived from 'Open' would destroy its members before it is joined [weavec-safe:escape]}}
  Open(const Open &) = delete;
  Open &operator=(const Open &) = delete;
  virtual ~Open() = default;
  virtual void tick() { ++hits_; }
};

// With no virtual function the thread reaches only this class's members.
class Plain {
  std::atomic<int> hits_{0};
  std::jthread thread_;

public:
  Plain() : thread_([this] { ++hits_; }) {}
  Plain(const Plain &) = delete;
  Plain &operator=(const Plain &) = delete;
};

class Movable final {
  std::atomic<int> *hits_ = nullptr;
  std::jthread thread_;

public:
  Movable() : thread_([this] { (void)hits_; }) {} // expected-error {{the thread in 'thread_' uses 'this', and 'Movable' can be copied or moved away from it [weavec-safe:escape]}}
};

class Unlocked final {
  std::vector<std::string> names_;
  std::jthread thread_;

public:
  Unlocked() : thread_([this] { names_.push_back("x"); }) {} // expected-error {{'this' is shared with the thread in 'thread_', and 'Unlocked' is not safe to share [weavec-safe:thread-share]}}
  Unlocked(const Unlocked &) = delete;
  Unlocked &operator=(const Unlocked &) = delete;
};

class Shared : public std::enable_shared_from_this<Shared> {
  std::atomic<int> hits_{0};

public:
  std::jthread start() {
    return std::jthread([self = shared_from_this()] { ++self->hits_; });
  }
};

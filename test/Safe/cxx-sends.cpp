// RFC 0046 §8: WEAVEC_SENDS on a parameter that hands a type-erased callable
// to another thread (`thread-send`, `bad-annotation`), and a pool of
// worker threads built on it.
// RUN: %weavec_cxx -std=c++20 %cxx_threads -fsyntax-only -fweavec-safe -w -ferror-limit=0 -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>
#include <vector>
#include <weavec.h>

class Pool final {
  std::mutex mu_;
  std::condition_variable ready_;
  std::queue<std::function<void()>> tasks_;
  bool stop_ = false;
  std::vector<std::jthread> workers_;

  void run() {
    for (;;) {
      std::function<void()> task;
      {
        std::unique_lock lock(mu_);
        ready_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
        if (stop_ && tasks_.empty())
          return;
        task = std::move(tasks_.front());
        tasks_.pop();
      }
      task();
    }
  }

public:
  explicit Pool(int n) {
    for (int i = 0; i < n; ++i)
      workers_.emplace_back([this] { run(); });
  }
  Pool(const Pool &) = delete;
  Pool &operator=(const Pool &) = delete;
  ~Pool() {
    {
      const std::lock_guard lock(mu_);
      stop_ = true;
    }
    ready_.notify_all();
  }
  void submit(std::function<void()> WEAVEC_SENDS task) {
    {
      const std::lock_guard lock(mu_);
      tasks_.push(std::move(task));
    }
    ready_.notify_one();
  }
};

void use(Pool &pool) {
  int local = 0;
  pool.submit([local] { (void)local; });
  pool.submit([&local] { ++local; }); // expected-error {{'local' is captured by reference by a callable handed to a WEAVEC_SENDS parameter, which may run it on another thread after this function returns [weavec-safe:thread-send]}} // expected-error {{a borrow of 'local' is stored in what 'submit' keeps, which outlives it [weavec-safe:escape]}}
  std::vector<int> big{1, 2};
  pool.submit([big = std::move(big)] { (void)big.size(); });
  const std::function<void()> stored = [local] { (void)local; };
  pool.submit(stored);
}
void relay(Pool &pool, std::function<void()> task) {
  pool.submit(std::move(task)); // expected-error {{'std::move(task)' is handed to a WEAVEC_SENDS parameter, and what it captured is not in its type [weavec-safe:thread-send]}}
}
void forward(Pool &pool, std::function<void()> WEAVEC_SENDS task) {
  pool.submit(std::move(task));
}
void start(std::function<void()> WEAVEC_SENDS task) {
  std::jthread t(std::move(task));
}
void wrongPlace(int WEAVEC_SENDS n) { (void)n; } // expected-error {{WEAVEC_SENDS is on a parameter whose type is not a callable that hides what it captured [weavec-safe:bad-annotation]}}

class Unannotated {
  std::mutex mu_;
  std::vector<std::function<void()>> tasks_;

public:
  void add(std::function<void()> task) {
    const std::lock_guard lock(mu_);
    tasks_.push_back(std::move(task)); // expected-error {{'std::move(task)' is stored in 'this->tasks_', which another thread may run, and what it captured is not in its type [weavec-safe:thread-send]}}
  }
};

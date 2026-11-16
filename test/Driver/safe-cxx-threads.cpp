// RFC 0046 §12: the ledger of a safe C++ unit records, for each class it
// defines, whether the class is sendable and shareable and which members
// are its lock, its locked members and its thread members; for each
// function, the thread starts it makes and whether it uses locks or a
// WEAVEC_SENDS parameter. The summary line counts locked classes and thread
// starts; the audit lists what (Z1) to (Z6) rest on. Nothing at run time
// changes (G6): the object is the same with the rules off.
// RUN: rm -rf %t && mkdir -p %t/ledger
// RUN: %weavec_cxx -std=c++20 %cxx_threads -O2 -fweavec-safe -fweavec-summary -fweavec-ledger=%t/ledger/ -c %s -o %t/on.o 2>&1 | FileCheck %s --check-prefix=SUMMARY
// RUN: %weavec_cxx -std=c++20 %cxx_threads -O2 -fweavec-safe -mllvm -weavec-thread-rules=0 -c %s -o %t/off.o
// RUN: cmp %t/on.o %t/off.o
// RUN: FileCheck %s --check-prefix=LEDGER < %t/ledger/on.o.ledger.json
// RUN: %weavec_cc --weavec-audit %t/ledger | FileCheck %s --check-prefix=AUDIT
// RUN: %weavec_cc --weavec-audit --format=json %t/ledger | FileCheck %s --check-prefix=JSON
// RUN: %weavec_cxx -std=c++20 %cxx_threads -O2 -fweavec-safe %s -o %t/program
// RUN: %t/program
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <weavec.h>

class Log final {
  std::mutex mu_;
  std::vector<std::string> lines_;
  std::jthread flusher_;

public:
  Log() : flusher_([this] { add("start"); }) {}
  Log(const Log &) = delete;
  Log &operator=(const Log &) = delete;
  void add(std::string line) {
    const std::lock_guard lock(mu_);
    lines_.push_back(std::move(line));
  }
  std::size_t size() {
    const std::lock_guard lock(mu_);
    return lines_.size();
  }
};

struct Plain {
  int count = 0;
};

// Code that is not safe: what safe code trusts.
WEAVEC_GUARDED void spawn(std::function<void()> WEAVEC_SENDS task) {
  std::thread(std::move(task)).detach();
}
WEAVEC_GUARDED int counter = 0;

int total() {
  std::vector<int> parts(4);
  {
    std::vector<std::jthread> workers;
    workers.reserve(4);
    for (int i = 0; i < 4; ++i)
      workers.emplace_back([&parts, i] { parts[i] = i; });
  }
  int sum = 0;
  for (const int part : parts)
    sum += part;
  return sum;
}

int main() {
  Log log;
  log.add("x");
  return total() == 6 && log.size() >= 1 && counter == 0 ? 0 : 1;
}

// SUMMARY: weavec: {{.*}}safe-cxx-threads.cpp: {{.*}} functions safe; {{.*}}; 1 locked classes, 3 thread starts

// LEDGER: "fields": [
// LEDGER: "kind": "class"
// LEDGER-NEXT: "line"
// LEDGER-NEXT: "record": "Log"
// LEDGER-NEXT: "sendable": true
// LEDGER-NEXT: "shareable": true
// LEDGER: "field": "mu_"
// LEDGER-NEXT: "file":
// LEDGER-NEXT: "kind": "lock"
// LEDGER: "field": "lines_"
// LEDGER-NEXT: "file":
// LEDGER-NEXT: "kind": "locked"
// LEDGER: "field": "flusher_"
// LEDGER-NEXT: "file":
// LEDGER-NEXT: "kind": "thread"
// LEDGER: "kind": "class"
// LEDGER-NEXT: "line"
// LEDGER-NEXT: "record": "Plain"
// LEDGER-NEXT: "sendable": true
// LEDGER-NEXT: "shareable": false
// LEDGER: "functions": [
// LEDGER: "function": "Log::Log"
// LEDGER-NEXT: "line"
// LEDGER-NEXT: "locks": false
// LEDGER-NEXT: "owns": false
// LEDGER-NEXT: "sends": false
// LEDGER-NEXT: "starts": 1
// LEDGER-NEXT: "tier": "safe"
// LEDGER: "function": "Log::add"
// LEDGER-NEXT: "line"
// LEDGER-NEXT: "locks": true
// LEDGER: "function": "spawn"
// LEDGER-NEXT: "line"
// LEDGER-NEXT: "locks": false
// LEDGER-NEXT: "owns": false
// LEDGER-NEXT: "sends": true
// LEDGER-NEXT: "starts": 1
// LEDGER-NEXT: "tier": "guarded"
// LEDGER: "function": "total"
// LEDGER-NEXT: "line"
// LEDGER-NEXT: "locks": false
// LEDGER-NEXT: "owns": false
// LEDGER-NEXT: "sends": false
// LEDGER-NEXT: "starts": 1
// LEDGER: "version": 11

// AUDIT: functions that are not safe and start threads or use locks
// AUDIT-NEXT: safe-cxx-threads.cpp:{{[0-9]+}}: spawn (guarded)
// AUDIT: WEAVEC_SENDS parameters of functions that are not safe
// AUDIT-NEXT: safe-cxx-threads.cpp:{{[0-9]+}}: spawn (guarded)
// AUDIT: globals of code that is not safe that safe code names
// AUDIT-NEXT: safe-cxx-threads.cpp:{{[0-9]+}}: main.counter
// JSON-DAG: "schema": "weavec-audit"
// JSON-DAG: "version": 7
// JSON-DAG: "threads": [
// JSON-DAG: "sends": [

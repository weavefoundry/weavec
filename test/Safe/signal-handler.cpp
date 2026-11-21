// RFC 0051 §4.3: a safe function does not register code the system runs
// between any two instructions of the program (the table's `handler` rows);
// `signal` with SIG_IGN or SIG_DFL registers nothing.
// RUN: %weavec_cxx -std=c++20 -fsyntax-only -fweavec-safe -w -ferror-limit=0 -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
#include <atomic>
#include <csignal>
#include <weavec.h>

std::atomic<int> stops{0};

void onStop(int) { stops.fetch_add(1); }

void watch() {
  std::signal(SIGINT, onStop); // expected-error {{'signal' registers a handler that runs between any two instructions of the program}}
  struct sigaction action {};
  action.sa_handler = onStop;
  sigaction(SIGTERM, &action, nullptr); // expected-error {{'sigaction' registers a handler}}
}

void quiet() {
  std::signal(SIGPIPE, SIG_IGN);
  std::signal(SIGINT, SIG_DFL);
  struct sigaction old {};
  sigaction(SIGTERM, nullptr, &old);
}

// The accepted form: the handler is registered by code that is not safe.
WEAVEC_GUARDED void install() {
  struct sigaction action {};
  action.sa_handler = onStop;
  sigaction(SIGTERM, &action, nullptr);
}

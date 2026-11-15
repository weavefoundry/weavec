// RFC 0046 §2 (D4): what an owning pointer reached through a `const` path
// holds is used as `const` (`owner-path`); a raw pointer member is not one.
// RUN: %weavec_cxx -std=c++20 -fsyntax-only -fweavec-safe -w -ferror-limit=0 -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
#include <memory>
#include <string>

struct Impl {
  int count = 0;
  std::string name;
};
void touch(Impl &impl);
void see(const Impl &impl);

class Widget {
  std::unique_ptr<Impl> impl_ = std::make_unique<Impl>();
  int *raw_ = nullptr;

public:
  int count() const { return impl_->count; }
  void bump() const { impl_->count++; } // expected-error {{'this->impl_' is 'const' here, and what it points to is written [weavec-safe:owner-path]}}
  void rename() const { impl_->name = "x"; } // expected-error {{'this->impl_' is 'const' here, and what it points to is written [weavec-safe:owner-path]}}
  const std::string &name() const { return impl_->name; }
  void bumpRaw() const { ++*raw_; }
  void reset() { impl_->count = 0; }
  void pass() const { touch(*impl_); } // expected-error {{'this->impl_' is 'const' here, and what it points to is written [weavec-safe:owner-path]}}
  void look() const { see(*impl_); }
};
int throughConstLocal(const std::unique_ptr<Impl> &p) {
  p->count = 1; // expected-error {{'p' is 'const' here, and what it points to is written [weavec-safe:owner-path]}}
  return p->count;
}

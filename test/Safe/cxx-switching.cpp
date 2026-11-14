// RFC 0045 §7: a pointer into storage that changes type in place (an
// alternative of a variant, the characters of a string, which share their
// bytes with its pointer when it grows) is not kept in a pointer member or
// element, where the rules stop following it, unless it points to 'const'
// bytes that hold no pointer. A pointer to the object itself, or to an
// element of a container, may be kept; an optional holds one type.
// RUN: %weavec_cxx -std=c++20 -fsyntax-only -fweavec-safe -w -ferror-limit=0 -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
#include <optional>
#include <string>
#include <variant>
#include <vector>

using Value = std::variant<std::string, std::vector<int>>;

struct Holder {
  std::string *name = nullptr;
  char *chars = nullptr;
  const char *text = nullptr;
  int *number = nullptr;
  std::vector<std::string> *list = nullptr;
  std::string *element = nullptr;
  Value *whole = nullptr;
  std::vector<std::string *> names;

  void keep(Value &v) {
    name = &std::get<std::string>(v); // expected-error {{a pointer into 'v', whose contents change type, is stored in 'this->name' [weavec-safe:escape]}}
  }
  void keepChars(std::string &s) {
    chars = s.data(); // expected-error {{a pointer into 's', whose contents change type, is stored in 'this->chars' [weavec-safe:escape]}}
  }
  void keepText(const std::string &s) { text = s.c_str(); }
  void keepNumber(std::variant<int, std::string> &v) {
    number = &std::get<int>(v); // expected-error {{a pointer into 'v', whose contents change type}}
  }
  void keepOptional(std::optional<std::vector<std::string>> &o) { list = &*o; }
  void keepElement(std::vector<std::string> &v) { element = &v[0]; }
  void keepWhole(Value &v) { whole = &v; }
  void keepInner(std::vector<std::variant<std::string, long>> &v) {
    name = &std::get<std::string>(v[0]); // expected-error {{a pointer into 'v', whose contents change type}}
  }
  void keepInList(Value &v) {
    names.push_back(&std::get<std::string>(v)); // expected-error {{a pointer into 'v', whose contents change type, is stored in 'this->names'}}
  }
  explicit Holder(Value &v) : name(&std::get<std::string>(v)) {} // expected-error {{a pointer into 'v', whose contents change type, is stored in 'name'}}
  void keepIf(Value &v) {
    name = std::get_if<std::string>(&v); // expected-error {{a pointer into 'v', whose contents change type, is stored in 'this->name'}}
  }
  void keepPair(std::pair<std::string, long> &p) {
    number = nullptr;
    long *second = &p.second; // a pair holds both: nothing switches
    (void)second;
  }
};

// What std::get_if returns points into the variant, and ends with it.
long stale(Value &v) {
  std::string *p = std::get_if<std::string>(&v);
  v = std::vector<int>{};
  return p != nullptr ? static_cast<long>(p->size()) : 0; // expected-error {{'p' points into 'v', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
}

// The accepted form: the key, looked up when it is used.
struct Lookup {
  const std::vector<Value> *values = nullptr;
  std::size_t index = 0;
  const std::string *find() const {
    return std::get_if<std::string>(&(*values)[index]);
  }
};

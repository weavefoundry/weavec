---
title: Convert C++ in place
description: Make an existing C++ project safe a file at a time, with its build, its tests and its callers unchanged, using the survey, WEAVEC_SAFE_FILE, bounds on raw pointers and aliased classes.
---

A rewrite into Rust starts over: a new build, bindings at the boundary, tests ported or bridged, and every cyclic structure redesigned before the first function compiles. A conversion with WeaveC keeps the code where it is. Each file is marked safe when the compiler accepts it, the project's own tests run after each step, and a header is converted once for every file that includes it. The design is [RFC 0047](/rfcs/0047-conversion-in-place/); the rules are those of [safe C++](/guides/safe-cxx/).

## The loop

1. **Survey.** Build the project with `weavec-c++` (or `weavec-cc` for C) and two flags added: `-fweavec-safe-survey` runs the rules over every function that is not safe yet, without printing or failing on their errors, and `-fweavec-ledger=<dir>/` records what they would have been.

   ```sh
   cmake -S . -B build-survey -G Ninja -DCMAKE_CXX_COMPILER=weavec-c++ \
     -DCMAKE_CXX_FLAGS="-fweavec-safe-survey -fweavec-ledger=$PWD/ledger/"
   cmake --build build-survey
   weavec-c++ --weavec-audit ledger/
   ```

2. **Read the plan.** The audit starts with it:

   ```text
   conversion: 412 of 1930 functions safe (21%); 388 more have no error
   ready (mark WEAVEC_SAFE_FILE):
     util/arena.h  6 functions
     util/coding.cc  31 functions
   blocked, fewest errors first:
     table/block.cc  4 errors: index-single 3, pointer-cast 1
     db/memtable.cc  9 errors: owner-path 6, raw-owner 3
   by rule: index-single 211, owner-path 96, raw-owner 80
   ```

3. **Mark the ready files.** Write `WEAVEC_SAFE_FILE` once, after `#include <weavec.h>`, at the top of each. Every function the file defines after it is safe, with the member functions of the classes defined there, in every unit that includes it. It does not reach the files it includes. `WEAVEC_GUARDED` keeps one function or class guarded inside it.

4. **Take the blocked file with the fewest errors.** Compile it with the rules on (`-fweavec-safe`, or mark it and build) and fix what they say. `weavec-c++ --weavec-explain <rule>` prints a rule with a rejected example and its rewrite.

5. **Run the project's tests in the checked build** (`-fweavec-safe-checks=full`, the default of a build without optimization): every guard stays, so a mistake the rules do not see stops with a full report. Then repeat from 1.

The errors that come with pointers a fast build believes (RFC 0048) are each one annotation on an existing declaration: a `const char *` handed to `memcpy` with a length, or to `strncmp`, says how many bytes it has (`WEAVEC_COUNTED_BY(n)`) or that it is a string (`WEAVEC_NULL_TERMINATED`); a function that returns or stores an end declares it `WEAVEC_COUNTED_BY(0)`; a function that keeps a parameter's pointer, a reference's address or `this` (a constructor that stores its argument, a `Set(start, end)`, a `push(item)` into a member) declares it `WEAVEC_ESCAPES`, and so does a constructor that keeps the object it makes (a small buffer that points into itself) and each class whose constructor makes one; a pointer member of a short-lived cursor or wrapper that only borrows is `WEAVEC_BORROWED` instead, which keeps nothing. RFC 0047's conversion of tinyxml2 meets 19 such errors, 12 of them strings handed to the C library; yaml-cpp's needed 21 annotations, all but one `WEAVEC_ESCAPES` up four call chains (the emitter's stream, its settings, the graph builder's nodes, `Binary`).

`-fweavec-safe` on a unit's command line makes every file of the unit safe at once and turns on the fast build; use it when a unit's files are all converted.

## Raw pointers keep their place

Existing C++ indexes raw pointers. Safe C++ indexes one whose declaration says how many elements it has, and compares each access with that bound when it runs. Declare the bound where the pointer is declared; the signature stays the same for callers that compile without WeaveC.

| The code has           | Declare                                                             |
| ---------------------- | ------------------------------------------------------------------- |
| a pointer and a count  | `const T *WEAVEC_COUNTED_BY(n) p, std::size_t n`                    |
| a pointer and an end   | `const char *WEAVEC_ENDED_BY(limit) p, const char *limit`           |
| a C string             | `const char *WEAVEC_NULL_TERMINATED s`                              |
| a member and its count | `char *WEAVEC_COUNTED_BY(size_) data_;` beside `std::size_t size_;` |
| an accessor            | `const char *WEAVEC_COUNTED_BY(size_) data() const`                 |

Arrays keep their length as pointers, a container's `data()` has `size()` elements, and the library's pointer-and-count functions (`std::string(p, n)`, `append`, `std::string_view(p, n)`, `std::span(p, n)`) ask that their pointer have a bound. A count changes beside its pointer (`counted-pair`), and neither is passed by a reference that is not `const` (`field-address`).

```cpp accepted
class Slice {
public:
  Slice() = default;
  Slice(const char *WEAVEC_COUNTED_BY(n) WEAVEC_ESCAPES data, std::size_t n)
      : data_(data), size_(n) {}
  Slice(const std::string &s) : data_(s.data()), size_(s.size()) {}
  const char *WEAVEC_COUNTED_BY(size_) data() const { return data_; }
  std::size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  char operator[](std::size_t i) const { return data_[i]; }
  void remove_prefix(std::size_t n) {
    data_ += n;
    size_ -= n;
  }
  bool starts_with(const Slice &x) const {
    return size_ >= x.size_ && std::memcmp(data_, x.data_, x.size_) == 0;
  }
  std::string ToString() const { return std::string(data_, size_); }

private:
  const char *WEAVEC_COUNTED_BY(size_) data_ = "";
  std::size_t size_ = 0;
};

unsigned DecodeFixed32(const char *WEAVEC_COUNTED_BY(4) p) {
  return unsigned(static_cast<unsigned char>(p[0])) |
         (unsigned(static_cast<unsigned char>(p[1])) << 8) |
         (unsigned(static_cast<unsigned char>(p[2])) << 16) |
         (unsigned(static_cast<unsigned char>(p[3])) << 24);
}

bool GetVarint32(Slice *input, unsigned *value) {
  const char *p = input->data();
  const char *limit = p + input->size();
  unsigned result = 0;
  for (unsigned shift = 0; shift <= 28 && p < limit; shift += 7) {
    const unsigned byte = static_cast<unsigned char>(*p++);
    result |= (byte & 127U) << shift;
    if ((byte & 128U) == 0) {
      *value = result;
      input->remove_prefix(static_cast<std::size_t>(p - input->data()));
      return true;
    }
  }
  return false;
}
```

A function that returns a pointer into one of its arguments (a parser's cursor, `const char *SkipWhiteSpace(const char *p WEAVEC_LIFETIMEBOUND)`) says so with `WEAVEC_LIFETIMEBOUND` after that parameter's name; without it, the result is taken to point into every argument, and a local array passed beside the cursor is then `escape` or `stale-borrow`. A string (`WEAVEC_NULL_TERMINATED`) is checked when the program compiles and keeps its guards when it runs; a pointer and a count, or an end, is checked by a comparison.

A pointer member with a bound is a reference: it may live in any object, and a safe function tests it when it loads it. Declared `WEAVEC_BORROWED` as well, it is a borrow instead, which ties its class to the scope of what it points into, as a `std::string_view` member does.

Byte views of objects are allowed for reading (`reinterpret_cast<const char *>(&header)`) and for writing objects that are trivially copyable and hold no pointer. Other conversions between pointer types stay `pointer-cast`: use the object through its own type, `dynamic_cast` or a `std::variant`.

## Objects changed through pointers

Existing code keeps raw pointers to objects it does not own (an environment, a cache, a parent) and calls their member functions. Safe C++ allows that when the class says so:

```cpp accepted
class WEAVEC_ALIASED Env {
public:
  virtual ~Env() = default;
  void Log(const std::string &line) { lines_.push_back(line); }

private:
  std::vector<std::string> lines_;
};

class DB {
public:
  explicit DB(Env *env) : env_(env) {}
  void Open() { env_->Log("open"); }

private:
  Env *env_;
};
```

`WEAVEC_ALIASED` after `class` or `struct` makes the class, and the classes derived from it, changeable through a pointer kept in memory or a `std::shared_ptr`. An aliased class owns no `std::variant`, `std::any` or `std::function`, and no object of a polymorphic class that is not aliased itself: storage that changes type in place under a borrow is the one error no check at run time catches. What a borrow points into in an aliased object is not followed across a call that may reach it; the checked build reports such a use. Declare it on the classes the survey's `owner-path` errors name, and nowhere else.

A class whose objects are only reached from what owns them needs nothing: own it (a `std::unique_ptr` member), pass it as a parameter, or make the member a reference.

## Owners

`new` and `delete` stay out of safe code (`raw-owner`), with one form allowed: `std::unique_ptr<T>(new T(...))` and `p.reset(new T)`, where the block has its owner at once. A member a class allocates and its destructor deletes becomes a `std::unique_ptr`, with `.get()` where a raw pointer went out. A class that manages memory itself (an arena, a pool, a class that counts its references and deletes itself) is the implementation of an owner: keep it `WEAVEC_GUARDED`; the audit lists it.

## What to expect

On four projects WeaveC was not written for (yaml-cpp, leveldb, tinyxml2, pugixml), unedited, the rules give about five errors per hundred lines, most of them `index-single` (a pointer to declare), `owner-path` (a class to declare aliased, or an owner to hold) and `raw-owner` (a member to make a `std::unique_ptr`). The rules about borrows and moves give a few per thousand lines, and those are the ones to read twice.

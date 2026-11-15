// RFC 0047 §5.2 (S): a borrow that can write the characters of a string the
// function does not own ends at a call that may reach the string
// (`stale-borrow`); a string's own local, a `const` borrow and a borrow
// taken again after the call are not affected.
// RUN: %weavec_cxx -std=c++20 -fsyntax-only -fweavec-safe -w -ferror-limit=0 -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
#include <cstddef>
#include <span>
#include <string>
#include <vector>
#include <weavec.h>

void flush();
struct Line {
  std::string text;
};

void fill(std::string &name) {
  char *p = name.data();
  flush();
  *p = 'x'; // expected-error {{'p' points at the characters of 'name', which the call to 'flush' may have moved [weavec-safe:stale-borrow]}}
}
void member(Line &line) {
  char &c = line.text[0];
  flush();
  c = 'y'; // expected-error {{'c' points at the characters of 'line.text', which the call to 'flush' may have moved [weavec-safe:stale-borrow]}}
}
void spanned(std::vector<std::string> &lines) {
  std::span<char> s(lines[0].data(), lines[0].size());
  flush();
  s[0] = 'z'; // expected-error {{'s' points at the characters of 'lines', which the call to 'flush' may have moved [weavec-safe:stale-borrow]}}
}
void local() {
  std::string s = "abc";
  char *p = s.data();
  flush();
  *p = 'x';
}
std::size_t reads(const std::string &name) {
  const char *p = name.c_str();
  flush();
  return p[0] == 'a' ? 1 : 0;
}
void again(std::string &name) {
  flush();
  char *p = name.data();
  *p = 'y';
}
void library(std::string &name) {
  char *p = name.data();
  const std::size_t n = name.size();
  if (n != 0)
    *p = 'w';
}

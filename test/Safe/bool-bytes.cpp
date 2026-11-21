// RFC 0051 §4.2: a `bool` is 0 or 1. Bytes are not written into an object
// that holds one (a library call's written argument, a bit cast to it), and
// a union's `bool` is not read where another member shares its bytes. A
// whole object zeroed, or copied from its own type, is accepted.
// RUN: %weavec_cxx -std=c++20 -fsyntax-only -fweavec-safe -w -ferror-limit=0 -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>

struct Header {
  unsigned length = 0;
  bool compressed = false;
};

bool load(std::FILE *file, Header &h) {
  return std::fread(&h, sizeof h, 1, file) == 1; // expected-error {{bytes are written into a 'Header', which holds a 'bool' ('compressed')}}
}

bool flag(const unsigned char *bytes) {
  bool on = false;
  std::memcpy(&on, bytes, 1); // expected-error {{bytes are written into a 'bool'}}
  return on;
}

bool cast(unsigned char byte) {
  return std::bit_cast<bool>(byte); // expected-error {{bytes are written into a 'bool'}}
}

union Flag {
  bool on;
  unsigned char byte;
};

bool read(unsigned char byte) {
  Flag f;
  f.byte = byte;
  return f.on; // expected-error {{'on' is a 'bool' that shares its bytes with 'byte'}}
}

void patch(Header &h, unsigned char byte) {
  reinterpret_cast<unsigned char *>(&h)[4] = byte; // expected-error {{a pointer is converted to a pointer to another type}}
}

// Accepted: the whole object zeroed, a copy of its own type, the bytes
// read and assigned, and a byte of the bool read.
void clear(Header &h) { std::memset(&h, 0, sizeof h); }
void copy(Header &to, const Header &from) { std::memcpy(&to, &from, sizeof to); }
bool parse(const unsigned char *bytes) { return bytes[0] != 0; }
unsigned char peek(const Header &h) {
  return reinterpret_cast<const unsigned char *>(&h)[4];
}
unsigned char store(bool on) {
  Flag f;
  f.on = on;
  return f.byte;
}

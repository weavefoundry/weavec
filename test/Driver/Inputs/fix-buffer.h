// RFC 0049: a header two units share, for test/Driver/safe-fix.cpp.
#pragma once
#include <cstddef>
#include <cstring>

class Buffer {
public:
  Buffer(const char *d, std::size_t n) : data_(d), size_(n) {}
  const char *data() const { return data_; }
  std::size_t size() const { return size_; }
  bool same(const Buffer &other) const {
    return size_ == other.size_ && std::memcmp(data_, other.data_, size_) == 0;
  }

private:
  const char *data_;
  std::size_t size_;
};

std::size_t count(const char *text, std::size_t n, char c);

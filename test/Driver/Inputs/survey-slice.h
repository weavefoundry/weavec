// A header converted with WEAVEC_SAFE_FILE (RFC 0047 §6).
#pragma once
#include <cstddef>
#include <weavec.h>
WEAVEC_SAFE_FILE
class WEAVEC_ALIASED Slice {
public:
  Slice(const char *WEAVEC_COUNTED_BY(n) d, std::size_t n) : data_(d), size_(n) {}
  char at(std::size_t i) const { return data_[i]; }
  std::size_t size() const { return size_; }

private:
  const char *WEAVEC_COUNTED_BY(size_) data_;
  std::size_t size_;
};

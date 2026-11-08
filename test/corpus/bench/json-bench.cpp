// The nlohmann/json workload of the corpus gate's benchmarks (RFC 0039,
// section 9): parse a large document (json_test_data's jeopardy.json, 55
// MB of an array of 216,930 objects), walk it, serialize it, and parse the
// serialization again.
//
//   json-bench FILE [iterations]      (default 2)
//
// The gate builds this file against single_include/ from the pinned checkout
// (see test/corpus/manifest.json) and compares the checksum line it prints
// between the reference and the WeaveC build.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "nlohmann/json.hpp"

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: json-bench FILE [iterations]\n");
    return 2;
  }
  std::ifstream in(argv[1], std::ios::binary);
  std::stringstream buffer;
  buffer << in.rdbuf();
  const std::string text = buffer.str();
  const int iterations = argc > 2 ? std::atoi(argv[2]) : 2;
  unsigned long checksum = 0;
  for (int round = 0; round < iterations; ++round) {
    const nlohmann::json document = nlohmann::json::parse(text);
    unsigned long values = 0;
    for (const auto &entry : document) {
      for (const auto &[key, value] : entry.items()) {
        values += key.size();
        if (value.is_string())
          values += value.get_ref<const std::string &>().size();
      }
    }
    const std::string dumped = document.dump();
    const nlohmann::json again = nlohmann::json::parse(dumped);
    checksum += values + dumped.size() + again.size();
  }
  std::printf("json-bench %d %lu\n", iterations, checksum);
  return 0;
}

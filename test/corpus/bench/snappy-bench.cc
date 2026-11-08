// The Snappy workload of the corpus gate's benchmarks (RFC 0039, section 9):
// compress and uncompress each file of the checkout's testdata/ (text, HTML,
// a JPEG, a PDF, protocol buffers, URLs) through the string API and the
// sink/source API, repeatedly, checking every round trip.
//
//   snappy-bench DIR [iterations]      (default 40)
//
// The gate builds this file with the library from the pinned checkout (see
// test/corpus/manifest.json) and compares the checksum line it prints
// between the reference and the WeaveC build.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "snappy-sinksource.h"
#include "snappy.h"

static const char *const Files[] = {"alice29.txt", "asyoulik.txt", "fireworks.jpeg", "geo.protodata", "html",
                                    "html_x_4",    "kppkn.gtb",    "lcet10.txt",     "paper-100k.pdf", "plrabn12.txt",
                                    "urls.10K"};

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: snappy-bench DIR [iterations]\n");
    return 2;
  }
  std::vector<std::string> inputs;
  for (const char *name : Files) {
    std::ifstream in(std::string(argv[1]) + "/" + name, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    inputs.push_back(buffer.str());
  }
  const int iterations = argc > 2 ? std::atoi(argv[2]) : 40;
  unsigned long checksum = 0;
  for (int round = 0; round < iterations; ++round) {
    for (const std::string &input : inputs) {
      std::string compressed, restored;
      snappy::Compress(input.data(), input.size(), &compressed);
      if (!snappy::Uncompress(compressed.data(), compressed.size(), &restored) || restored != input) {
        std::fprintf(stderr, "snappy-bench: round trip failed\n");
        return 1;
      }
      std::vector<char> flat(snappy::MaxCompressedLength(input.size()));
      snappy::ByteArraySource source(input.data(), input.size());
      snappy::UncheckedByteArraySink sink(flat.data());
      const size_t length = snappy::Compress(&source, &sink);
      std::vector<char> back(input.size());
      if (!snappy::RawUncompress(flat.data(), length, back.data())) {
        std::fprintf(stderr, "snappy-bench: raw round trip failed\n");
        return 1;
      }
      checksum += compressed.size() + length + static_cast<unsigned char>(back[back.size() / 2]);
    }
  }
  std::printf("snappy-bench %d %lu\n", iterations, checksum);
  return 0;
}

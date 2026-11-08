// RFC 0039 §7: the members of std::string and the streams that the C++
// library's shared object defines are guarded at their calls by their
// rows: exactly the bytes they are given runs clean, one more stops.
// RUN: %weavec_cxx -O1 %s -o %t
// RUN: %t 8
// RUN: not --crash %t 9 append 2>&1 | FileCheck --check-prefix=APPEND %s
// RUN: not --crash %t 9 assign 2>&1 | FileCheck --check-prefix=ASSIGN %s
// RUN: not --crash %t 9 insert 2>&1 | FileCheck --check-prefix=INSERT %s
// RUN: not --crash %t 9 replace 2>&1 | FileCheck --check-prefix=REPLACE %s
// RUN: not --crash %t 9 find 2>&1 | FileCheck --check-prefix=FIND %s
// RUN: not --crash %t 9 compare 2>&1 | FileCheck --check-prefix=COMPARE %s
// RUN: not --crash %t 9 write 2>&1 | FileCheck --check-prefix=WRITE %s
// RUN: not --crash %t 9 read 2>&1 | FileCheck --check-prefix=READ %s
// RUN: not --crash %t 9 getline 2>&1 | FileCheck --check-prefix=GETLINE %s
// RUN: not --crash %t 9 wide 2>&1 | FileCheck --check-prefix=WIDE %s
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

int main(int argc, char **argv) {
  const auto n = static_cast<std::size_t>(atoi(argv[1]));
  const char *how = argc > 2 ? argv[2] : "all";
  const bool all = strcmp(how, "all") == 0;
  char *bytes = static_cast<char *>(malloc(8));
  memset(bytes, 'b', 8);
  auto *wide = static_cast<wchar_t *>(malloc(8 * sizeof(wchar_t)));
  wmemset(wide, L'w', 8);
  std::string s(20, 's');
  std::size_t sum = 0;
  // APPEND: weavec: heap-buffer-overflow at {{.*}}cxx-string-rows.cpp:[[@LINE+1]]:{{[0-9]+}}: read of 9 bytes
  if (all || strcmp(how, "append") == 0) s.append(bytes, n);
  // ASSIGN: weavec: heap-buffer-overflow at {{.*}}cxx-string-rows.cpp:[[@LINE+1]]:{{[0-9]+}}: read of 9 bytes
  if (all || strcmp(how, "assign") == 0) s.assign(bytes, n);
  // INSERT: weavec: heap-buffer-overflow at {{.*}}cxx-string-rows.cpp:[[@LINE+1]]:{{[0-9]+}}: read of 9 bytes
  if (all || strcmp(how, "insert") == 0) s.insert(2, bytes, n);
  // REPLACE: weavec: heap-buffer-overflow at {{.*}}cxx-string-rows.cpp:[[@LINE+1]]:{{[0-9]+}}: read of 9 bytes
  if (all || strcmp(how, "replace") == 0) s.replace(1, 3, bytes, n);
  // FIND: weavec: heap-buffer-overflow at {{.*}}cxx-string-rows.cpp:[[@LINE+1]]:{{[0-9]+}}: read of 9 bytes
  if (all || strcmp(how, "find") == 0) sum += s.find(bytes, 0, n);
  // COMPARE: weavec: heap-buffer-overflow at {{.*}}cxx-string-rows.cpp:[[@LINE+1]]:{{[0-9]+}}: read of 9 bytes
  if (all || strcmp(how, "compare") == 0) sum += s.compare(0, 4, bytes, n);
  std::ostringstream out;
  // WRITE: weavec: heap-buffer-overflow at {{.*}}cxx-string-rows.cpp:[[@LINE+1]]:{{[0-9]+}}: read of 9 bytes
  if (all || strcmp(how, "write") == 0) out.write(bytes, static_cast<std::streamsize>(n));
  std::istringstream in(std::string(64, 'i') + "\nrest");
  // READ: weavec: heap-buffer-overflow at {{.*}}cxx-string-rows.cpp:[[@LINE+1]]:{{[0-9]+}}: write of 9 bytes
  if (all || strcmp(how, "read") == 0) in.read(bytes, static_cast<std::streamsize>(n));
  // GETLINE: weavec: heap-buffer-overflow at {{.*}}cxx-string-rows.cpp:[[@LINE+1]]:{{[0-9]+}}: write of 9 bytes
  if (all || strcmp(how, "getline") == 0) in.getline(bytes, static_cast<std::streamsize>(n));
  std::wstring w;
  // WIDE: weavec: heap-buffer-overflow at {{.*}}cxx-string-rows.cpp:[[@LINE+1]]:{{[0-9]+}}: read of 36 bytes
  if (all || strcmp(how, "wide") == 0) w.append(wide, n);
  static volatile std::size_t sink;
  sink = sum + s.size() + out.str().size() + w.size();
  return 0;
}

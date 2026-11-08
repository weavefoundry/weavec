// RFC 0039 §1: weavec-c++ is Clang's g++ mode (as is --driver-mode=g++):
// a link adds the C++ library after the runtime's; C++ compiles get the
// C++ libraries' bounds checks unless the command line chooses them; C
// compiles do not.
// RUN: %weavec_cxx -### -c %s 2>&1 | FileCheck --check-prefix=HARDEN %s
// RUN: %weavec_cc -### -c %s 2>&1 | FileCheck --check-prefix=HARDEN %s
// RUN: %weavec_cc --driver-mode=g++ -### -c %s 2>&1 | FileCheck --check-prefix=HARDEN %s
// RUN: %weavec_cxx -### -D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_NONE -c %s 2>&1 | FileCheck --check-prefix=CHOSEN %s
// RUN: %weavec_cxx -### -U_GLIBCXX_ASSERTIONS -c %s 2>&1 | FileCheck --check-prefix=CHOSEN %s
// RUN: %weavec_cxx -### -fweavec-checks=none -c %s 2>&1 | FileCheck --check-prefix=CHOSEN %s
// RUN: %weavec_cc -### -x c -c %s 2>&1 | FileCheck --check-prefix=CHOSEN %s
//
// HARDEN: "-D" "_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST" "-D" "_GLIBCXX_ASSERTIONS" "-D" "_GLIBCXX_SANITIZE_VECTOR=1"
// CHOSEN-NOT: _LIBCPP_HARDENING_MODE_FAST
//
// RUN: rm -rf %t && mkdir -p %t
// RUN: %weavec_cxx -c %s -o %t/main.o
// RUN: %if host-darwin %{ %weavec_cxx -### %t/main.o -o %t/prog 2>&1 | FileCheck --check-prefix=DARWIN %s %}
// RUN: %if host-linux %{ %weavec_cxx -### %t/main.o -o %t/prog 2>&1 | FileCheck --check-prefix=LINUX %s %}
// RUN: %if host-linux %{ %weavec_cxx -### -static %t/main.o -o %t/prog 2>&1 | FileCheck --check-prefix=STATIC %s %}
// RUN: %weavec_cxx %t/main.o -o %t/prog && %t/prog
//
// DARWIN: libweavec_rt.dylib"
// DARWIN-SAME: "-lc++"
// LINUX: libweavec_rt.so"
// LINUX-SAME: "-lstdc++"
// STATIC: libweavec_rt.a" "-lpthread" "--wrap=_Unwind_RaiseException" "--wrap=_Unwind_Resume_or_Rethrow" "--wrap=__cxa_begin_catch" "-u" "__wrap__Unwind_RaiseException"
#include <string>
#include <vector>

int main() {
  std::vector<std::string> words{"a", "b"};
  return words.size() == 2 ? 0 : 1;
}

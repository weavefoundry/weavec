# -*- Python -*-
"""lit configuration for WeaveC's integration tests.

Test files are C and C++ sources with `// RUN:` lines. Available
substitutions:

  %weavec_cc      the weavec-cc compiler driver
  %weavec_cxx     weavec-c++, its C++ driver (RFC 0039)
  %resource_dir   directory containing weavec.h
  FileCheck, not, count   LLVM test utilities
"""

import os
import platform

import lit.formats

config.name = "WeaveC"
config.test_format = lit.formats.ShTest(execute_external=False)
config.suffixes = [".c", ".cpp"]
# `cases/` and `corpus/` are run by scripts/run-cases.py and
# scripts/corpus-gate.py (RFC 0030 sections 15 and 17), not lit.
config.excludes = ["Inputs", "cases", "corpus", "CMakeLists.txt", "README.md"]

config.test_source_root = os.path.dirname(__file__)
config.test_exec_root = os.path.join(config.weavec_obj_root, "test")

# Tools --------------------------------------------------------------------

weavec_cc = os.path.join(config.weavec_tools_dir, "weavec-cc")
if not os.path.exists(weavec_cc):
    lit_config.fatal(f"weavec-cc binary not found at {weavec_cc}; build it first")

config.substitutions.append(("%weavec_cxx", os.path.join(config.weavec_tools_dir, "weavec-c++")))
config.substitutions.append(("%weavec_cc", weavec_cc))
# RFC 0038: the runtime's library, the extension of shared libraries, and
# the reference compiler's flags for one.
shared = ".dylib" if platform.system() == "Darwin" else ".so"
config.substitutions.append(
    ("%weavec_rt", os.path.join(config.weavec_lib_dir, "libweavec_rt" + shared))
)
config.substitutions.append(("%so", shared))
config.substitutions.append(("%{rt_dir}", config.weavec_lib_dir))
config.substitutions.append(
    ("%shared", "-dynamiclib" if platform.system() == "Darwin" else "-shared -fPIC")
)
# The reference compiler: the clang of the LLVM WeaveC is built against.
config.substitutions.append(("%clang", config.clang or "clang"))
config.substitutions.append(
    ("%resource_dir", os.path.join(config.weavec_resource_dir, "include"))
)

path = os.pathsep.join(
    [config.weavec_tools_dir, config.llvm_tools_dir, config.environment.get("PATH", "")]
)
config.environment["PATH"] = path

# Sanitizer runtimes and other environment passthroughs.
for var in ("ASAN_OPTIONS", "UBSAN_OPTIONS", "MSAN_OPTIONS", "TSAN_OPTIONS"):
    if var in os.environ:
        config.environment[var] = os.environ[var]

# Features -----------------------------------------------------------------

config.available_features.add(f"host-{platform.system().lower()}")
if config.sanitizers:
    for san in config.sanitizers.split(";"):
        config.available_features.add(f"sanitizer-{san}")

major = config.llvm_version.split(".")[0]
config.available_features.add(f"llvm-{major}")

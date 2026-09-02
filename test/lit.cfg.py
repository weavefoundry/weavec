# -*- Python -*-
"""lit configuration for WeaveC's integration tests.

Test files are C sources with `// RUN:` lines. Available substitutions:

  %weavec         the weavec binary (annotation header already on the path)
  %weavec_cc      the weavec-cc compiler driver
  %resource_dir   directory containing weavec.h
  FileCheck, not, count   LLVM test utilities
"""

import os
import platform

import lit.formats

config.name = "WeaveC"
config.test_format = lit.formats.ShTest(execute_external=False)
config.suffixes = [".c"]
config.excludes = ["Inputs", "CMakeLists.txt", "README.md"]

config.test_source_root = os.path.dirname(__file__)
config.test_exec_root = os.path.join(config.weavec_obj_root, "test")

# Tools --------------------------------------------------------------------

weavec = os.path.join(config.weavec_tools_dir, "weavec")
if not os.path.exists(weavec):
    lit_config.fatal(f"weavec binary not found at {weavec}; build it first")

weavec_cc = os.path.join(config.weavec_tools_dir, "weavec-cc")
if not os.path.exists(weavec_cc):
    lit_config.fatal(f"weavec-cc binary not found at {weavec_cc}; build it first")

# Longer names first so `%weavec_cc` is not rewritten as `%weavec` + `_cc`.
config.substitutions.append(("%weavec_cc", weavec_cc))
config.substitutions.append(("%weavec", weavec))
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

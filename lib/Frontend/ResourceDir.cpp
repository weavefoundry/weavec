//===- ResourceDir.cpp - Locating WeaveC's resource directory -------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/ResourceDir.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"

#include <array>
#include <cstdlib>
#include <optional>

#ifndef WEAVEC_BUILD_RESOURCE_DIR
#define WEAVEC_BUILD_RESOURCE_DIR ""
#endif
#ifndef WEAVEC_RESOURCE_DIR_RELATIVE
#define WEAVEC_RESOURCE_DIR_RELATIVE "lib/weavec"
#endif
#ifndef WEAVEC_CLANG_RESOURCE_DIR
#define WEAVEC_CLANG_RESOURCE_DIR ""
#endif
#ifndef WEAVEC_CLANG_EXECUTABLE
#define WEAVEC_CLANG_EXECUTABLE ""
#endif

namespace weavec::frontend {

static bool hasHeader(llvm::StringRef dir) {
  llvm::SmallString<256> path(dir);
  llvm::sys::path::append(path, "weavec.h");
  return llvm::sys::fs::exists(path);
}

std::string findResourceIncludeDir(const char *argv0, void *mainAddr) {
  // NOLINTNEXTLINE(concurrency-mt-unsafe) -- read once at startup.
  if (const char *env = std::getenv("WEAVEC_RESOURCE_DIR")) {
    llvm::SmallString<256> path(env);
    llvm::sys::path::append(path, "include");
    if (hasHeader(path))
      return path.str().str();
  }

  const std::string exe = llvm::sys::fs::getMainExecutable(argv0, mainAddr);
  if (!exe.empty()) {
    // <prefix>/bin/weavec -> <prefix>/lib/weavec/include
    llvm::SmallString<256> path(llvm::sys::path::parent_path(exe));
    llvm::sys::path::append(path, "..", WEAVEC_RESOURCE_DIR_RELATIVE,
                            "include");
    llvm::sys::path::remove_dots(path, /*remove_dot_dot=*/true);
    if (hasHeader(path))
      return path.str().str();
  }

  llvm::SmallString<256> buildTree(llvm::StringRef(WEAVEC_BUILD_RESOURCE_DIR));
  if (!buildTree.empty()) {
    llvm::sys::path::append(buildTree, "include");
    if (hasHeader(buildTree))
      return buildTree.str().str();
  }

  return {};
}

std::string getClangResourceDir() {
  const llvm::StringRef dir(WEAVEC_CLANG_RESOURCE_DIR);
  if (dir.empty() || !llvm::sys::fs::is_directory(dir))
    return {};
  return dir.str();
}

std::string getClangExecutable() {
  // NOLINTNEXTLINE(concurrency-mt-unsafe) -- read once at startup.
  if (const char *env = std::getenv("WEAVEC_CLANG");
      env != nullptr && llvm::sys::fs::can_execute(env))
    return env;
  const llvm::StringRef built(WEAVEC_CLANG_EXECUTABLE);
  if (!built.empty() && llvm::sys::fs::can_execute(built))
    return built.str();
  if (llvm::ErrorOr<std::string> found = llvm::sys::findProgramByName("clang"))
    return *found;
  return {};
}

#ifdef __APPLE__
static std::string findAppleSdk() {
  // NOLINTNEXTLINE(concurrency-mt-unsafe) -- read once at startup.
  const char *env = std::getenv("SDKROOT");
  if (env != nullptr && llvm::sys::fs::is_directory(env))
    return env;

  llvm::ErrorOr<std::string> xcrun = llvm::sys::findProgramByName("xcrun");
  if (!xcrun)
    return {};

  llvm::SmallString<128> outputPath;
  if (llvm::sys::fs::createTemporaryFile("weavec-sdk", "txt", outputPath))
    return {};
  llvm::FileRemover remover(outputPath);

  const std::array<std::optional<llvm::StringRef>, 3> redirects = {
      std::nullopt, llvm::StringRef(outputPath), std::nullopt};
  const int rc = llvm::sys::ExecuteAndWait(*xcrun, {"xcrun", "--show-sdk-path"},
                                           /*Env=*/std::nullopt, redirects);
  if (rc != 0)
    return {};

  auto buffer = llvm::MemoryBuffer::getFile(outputPath);
  if (!buffer)
    return {};
  const std::string sdk = (*buffer)->getBuffer().trim().str();
  return llvm::sys::fs::is_directory(sdk) ? sdk : std::string();
}
#endif

std::string getDefaultSysroot() {
  std::string sysroot;
#ifdef __APPLE__
  sysroot = findAppleSdk();
#endif
  return sysroot;
}

} // namespace weavec::frontend

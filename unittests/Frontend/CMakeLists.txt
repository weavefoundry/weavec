weavec_add_unittest(
  WeaveCFrontendTests
  SOURCES GuardPassTest.cpp
  DEPS weavec::Frontend)

# The guard pass tests parse IR and run the passes on it.
weavec_link_llvm(WeaveCFrontendTests AsmParser Passes)

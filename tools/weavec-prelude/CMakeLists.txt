# `weavec-prelude` prints the check prelude of RFC 0030 (section 10.2). It is
# a build and test tool: the runtime generates libweavec_chk.a's helpers with
# it, and the prelude tests compile its output. It is not installed.
add_executable(weavec-prelude main.cpp)
target_link_libraries(weavec-prelude PRIVATE weavec::Frontend weavec::options
                                             weavec::llvm)
set_target_properties(weavec-prelude PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                                                "${PROJECT_BINARY_DIR}/bin")
weavec_link_llvm(weavec-prelude Support TargetParser)

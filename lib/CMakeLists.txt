# Library components, ordered by dependency:
#   Core      -- Clang/LLVM-independent ownership model
#   Analysis  -- Clang AST -> core facts; the checkers
#   Frontend  -- Clang FrontendAction / tooling integration
add_subdirectory(Core)
add_subdirectory(Analysis)
add_subdirectory(Frontend)

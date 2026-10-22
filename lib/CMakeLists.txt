# Library components, ordered by dependency:
#   Core      -- the C library table of the guards (no Clang or LLVM)
#   Frontend  -- the guard passes, the unsafe regions, the enforcement ledger
#                and the compiler driver
add_subdirectory(Core)
add_subdirectory(Frontend)

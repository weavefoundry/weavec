// LevelDB's port/port_config.h as its CMake build generates it, for the
// corpus gate's per-file compiles: fullfsync on Darwin, fdatasync on Linux.
#ifndef STORAGE_LEVELDB_PORT_PORT_CONFIG_H_
#define STORAGE_LEVELDB_PORT_PORT_CONFIG_H_

#if defined(__APPLE__)
#define HAVE_FDATASYNC 0
#define HAVE_FULLFSYNC 1
#else
#define HAVE_FDATASYNC 1
#define HAVE_FULLFSYNC 0
#endif
#define HAVE_O_CLOEXEC 1
#define HAVE_CRC32C 0
#define HAVE_SNAPPY 0

#endif  // STORAGE_LEVELDB_PORT_PORT_CONFIG_H_

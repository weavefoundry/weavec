// RFC 0033 §1: a platform function handed the address of a pointer (through a cast) may
// write a pointer there; the read after it is not of an uninitialised value (libuv's
// uv_cpu_info on Darwin).
// STAGE: S1
// CLEAN
// RUN-INPUT:
#include <stdlib.h>
#if defined(__APPLE__)
#include <mach/mach.h>
int main(void) {
  natural_t count;
  processor_info_array_t info;
  mach_msg_type_number_t info_count;
  if (host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &count,
                          (processor_info_array_t *)&info, &info_count) != KERN_SUCCESS)
    return 1;
  int ok = count > 0 && info[0] >= 0;
  vm_deallocate(mach_task_self(), (vm_address_t)info, info_count * sizeof(*info));
  return ok ? 0 : 1;
}
#else
#include <glob.h>
int main(void) {
  glob_t g;
  char **paths;
  if (glob("/", 0, NULL, &g) != 0) return 1;
  *(char ***)&paths = g.gl_pathv;
  int ok = paths[0][0] == '/';
  globfree(&g);
  return ok ? 0 : 1;
}
#endif

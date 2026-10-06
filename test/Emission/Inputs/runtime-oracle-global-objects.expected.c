/* runtime-oracle-global-objects.c as the check emitter rewrites it. */
#if defined(__APPLE__)
#define DESCRIPTOR __attribute__((section("__DATA,__weavec_glob"), used))
#else
#define DESCRIPTOR __attribute__((section("weavec_globals"), used))
#endif

extern int Elsewhere[4];
int Table[8] __attribute__((aligned(16)));
static char Name[5] __attribute__((aligned(16))) = "name";

char *name(void) { return Name; }
int *table(void) { return Table; }
int *elsewhere(void) { return Elsewhere; }

static const void *const __weavec_global_1[2] DESCRIPTOR = {&Table, (const void *)sizeof Table};
static const void *const __weavec_global_2[2] DESCRIPTOR = {&Name, (const void *)sizeof Name};

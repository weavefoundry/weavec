// RFC 0030 §9.3: the unit's function-pointer slots. Designators stored,
// returned or passed are members of their slot; copies, direct calls and
// indirect calls connect slots; outside values open them. Per TU, a field
// of a record the main file defines is closed while no object of it comes
// from or goes to code outside the unit, and a `static` variable while its
// address does not escape. The Lua pattern: `g->frealloc` stored from an
// exported function's parameter is open, so calls through it are
// `open-unknown` in a per-TU compile (and `unresolved(callback)`, not
// silent).
// RUN: %weavec --dump-kinds %s -- -I %S/Inputs | FileCheck %s
#include "kinds-lstate.h"

// Lua's lstate.c: the allocator comes from the caller.
struct global_State *lua_newstate(lua_Alloc f, void *ud) {
  struct global_State *g = new_state();
  g->frealloc = f;
  g->ud = ud;
  return g;
}

void *lua_grow(struct global_State *g, void *block, size_t n) {
  return (*g->frealloc)(g->ud, block, 0, n);
}

// A table the unit owns: its record is defined here and never leaves.
static void hello(void) {}
static void bye(void) {}
struct ops { void (*run)(void); };
static struct ops table = {hello};
static void (*hook)(void) = bye;
static void (*pick(int c))(void) { return c ? hello : bye; }
static void apply(void (*f)(void)) { f(); }

int drive(int c, unsigned long bits) {
  table.run();
  hook();
  pick(c)();
  apply(hello);
  void (*raw)(void) = (void (*)(void))bits;
  raw();
  return 0;
}

// CHECK: slot constraints:
// CHECK-DAG: member <unit>:hello -> field <unit>:struct ops run
// CHECK-DAG: member <unit>:bye -> static <unit>:hook
// CHECK-DAG: member <unit>:hello -> param <unit>:apply 0
// CHECK-DAG: member <unit>:bye -> result <unit>:pick
// CHECK-DAG: member <unit>:hello -> result <unit>:pick
// CHECK-DAG: subset param lua_newstate 0 -> field struct global_State frealloc
// CHECK-DAG: open local drive raw (a conversion from an integer)
// CHECK: slot rules: confined(<unit>:struct ops)
// CHECK-NEXT: indirect calls:
// CHECK-NEXT: 22:10: through field struct global_State frealloc: open-unknown ('struct global_State.frealloc' may be stored by code outside this unit)
// CHECK-NEXT: 32:38: through param <unit>:apply 0: closed-single {<unit>:hello}
// CHECK-NEXT: 35:3: through field <unit>:struct ops run: closed-single {<unit>:hello}
// CHECK-NEXT: 36:3: through static <unit>:hook: closed-single {<unit>:bye}
// CHECK-NEXT: 37:3: through result <unit>:pick: closed-join {<unit>:bye, <unit>:hello}
// CHECK-NEXT: 40:3: through local drive raw: open-unknown (a conversion from an integer)

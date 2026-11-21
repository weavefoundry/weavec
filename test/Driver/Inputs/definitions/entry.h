#pragma once
struct Shared {
  long count = 0;
};
long tally(const Shared &shared);
#ifndef LIMITS
#define LIMITS 4
#endif
// One header, compiled with two values of LIMITS.
extern const long limits[LIMITS];

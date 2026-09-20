// RFC 0030 §7.2 (malloc, ownership_returns, ownership_takes, ownership_holds): ownership attributes are contracts.
// STAGE: S6
// The functions are defined elsewhere, but each pointer parameter carries an ownership
// attribute, so their contracts apply and their Call sites' temporal facets are
// trusted(extern-contract) (§5.1): 'pool_get' returns a fresh object of family 'pool',
// 'pool_keep' retains its argument and 'pool_put' releases it. The use after 'pool_put'
// is a definite use-after-free.
#include <stddef.h>

void *pool_get(size_t n) __attribute__((ownership_returns(pool)));
void pool_keep(void *p) __attribute__((ownership_holds(pool, 1)));
void pool_put(void *p) __attribute__((ownership_takes(pool, 1)));

void hold(char *q) { pool_keep(q); } // TRUSTED: temporal:extern-contract

int use(void) {
  char *p = pool_get(8); // TRUSTED: temporal:extern-contract
  if (p == NULL) return 0;
  p[0] = 1;
  pool_put(p); // TRUSTED: temporal:extern-contract
  return p[0]; // BUG: use-after-free definite
}

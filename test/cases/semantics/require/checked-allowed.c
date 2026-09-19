// RFC 0030 §6.3: under -fweavec-require=checked, checked facets are allowed.
// STAGE: S6
// '*p' has null checked, spatial proven by A1's Single default and temporal proven, so it
// meets the checked level: no error. (Under proven it is an unchecked-operation error,
// examples/require-proven.c.)
// FLAGS: -fweavec-require=checked
// CLEAN
int deref(int *p) { return *p; }

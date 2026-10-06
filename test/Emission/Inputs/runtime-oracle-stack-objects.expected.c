/* runtime-oracle-stack-objects.c as the check emitter rewrites it. */
extern void fill(char *p, unsigned long n);
extern void take(int *p);

int outer(void) {
  char buffer[16] __attribute__((aligned(16)));
  void *__weavec_frame_1 __attribute__((cleanup(__weavec_stack_leave), unused)) =
      __weavec_stack_enter((void *)&buffer, sizeof buffer, 0);
  int kept[4];
  fill(buffer, sizeof buffer);
  kept[0] = buffer[0];
  return kept[0];
}

int nested(int n) {
  int total = 0;
  if (n > 0) {
    int value __attribute__((aligned(16))) = n;
    void *__weavec_frame_2 __attribute__((cleanup(__weavec_stack_leave), unused)) =
        __weavec_stack_enter((void *)&value, sizeof value, 2);
    take(&value);
    total = value;
  }
  return total;
}

int parameter(int n __attribute__((aligned(16)))) {
  void *__weavec_frame_3 __attribute__((cleanup(__weavec_stack_leave), unused)) =
      __weavec_stack_enter((void *)&n, sizeof n, 0);
  take(&n);
  return n;
}

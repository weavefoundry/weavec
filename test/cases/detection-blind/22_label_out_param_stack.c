// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * make_label() formats a label into a local buffer and stores a pointer to
 * that buffer in the caller's struct. Once make_label() returns, the label
 * points into a dead stack frame, which the next call reuses.
 * Category: temporal (stack use after return, through an out-parameter).
 * Why it may be missed: compilers warn about `return buf;` but not about
 * storing &buf through a pointer parameter, and the code reads naturally.
 */
#include <stdio.h>
#include <string.h>

struct label {
    const char *text;
    int width;
#ifdef FIX
    char storage[32];
#endif
};

static void make_label(struct label *l, const char *prefix, unsigned id)
{
#ifdef FIX
    char *buf = l->storage;
    size_t cap = sizeof l->storage;
#else
    char tmp[32];
    char *buf = tmp;
    size_t cap = sizeof tmp;
#endif
    int w = snprintf(buf, cap, "%s-%04u", prefix, id);
    l->text = buf;
    l->width = w < 0 ? 0 : w;
}

int main(void)
{
    static const char *const want[] = {"disk-0007", "net-0042", "cpu-0003"};
    struct label labels[3];
    make_label(&labels[0], "disk", 7);
    make_label(&labels[1], "net", 42);
    make_label(&labels[2], "cpu", 3);
    size_t total = 0;
    int ok = 1;
    for (int i = 0; i < 3; i++) {
        total += strlen(labels[i].text); // STOP // MISS: a stack object is untracked once its scope ends (RFC 0032 §4): use after return
        if (strcmp(labels[i].text, want[i]) != 0)
            ok = 0;
    }
    printf("%zu label characters\n", total);
    return ok && total == 25 ? 0 : 1;
}

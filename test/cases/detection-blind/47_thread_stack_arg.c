// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * Worker threads started by a helper that passes each thread a pointer to a
 * job struct on the helper's own stack. The workers wait for a start signal
 * that main gives only after all of them are started, so by the time a
 * worker reads its job the helper's frame is long dead.
 * Category: threading / temporal (stack use after return from another
 * thread).
 * Why it may be missed: pthread_create() with &local is a common pattern
 * that is fine when the creator joins before returning; here it does not.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#define NWORKERS 4

struct job {
    int id;
    int input;
};

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int released;
static int results[NWORKERS];

static void *worker(void *arg)
{
    pthread_mutex_lock(&mu);
    while (!released)
        pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
    const struct job *j = arg;
    int id = j->id; // STOP // MISS: a stack object is untracked once its scope ends (RFC 0032 §4): use after return
    int input = j->input;
#ifdef FIX
    free(arg);
#endif
    if (id >= 0 && id < NWORKERS)
        results[id] = input * input;
    return NULL;
}

static int start_worker(pthread_t *t, int id, int input)
{
#ifdef FIX
    struct job *j = malloc(sizeof *j);
    if (!j)
        return -1;
    j->id = id;
    j->input = input;
    if (pthread_create(t, NULL, worker, j) != 0) {
        free(j);
        return -1;
    }
    return 0;
#else
    struct job j = {id, input};
    return pthread_create(t, NULL, worker, &j);
#endif
}

int main(void)
{
    pthread_t th[NWORKERS];
    for (int i = 0; i < NWORKERS; i++)
        if (start_worker(&th[i], i, i + 2) != 0)
            return 1;
    pthread_mutex_lock(&mu);
    released = 1;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    for (int i = 0; i < NWORKERS; i++)
        pthread_join(th[i], NULL);
    int ok = 1;
    for (int i = 0; i < NWORKERS; i++) {
        printf("worker %d: %d\n", i, results[i]);
        if (results[i] != (i + 2) * (i + 2))
            ok = 0;
    }
    return ok ? 0 : 1;
}

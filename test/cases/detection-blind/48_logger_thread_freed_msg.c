// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * Batched background logger. log_post() queues the caller's string pointer
 * without copying it, and the caller frees its message right after posting;
 * the writer thread reads the queued strings only on log_flush(), after
 * they have all been freed.
 * Category: threading / temporal (heap use-after-free across threads).
 * Why it may be missed: the API looks like it takes the message (as most
 * loggers copy), and the free and the read happen on different threads.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QCAP 16

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static pthread_cond_t flushed = PTHREAD_COND_INITIALIZER;
static const char *queue[QCAP];
static size_t qlen;
static int flush_requested, stopping;
static size_t bytes_written, lines_written;

static int log_post(const char *msg)
{
    int rc = -1;
    pthread_mutex_lock(&mu);
    if (qlen < QCAP) {
#ifdef FIX
        size_t n = strlen(msg) + 1;
        char *copy = malloc(n);
        if (copy) {
            memcpy(copy, msg, n);
            queue[qlen++] = copy;
            rc = 0;
        }
#else
        queue[qlen++] = msg;
        rc = 0;
#endif
    }
    pthread_mutex_unlock(&mu);
    return rc;
}

static void *writer(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&mu);
    for (;;) {
        while (!flush_requested && !stopping)
            pthread_cond_wait(&wake, &mu);
        if (flush_requested) {
            for (size_t i = 0; i < qlen; i++) {
                bytes_written += strlen(queue[i]); // STOP
                lines_written++;
#ifdef FIX
                free((void *)queue[i]);
#endif
            }
            qlen = 0;
            flush_requested = 0;
            pthread_cond_broadcast(&flushed);
        }
        if (stopping)
            break;
    }
    pthread_mutex_unlock(&mu);
    return NULL;
}

static void log_flush(void)
{
    pthread_mutex_lock(&mu);
    flush_requested = 1;
    pthread_cond_signal(&wake);
    while (flush_requested)
        pthread_cond_wait(&flushed, &mu);
    pthread_mutex_unlock(&mu);
}

int main(void)
{
    pthread_t th;
    if (pthread_create(&th, NULL, writer, NULL) != 0)
        return 1;
    for (int i = 0; i < 4; i++) {
        char *msg = malloc(32);
        if (!msg)
            return 1;
        snprintf(msg, 32, "request %d handled", i);
        log_post(msg);
        free(msg);
    }
    log_flush();
    pthread_mutex_lock(&mu);
    stopping = 1;
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&mu);
    pthread_join(th, NULL);
    printf("%zu lines, %zu bytes\n", lines_written, bytes_written);
    return lines_written == 4 && bytes_written == 68 ? 0 : 1;
}

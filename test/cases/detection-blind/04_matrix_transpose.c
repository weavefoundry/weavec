// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * Matrix transpose of a 3x5 matrix into a 5x3 one. The destination index
 * uses the source's column count as the row stride, so for a non-square
 * matrix the writes run past the end of the transposed matrix's data.
 * Category: spatial (heap buffer overflow, index computed from the shape).
 * Why it may be missed: `m->cols` and `t->cols` look alike, and square test
 * matrices (the usual unit test) never expose the wrong stride.
 */
#include <stdio.h>
#include <stdlib.h>

struct mat {
    size_t rows, cols;
    double *data;
};

static struct mat *mat_new(size_t rows, size_t cols)
{
    struct mat *m = malloc(sizeof *m);
    if (!m)
        return NULL;
    m->data = calloc(rows * cols, sizeof *m->data);
    if (!m->data) {
        free(m);
        return NULL;
    }
    m->rows = rows;
    m->cols = cols;
    return m;
}

static void mat_free(struct mat *m)
{
    if (m) {
        free(m->data);
        free(m);
    }
}

static struct mat *mat_transpose(const struct mat *m)
{
    struct mat *t = mat_new(m->cols, m->rows);
    if (!t)
        return NULL;
    for (size_t i = 0; i < m->rows; i++)
        for (size_t j = 0; j < m->cols; j++)
#ifdef FIX
            t->data[j * t->cols + i] = m->data[i * m->cols + j];
#else
            t->data[j * m->cols + i] = m->data[i * m->cols + j]; // STOP
#endif
    return t;
}

static struct mat *mat_mul(const struct mat *a, const struct mat *b)
{
    if (a->cols != b->rows)
        return NULL;
    struct mat *p = mat_new(a->rows, b->cols);
    if (!p)
        return NULL;
    for (size_t i = 0; i < a->rows; i++)
        for (size_t j = 0; j < b->cols; j++) {
            double s = 0;
            for (size_t k = 0; k < a->cols; k++)
                s += a->data[i * a->cols + k] * b->data[k * b->cols + j];
            p->data[i * p->cols + j] = s;
        }
    return p;
}

int main(void)
{
    struct mat *a = mat_new(3, 5);
    if (!a)
        return 1;
    double sumsq = 0;
    for (size_t i = 0; i < a->rows; i++)
        for (size_t j = 0; j < a->cols; j++) {
            double v = (double)(i * 2 + j);
            a->data[i * a->cols + j] = v;
            sumsq += v * v;
        }
    struct mat *t = mat_transpose(a);
    struct mat *g = t ? mat_mul(a, t) : NULL; /* Gram matrix A * A^T */
    double trace = 0;
    if (g)
        for (size_t i = 0; i < g->rows; i++)
            trace += g->data[i * g->cols + i];
    printf("trace %.1f, sum of squares %.1f\n", trace, sumsq);
    int ok = g != NULL && trace == sumsq;
    mat_free(g);
    mat_free(t);
    mat_free(a);
    return ok ? 0 : 1;
}

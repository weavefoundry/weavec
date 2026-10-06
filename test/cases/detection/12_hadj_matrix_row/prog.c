// RFC 0034 detection set, case 12 (heap stride into a neighbour): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: 8 32 1 9
#include <stdio.h>
#include <stdlib.h>

struct matrix {
  size_t rows, cols;
  double *cells;
};

static int matrix_init(struct matrix *m, size_t rows, size_t cols) {
  m->rows = rows;
  m->cols = cols;
  m->cells = calloc(rows * cols, sizeof *m->cells);
  return m->cells ? 0 : -1;
}

static double *matrix_row(struct matrix *m, size_t r) { return m->cells + r * m->cols; }

/* rows are numbered from 1 in the input format */
static void matrix_add_row(struct matrix *m, size_t row1, double v) {
#ifdef FIX
  if (row1 == 0 || row1 > m->rows)
    return;
  double *row = matrix_row(m, row1 - 1);
#else
  double *row = matrix_row(m, row1); /* forgot the -1 */
#endif
  for (size_t c = 0; c < m->cols; c++)
    row[c] += v; // STOP
}

int main(int argc, char **argv) {
  if (argc < 4)
    return 2;
  struct matrix weights, bias;
  size_t rows = strtoul(argv[1], NULL, 10), cols = strtoul(argv[2], NULL, 10);
  if (matrix_init(&weights, rows, cols) || matrix_init(&bias, rows, cols))
    return 1;
  for (int i = 3; i < argc; i++)
    matrix_add_row(&weights, strtoul(argv[i], NULL, 10), 1.5);
  double s = 0;
  for (size_t i = 0; i < rows * cols; i++)
    s += weights.cells[i] - bias.cells[i];
  printf("sum=%.1f bias0=%.1f\n", s, bias.cells[0]);
  free(weights.cells);
  free(bias.cells);
  return 0;
}

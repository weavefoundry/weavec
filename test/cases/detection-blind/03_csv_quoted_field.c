// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * CSV reader that unquotes each field into a fixed 16-byte stack buffer. The
 * copy loop keeps at most FIELD_MAX characters, but the terminator is then
 * stored at out[FIELD_MAX], one byte past the buffer, for a long field.
 * Category: spatial (stack buffer overflow by 1 byte).
 * Why it may be missed: the loop bound looks right; the overflow is in the
 * terminator store after the loop and needs a field of 16+ characters.
 */
#include <stdio.h>
#include <string.h>

#define FIELD_MAX 16

/* Copies the field starting at *pos into out (FIELD_MAX bytes), undoing CSV
   quoting, and advances *pos past the field and its comma. */
static void read_field(const char *line, size_t *pos, char *out)
{
    size_t i = *pos, o = 0;
    int quoted = line[i] == '"';
    if (quoted)
        i++;
    while (line[i] != '\0') {
        char c = line[i];
        if (quoted && c == '"') {
            if (line[i + 1] != '"') { /* closing quote */
                quoted = 0;
                i++;
                continue;
            }
            i++; /* "" is an escaped quote */
        } else if (!quoted && c == ',') {
            break;
        }
#ifdef FIX
        if (o < FIELD_MAX - 1)
#else
        if (o < FIELD_MAX)
#endif
            out[o++] = c;
        i++;
    }
    out[o] = '\0'; // STOP
    if (line[i] == ',')
        i++;
    *pos = i;
}

int main(void)
{
    static const char *const rows[] = {
        "17,bolt,0.25",
        "42,\"Widget \"\"Pro\"\" Max Edition\",19.99",
        "43,\"nut, hex\",0.10",
    };
    unsigned fields = 0;
    size_t chars = 0;
    for (size_t r = 0; r < sizeof rows / sizeof rows[0]; r++) {
        size_t pos = 0;
        unsigned col = 0;
        while (rows[r][pos] != '\0') {
            char field[FIELD_MAX];
            read_field(rows[r], &pos, field);
            chars += strlen(field);
            if (col == 1)
                printf("name: %s\n", field);
            col++;
            fields++;
        }
    }
    printf("%u fields, %zu characters\n", fields, chars);
    return fields == 9 ? 0 : 1;
}

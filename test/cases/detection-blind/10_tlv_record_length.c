// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * Type-length-value stream parser. parse_stream() checks that each record's
 * 16-bit length fits in the input, but handle_record() then copies that many
 * bytes into a 32-byte stack payload buffer; a 36-byte record overflows it
 * by 4 bytes.
 * Category: spatial (stack buffer overflow, length taken from the input).
 * Why it may be missed: there is a length check, just against the wrong
 * bound (the input, not the destination), one function away from the copy.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PAYLOAD_MAX 32

static uint32_t handle_record(uint8_t type, const uint8_t *body, size_t len)
{
    uint8_t payload[PAYLOAD_MAX];
#ifdef FIX
    if (len > sizeof payload)
        return 0; /* oversized record: ignored */
#endif
    memcpy(payload, body, len); // STOP
    uint32_t h = type;
    for (size_t i = 0; i < len; i++)
        h = h * 31u + payload[i];
    return h;
}

static int parse_stream(const uint8_t *buf, size_t n, uint32_t *digest, unsigned *records)
{
    size_t pos = 0;
    while (pos < n) {
        if (n - pos < 3)
            return -1;
        uint8_t type = buf[pos];
        size_t len = ((size_t)buf[pos + 1] << 8) | buf[pos + 2];
        pos += 3;
        if (len > n - pos)
            return -1; /* truncated record */
        *digest ^= handle_record(type, buf + pos, len);
        (*records)++;
        pos += len;
    }
    return 0;
}

static size_t put_record(uint8_t *out, uint8_t type, const char *body, size_t len)
{
    out[0] = type;
    out[1] = (uint8_t)(len >> 8);
    out[2] = (uint8_t)(len & 0xff);
    memcpy(out + 3, body, len);
    return len + 3;
}

int main(void)
{
    uint8_t wire[128];
    size_t n = 0;
    n += put_record(wire + n, 1, "hello", 5);
    n += put_record(wire + n, 2, "0123456789abcdef0123456789abcdefWXYZ", 36);
    n += put_record(wire + n, 3, "", 0);
    uint32_t digest = 0;
    unsigned records = 0;
    if (parse_stream(wire, n, &digest, &records) != 0)
        return 1;
    printf("%u records, digest %08x\n", records, (unsigned)digest);
    return records == 3 ? 0 : 1;
}

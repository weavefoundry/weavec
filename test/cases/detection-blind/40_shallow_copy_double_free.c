// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * Image flip into a second image. image_flip() initialises the output with a
 * struct assignment from the source "to copy the dimensions", which also
 * copies the pixel pointer; both images then own the same buffer and the
 * second image_free() frees it again.
 * Category: release (double free through a shallow struct copy).
 * Why it may be missed: `*out = *src` reads as copying the header, and the
 * flip result is only subtly wrong because it works in place.
 */
#include <stdio.h>
#include <stdlib.h>

struct image {
    unsigned w, h;
    unsigned char *px;
};

static int image_init(struct image *im, unsigned w, unsigned h)
{
    im->w = w;
    im->h = h;
    im->px = calloc((size_t)w * h, 1);
    return im->px ? 0 : -1;
}

static void image_free(struct image *im)
{
    free(im->px); // STOP
    im->px = NULL;
}

/* Writes a vertically flipped copy of src into out. */
static int image_flip(const struct image *src, struct image *out)
{
#ifdef FIX
    if (image_init(out, src->w, src->h) != 0)
        return -1;
#else
    *out = *src; /* same dimensions */
#endif
    for (unsigned y = 0; y < src->h; y++)
        for (unsigned x = 0; x < src->w; x++)
            out->px[(size_t)y * src->w + x] = src->px[(size_t)(src->h - 1 - y) * src->w + x];
    return 0;
}

int main(void)
{
    struct image img, flipped;
    if (image_init(&img, 4, 3) != 0)
        return 1;
    for (unsigned y = 0; y < 3; y++)
        for (unsigned x = 0; x < 4; x++)
            img.px[y * 4 + x] = (unsigned char)(y * 16 + x);
    if (image_flip(&img, &flipped) != 0) {
        image_free(&img);
        return 1;
    }
    int ok = 1;
    for (unsigned y = 0; y < 3; y++)
        for (unsigned x = 0; x < 4; x++)
            if (flipped.px[y * 4 + x] != (2 - y) * 16 + x)
                ok = 0;
    printf("flip %s\n", ok ? "ok" : "wrong");
    image_free(&flipped);
    image_free(&img);
    return ok ? 0 : 1;
}

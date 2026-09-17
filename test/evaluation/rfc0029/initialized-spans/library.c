int pair(const unsigned char *first, const unsigned char *last) {
    const unsigned char *copy = first;
    if (last - copy < 2) return 0;
    return copy[0] + copy[1];
}

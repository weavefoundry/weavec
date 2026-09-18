int decode(const unsigned char *first, const unsigned char *last, unsigned char **out) {
 if(last-first<2)return 0;
 **out=first[1]; ++*out; return 1;
}

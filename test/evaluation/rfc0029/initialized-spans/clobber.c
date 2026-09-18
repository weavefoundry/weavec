extern void clobber(unsigned char *);
int pair(unsigned char *first, unsigned char *last) {
 if(last-first<2)return 0; clobber(first);return first[1];
}
int main(void) { unsigned char a[2]={1,2};return pair(a,a+2); }

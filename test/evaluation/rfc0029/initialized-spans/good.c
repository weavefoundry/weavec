int pair(const unsigned char *, const unsigned char *);
int main(void) { const unsigned char a[4] = {1,2,3,4}; return pair(a+1,a+4)==5 ? 0 : 1; }

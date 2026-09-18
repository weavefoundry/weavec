int pair(const unsigned char *, const unsigned char *);
int main(void) { const unsigned char a[2] = {1,2}, b[2] = {3,4}; return pair(a,b+2); }

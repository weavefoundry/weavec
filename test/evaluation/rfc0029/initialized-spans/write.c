int overwrite(unsigned char *first, unsigned char *last) {
 if(last-first<2)return 0; first[1]=3;return 1;
}
int main(void) { const unsigned char a[2]={1,2};return overwrite((unsigned char*)a,(unsigned char*)a+2); }

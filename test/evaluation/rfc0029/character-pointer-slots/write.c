static void set(char **out,char *p) { *out=p; }
int main(void) { unsigned char a[2]={7,0}; unsigned char *p=0; set((char **)&p,(char *)a); return *p; }

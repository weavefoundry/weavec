int probe(const unsigned char *,const unsigned char *,int);
int main(void){unsigned char a[4];a[0]=1;return probe(a,a+4,1);}

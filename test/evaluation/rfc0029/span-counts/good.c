int probe(const unsigned char *,const unsigned char *,int);
int main(int argc,char **argv){const unsigned char a[4]={1,2,3,4};return probe(a,a+4,argc>1);}

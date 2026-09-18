int probe(const unsigned char*,const unsigned char*,int);
int main(int argc,char**argv){(void)argv;const unsigned char bytes[4]={1,2,3,4};return probe(bytes,bytes+4,argc>1);}

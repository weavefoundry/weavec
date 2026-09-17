void encode(unsigned char **out,unsigned code){unsigned n=code>255?4:1;for(unsigned i=n-1;i>0;i--)(*out)[i]=42;(*out)[0]=1;*out+=n+1;}
int main(void){unsigned char out[5];unsigned char *p=out;encode(&p,65536);return p[-1];}

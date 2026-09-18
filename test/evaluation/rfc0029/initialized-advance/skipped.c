void encode(unsigned char **out,unsigned code){unsigned n=code>255?4:1;for(unsigned i=n-1;i>0;i--){if(i!=2)(*out)[i]=42;}(*out)[0]=1;*out+=n;}
int main(void){unsigned char out[4];unsigned char *p=out;encode(&p,65536);return out[2];}

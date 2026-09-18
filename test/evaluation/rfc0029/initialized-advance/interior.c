void encode(unsigned char**,unsigned);
int main(int argc,char**argv){(void)argv;unsigned char out[5];unsigned char *p=out+1;encode(&p,argc>1?65536:1);if(p>out+1)return p[-1];return 0;}

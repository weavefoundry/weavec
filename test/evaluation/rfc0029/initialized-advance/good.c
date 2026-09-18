void encode(unsigned char**,unsigned);
int main(int argc,char**argv){(void)argv;unsigned char out[4];unsigned char *p=out;encode(&p,argc>1?65536:1);if(p>out)return p[-1];return 0;}

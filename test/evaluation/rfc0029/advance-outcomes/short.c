unsigned encode(unsigned char**,unsigned);
int main(int argc,char**argv){(void)argv;unsigned char out[3],*p=out;encode(&p,argc>1?65536:0);if(p>out)return p[-1];return 0;}

unsigned encode(unsigned char**,unsigned);
int main(int argc,char**argv){(void)argv;unsigned char out[5],*p=out+1;encode(&p,argc>1?65536:0);if(p>out+1)return p[-1];return 0;}

unsigned encode(unsigned char**,unsigned);
int main(int argc,char **argv){(void)argv;unsigned char out[5],*p=out+1;unsigned ok=encode(&p,argc>1?65536:0);if(!ok)return 0;return p[-1];}

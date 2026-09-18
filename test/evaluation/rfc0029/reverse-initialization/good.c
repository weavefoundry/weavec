int fill(unsigned char*,unsigned char);
int main(int argc,char**argv){(void)argv;unsigned char out[4];return fill(out,argc>1?4:2);}

void encode(unsigned char **,unsigned);
int main(void){unsigned char out[4];unsigned char *p=out;encode(&p,0x10000);return 0;}

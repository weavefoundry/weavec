void encode(unsigned char**,unsigned);
int main(void){unsigned char out[3];unsigned char *p=out;encode(&p,65536);return 0;}

void encode(unsigned char **,unsigned);
int main(void){const unsigned char out[4]={0};unsigned char *p=(unsigned char*)out;encode(&p,0x10000);return 0;}

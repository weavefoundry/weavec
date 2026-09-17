int decode(const unsigned char *,const unsigned char *,unsigned char **);
int main(void){const unsigned char in[2]={1,2},a[2]={0,0};unsigned char *p=(unsigned char*)a;return decode(in,in+2,&p);}

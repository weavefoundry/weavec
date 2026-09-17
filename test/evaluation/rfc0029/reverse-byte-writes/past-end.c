void encode(unsigned char **out, unsigned code) {
 unsigned char length;
 if(code<128)length=1;
 else if(code<2048)length=2;
 else if(code<65536)length=3;
 else length=4;
 unsigned char i;
 for(i=(unsigned char)(length);i>0;i--) (*out)[i]=42;
 (*out)[0]=1;
 *out+=length;
}
int main(void){unsigned char out[4];unsigned char *p=out;encode(&p,0x10000);return 0;}

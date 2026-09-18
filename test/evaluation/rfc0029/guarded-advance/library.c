unsigned encode(unsigned char **out,unsigned code) {
 if(code==0){*out+=1;return 0;}
 unsigned char n=code>255?4:1;
 unsigned char i;
 for(i=(unsigned char)(n-1);i>0;i--) (*out)[i]=42;
 (*out)[0]=1; *out+=n;return n;
}

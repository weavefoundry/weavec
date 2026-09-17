int fill(unsigned char *out,unsigned char length) {
 if(length<1 || length>4)return 0;
 unsigned char i;
 unsigned code=42;
 for(i=(unsigned char)(length-1);i>0;i--){out[i]=(unsigned char)code;code>>=1;}
 out[0]=1;
 return out[length-1];
}

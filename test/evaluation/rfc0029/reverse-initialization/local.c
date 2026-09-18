int main(int argc,char**argv){
 (void)argv;unsigned char out[4];unsigned char length=argc>1?4:2;
 unsigned char i;unsigned code=42;
 for(i=(unsigned char)(length-1);i>0;i--){out[i]=(unsigned char)code;code>>=1;}
 out[0]=1;return out[length-1];
}

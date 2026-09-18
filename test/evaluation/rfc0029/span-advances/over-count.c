unsigned char consume(const unsigned char *first,const unsigned char *last,int mode){
 unsigned char n=0;if(last-first<2)return 0;
 if(mode){n=5;if(last-(first+2)<2)return 0;}else n=2;
 return n;
}
int main(int argc,char**argv){(void)argv;const unsigned char input[8]={0};const unsigned char *p=input,*end=input+8;
 while(p<end){unsigned char n=consume(p,end,argc>1);if(!n)return 0;p+=n;}
 return p==end?0:1;
}

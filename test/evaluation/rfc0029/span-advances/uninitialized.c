unsigned char consume(const unsigned char*,const unsigned char*,int);
int main(int argc,char**argv){(void)argv;unsigned char input[8];input[0]=0;const unsigned char *p=input,*end=input+8;
 while(p<end){unsigned char n=consume(p,end,argc>1);if(!n)return 0;p+=n;}
 return p==end?0:1;
}

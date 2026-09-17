unsigned char consume(const unsigned char *first,const unsigned char *last,int mode){
 unsigned char n=0;if(last-first<2)return 0;
 if(mode){n=4;if(last-(first+2)<2)return 0;}else n=2;
 return n;
}

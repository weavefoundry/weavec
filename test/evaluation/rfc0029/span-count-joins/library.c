unsigned char consume(const unsigned char *first,const unsigned char *last,int mode) {
 unsigned char count=0;
 if(last-first<2)return 0;
 if(mode){count=4; if(last-(first+2)<2)return 0;}
 else {count=2;}
 return count;
}
int probe(const unsigned char *first,const unsigned char *last,int mode){
 if(last-first<1)return 0;
 unsigned char n=consume(first,last,mode);
 if(!n)return 0;
 return first[n-1];
}

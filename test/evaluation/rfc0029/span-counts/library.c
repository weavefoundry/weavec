unsigned consume(const unsigned char *first, const unsigned char *last, int mode) {
 if(last-first<2)return 0;
 if(mode){if(last-first<4)return 0;return 4;}
 return 2;
}
int probe(const unsigned char *first,const unsigned char *last,int mode){
 if(last-first<1)return 0;
 unsigned n=consume(first,last,mode);
 if(!n)return 0;
 return first[n-1];
}

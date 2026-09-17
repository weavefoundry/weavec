unsigned consume(const unsigned char *first,const unsigned char *last,int mode){
 unsigned count=0;
 if(last-first<2)return 0;
 if(mode){count=5;if(last-first<4)return 0;}
 else count=2;
 return count;
}
int main(void){const unsigned char bytes[4]={1,2,3,4};return bytes[consume(bytes,bytes+4,1)-1];}

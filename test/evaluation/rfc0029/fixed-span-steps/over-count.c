unsigned consume(const unsigned char *first,const unsigned char *last){if(last-first<1)return 0;(void)first[0];return 2;}
int main(void){const unsigned char src[9]={0};const unsigned char *p=src,*end=src+9;while(p<end){unsigned n=consume(p,end);if(!n)break;p+=n;}return 0;}

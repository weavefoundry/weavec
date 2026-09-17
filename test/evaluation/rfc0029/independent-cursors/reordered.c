int main(void){unsigned char dst[10];const unsigned char src[10]={0};unsigned char *q=dst;const unsigned char *p=src;while(p<src+10){*q=*p;p++;q++;}return 0;}

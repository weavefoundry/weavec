int main(void){const unsigned char src[10]={0};unsigned char dst[10];const unsigned char *p=src;unsigned char *q=dst;while(p<src+10){*q++=*p++;q++;}return 0;}

unsigned decode(const unsigned char *p,const unsigned char *end,unsigned char **out){if(end-p<2)return 0;(*out)[0]=p[1];(*out)+=3;return 2;}
int main(void){const unsigned char bytes[10]={0};unsigned char dst[10];const unsigned char *p=bytes,*end=bytes+10;unsigned char *out=dst;while(p<end){unsigned n=decode(p,end,&out);if(!n)break;p+=n;}return 0;}

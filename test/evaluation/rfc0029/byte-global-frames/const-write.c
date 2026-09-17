static int mutate(unsigned char *p){p[1]=92;return 0;}
int main(void){static const unsigned char text[]={97,98,99};return mutate((unsigned char*)text);}

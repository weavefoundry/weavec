int main(void){const unsigned char text[]="abc";const unsigned char *p=text;const unsigned char **slot=&p;while(p<text+4){if(*p=='\\')return p[4];(*slot)++;}return 0;}

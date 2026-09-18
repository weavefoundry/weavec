int main(void){const unsigned char text[]="abc";const unsigned char *p=text;while(p<text+4){if(*p=='\\')return p[4];++p;}return 0;}

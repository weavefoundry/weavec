int main(void){unsigned char text[]="abc";unsigned char *p=text;unsigned char **slot=&p;(*slot)[1]='\\';while(p<text+4){if(*p=='\\')return p[4];(*slot)++;}return 0;}

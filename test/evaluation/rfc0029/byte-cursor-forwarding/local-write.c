int main(void){unsigned char text[]="abc";text[1]='\\';const unsigned char *p=text;while(p<text+4){if(*p=='\\')return p[4];++p;}return 0;}

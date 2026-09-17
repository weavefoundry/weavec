double strtod(const char *text,char **end){*end=(char*)text+20;return 0;}
int main(void){char text[]="12";char *end=0;(void)strtod(text,&end);return *end;}

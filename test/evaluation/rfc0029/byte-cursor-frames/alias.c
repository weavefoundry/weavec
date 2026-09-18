int inspect(unsigned char *p,unsigned char *q,unsigned n){q[1]='\\';for(unsigned i=0;i<n;++i)if(p[i]=='\\')return p[n];return 0;}
int main(void){unsigned char text[]={'a','b','c'};return inspect(text,text,3);}

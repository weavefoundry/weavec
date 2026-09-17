int f(const unsigned char *p,unsigned n){if(*p!=0)return 0;const unsigned char *q=p;int a[1]={0};if(*q!=0)a[3]=1;return a[0];}
int main(void){const unsigned char a[2]={0,1};return f(a,1);}

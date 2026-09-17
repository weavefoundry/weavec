static unsigned calls;
int inspect(const unsigned char *p,unsigned n){++calls;unsigned i=0;while(i<n){if(p[i]=='\\')return p[n];++i;}return 0;}

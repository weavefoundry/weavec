static int inspect_bytes(const unsigned char *p,unsigned n){unsigned i=0;while(i<n){if(p[i]=='\\')return p[n];++i;}return 0;}
int inspect(const unsigned char *p,unsigned n){return inspect_bytes(p,n);}

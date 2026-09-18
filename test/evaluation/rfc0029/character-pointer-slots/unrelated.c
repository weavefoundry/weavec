static void set(float **out,float *p) { *out=p; }
int main(void) { float a=0; int *p=0; set((float **)&p,&a); return *p; }

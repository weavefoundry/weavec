int inspect(const unsigned char *,unsigned,unsigned);
int main(void){unsigned char p[]="abc"; p[1]=120; return inspect(p,4,4);}

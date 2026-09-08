// RFC 0018 fixed checked-code evaluation: recorded assertion trust.
#include <weavec.h>
int main(int argc,char **argv) { char b[4]={0}; WEAVEC_ASSUME(argc>=0 && argc<4); return b[argc]; }

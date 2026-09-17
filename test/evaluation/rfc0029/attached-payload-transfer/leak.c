#include "api.h"
int main(void){reset_hooks();struct node *o=create();if(!o)return 0;struct node *i=create();if(!i){destroy(o);return 0;}
if(!attach_wrapper(o,i)){destroy(o);return 0;}
destroy(o);return 0;}

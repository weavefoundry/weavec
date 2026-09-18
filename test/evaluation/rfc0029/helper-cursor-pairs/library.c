unsigned decode(const unsigned char *p,const unsigned char *end,unsigned char **out){if(end-p<2)return 0;(*out)[0]=p[1];(*out)++;return 2;}

#include <stdlib.h>
#include <string.h>
#include <limits.h>
struct reader { const unsigned char *data; size_t position,capacity,extra; };
int scan(struct reader *r) {
 char *out;
 size_t i=0,count=0;int decimal=0;
 if(!r||!r->data)return 0;
 for(i=0;r->position+i<r->capacity;i++) {
  switch((r->data+r->position)[i]) {
  case '0':case '1':++count;break;
  case '.':++count;decimal=1;break;
  default:goto done;
  }
 }
done:;
 out=malloc(count+1);if(!out)return 0;
 memcpy(out,r->data+r->position,count);out[count]=0;
 if(decimal)for(i=0;i<count;i++)if(out[i]=='.')out[i]='.';
 double value=strtod(out,0);free(out);
 if(value>=INT_MAX)return INT_MAX;else if(value<=INT_MIN)return INT_MIN;else return (int)value;
}
int main(void){const unsigned char input[]="111";struct reader r={input,0,sizeof input,0};return scan(&r);}

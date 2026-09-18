#include <stdlib.h>
#include <string.h>
struct reader { const unsigned char *data; size_t position,capacity,extra; };
long scan(struct reader *r) {
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
 char *out=malloc(count+1);if(!out)return 0;
 memcpy(out,r->data+r->position,count);out[count]=0;
 if(decimal)for(i=0;i<count;i++)if(out[i]=='.')out[i]='.';
 char *end=0;(void)strtod(out,&end);free(out);long result=end-out;return result;
}
int main(void){const unsigned char input[]="1";struct reader r={input,0,sizeof input,0};return (int)scan(&r);}

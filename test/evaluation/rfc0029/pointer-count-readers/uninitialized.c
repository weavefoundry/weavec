#include <stdlib.h>
int scan(const unsigned char *data,size_t size) {
 const unsigned char *first=data+1,*last=data+1;
 while((size_t)(last-data)<size && *last!='"') {
  if(*last=='\\') {if((size_t)(last+1-data)>=size)return 0;last++;}
  last++;
 }
 if((size_t)(last-data)>=size || *last!='"')return 0;
 while(first<last) {if(*first=='\\')first++;first++;}
 return first==last;
}
int main(void){unsigned char data[7];data[0]=34;return scan(data,sizeof data);}

int main(void){const unsigned char data[6]={0};const unsigned char *first=data+1,*last=data+1;
 for(;last<data+5;last++){}
 for(;first<last;first++){}
 return first==last?0:1;
}

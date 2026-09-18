int main(void){const unsigned char data[6]={0},other[6]={0};const unsigned char *first=data+1,*last=data+1;
 while(last<data+5)last++;
 last=other+5;while(first<last)first++;
 return first==last?0:1;
}

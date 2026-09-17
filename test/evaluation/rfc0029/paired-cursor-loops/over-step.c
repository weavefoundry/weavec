int main(void){const unsigned char data[6]={0};const unsigned char *first=data+1,*last=data+1;
 while(last<data+5)last++;
 while(first<last)first+=3;
 return first==last?0:1;
}

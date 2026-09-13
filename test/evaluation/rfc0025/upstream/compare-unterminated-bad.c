#include "../../../../build/corpus/cJSON-program/cJSON.c"
int main(void){char s[1]={65};cJSON a={0},b={0};a.type=cJSON_String;b.type=cJSON_String;a.valuestring=s;b.valuestring="a";return cJSON_Compare(&a,&b,1);}

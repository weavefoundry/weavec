#include "../../../../build/corpus/cJSON-program/cJSON.c"
int main(void){cJSON a={0},b={0};a.type=cJSON_String;b.type=cJSON_String;a.valuestring="a";b.valuestring="b";return cJSON_Compare(&a,&b,1);}

#include "../../../../build/corpus/cJSON-program/cJSON.c"
int main(void){cJSON a={0},b={0};a.type=cJSON_True|cJSON_IsReference;b.type=cJSON_True;return cJSON_Compare(&a,&b,1);}

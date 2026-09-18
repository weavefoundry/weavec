#include "api.h"
int clamp(double x){x=__builtin_nan("");if(x>=INT_MAX)return INT_MAX;if(x<=(double)INT_MIN)return INT_MIN;return (int)x;}

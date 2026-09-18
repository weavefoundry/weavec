#include "api.h"
int clamp(double x){if(x>=INT_MAX)return INT_MAX;if(x<=(double)INT_MIN)return INT_MIN;return (int)x;}

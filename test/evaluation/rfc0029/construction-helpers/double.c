#include <stdlib.h>
struct node { unsigned char value; struct node *left, *right; };
static void destroy(struct node *p) { if(p){destroy(p->left);destroy(p->right);free(p);} }
static unsigned char first(const unsigned char *data) { return *data; }
struct node *build(const unsigned char *data, size_t n) {
    if (!n) return 0;
    struct node *p=calloc(1,sizeof *p);
    if (!p) return 0;
    p->value=first(data);
    if (n>1) {
      p->left=build(data+1,n-1);
      if (!p->left) {free(p);return 0;}
      p->right=build(data+1,n-1);
      if (!p->right) {destroy(p->left);destroy(p->left);free(p);return 0;}
    }
    return p;
}
int main(void) {const unsigned char data[]={1,2,3};struct node *p=build(data,3);destroy(p);return 0;}

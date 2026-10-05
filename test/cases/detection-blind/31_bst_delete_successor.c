// RFC 0034 gate F6: blind detection set (written after the first set, by an author who
// had not seen it). -DFIX selects the fixed twin.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT:
/*
 * Binary search tree deletion. For a node with two children it finds the
 * in-order successor, deletes the successor from the right subtree (which
 * frees it), and only then copies the successor's key into the node.
 * Category: temporal (heap use-after-free, statement order).
 * Why it may be missed: the two statements are each correct; swapping them
 * is the whole fix, and the recursive call hides that it frees succ.
 */
#include <stdio.h>
#include <stdlib.h>

struct node {
    int key;
    struct node *left, *right;
};

static struct node *bst_insert(struct node *root, int key)
{
    if (!root) {
        struct node *n = malloc(sizeof *n);
        if (n) {
            n->key = key;
            n->left = n->right = NULL;
        }
        return n;
    }
    if (key < root->key)
        root->left = bst_insert(root->left, key);
    else if (key > root->key)
        root->right = bst_insert(root->right, key);
    return root;
}

static struct node *bst_delete(struct node *root, int key)
{
    if (!root)
        return NULL;
    if (key < root->key) {
        root->left = bst_delete(root->left, key);
    } else if (key > root->key) {
        root->right = bst_delete(root->right, key);
    } else if (!root->left || !root->right) {
        struct node *child = root->left ? root->left : root->right;
        free(root);
        return child;
    } else {
        struct node *succ = root->right;
        while (succ->left)
            succ = succ->left;
#ifdef FIX
        root->key = succ->key;
        root->right = bst_delete(root->right, succ->key);
#else
        root->right = bst_delete(root->right, succ->key);
        root->key = succ->key; // STOP
#endif
    }
    return root;
}

static size_t inorder(const struct node *n, int *out, size_t k)
{
    if (!n)
        return k;
    k = inorder(n->left, out, k);
    out[k++] = n->key;
    return inorder(n->right, out, k);
}

static void bst_free(struct node *n)
{
    if (n) {
        bst_free(n->left);
        bst_free(n->right);
        free(n);
    }
}

int main(void)
{
    static const int keys[] = {50, 30, 70, 20, 40, 60, 80};
    static const int want[] = {30, 40, 60, 70, 80};
    struct node *root = NULL;
    for (size_t i = 0; i < 7; i++)
        root = bst_insert(root, keys[i]);
    root = bst_delete(root, 20); /* leaf */
    root = bst_delete(root, 50); /* two children */
    int out[8];
    size_t n = inorder(root, out, 0);
    int ok = n == 5;
    for (size_t i = 0; ok && i < n; i++)
        ok = out[i] == want[i];
    for (size_t i = 0; i < n; i++)
        printf("%d ", out[i]);
    printf("\n");
    bst_free(root);
    return ok ? 0 : 1;
}

/**
 * @file        RedBlackTree.c
 * @brief       LinuxARM-PublicLib-内核风格侵入式红黑树-实现文件
 * @details     IMX6ULL平台
 *              红黑树节点嵌入到用户结构体中，不分配/释放节点内存。
 *              参照 Linux Kernel rbtree 实现，使用侵入式设计。
 *
 *              红黑树五大性质（必须始终维护）:
 *              1. 每个节点是红色或黑色
 *              2. 根节点是黑色
 *              3. 叶子节点（NIL/NULL）是黑色
 *              4. 红色节点的两个子节点都是黑色（不能有连续红节点）
 *              5. 从任一节点到其所有叶子节点的路径包含相同数目的黑色节点
 *
 * @author      zlzksrl
 * @Version     V1.0.0
 * @date        2026-05-08
 * @copyright   copyright (C) 2026
 */

/**
 * @date        2026-05-08
 * @Version     V1.0.0
 * @brief       创建文件，实现内核风格侵入式红黑树
 * @author      zlzksrl
 */

#include "../include/RedBlackTree.h"
#include <stddef.h>

/* ======================== 内部函数声明 ======================== */

/**
 * @func        RedBlackTree_rotate_left
 * @brief       红黑树内部-左旋操作
 */
static void RedBlackTree_rotate_left(struct rb_root *root, struct rb_node *node);

/**
 * @func        RedBlackTree_rotate_right
 * @brief       红黑树内部-右旋操作
 */
static void RedBlackTree_rotate_right(struct rb_root *root, struct rb_node *node);

/**
 * @func        RedBlackTree_insert_fixup
 * @brief       红黑树内部-插入后修复平衡
 */
static void RedBlackTree_insert_fixup(struct rb_root *root, struct rb_node *node);

/**
 * @func        RedBlackTree_delete_fixup
 * @brief       红黑树内部-删除后修复平衡
 */
static void RedBlackTree_delete_fixup(struct rb_root *root, struct rb_node *parent,
                            struct rb_node *node);

/**
 * @func        RedBlackTree_transplant
 * @brief       红黑树内部-节点替换（将 v 替换 u 的位置）
 */
static void RedBlackTree_transplant(struct rb_root *root, struct rb_node *u,
                          struct rb_node *v);

/**
 * @func        RedBlackTree_subtree_min
 * @brief       红黑树内部-获取子树最小节点（最左节点）
 */
static struct rb_node *RedBlackTree_subtree_min(struct rb_node *node);

/**
 * @func        RedBlackTree_subtree_max
 * @brief       红黑树内部-获取子树最大节点（最右节点）
 */
static struct rb_node *RedBlackTree_subtree_max(struct rb_node *node);

/**
 * @func        RedBlackTree_destroy_subtree
 * @brief       红黑树内部-递归销毁子树（后序遍历）
 */
static void RedBlackTree_destroy_subtree(struct rb_node *node,
                               void (*free_fn)(struct rb_node *, void *),
                               void *arg);


/* ======================== 核心操作实现 ======================== */

/**
 * @func        RedBlackTree_insert
 * @brief       向红黑树插入节点
 * @details     插入流程:
 *              1. 参数校验
 *              2. 新节点初始化为红色（性质5不会被破坏，可能破坏性质4）
 *              3. BST 标准查找找到插入位置
 *              4. 链接到父节点
 *              5. 调用 insert_fixup 修复红黑树性质
 *
 *              时间复杂度: O(log n)
 */
int RedBlackTree_insert(struct rb_root *root, struct rb_node *node,
              int (*cmp)(struct rb_node *, struct rb_node *, void *),
              void *arg)
{
    /* ---- 参数合法性检查 ---- */
    if (root == NULL || node == NULL || cmp == NULL)
    {
        return -1;
    }

    /* ---- 初始化新节点 ----
     * 新节点设为红色。插入红色节点不会影响任何路径上的黑色节点数（性质5），
     * 但如果父节点也是红色，则违反性质4（不能有连续红节点），需要后续修复。
     * 所有指针置 NULL，确保不会误用旧数据。
     */
    node->color  = REDBLACKTREE_RED;
    node->left   = NULL;
    node->right  = NULL;
    node->parent = NULL;

    /* ---- 空树特判 ----
     * 若树为空，新节点直接成为根节点。
     * 根节点必须为黑色（性质2），因此将颜色从红色改为黑色。
     * 这不会违反性质5，因为之前没有路径。
     */
    if (root->rb_node == NULL) {
        root->rb_node = node;
        root->rb_node->color = REDBLACKTREE_BLACK;
        root->count = 1;
        return 0;
    }

    /* ---- BST 查找插入位置 ----
     * 从根节点开始，通过比较函数确定向左或向右走。
     * 如果键值已存在（cmp返回0），则插入失败（不允许重复键值）。
     * 循环结束后:
     *   - parent 指向新节点的父节点
     *   - cmp_result 保存最后一次比较结果，决定新节点是左子还是右子
     */
    struct rb_node *parent = NULL;
    struct rb_node *current = root->rb_node;
    int cmp_result = 0;

    while (current != NULL) {
        parent = current;
        cmp_result = cmp(node, current, arg);

        if (cmp_result < 0) {
            /* 新节点键值较小，进入左子树 */
            current = current->left;
        } else if (cmp_result > 0) {
            /* 新节点键值较大，进入右子树 */
            current = current->right;
        } else {
            /* 键值已存在，不允许重复插入 */
            return -1;
        }
    }

    /* ---- 将新节点链接到树中 ----
     * 根据 cmp_result 的符号决定新节点是 parent 的左子还是右子。
     */
    node->parent = parent;
    if (cmp_result < 0) {
        parent->left = node;
    } else {
        parent->right = node;
    }

    /* 节点计数递增 */
    root->count++;

    /* ---- 修复红黑树性质 ----
     * 插入红色节点后可能违反性质4（红色节点的子节点也必须是黑色）。
     * insert_fixup 通过重新着色和旋转恢复平衡。
     */
    RedBlackTree_insert_fixup(root, node);

    return 0;
}

/**
 * @func        RedBlackTree_delete
 * @brief       从红黑树删除节点
 * @details     删除流程（参照 CLRS 算法）:
 *              1. 确定被移除的实际节点及其颜色
 *              2. 用子节点替换被移除节点（transplant）
 *              3. 如果被移除的节点是黑色，需要修复平衡
 *
 *              三种情况:
 *              - 无左子: 直接用右子替换
 *              - 无右子: 直接用左子替换
 *              - 有两子: 找后继节点（右子树最小值）替换
 *
 *              时间复杂度: O(log n)
 */
int RedBlackTree_delete(struct rb_root *root, struct rb_node *node)
{
    if (root == NULL || node == NULL)
    {
        return -1;
    }

    struct rb_node *child = NULL;   /* 被移除位置的实际子节点（可能为 NULL） */
    struct rb_node *parent = NULL;  /* child 的父节点（用于 delete_fixup） */
    unsigned int color = node->color; /* 记录被移除节点的颜色 */

    if (node->left == NULL) {
        /* ---- 情况1: 只有右子节点（或无子节点） ----
         * 直接用 node->right 替换 node。
         * child 可能为 NULL（无子节点的情况）。
         * 必须在 transplant 之前保存 parent，因为 transplant 会修改树结构。
         */
        child = node->right;
        parent = node->parent;
        RedBlackTree_transplant(root, node, child);
    } else if (node->right == NULL) {
        /* ---- 情况2: 只有左子节点 ----
         * 直接用 node->left 替换 node。
         */
        child = node->left;
        parent = node->parent;
        RedBlackTree_transplant(root, node, child);
    } else {
        /* ---- 情况3: 有两个子节点 ----
         * 使用中序后继（右子树的最小节点）来替换 node。
         * 后继节点一定没有左子节点（因为它是子树最小值）。
         */
        struct rb_node *successor = RedBlackTree_subtree_min(node->right);

        /* 记录后继节点的颜色（这是实际被移除位置的节点颜色）
         * 如果后继是黑色，删除后会破坏黑高平衡，需要修复。 */
        color = successor->color;

        /* 后继的右子节点将成为替换后继位置的节点 */
        child = successor->right;

        if (successor->parent == node) {
            /* ---- 子情况3a: 后继是 node 的直接右子 ----
             *
             * 图示（删除 P，后继 S 是 P 的直接右子）:
             *     P                S
             *    / \     =>       / \
             *   L   S            L   C
             *        \
             *         C (child)
             *
             * 此时 child 的父节点就是 successor（因为 successor 取代了 node，
             * 而 child 原来就是 successor 的子节点）。
             * 注意: 必须将 parent 设为 successor，而非 successor->parent，
             * 因为在后续 transplant 后 successor 会成为 child 的父节点。
             */
            parent = successor;
        } else {
            /* ---- 子情况3b: 后继在更深层 ----
             *
             * 图示（删除 P，后继 S 在右子树深处）:
             *     P                  S
             *    / \       =>       / \
             *   L   PR             L  PR
             *      / \                 / \
             *     S   ...    =>  (SP)  ...
             *      \                  \
             *       C (child)          C (child)
             *
             * 步骤:
             * 1. 先将 successor 从原位置移除，用 child 替换
             * 2. 将 successor 放到 node 的位置
             *
             * 必须在 transplant(successor) 之前保存 parent，
             * 因为 transplant 后 successor->parent 会改变。
             */
            parent = successor->parent;

            /* 步骤1: 用 child 替换 successor 的位置 */
            RedBlackTree_transplant(root, successor, child);

            /* 步骤2: successor 接管 node 的右子树 */
            successor->right = node->right;
            if (successor->right != NULL) {
                successor->right->parent = successor;
            }
        }

        /* 步骤3: 用 successor 替换 node 的位置 */
        RedBlackTree_transplant(root, node, successor);

        /* 步骤4: successor 接管 node 的左子树 */
        successor->left = node->left;
        if (successor->left != NULL) {
            successor->left->parent = successor;
        }

        /* successor 继承 node 的颜色，保持该位置的颜色不变 */
        successor->color = node->color;
    }

    /* 清除被删除节点的链接，防止悬空指针被误用 */
    node->parent = NULL;
    node->left   = NULL;
    node->right  = NULL;

    /* 节点计数递减 */
    root->count--;

    /* ---- 删除后修复 ----
     * 只有当被移除的节点（或后继节点）是黑色时才需要修复。
     * 删除红色节点不会破坏任何红黑树性质。
     * 删除黑色节点会导致:
     *   - 通过该节点的路径黑高减少（违反性质5）
     *   - 如果其红色子节点被提升到黑色父节点位置，可能违反性质4
     *
     * 传入 parent 和 child:
     *   - child 是需要修复的"双重黑"节点（可能为 NULL）
     *   - parent 是 child 的父节点（当 child 为 NULL 时需要用到）
     */
    if (color == REDBLACKTREE_BLACK) {
        RedBlackTree_delete_fixup(root, parent, child);
    }

    return 0;
}

/**
 * @func        RedBlackTree_search
 * @brief       在红黑树中查找节点
 * @details     标准二叉搜索树查找。
 *              利用红黑树的 BST 性质，从根节点开始比较，
 *              小于走左子树，大于走右子树，等于则找到。
 *
 *              时间复杂度: O(log n)（红黑树保证平衡）
 */
struct rb_node *RedBlackTree_search(const struct rb_root *root, struct rb_node *key,
                           int (*cmp)(struct rb_node *, struct rb_node *, void *),
                           void *arg)
{
    if (root == NULL || key == NULL || cmp == NULL)
    {
        return NULL;
    }

    /* 从根节点开始，沿 BST 路径查找 */
    struct rb_node *current = root->rb_node;
    while (current != NULL) {
        int cmp_result = cmp(key, current, arg);
        if (cmp_result < 0) {
            /* key 较小，进入左子树 */
            current = current->left;
        } else if (cmp_result > 0) {
            /* key 较大，进入右子树 */
            current = current->right;
        } else {
            /* 找到匹配节点 */
            return current;
        }
    }

    /* 未找到 */
    return NULL;
}

/**
 * @func        RedBlackTree_replace
 * @brief       替换树中的节点
 * @details     将 old_node 从树中移除，将 new_node 放到 old_node 的位置。
 *              new_node 继承 old_node 的颜色、父节点、子节点。
 *              不触发重平衡，因此要求新旧节点的键值相同。
 *
 *              时间复杂度: O(1)
 */
int RedBlackTree_replace(struct rb_root *root, struct rb_node *old_node,
               struct rb_node *new_node)
{
    if (root == NULL || old_node == NULL || new_node == NULL)
    {
        return -1;
    }

    /* old_node 和 new_node 相同时无需替换，避免清除自身指针 */
    if (old_node == new_node)
    {
        return -1;
    }

    /* ---- 复制旧节点的树链接到新节点 ---- */
    new_node->parent = old_node->parent;
    new_node->left   = old_node->left;
    new_node->right  = old_node->right;
    new_node->color  = old_node->color;

    /* ---- 更新父节点的指针 ----
     * 如果 old_node 是根节点，更新 root->rb_node；
     * 否则更新父节点的 left 或 right 指针。
     */
    if (old_node->parent == NULL) {
        root->rb_node = new_node;
    } else {
        if (old_node == old_node->parent->left) {
            old_node->parent->left = new_node;
        } else {
            old_node->parent->right = new_node;
        }
    }

    /* ---- 更新子节点的父指针 ----
     * 让 old_node 的子节点指向 new_node。
     */
    if (old_node->left != NULL) {
        old_node->left->parent = new_node;
    }
    if (old_node->right != NULL) {
        old_node->right->parent = new_node;
    }

    /* 清除旧节点的链接，防止悬空指针被误用 */
    old_node->parent = NULL;
    old_node->left   = NULL;
    old_node->right  = NULL;

    return 0;
}


/* ======================== 迭代操作实现 ======================== */

/**
 * @func        RedBlackTree_first
 * @brief       获取红黑树最小节点（最左节点）
 * @details     红黑树的 BST 性质保证最小值在最左位置。
 *              从根节点一直向左走到底即可。
 *
 *              时间复杂度: O(log n)
 */
struct rb_node *RedBlackTree_first(const struct rb_root *root)
{
    if (root == NULL || root->rb_node == NULL)
    {
        return NULL;
    }
    return RedBlackTree_subtree_min(root->rb_node);
}

/**
 * @func        RedBlackTree_last
 * @brief       获取红黑树最大节点（最右节点）
 * @details     红黑树的 BST 性质保证最大值在最右位置。
 *              从根节点一直向右走到底即可。
 *
 *              时间复杂度: O(log n)
 */
struct rb_node *RedBlackTree_last(const struct rb_root *root)
{
    if (root == NULL || root->rb_node == NULL)
    {
        return NULL;
    }
    return RedBlackTree_subtree_max(root->rb_node);
}

/**
 * @func        RedBlackTree_next
 * @brief       获取后继节点（中序遍历的下一个节点）
 * @details     分两种情况:
 *              1. 有右子树: 后继是右子树的最小节点
 *              2. 无右子树: 向上找第一个"左拐"的祖先
 *                 （即 node 是 parent 的左子时，parent 就是后继）
 *
 *              时间复杂度: O(log n)，均摊 O(1)
 */
struct rb_node *RedBlackTree_next(const struct rb_node *node)
{
    if (node == NULL)
    {
        return NULL;
    }

    /* 情况1: 有右子树，后继是右子树的最左节点 */
    if (node->right != NULL) {
        return RedBlackTree_subtree_min(node->right);
    }

    /* 情况2: 无右子树，向上回溯找第一个"左拐"祖先
     *
     * 图示: 在中序遍历中，如果当前节点是父节点的右子，
     * 说明父节点已经在当前节点之前被访问过了，需要继续向上。
     * 当找到第一个"当前节点是父节点左子"的情况时，
     * 该父节点就是中序后继。
     */
    struct rb_node *parent = node->parent;
    struct rb_node *current = (struct rb_node *)node;

    while (parent != NULL && current == parent->right) {
        current = parent;
        parent = parent->parent;
    }

    return parent;
}

/**
 * @func        RedBlackTree_prev
 * @brief       获取前驱节点（中序遍历的上一个节点）
 * @details     分两种情况:
 *              1. 有左子树: 前驱是左子树的最大节点
 *              2. 无左子树: 向上找第一个"右拐"的祖先
 *                 （即 node 是 parent 的右子时，parent 就是前驱）
 *
 *              时间复杂度: O(log n)，均摊 O(1)
 */
struct rb_node *RedBlackTree_prev(const struct rb_node *node)
{
    if (node == NULL)
    {
        return NULL;
    }

    /* 情况1: 有左子树，前驱是左子树的最右节点 */
    if (node->left != NULL) {
        return RedBlackTree_subtree_max(node->left);
    }

    /* 情况2: 无左子树，向上回溯找第一个"右拐"祖先
     * 与 next 对称: 找第一个"当前节点是父节点右子"的情况。
     */
    struct rb_node *parent = node->parent;
    struct rb_node *current = (struct rb_node *)node;

    while (parent != NULL && current == parent->left) {
        current = parent;
        parent = parent->parent;
    }

    return parent;
}


/* ======================== 批量操作实现 ======================== */

/**
 * @func        RedBlackTree_destroy
 * @brief       销毁红黑树中所有节点
 * @details     使用后序遍历递归释放所有节点，然后重置根结构体。
 *              如果 free_fn 为 NULL，只断开所有链接但不释放内存。
 *
 * @warning     内部使用递归后序遍历，极深的树可能导致栈溢出。
 *              对于嵌入式系统，如果节点数量可能很大，建议使用
 *              RedBlackTree_for_each_safe + RedBlackTree_delete 的迭代方式。
 */
int RedBlackTree_destroy(struct rb_root *root,
               void (*free_fn)(struct rb_node *, void *),
               void *arg)
{
    if (root == NULL)
    {
        return -1;
    }

    /* 递归销毁所有节点 */
    RedBlackTree_destroy_subtree(root->rb_node, free_fn, arg);

    /* 重置根结构体 */
    root->rb_node = NULL;
    root->count = 0;

    return 0;
}

/**
 * @func        RedBlackTree_traverse
 * @brief       中序遍历红黑树
 * @details     使用 RedBlackTree_first/RedBlackTree_next 进行迭代式中序遍历。
 *              预存下一个节点，允许回调函数安全地删除当前节点。
 *
 *              遍历顺序: 键值从小到大。
 */
int RedBlackTree_traverse(const struct rb_root *root,
                int (*callback)(struct rb_node *, void *),
                void *arg)
{
    if (root == NULL || callback == NULL)
    {
        return -1;
    }

    /* 使用 first/next 进行中序遍历，预存 next 以支持安全删除 */
    struct rb_node *node = RedBlackTree_first(root);
    struct rb_node *next = NULL;

    while (node != NULL) {
        next = RedBlackTree_next(node);
        int ret = callback(node, arg);
        if (ret != 0)
        {
            /* 回调返回非0，提前终止遍历 */
            return ret;
        }
        node = next;
    }

    return 0;
}


/* ======================== 内部函数实现 ======================== */

/**
 * @func        RedBlackTree_rotate_left
 * @brief       红黑树内部-左旋操作
 * @details     以 node 为支点进行左旋:
 *
 *     旋转前:          旋转后:
 *        P                P
 *        |                |
 *        node             right
 *       / \              /    \
 *      L   right  =>  node    R3
 *         / \         / \
 *        R2  R3      L   R2
 *
 *   步骤:
 *   1. right 的左子(R2)成为 node 的右子
 *   2. right 取代 node 在父节点中的位置
 *   3. node 成为 right 的左子
 *
 * @param[in]   root: 红黑树根（需要更新根节点指针）
 * @param[in]   node: 旋转支点（将成为其右子的左子）
 *
 * @note        调用前必须保证 node->right != NULL
 */
static void RedBlackTree_rotate_left(struct rb_root *root, struct rb_node *node)
{
    if (root == NULL || node == NULL || node->right == NULL)
    {
        return;
    }

    /* 保存右子节点（旋转后将上升的节点） */
    struct rb_node *right = node->right;

    /* 步骤1: right 的左子树挂到 node 的右边 */
    node->right = right->left;
    if (right->left != NULL) {
        right->left->parent = node;
    }

    /* 步骤2: right 取代 node 的位置 */
    right->parent = node->parent;
    if (node->parent == NULL) {
        /* node 是根节点，更新根指针 */
        root->rb_node = right;
    } else if (node == node->parent->left) {
        node->parent->left = right;
    } else {
        node->parent->right = right;
    }

    /* 步骤3: node 成为 right 的左子 */
    right->left = node;
    node->parent = right;
}

/**
 * @func        RedBlackTree_rotate_right
 * @brief       红黑树内部-右旋操作
 * @details     以 node 为支点进行右旋（左旋的镜像）:
 *
 *     旋转前:          旋转后:
 *        P                P
 *        |                |
 *        node             left
 *       / \              /    \
 *     left  R    =>    L1     node
 *     / \                     / \
 *   L1  L2                  L2   R
 *
 *   步骤:
 *   1. left 的右子(L2)成为 node 的左子
 *   2. left 取代 node 在父节点中的位置
 *   3. node 成为 left 的右子
 *
 * @param[in]   root: 红黑树根（需要更新根节点指针）
 * @param[in]   node: 旋转支点（将成为其左子的右子）
 *
 * @note        调用前必须保证 node->left != NULL
 */
static void RedBlackTree_rotate_right(struct rb_root *root, struct rb_node *node)
{
    if (root == NULL || node == NULL || node->left == NULL)
    {
        return;
    }

    /* 保存左子节点（旋转后将上升的节点） */
    struct rb_node *left = node->left;

    /* 步骤1: left 的右子树挂到 node 的左边 */
    node->left = left->right;
    if (left->right != NULL) {
        left->right->parent = node;
    }

    /* 步骤2: left 取代 node 的位置 */
    left->parent = node->parent;
    if (node->parent == NULL) {
        /* node 是根节点，更新根指针 */
        root->rb_node = left;
    } else if (node == node->parent->right) {
        node->parent->right = left;
    } else {
        node->parent->left = left;
    }

    /* 步骤3: node 成为 left 的右子 */
    left->right = node;
    node->parent = left;
}

/**
 * @func        RedBlackTree_insert_fixup
 * @brief       红黑树内部-插入修复
 * @details     插入红色节点后，可能违反性质4（红色节点的子节点必须是黑色）。
 *              修复策略分三大类情况（每类有镜像版本）:
 *
 *              Case 1: 叔节点是红色
 *                - 将父节点和叔节点染黑，祖父染红
 *                - 祖父成为新的"问题节点"，继续向上修复
 *
 *              Case 2: 叔节点是黑色，且节点是"内侧孙"（LR 或 RL 型）
 *                - 对父节点做一次旋转，转化为 Case 3
 *
 *              Case 3: 叔节点是黑色，且节点是"外侧孙"（LL 或 RR 型）
 *                - 父节点染黑，祖父染红
 *                - 对祖父做一次旋转，修复完成
 *
 *              循环条件: 当前节点不是根，且父节点是红色
 *              循环结束后: 确保根节点为黑色（性质2）
 *
 * @param[in]   root: 红黑树根
 * @param[in]   node: 新插入的节点（可能需要向上迭代修复）
 *
 * @note        最坏情况 O(log n) 次迭代（Case 1 可能一直向上传播），
 *              但 Case 2/3 只需一次旋转即完成，均摊旋转次数 O(1)。
 */
static void RedBlackTree_insert_fixup(struct rb_root *root, struct rb_node *node)
{
    if (root == NULL || node == NULL)
    {
        return;
    }

    /* 循环条件: 父节点是红色（违反性质4）
     * 因为根节点始终为黑色，所以父节点存在时祖父节点也一定存在
     */
    while (node->parent != NULL && node->parent->color == REDBLACKTREE_RED) {

        if (node->parent == node->parent->parent->left) {
            /* ---- 父节点是祖父的左子 ---- */
            struct rb_node *uncle = node->parent->parent->right;

            if (uncle != NULL && uncle->color == REDBLACKTREE_RED) {
                /* ---- Case 1: 叔节点是红色 ----
                 *
                 *        G(黑)             G(红) ← 新的问题节点
                 *       / \              / \
                 *     P(红) U(红)  =>  P(黑) U(黑)
                 *     /                /
                 *   N(红)            N(红)
                 *
                 * 策略: 父、叔染黑，祖父染红，继续向上修复。
                 * 这不会影响黑高（祖父位置的黑高不变）。
                 */
                node->parent->color = REDBLACKTREE_BLACK;
                uncle->color = REDBLACKTREE_BLACK;
                node->parent->parent->color = REDBLACKTREE_RED;
                node = node->parent->parent;
            } else {
                if (node == node->parent->right) {
                    /* ---- Case 2: LR 型（节点是父的右子，父是祖父的左子） ----
                     *
                     *        G(黑)             G(黑)
                     *       / \              / \
                     *     P(红) U(黑)  =>  N(红) U(黑)
                     *       \              /
                     *       N(红)        P(红)
                     *
                     * 策略: 对父节点左旋，转化为 Case 3。
                     */
                    node = node->parent;
                    RedBlackTree_rotate_left(root, node);
                }
                /* ---- Case 3: LL 型（节点是父的左子，父是祖父的左子） ----
                 *
                 *        G(黑)             P(黑)
                 *       / \              / \
                 *     P(红) U(黑)  =>  N(红) G(红)
                 *     /                        \
                 *   N(红)                      U(黑)
                 *
                 * 策略: 父染黑，祖父染红，对祖父右旋。修复完成。
                 */
                node->parent->color = REDBLACKTREE_BLACK;
                node->parent->parent->color = REDBLACKTREE_RED;
                RedBlackTree_rotate_right(root, node->parent->parent);
            }
        } else {
            /* ---- 父节点是祖父的右子（上述情况的镜像） ---- */
            struct rb_node *uncle = node->parent->parent->left;

            if (uncle != NULL && uncle->color == REDBLACKTREE_RED) {
                /* ---- Case 1 镜像: 叔节点是红色 ---- */
                node->parent->color = REDBLACKTREE_BLACK;
                uncle->color = REDBLACKTREE_BLACK;
                node->parent->parent->color = REDBLACKTREE_RED;
                node = node->parent->parent;
            } else {
                if (node == node->parent->left) {
                    /* ---- Case 2 镜像: RL 型 ----
                     * 策略: 对父节点右旋，转化为 Case 3。
                     */
                    node = node->parent;
                    RedBlackTree_rotate_right(root, node);
                }
                /* ---- Case 3 镜像: RR 型 ----
                 * 策略: 父染黑，祖父染红，对祖父左旋。修复完成。
                 */
                node->parent->color = REDBLACKTREE_BLACK;
                node->parent->parent->color = REDBLACKTREE_RED;
                RedBlackTree_rotate_left(root, node->parent->parent);
            }
        }
    }

    /* 确保根节点为黑色（性质2）
     * Case 1 可能将根节点染红（当祖父是根时），这里兜底修复。
     */
    root->rb_node->color = REDBLACKTREE_BLACK;
}

/**
 * @func        RedBlackTree_transplant
 * @brief       红黑树内部-节点替换（将 v 替换 u 的位置）
 * @details     将子树 v 整体替换子树 u 的位置。
 *              仅修改 u 的父节点和 v 的 parent 指针，
 *              不修改 u 和 v 的子节点指针。
 *
 *     替换前:          替换后:
 *       P                P
 *       |                |
 *       u    =>          v
 *      / \              / \
 *     ..  ..           ..  ..
 *
 * @param[in]   root: 红黑树根（可能需要更新根指针）
 * @param[in]   u:    被替换的节点
 * @param[in]   v:    替换上去的节点（可以为 NULL）
 *
 * @note        调用后 u 的子节点指针不变，但 u 已不在树中。
 *              如果 v 不为 NULL，v->parent 被设为 u 原来的 parent。
 */
static void RedBlackTree_transplant(struct rb_root *root, struct rb_node *u,
                          struct rb_node *v)
{
    if (root == NULL || u == NULL)
    {
        return;
    }

    /* 更新父节点的指针 */
    if (u->parent == NULL) {
        /* u 是根节点 */
        root->rb_node = v;
    } else if (u == u->parent->left) {
        /* u 是左子 */
        u->parent->left = v;
    } else {
        /* u 是右子 */
        u->parent->right = v;
    }

    /* 设置 v 的父指针 */
    if (v != NULL) {
        v->parent = u->parent;
    }
}

/**
 * @func        RedBlackTree_subtree_min
 * @brief       红黑树内部-获取子树最小节点（最左节点）
 * @details     沿左子链一直走到底。
 *
 *              时间复杂度: O(log n)
 */
static struct rb_node *RedBlackTree_subtree_min(struct rb_node *node)
{
    if (node == NULL)
    {
        return NULL;
    }
    /* 一直向左走，直到没有左子节点 */
    while (node->left != NULL) {
        node = node->left;
    }
    return node;
}

/**
 * @func        RedBlackTree_subtree_max
 * @brief       红黑树内部-获取子树最大节点（最右节点）
 * @details     沿右子链一直走到底。
 *
 *              时间复杂度: O(log n)
 */
static struct rb_node *RedBlackTree_subtree_max(struct rb_node *node)
{
    if (node == NULL)
    {
        return NULL;
    }
    /* 一直向右走，直到没有右子节点 */
    while (node->right != NULL) {
        node = node->right;
    }
    return node;
}

/**
 * @func        RedBlackTree_delete_fixup
 * @brief       红黑树内部-删除修复
 * @details     删除黑色节点后，通过该节点的路径黑高减少1（违反性质5）。
 *              修复的核心思想是将"多余的黑色"向上传播，直到可以消耗掉。
 *
 *              node 参数代表"双重黑"节点（概念上多带了一重黑色），
 *              可以为 NULL（NULL 节点也被视为双重黑）。
 *
 *              四种情况（每类有镜像版本）:
 *
 *              Case 1: 兄弟节点是红色
 *                - 兄弟染黑，父染红，对父旋转
 *                - 转化为 Case 2/3/4（新兄弟一定是黑色）
 *
 *              Case 2: 兄弟是黑色，且兄弟的两个子节点都是黑色
 *                - 兄弟染红，"双重黑"上移到父节点
 *                - 可能继续循环（如果父是红色则结束）
 *
 *              Case 3: 兄弟是黑色，兄弟远端子节点是黑色，近端是红色
 *                - 近端子节点染黑，兄弟染红，对兄弟旋转
 *                - 转化为 Case 4
 *
 *              Case 4: 兄弟是黑色，兄弟远端子节点是红色
 *                - 兄弟染成父的颜色，父染黑，远端子节点染黑
 *                - 对父旋转，修复完成
 *
 * @param[in]   root:    红黑树根
 * @param[in]   parent:  "双重黑"节点的父节点（当 node 为 NULL 时使用）
 * @param[in]   node:    "双重黑"节点（可能为 NULL）
 *
 * @note        安全性分析:
 *              - 当 node 和 parent 都为 NULL 时（删除最后的根节点），
 *                循环条件 `node != root->rb_node` 即 `NULL != NULL` 为假，
 *                循环不执行，直接跳到最后的 `if (node != NULL)` 判断，安全退出。
 *
 *              最坏情况 O(log n) 次迭代（Case 2 可能一直向上传播），
 *              但最多 3 次旋转（Case 1 -> Case 3 -> Case 4）。
 */
static void RedBlackTree_delete_fixup(struct rb_root *root, struct rb_node *parent,
                            struct rb_node *node)
{
    if (root == NULL)
    {
        return;
    }

    /* 循环条件: node 是"双重黑"（黑色或 NULL），且不是根节点
     * 当 node 为 NULL 时，表示 NULL 节点带有"额外黑色"
     * 当 node 为根时，直接将额外黑色消耗掉（根变黑即可）
     */
    while ((node == NULL || node->color == REDBLACKTREE_BLACK) &&
           node != root->rb_node) {

        if (node == parent->left) {
            /* ---- node 是 parent 的左子 ---- */
            struct rb_node *sibling = parent->right;

            if (sibling != NULL && sibling->color == REDBLACKTREE_RED) {
                /* ---- Case 1: 兄弟节点是红色 ----
                 *
                 *       P(黑)           S(黑)
                 *       / \            / \
                 *   (2B)   S(红) =>  P(红)  Sr
                 *         / \        / \
                 *       Sl   Sr   (2B)  Sl(黑)
                 *
                 * 策略: 兄弟染黑，父染红，对父左旋。
                 * 旋转后 sibling 变为 parent->right（新的兄弟，一定是黑色）。
                 * 不改变黑高，但转化为 Case 2/3/4。
                 */
                sibling->color = REDBLACKTREE_BLACK;
                parent->color = REDBLACKTREE_RED;
                RedBlackTree_rotate_left(root, parent);
                sibling = parent->right;
            }

            if ((sibling == NULL) ||
                ((sibling->left == NULL || sibling->left->color == REDBLACKTREE_BLACK) &&
                 (sibling->right == NULL || sibling->right->color == REDBLACKTREE_BLACK))) {
                /* ---- Case 2: 兄弟的两个子节点都是黑色 ----
                 *
                 *       P(?)           P(2B) ← 新的问题节点
                 *       / \            / \
                 *   (2B)   S(黑) => node  S(红)
                 *         / \              / \
                 *       Sl(B) Sr(B)     Sl(B) Sr(B)
                 *
                 * 策略: 兄弟染红，"双重黑"上移到父节点。
                 * 如果父节点原来是红色，下一轮循环会退出并染黑。
                 */
                if (sibling != NULL)
                {
                    sibling->color = REDBLACKTREE_RED;
                }
                node = parent;
                parent = node->parent;
            } else {
                if (sibling->right == NULL ||
                    sibling->right->color == REDBLACKTREE_BLACK) {
                    /* ---- Case 3: 兄弟的远端子节点是黑色，近端是红色 ----
                     *
                     *       P(?)           P(?)
                     *       / \            / \
                     *   (2B)   S(黑) => (2B)  Sl(黑)
                     *         / \                \
                     *       Sl(红) Sr(B)         S(红)
                     *                               \
                     *                               Sr(B)
                     *
                     * 策略: 近端子节点染黑，兄弟染红，对兄弟右旋。
                     * 转化为 Case 4。
                     */
                    if (sibling->left != NULL)
                    {
                        sibling->left->color = REDBLACKTREE_BLACK;
                    }
                    sibling->color = REDBLACKTREE_RED;
                    RedBlackTree_rotate_right(root, sibling);
                    sibling = parent->right;
                }
                /* ---- Case 4: 兄弟的远端子节点是红色 ----
                 *
                 *       P(?)           S(P的颜色)
                 *       / \            / \
                 *   (2B)   S(黑) => P(黑)  Sr(黑)
                 *         / \        / \
                 *       Sl(?) Sr(红) (2B→B) Sl(?)
                 *
                 * 策略: 兄弟染成父的颜色，父染黑，远端子节点染黑，
                 * 对父左旋。修复完成，设置 node 为根以退出循环。
                 */
                if (sibling != NULL) {
                    sibling->color = parent->color;
                    parent->color = REDBLACKTREE_BLACK;
                    if (sibling->right != NULL)
                    {
                        sibling->right->color = REDBLACKTREE_BLACK;
                    }
                    RedBlackTree_rotate_left(root, parent);
                }
                /* 修复完成，设 node 为根节点以退出循环 */
                node = root->rb_node;
            }
        } else {
            /* ---- node 是 parent 的右子（上述情况的镜像） ---- */
            struct rb_node *sibling = parent->left;

            if (sibling != NULL && sibling->color == REDBLACKTREE_RED) {
                /* ---- Case 1 镜像: 兄弟节点是红色 ---- */
                sibling->color = REDBLACKTREE_BLACK;
                parent->color = REDBLACKTREE_RED;
                RedBlackTree_rotate_right(root, parent);
                sibling = parent->left;
            }

            if ((sibling == NULL) ||
                ((sibling->right == NULL || sibling->right->color == REDBLACKTREE_BLACK) &&
                 (sibling->left == NULL || sibling->left->color == REDBLACKTREE_BLACK))) {
                /* ---- Case 2 镜像: 兄弟的两个子节点都是黑色 ---- */
                if (sibling != NULL)
                {
                    sibling->color = REDBLACKTREE_RED;
                }
                node = parent;
                parent = node->parent;
            } else {
                if (sibling->left == NULL ||
                    sibling->left->color == REDBLACKTREE_BLACK) {
                    /* ---- Case 3 镜像: 兄弟的远端子节点是黑色 ---- */
                    if (sibling->right != NULL)
                    {
                        sibling->right->color = REDBLACKTREE_BLACK;
                    }
                    sibling->color = REDBLACKTREE_RED;
                    RedBlackTree_rotate_left(root, sibling);
                    sibling = parent->left;
                }
                /* ---- Case 4 镜像: 兄弟的远端子节点是红色 ---- */
                if (sibling != NULL) {
                    sibling->color = parent->color;
                    parent->color = REDBLACKTREE_BLACK;
                    if (sibling->left != NULL)
                    {
                        sibling->left->color = REDBLACKTREE_BLACK;
                    }
                    RedBlackTree_rotate_right(root, parent);
                }
                /* 修复完成，设 node 为根节点以退出循环 */
                node = root->rb_node;
            }
        }
    }

    /* 确保 node 是黑色（消耗掉"额外黑色"）
     * 如果 node 是红色，直接染黑即可（红色 + 额外黑色 = 黑色）。
     * 如果 node 是根，染黑不影响性质。
     */
    if (node != NULL) {
        node->color = REDBLACKTREE_BLACK;
    }
}

/**
 * @func        RedBlackTree_destroy_subtree
 * @brief       红黑树内部-递归销毁子树（后序遍历）
 * @details     使用后序遍历（先递归左子树、右子树，再处理当前节点）
 *              确保在释放当前节点时，其子树已经被处理完毕。
 *
 * @param[in]   node:     子树根节点
 * @param[in]   free_fn:  节点释放回调函数（NULL 表示不释放内存）
 * @param[in]   arg:      传递给释放回调的用户参数
 *
 * @warning     递归深度等于树高，红黑树保证 O(log n)，
 *              对于 1000 个节点，递归深度最多约 20 层，嵌入式系统可接受。
 *              但对于极大树（>100万节点），建议改用迭代方式。
 */
static void RedBlackTree_destroy_subtree(struct rb_node *node,
                               void (*free_fn)(struct rb_node *, void *),
                               void *arg)
{
    if (node == NULL)
    {
        return;
    }

    /* 后序遍历: 先处理左右子树，再处理当前节点 */
    RedBlackTree_destroy_subtree(node->left, free_fn, arg);
    RedBlackTree_destroy_subtree(node->right, free_fn, arg);

    /* 释放当前节点 */
    if (free_fn != NULL) {
        free_fn(node, arg);
    }
}
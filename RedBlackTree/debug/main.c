/**
 * @file        main.c
 * @brief       RedBlackTree 内核风格侵入式红黑树 - 测试程序
 * @details     本程序演示红黑树的完整使用流程，覆盖所有API:
 *
 *              Part 1:  基础操作 - 初始化、插入、删除、查找
 *              Part 2:  查询操作 - count/empty
 *              Part 3:  迭代操作 - first/last/next/prev
 *              Part 4:  替换操作 - replace
 *              Part 5:  遍历宏 - RedBlackTree_for_each / RedBlackTree_for_each_safe / RedBlackTree_for_each_entry
 *              Part 6:  静态初始化 - DEFINE_REDBLACKTREE_ROOT
 *              Part 7:  批量操作 - destroy / traverse
 *              Part 8:  综合测试 - 大规模插入/删除/查找
 *
 * @author      zlzksrl
 * @Version     V1.0.0
 * @date        2026-05-08
 * @copyright   copyright (C) 2026
 */

/**
 * @date        2026-05-08
 * @Version     V1.0.0
 * @brief       创建文件，RedBlackTree 全套API测试
 * @author      zlzksrl
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "../include/RedBlackTree.h"


/* ========================== 调试宏 ========================== */

/**
 * @def   Debug_printx
 * @brief 调试打印宏，输出格式: [Debug]-[#####]-[用户信息##@line:[行号]@func:[函数名]]
 * @details 使用 do-while(0) 包裹，确保宏在 if/else 语句中安全使用。
 *          ##__VA_ARGS__ 是 GCC 扩展，允许零个可变参数。
 *          将 #if 1 改为 #if 0 可关闭所有调试输出。
 */
#if 1
#define Debug_printx(format,...)\
                do\
                {\
                    printf("[Debug]-[#####]-["format"##@line:[%d]@func:[%s]]\r\n",##__VA_ARGS__,__LINE__,__FUNCTION__);\
                }while(0)
#else
#define Debug_printx(format,...)\
                do\
                {\
                }while(0)
#endif

/**
 * @def   TEST_PASS
 * @brief 测试通过打印宏
 */
#define TEST_PASS(test_name) \
    do { \
        printf("  [PASS] %s\r\n", test_name); \
    } while(0)

/**
 * @def   TEST_FAIL
 * @brief 测试失败打印宏
 */
#define TEST_FAIL(test_name, reason) \
    do { \
        printf("  [FAIL] %s - %s\r\n", test_name, reason); \
    } while(0)

/**
 * @def   TEST_ASSERT
 * @brief 断言宏，条件为假时打印失败信息
 */
#define TEST_ASSERT(cond, test_name, reason) \
    do { \
        if (cond) { \
            TEST_PASS(test_name); \
        } else { \
            TEST_FAIL(test_name, reason); \
        } \
    } while(0)


/* ================================================================== */
/*                                                                    */
/*     测试用数据结构定义                                               */
/*                                                                    */
/* ================================================================== */

/**
 * @struct      my_data
 * @brief       测试用数据结构，包含一个整数值和红黑树节点
 */
/**
 * @struct      my_data
 * @brief       测试用数据结构，包含一个整数值和红黑树节点
 * @details     这是侵入式红黑树的典型用法:
 *              - value: 用户的实际数据（此处为整型键值）
 *              - node:  红黑树节点，嵌入到用户结构体中
 *
 *              通过 RedBlackTree_entry 宏可以从 node 指针反查到此结构体。
 *              内存布局: [value(4字节)][node(父指针+左指针+右指针+颜色)]
 */
struct my_data {
    int value;              /**< 数据值（用作红黑树键值，用于比较和排序） */
    struct rb_node node;    /**< 红黑树节点（侵入式嵌入，由红黑树库管理） */
};

/**
 * @func        create_data
 * @brief       动态创建一个 my_data 结构体
 * @param[in]   value: 数据值
 * @return      指向新创建的 my_data 的指针
 */
static struct my_data *create_data(int value)
{
    struct my_data *data = (struct my_data *)malloc(sizeof(struct my_data));
    if (data != NULL) {
        data->value = value;
        RedBlackTree_init_node(&data->node);
    }
    return data;
}

/**
 * @func        my_cmp
 * @brief       红黑树比较函数（按 value 比较）
 * @param[in]   a:   节点a
 * @param[in]   b:   节点b
 * @param[in]   arg: 未使用
 * @return      int <0:a<b, >0:a>b, 0:相等
 */
static int my_cmp(struct rb_node *a, struct rb_node *b, void *arg)
{
    struct my_data *da = RedBlackTree_entry(a, struct my_data, node);
    struct my_data *db = RedBlackTree_entry(b, struct my_data, node);
    (void)arg;
    if (da->value < db->value) return -1;
    if (da->value > db->value) return 1;
    return 0;
}

/**
 * @func        print_tree
 * @brief       打印红黑树中所有节点的值（中序遍历）
 * @param[in]   root:  红黑树根指针
 * @param[in]   label: 打印标签
 */
/**
 * @func        print_tree
 * @brief       打印红黑树中所有节点的值（中序遍历）
 * @details     使用 RedBlackTree_for_each 宏进行中序遍历，
 *              通过 RedBlackTree_entry 从 rb_node 指针获取宿主结构体指针。
 *              输出格式: "label: [v1, v2, v3, ...]"
 *              利用 RedBlackTree_next 判断是否为最后一个节点来控制逗号输出。
 * @param[in]   root:  红黑树根指针
 * @param[in]   label: 打印标签（用于标识不同的测试阶段）
 */
static void print_tree(const struct rb_root *root, const char *label)
{
    struct rb_node *pos;
    printf("  %s: [", label);
    RedBlackTree_for_each(pos, root) {
        /* 从 rb_node 指针反查到 my_data 宿主结构体 */
        struct my_data *d = RedBlackTree_entry(pos, struct my_data, node);
        printf("%d", d->value);
        /* 判断是否为最后一个节点，控制逗号输出 */
        if (RedBlackTree_next(pos) != NULL)
        {
            printf(", ");
        }
    }
    printf("]\r\n");
}

/**
 * @func        free_all_nodes
 * @brief       安全释放红黑树中所有节点
 * @param[in]   root: 红黑树根指针
 */
/**
 * @func        free_all_nodes
 * @brief       安全释放红黑树中所有节点
 * @details     使用 RedBlackTree_for_each_safe 安全遍历宏，
 *              预存下一个节点 n，允许在循环体内安全删除当前节点 pos。
 *              步骤: 删除节点 -> 获取宿主结构体 -> 释放内存。
 * @param[in]   root: 红黑树根指针
 * @note        使用此函数而非 RedBlackTree_destroy，因为它是迭代式（非递归），
 *              适用于任何深度的红黑树。
 */
static void free_all_nodes(struct rb_root *root)
{
    struct rb_node *pos, *n;
    RedBlackTree_for_each_safe(pos, n, root) {
        /* 先从树中移除节点 */
        RedBlackTree_delete(root, pos);
        /* 通过 RedBlackTree_entry 获取宿主结构体指针并释放 */
        free(RedBlackTree_entry(pos, struct my_data, node));
    }
}

/**
 * @func        my_free_fn
 * @brief       RedBlackTree_destroy 使用的释放回调函数
 */
static void my_free_fn(struct rb_node *node, void *arg)
{
    struct my_data *d = RedBlackTree_entry(node, struct my_data, node);
    (void)arg;
    free(d);
}


/* ================================================================== */
/*                                                                    */
/*     Part 1: 基础操作 - 初始化、插入、删除、查找                       */
/*                                                                    */
/* ================================================================== */

/**
 * @func         test_basic_operations
 * @brief        测试红黑树基础操作
 * @details      测试 RedBlackTree_init_root, RedBlackTree_init_node, RedBlackTree_insert, RedBlackTree_delete, RedBlackTree_search
 */
static void test_basic_operations(void)
{
    Debug_printx("========== Part 1: Basic Operations ==========");

    /* ---- 初始化空树 ---- */
    struct rb_root root;
    RedBlackTree_init_root(&root);

    /* 验证初始化后树为空 */
    TEST_ASSERT(RedBlackTree_empty(&root), "RedBlackTree_init_root - empty tree",
                "tree should be empty after init");
    TEST_ASSERT(RedBlackTree_count(&root) == 0, "RedBlackTree_count - 0",
                "empty tree should have 0 nodes");

    /* ---- 插入测试 ----
     * 插入 7 个节点: 50, 30, 70, 20, 40, 60, 80
     * 这会构建一棵平衡的 BST:
     *          50(B)
     *        /      \
     *      30(B)    70(B)
     *     /  \     /  \
     *   20(R) 40(R) 60(R) 80(R)
     * （实际颜色可能因插入顺序和重平衡而不同）
     */
    struct my_data *d50 = create_data(50);
    struct my_data *d30 = create_data(30);
    struct my_data *d70 = create_data(70);
    struct my_data *d20 = create_data(20);
    struct my_data *d40 = create_data(40);
    struct my_data *d60 = create_data(60);
    struct my_data *d80 = create_data(80);

    TEST_ASSERT(RedBlackTree_insert(&root, &d50->node, my_cmp, NULL) == 0,
                "RedBlackTree_insert 50 - success", "insert should succeed");
    TEST_ASSERT(RedBlackTree_insert(&root, &d30->node, my_cmp, NULL) == 0,
                "RedBlackTree_insert 30 - success", "insert should succeed");
    TEST_ASSERT(RedBlackTree_insert(&root, &d70->node, my_cmp, NULL) == 0,
                "RedBlackTree_insert 70 - success", "insert should succeed");
    TEST_ASSERT(RedBlackTree_insert(&root, &d20->node, my_cmp, NULL) == 0,
                "RedBlackTree_insert 20 - success", "insert should succeed");
    TEST_ASSERT(RedBlackTree_insert(&root, &d40->node, my_cmp, NULL) == 0,
                "RedBlackTree_insert 40 - success", "insert should succeed");
    TEST_ASSERT(RedBlackTree_insert(&root, &d60->node, my_cmp, NULL) == 0,
                "RedBlackTree_insert 60 - success", "insert should succeed");
    TEST_ASSERT(RedBlackTree_insert(&root, &d80->node, my_cmp, NULL) == 0,
                "RedBlackTree_insert 80 - success", "insert should succeed");

    /* 验证插入后树不为空且节点数正确 */
    TEST_ASSERT(!RedBlackTree_empty(&root), "RedBlackTree_insert - tree not empty",
                "tree should not be empty after inserts");
    TEST_ASSERT(RedBlackTree_count(&root) == 7, "RedBlackTree_count - 7",
                "expected 7 nodes");

    /* ---- 重复插入测试 ----
     * 键值 50 已存在，插入应返回 -1（失败）
     */
    struct my_data *dup = create_data(50);
    TEST_ASSERT(RedBlackTree_insert(&root, &dup->node, my_cmp, NULL) == -1,
                "RedBlackTree_insert duplicate - fail",
                "duplicate insert should fail");
    free(dup);

    print_tree(&root, "After inserts: 20, 30, 40, 50, 60, 70, 80");

    /* ---- 查找测试: 查找存在的节点 ----
     * 构造一个模板 key（栈上分配，无需释放），使用 RedBlackTree_search 查找。
     * 查找使用 my_cmp 比较函数，仅比较 value 字段。
     */
    {
        struct my_data key = { .value = 40 };
        struct rb_node *found = RedBlackTree_search(&root, &key.node, my_cmp, NULL);
        TEST_ASSERT(found != NULL, "RedBlackTree_search 40 - found",
                    "should find 40");
        if (found) {
            struct my_data *d = RedBlackTree_entry(found, struct my_data, node);
            TEST_ASSERT(d->value == 40, "RedBlackTree_search 40 - value correct",
                        "found value should be 40");
        }
    }

    /* ---- 查找测试: 查找不存在的节点 ---- */
    {
        struct my_data key = { .value = 999 };
        struct rb_node *found = RedBlackTree_search(&root, &key.node, my_cmp, NULL);
        TEST_ASSERT(found == NULL, "RedBlackTree_search 999 - not found",
                    "should not find 999");
    }

    /* ---- 删除测试: 删除叶子节点 20 ----
     * 叶子节点删除最简单，无需找后继，直接 transplant。
     * 删除红色叶子不影响黑高，无需修复。
     */
    TEST_ASSERT(RedBlackTree_delete(&root, &d20->node) == 0,
                "RedBlackTree_delete 20 - success", "delete should succeed");
    TEST_ASSERT(RedBlackTree_count(&root) == 6, "RedBlackTree_count after delete - 6",
                "expected 6 nodes");
    free(d20);

    /* ---- 删除测试: 删除有两个子节点的节点 70 ----
     * 需要找后继节点（右子树最小值 80），用后继替换 70 的位置。
     * 这是最复杂的删除情况。
     */
    TEST_ASSERT(RedBlackTree_delete(&root, &d70->node) == 0,
                "RedBlackTree_delete 70 - success", "delete should succeed");
    TEST_ASSERT(RedBlackTree_count(&root) == 5, "RedBlackTree_count after delete - 5",
                "expected 5 nodes");
    free(d70);

    /* ---- 删除测试: 删除根节点 50 ----
     * 删除根节点时需要更新 root->rb_node 指针。
     */
    TEST_ASSERT(RedBlackTree_delete(&root, &d50->node) == 0,
                "RedBlackTree_delete 50 (root) - success", "delete should succeed");
    TEST_ASSERT(RedBlackTree_count(&root) == 4, "RedBlackTree_count after delete - 4",
                "expected 4 nodes");
    free(d50);

    print_tree(&root, "After deletes (20, 70, 50): 30, 40, 60, 80");

    /* ---- 验证中序遍历顺序: 30, 40, 60, 80 ----
     * 红黑树是 BST，中序遍历应按键值从小到大排列。
     */
    {
        int expected[] = {30, 40, 60, 80};
        int i = 0;
        struct rb_node *pos;
        RedBlackTree_for_each(pos, &root) {
            struct my_data *d = RedBlackTree_entry(pos, struct my_data, node);
            if (d->value != expected[i]) {
                TEST_FAIL("inorder traversal - order check", "value mismatch");
                goto cleanup_basic;
            }
            i++;
        }
        TEST_PASS("inorder traversal - correct order (30, 40, 60, 80)");
    }

cleanup_basic:
    free_all_nodes(&root);
    Debug_printx("---------- Part 1 Done ----------\r\n");
}


/* ================================================================== */
/*                                                                    */
/*     Part 2: 查询操作                                                */
/*                                                                    */
/*                                                                    */
/* ================================================================== */

/**
 * @func         test_query_operations
 * @brief        测试红黑树查询操作
 * @details      测试 RedBlackTree_count, RedBlackTree_empty
 */
static void test_query_operations(void)
{
    Debug_printx("========== Part 2: Query Operations ==========");

    DEFINE_REDBLACKTREE_ROOT(root);

    /* 空树测试 */
    TEST_ASSERT(RedBlackTree_empty(&root), "RedBlackTree_empty - empty",
                "new tree should be empty");
    TEST_ASSERT(RedBlackTree_count(&root) == 0, "RedBlackTree_count - 0",
                "empty tree should have 0 nodes");

    /* NULL 参数测试 */
    TEST_ASSERT(RedBlackTree_empty(NULL) == 1, "RedBlackTree_empty(NULL) - returns 1",
                "NULL root should be considered empty");
    TEST_ASSERT(RedBlackTree_count(NULL) == 0, "RedBlackTree_count(NULL) - returns 0",
                "NULL root should have 0 count");

    /* 插入一个节点 */
    struct my_data *d1 = create_data(100);
    RedBlackTree_insert(&root, &d1->node, my_cmp, NULL);

    TEST_ASSERT(!RedBlackTree_empty(&root), "RedBlackTree_empty - not empty after insert",
                "tree with 1 node should not be empty");
    TEST_ASSERT(RedBlackTree_count(&root) == 1, "RedBlackTree_count - 1",
                "expected 1 node");

    /* 插入多个节点 */
    struct my_data *d2 = create_data(200);
    struct my_data *d3 = create_data(300);
    RedBlackTree_insert(&root, &d2->node, my_cmp, NULL);
    RedBlackTree_insert(&root, &d3->node, my_cmp, NULL);

    TEST_ASSERT(RedBlackTree_count(&root) == 3, "RedBlackTree_count - 3",
                "expected 3 nodes");

    /* 删除到空 */
    RedBlackTree_delete(&root, &d1->node); free(d1);
    RedBlackTree_delete(&root, &d2->node); free(d2);
    RedBlackTree_delete(&root, &d3->node); free(d3);

    TEST_ASSERT(RedBlackTree_empty(&root), "RedBlackTree_empty - empty after all deletes",
                "tree should be empty after deleting all nodes");
    TEST_ASSERT(RedBlackTree_count(&root) == 0, "RedBlackTree_count - 0 after all deletes",
                "expected 0 nodes");

    Debug_printx("---------- Part 2 Done ----------\r\n");
}


/* ================================================================== */
/*                                                                    */
/*     Part 3: 迭代操作                                                */
/*                                                                    */
/* ================================================================== */

/**
 * @func         test_iteration_operations
 * @brief        测试红黑树迭代操作
 * @details      测试 RedBlackTree_first, RedBlackTree_last, RedBlackTree_next, RedBlackTree_prev
 */
static void test_iteration_operations(void)
{
    Debug_printx("========== Part 3: Iteration Operations ==========");

    DEFINE_REDBLACKTREE_ROOT(root);

    /* ---- 空树边界测试 ----
     * 空树上调用 first/last 应返回 NULL，
     * NULL 参数调用 next/prev 也应安全返回 NULL。
     */
    TEST_ASSERT(RedBlackTree_first(&root) == NULL, "RedBlackTree_first - NULL on empty",
                "first of empty tree should be NULL");
    TEST_ASSERT(RedBlackTree_last(&root) == NULL, "RedBlackTree_last - NULL on empty",
                "last of empty tree should be NULL");
    TEST_ASSERT(RedBlackTree_next(NULL) == NULL, "RedBlackTree_next(NULL) - NULL",
                "next of NULL should be NULL");
    TEST_ASSERT(RedBlackTree_prev(NULL) == NULL, "RedBlackTree_prev(NULL) - NULL",
                "prev of NULL should be NULL");

    /* ---- 构建测试树 ----
     * 插入 7 个节点: 50, 25, 75, 10, 30, 60, 90
     * 中序遍历结果: 10, 25, 30, 50, 60, 75, 90
     * nodes 数组保存指针，用于后续通过索引访问特定节点。
     */
    int values[] = {50, 25, 75, 10, 30, 60, 90};
    struct my_data *nodes[7];
    int i;
    for (i = 0; i < 7; i++) {
        nodes[i] = create_data(values[i]);
        RedBlackTree_insert(&root, &nodes[i]->node, my_cmp, NULL);
    }

    print_tree(&root, "Tree: 10, 25, 30, 50, 60, 75, 90");

    /* RedBlackTree_first: 最小节点 */
    {
        struct rb_node *first = RedBlackTree_first(&root);
        TEST_ASSERT(first != NULL, "RedBlackTree_first - not NULL",
                    "first should not be NULL");
        if (first) {
            struct my_data *d = RedBlackTree_entry(first, struct my_data, node);
            TEST_ASSERT(d->value == 10, "RedBlackTree_first - value 10",
                        "first should be 10");
        }
    }

    /* RedBlackTree_last: 最大节点 */
    {
        struct rb_node *last = RedBlackTree_last(&root);
        TEST_ASSERT(last != NULL, "RedBlackTree_last - not NULL",
                    "last should not be NULL");
        if (last) {
            struct my_data *d = RedBlackTree_entry(last, struct my_data, node);
            TEST_ASSERT(d->value == 90, "RedBlackTree_last - value 90",
                        "last should be 90");
        }
    }

    /* ---- RedBlackTree_next 测试 ----
     * 25 有右子树（30），后继是右子树的最小值 30。
     * nodes[1] 对应 values[1] = 25。
     */
    {
        struct rb_node *next = RedBlackTree_next(&nodes[1]->node); /* nodes[1] = 25 */
        TEST_ASSERT(next != NULL, "RedBlackTree_next(25) - not NULL",
                    "next of 25 should exist");
        if (next) {
            struct my_data *d = RedBlackTree_entry(next, struct my_data, node);
            TEST_ASSERT(d->value == 30, "RedBlackTree_next(25) - value 30",
                        "next of 25 should be 30");
        }
    }

    /* ---- RedBlackTree_prev 测试 ----
     * 60 有左子树（50），前驱是左子树的最大值 50。
     * nodes[5] 对应 values[5] = 60。
     */
    {
        struct rb_node *prev = RedBlackTree_prev(&nodes[5]->node); /* nodes[5] = 60 */
        TEST_ASSERT(prev != NULL, "RedBlackTree_prev(60) - not NULL",
                    "prev of 60 should exist");
        if (prev) {
            struct my_data *d = RedBlackTree_entry(prev, struct my_data, node);
            TEST_ASSERT(d->value == 50, "RedBlackTree_prev(60) - value 50",
                        "prev of 60 should be 50");
        }
    }

    /* ---- 边界测试: 最大节点的后继应为 NULL ---- */
    {
        struct rb_node *last = RedBlackTree_last(&root);
        struct rb_node *next = RedBlackTree_next(last);
        TEST_ASSERT(next == NULL, "RedBlackTree_next(last) - NULL",
                    "next of last should be NULL");
    }

    /* ---- 边界测试: 最小节点的前驱应为 NULL ---- */
    {
        struct rb_node *first = RedBlackTree_first(&root);
        struct rb_node *prev = RedBlackTree_prev(first);
        TEST_ASSERT(prev == NULL, "RedBlackTree_prev(first) - NULL",
                    "prev of first should be NULL");
    }

    /* ---- 正向遍历完整性验证 ----
     * 使用 first/next 手动遍历，验证:
     * 1. 遍历顺序严格递增（BST 性质）
     * 2. 遍历节点数等于树中节点数
     */
    {
        int expected[] = {10, 25, 30, 50, 60, 75, 90};
        int i = 0;
        struct rb_node *pos;
        for (pos = RedBlackTree_first(&root); pos != NULL; pos = RedBlackTree_next(pos)) {
            struct my_data *d = RedBlackTree_entry(pos, struct my_data, node);
            if (d->value != expected[i]) {
                TEST_FAIL("RedBlackTree_next traversal - order check", "value mismatch");
                goto cleanup_iter;
            }
            i++;
        }
        TEST_ASSERT(i == 7, "RedBlackTree_next traversal - 7 nodes",
                    "should traverse 7 nodes");
        TEST_PASS("RedBlackTree_next traversal - correct order");
    }

    /* ---- 反向遍历完整性验证 ----
     * 使用 last/prev 手动遍历，验证:
     * 1. 遍历顺序严格递减
     * 2. 遍历节点数等于树中节点数
     */
    {
        int expected[] = {90, 75, 60, 50, 30, 25, 10};
        int i = 0;
        struct rb_node *pos;
        for (pos = RedBlackTree_last(&root); pos != NULL; pos = RedBlackTree_prev(pos)) {
            struct my_data *d = RedBlackTree_entry(pos, struct my_data, node);
            if (d->value != expected[i]) {
                TEST_FAIL("RedBlackTree_prev traversal - order check", "value mismatch");
                goto cleanup_iter;
            }
            i++;
        }
        TEST_ASSERT(i == 7, "RedBlackTree_prev traversal - 7 nodes",
                    "should traverse 7 nodes");
        TEST_PASS("RedBlackTree_prev traversal - correct order");
    }

cleanup_iter:
    free_all_nodes(&root);
    Debug_printx("---------- Part 3 Done ----------\r\n");
}


/* ================================================================== */
/*                                                                    */
/*     Part 4: 替换操作                                                */
/*                                                                    */
/* ================================================================== */

/**
 * @func         test_replace_operation
 * @brief        测试红黑树替换操作
 * @details      测试 RedBlackTree_replace
 */
static void test_replace_operation(void)
{
    Debug_printx("========== Part 4: Replace Operation ==========");

    DEFINE_REDBLACKTREE_ROOT(root);

    /* 构建初始树: 10, 20, 30 */
    struct my_data *d10 = create_data(10);
    struct my_data *d20 = create_data(20);
    struct my_data *d30 = create_data(30);
    RedBlackTree_insert(&root, &d10->node, my_cmp, NULL);
    RedBlackTree_insert(&root, &d20->node, my_cmp, NULL);
    RedBlackTree_insert(&root, &d30->node, my_cmp, NULL);

    print_tree(&root, "Initial: 10, 20, 30");

    /* ---- RedBlackTree_replace 测试 ----
     * 用 d25（值为 25）替换 d20（值为 20）。
     * replace 不改变树结构（不触发重平衡），只是替换节点位置。
     * 注意: 实际使用中应确保新节点的键值与旧节点相同或在正确位置，
     *       否则会破坏 BST 性质。此处用 25 替换 20 是为了演示功能。
     */
    struct my_data *d25 = create_data(25);
    TEST_ASSERT(RedBlackTree_replace(&root, &d20->node, &d25->node) == 0,
                "RedBlackTree_replace - success", "replace should succeed");
    /* replace 不改变节点计数 */
    TEST_ASSERT(RedBlackTree_count(&root) == 3, "RedBlackTree_count after replace - 3",
                "count should remain 3");
    /* 旧节点已从树中移除，可以安全释放 */
    free(d20);

    print_tree(&root, "After replace(20 -> 25): 10, 25, 30");

    /* 验证顺序: 10, 25, 30 */
    {
        int expected[] = {10, 25, 30};
        int i = 0;
        struct rb_node *pos;
        RedBlackTree_for_each(pos, &root) {
            struct my_data *d = RedBlackTree_entry(pos, struct my_data, node);
            if (d->value != expected[i]) {
                TEST_FAIL("replace - order check", "value mismatch");
                goto cleanup_replace;
            }
            i++;
        }
        TEST_PASS("replace - correct order (10, 25, 30)");
    }

    /* 搜索 25 应能找到 */
    {
        struct my_data key = { .value = 25 };
        struct rb_node *found = RedBlackTree_search(&root, &key.node, my_cmp, NULL);
        TEST_ASSERT(found != NULL, "RedBlackTree_search 25 after replace - found",
                    "should find 25");
    }

    /* 搜索 20 应找不到 */
    {
        struct my_data key = { .value = 20 };
        struct rb_node *found = RedBlackTree_search(&root, &key.node, my_cmp, NULL);
        TEST_ASSERT(found == NULL, "RedBlackTree_search 20 after replace - not found",
                    "should not find 20");
    }

cleanup_replace:
    free_all_nodes(&root);
    Debug_printx("---------- Part 4 Done ----------\r\n");
}


/* ================================================================== */
/*                                                                    */
/*     Part 5: 遍历宏                                                  */
/*                                                                    */
/* ================================================================== */

/**
 * @func         test_traverse_macros
 * @brief        测试红黑树遍历宏
 * @details      测试 RedBlackTree_for_each, RedBlackTree_for_each_reverse, RedBlackTree_for_each_safe,
 *               RedBlackTree_for_each_entry, RedBlackTree_for_each_entry_safe
 */
static void test_traverse_macros(void)
{
    Debug_printx("========== Part 5: Traverse Macros ==========");

    DEFINE_REDBLACKTREE_ROOT(root);

    /* ---- 构建测试树 ----
     * 插入 7 个节点: 5, 3, 7, 1, 4, 6, 8
     * 中序遍历: 1, 3, 4, 5, 6, 7, 8
     */
    int values[] = {5, 3, 7, 1, 4, 6, 8};
    struct my_data *nodes[7];
    int i;
    for (i = 0; i < 7; i++) {
        nodes[i] = create_data(values[i]);
        RedBlackTree_insert(&root, &nodes[i]->node, my_cmp, NULL);
    }

    print_tree(&root, "Tree: 1, 3, 4, 5, 6, 7, 8");

    /* RedBlackTree_for_each: 正向遍历 */
    {
        int expected[] = {1, 3, 4, 5, 6, 7, 8};
        int i = 0;
        struct rb_node *pos;
        RedBlackTree_for_each(pos, &root) {
            struct my_data *d = RedBlackTree_entry(pos, struct my_data, node);
            if (d->value != expected[i]) {
                TEST_FAIL("RedBlackTree_for_each - order check", "value mismatch");
                goto cleanup_traverse;
            }
            i++;
        }
        TEST_ASSERT(i == 7, "RedBlackTree_for_each - 7 nodes", "expected 7");
        TEST_PASS("RedBlackTree_for_each - correct order");
    }

    /* RedBlackTree_for_each_reverse: 反向遍历 */
    {
        int expected[] = {8, 7, 6, 5, 4, 3, 1};
        int i = 0;
        struct rb_node *pos;
        RedBlackTree_for_each_reverse(pos, &root) {
            struct my_data *d = RedBlackTree_entry(pos, struct my_data, node);
            if (d->value != expected[i]) {
                TEST_FAIL("RedBlackTree_for_each_reverse - order check", "value mismatch");
                goto cleanup_traverse;
            }
            i++;
        }
        TEST_ASSERT(i == 7, "RedBlackTree_for_each_reverse - 7 nodes", "expected 7");
        TEST_PASS("RedBlackTree_for_each_reverse - correct order");
    }

    /* RedBlackTree_for_each_entry: 直接获取宿主结构体 */
    {
        int expected[] = {1, 3, 4, 5, 6, 7, 8};
        int i = 0;
        struct my_data *pos;
        RedBlackTree_for_each_entry(pos, &root, node) {
            if (pos->value != expected[i]) {
                TEST_FAIL("RedBlackTree_for_each_entry - order check", "value mismatch");
                goto cleanup_traverse;
            }
            i++;
        }
        TEST_ASSERT(i == 7, "RedBlackTree_for_each_entry - 7 nodes", "expected 7");
        TEST_PASS("RedBlackTree_for_each_entry - correct order");
    }

    /* ---- RedBlackTree_for_each_safe: 安全遍历删除 ----
     * 使用 _safe 版本遍历，预存下一个节点 n，
     * 允许在循环体内安全删除当前节点 pos。
     * 删除所有奇数值节点: 1, 3, 5, 7（共 4 个）。
     * 剩余节点: 4, 6, 8（共 3 个）。
     */
    {
        int deleted = 0;
        struct rb_node *pos, *n;
        RedBlackTree_for_each_safe(pos, n, &root) {
            struct my_data *d = RedBlackTree_entry(pos, struct my_data, node);
            if (d->value % 2 != 0) {
                RedBlackTree_delete(&root, pos);  /* 从树中移除 */
                free(d);                           /* 释放宿主结构体内存 */
                deleted++;
            }
        }
        TEST_ASSERT(deleted == 4, "RedBlackTree_for_each_safe - deleted 4 odd",
                    "should delete 4 odd numbers (1, 3, 5, 7)");
        TEST_ASSERT(RedBlackTree_count(&root) == 3, "RedBlackTree_for_each_safe - 3 remaining",
                    "should have 3 nodes remaining");

        print_tree(&root, "After deleting odd: 4, 6, 8");
    }

    /* ---- RedBlackTree_for_each_entry_safe: 安全遍历删除全部 ----
     * 直接遍历宿主结构体指针（无需手动调用 RedBlackTree_entry）。
     * 删除剩余的所有节点，验证树最终为空。
     */
    {
        struct my_data *pos, *n;
        int deleted = 0;
        RedBlackTree_for_each_entry_safe(pos, n, &root, node) {
            RedBlackTree_delete(&root, &pos->node);  /* 从树中移除 */
            free(pos);                                /* 释放宿主结构体 */
            deleted++;
        }
        TEST_ASSERT(deleted == 3, "RedBlackTree_for_each_entry_safe - deleted 3",
                    "should delete all 3 remaining nodes");
        TEST_ASSERT(RedBlackTree_empty(&root), "RedBlackTree_for_each_entry_safe - tree empty",
                    "tree should be empty");
    }

cleanup_traverse:
    free_all_nodes(&root);
    Debug_printx("---------- Part 5 Done ----------\r\n");
}


/* ================================================================== */
/*                                                                    */
/*     Part 6: 静态初始化                                              */
/*                                                                    */
/* ================================================================== */

/**
 * @func         test_static_init
 * @brief        测试静态初始化宏
 * @details      测试 DEFINE_REDBLACKTREE_ROOT 宏和 REDBLACKTREE_ROOT_INIT
 */
static void test_static_init(void)
{
    Debug_printx("========== Part 6: Static Init ==========");

    /* DEFINE_REDBLACKTREE_ROOT 宏 */
    DEFINE_REDBLACKTREE_ROOT(static_root);
    TEST_ASSERT(RedBlackTree_empty(&static_root), "DEFINE_REDBLACKTREE_ROOT - empty",
                "DEFINE_REDBLACKTREE_ROOT should create empty tree");
    TEST_ASSERT(RedBlackTree_count(&static_root) == 0, "DEFINE_REDBLACKTREE_ROOT - count 0",
                "empty tree should have 0 nodes");

    /* REDBLACKTREE_ROOT_INIT 宏 */
    struct rb_root init_root = REDBLACKTREE_ROOT_INIT;
    TEST_ASSERT(RedBlackTree_empty(&init_root), "REDBLACKTREE_ROOT_INIT - empty",
                "REDBLACKTREE_ROOT_INIT should create empty tree");
    TEST_ASSERT(init_root.rb_node == NULL, "REDBLACKTREE_ROOT_INIT - root NULL",
                "root node should be NULL");

    /* 在静态初始化的树上操作 */
    struct my_data *d1 = create_data(42);
    RedBlackTree_insert(&static_root, &d1->node, my_cmp, NULL);
    TEST_ASSERT(RedBlackTree_count(&static_root) == 1, "static tree - add 1 node",
                "should have 1 node");

    struct rb_node *first = RedBlackTree_first(&static_root);
    TEST_ASSERT(first != NULL, "static tree - first not NULL",
                "first should exist");
    if (first) {
        struct my_data *d = RedBlackTree_entry(first, struct my_data, node);
        TEST_ASSERT(d->value == 42, "static tree - first value 42",
                    "first value should be 42");
    }

    free_all_nodes(&static_root);
    Debug_printx("---------- Part 6 Done ----------\r\n");
}


/* ================================================================== */
/*                                                                    */
/*     Part 7: 批量操作                                                */
/*                                                                    */
/* ================================================================== */

/**
 * @func         test_batch_operations
 * @brief        测试红黑树批量操作
 * @details      测试 RedBlackTree_destroy, RedBlackTree_traverse
 */

/** 遍历回调用的累加器 */
static int g_traverse_sum;

/**
 * @func        traverse_sum_callback
 * @brief       遍历回调函数：累加所有节点的 value
 */
static int traverse_sum_callback(struct rb_node *node, void *arg)
{
    struct my_data *d = RedBlackTree_entry(node, struct my_data, node);
    (void)arg;
    g_traverse_sum += d->value;
    return 0;
}

/**
 * @func        traverse_stop_callback
 * @brief       遍历回调函数：遇到指定值时停止
 */
static int traverse_stop_callback(struct rb_node *node, void *arg)
{
    struct my_data *d = RedBlackTree_entry(node, struct my_data, node);
    int *stop_val = (int *)arg;
    if (d->value == *stop_val) {
        return 1;  /* 停止遍历 */
    }
    return 0;
}

static void test_batch_operations(void)
{
    Debug_printx("========== Part 7: Batch Operations ==========");

    /* RedBlackTree_traverse: 中序遍历求和 */
    {
        DEFINE_REDBLACKTREE_ROOT(root);
        struct my_data *d1 = create_data(10);
        struct my_data *d2 = create_data(20);
        struct my_data *d3 = create_data(30);
        struct my_data *d4 = create_data(40);
        RedBlackTree_insert(&root, &d1->node, my_cmp, NULL);
        RedBlackTree_insert(&root, &d2->node, my_cmp, NULL);
        RedBlackTree_insert(&root, &d3->node, my_cmp, NULL);
        RedBlackTree_insert(&root, &d4->node, my_cmp, NULL);

        g_traverse_sum = 0;
        int ret = RedBlackTree_traverse(&root, traverse_sum_callback, NULL);
        TEST_ASSERT(ret == 0, "RedBlackTree_traverse - returns 0",
                    "traverse should complete successfully");
        TEST_ASSERT(g_traverse_sum == 100, "RedBlackTree_traverse - sum is 100",
                    "10+20+30+40 should be 100");

        free_all_nodes(&root);
    }

    /* RedBlackTree_traverse: 提前停止 */
    {
        DEFINE_REDBLACKTREE_ROOT(root);
        struct my_data *d1 = create_data(1);
        struct my_data *d2 = create_data(2);
        struct my_data *d3 = create_data(3);
        RedBlackTree_insert(&root, &d1->node, my_cmp, NULL);
        RedBlackTree_insert(&root, &d2->node, my_cmp, NULL);
        RedBlackTree_insert(&root, &d3->node, my_cmp, NULL);

        int stop_at = 2;
        int ret = RedBlackTree_traverse(&root, traverse_stop_callback, &stop_at);
        TEST_ASSERT(ret == 1, "RedBlackTree_traverse stop - returns 1",
                    "traverse should return callback value (1)");

        free_all_nodes(&root);
    }

    /* RedBlackTree_destroy: 销毁所有节点 */
    {
        DEFINE_REDBLACKTREE_ROOT(root);
        int i;
        for (i = 0; i < 100; i++) {
            struct my_data *d = create_data(i);
            RedBlackTree_insert(&root, &d->node, my_cmp, NULL);
        }
        TEST_ASSERT(RedBlackTree_count(&root) == 100, "RedBlackTree_destroy prep - 100 nodes",
                    "should have 100 nodes");

        int ret = RedBlackTree_destroy(&root, my_free_fn, NULL);
        TEST_ASSERT(ret == 0, "RedBlackTree_destroy - returns 0",
                    "destroy should succeed");
        TEST_ASSERT(RedBlackTree_empty(&root), "RedBlackTree_destroy - tree empty",
                    "tree should be empty after destroy");
        TEST_ASSERT(RedBlackTree_count(&root) == 0, "RedBlackTree_destroy - count 0",
                    "count should be 0 after destroy");
    }

    /* RedBlackTree_destroy: 不释放内存（free_fn 为 NULL） */
    {
        DEFINE_REDBLACKTREE_ROOT(root);
        struct my_data *d1 = create_data(10);
        struct my_data *d2 = create_data(20);
        RedBlackTree_insert(&root, &d1->node, my_cmp, NULL);
        RedBlackTree_insert(&root, &d2->node, my_cmp, NULL);

        RedBlackTree_destroy(&root, NULL, NULL);
        TEST_ASSERT(RedBlackTree_empty(&root), "RedBlackTree_destroy(NULL free_fn) - tree empty",
                    "tree should be empty");
        /* 注意：未释放内存，需要手动释放 */
        free(d1);
        free(d2);
    }

    Debug_printx("---------- Part 7 Done ----------\r\n");
}


/* ================================================================== */
/*                                                                    */
/*     Part 8: 综合测试 - 大规模插入/删除/查找                          */
/*                                                                    */
/* ================================================================== */

/**
 * @func         test_comprehensive
 * @brief        综合测试：大规模数据操作
 * @details      测试大规模插入、查找、删除的正确性和平衡性
 */
static void test_comprehensive(void)
{
    Debug_printx("========== Part 8: Comprehensive Test ==========");

    DEFINE_REDBLACKTREE_ROOT(root);
    const int N = 1000;
    /* 使用指针数组跟踪所有分配的节点，便于后续清理 */
    struct my_data **items = (struct my_data **)malloc((unsigned int)N * sizeof(struct my_data *));
    int i;

    /* ---- 大规模插入 ----
     * 插入 0 ~ 999 共 1000 个连续整数。
     * 顺序插入是最坏情况之一（对普通 BST 会退化为链表），
     * 但红黑树通过旋转保持 O(log n) 高度。
     */
    for (i = 0; i < N; i++) {
        items[i] = create_data(i);
    }
    for (i = 0; i < N; i++) {
        int ret = RedBlackTree_insert(&root, &items[i]->node, my_cmp, NULL);
        if (ret != 0)
        {
            TEST_FAIL("comprehensive insert", "insert failed");
            goto cleanup_comprehensive;
        }
    }
    TEST_ASSERT((int)RedBlackTree_count(&root) == N, "comprehensive insert - count correct",
                "count should match insert count");

    /* ---- 验证中序遍历有序 ----
     * 红黑树是 BST，中序遍历必须严格递增。
     * 同时验证所有节点都被遍历到。
     */
    {
        struct rb_node *pos;
        int prev_val = -1;
        int count = 0;
        int ordered = 1;
        RedBlackTree_for_each(pos, &root) {
            struct my_data *d = RedBlackTree_entry(pos, struct my_data, node);
            if (d->value <= prev_val) {
                ordered = 0;
                break;
            }
            prev_val = d->value;
            count++;
        }
        TEST_ASSERT(ordered, "comprehensive - inorder ordered",
                    "inorder traversal should be sorted");
        TEST_ASSERT(count == N, "comprehensive - all nodes traversed",
                    "should traverse all nodes");
    }

    /* ---- 查找所有节点 ----
     * 验证每个插入的节点都能被找到。
     */
    {
        int all_found = 1;
        for (i = 0; i < N; i++) {
            struct my_data key = { .value = i };
            struct rb_node *found = RedBlackTree_search(&root, &key.node, my_cmp, NULL);
            if (found == NULL) {
                all_found = 0;
                break;
            }
        }
        TEST_ASSERT(all_found, "comprehensive search - all found",
                    "all inserted nodes should be findable");
    }

    /* ---- 删除一半节点（偶数值） ----
     * 使用 _safe 遍历宏安全删除所有偶数值节点。
     * items 数组中对应位置置 NULL，标记已释放。
     * 这是对红黑树删除操作的密集测试（涉及各种删除情况）。
     */
    {
        int deleted = 0;
        struct rb_node *pos, *n;
        RedBlackTree_for_each_safe(pos, n, &root) {
            struct my_data *d = RedBlackTree_entry(pos, struct my_data, node);
            if (d->value % 2 == 0) {
                RedBlackTree_delete(&root, pos);
                items[d->value] = NULL;  /* 标记已删除，防止后续重复释放 */
                free(d);
                deleted++;
            }
        }
        TEST_ASSERT(deleted == N / 2, "comprehensive delete half - correct count",
                    "should delete half of nodes");
        TEST_ASSERT((int)RedBlackTree_count(&root) == N / 2, "comprehensive - count after delete",
                    "remaining count should be half");
    }

    /* ---- 验证剩余节点都是奇数 ---- */
    {
        int all_odd = 1;
        struct rb_node *pos;
        RedBlackTree_for_each(pos, &root) {
            struct my_data *d = RedBlackTree_entry(pos, struct my_data, node);
            if (d->value % 2 == 0) {
                all_odd = 0;
                break;
            }
        }
        TEST_ASSERT(all_odd, "comprehensive - remaining all odd",
                    "all remaining nodes should have odd values");
    }

    /* ---- 验证已删除的节点不可被找到 ---- */
    {
        int none_found = 1;
        for (i = 0; i < N; i += 2) {
            struct my_data key = { .value = i };
            struct rb_node *found = RedBlackTree_search(&root, &key.node, my_cmp, NULL);
            if (found != NULL) {
                none_found = 0;
                break;
            }
        }
        TEST_ASSERT(none_found, "comprehensive - deleted not found",
                    "deleted nodes should not be found");
    }

    /* ---- 使用 RedBlackTree_destroy 清理剩余节点 ----
     * destroy 使用递归后序遍历释放所有节点。
     * items 数组中已删除的位置为 NULL，剩余奇数节点由 destroy 释放。
     */
    RedBlackTree_destroy(&root, my_free_fn, NULL);
    TEST_ASSERT(RedBlackTree_empty(&root), "comprehensive - empty after destroy",
                "tree should be empty after destroy");

    /* 释放指针数组（节点内存已由 destroy 释放） */
    free(items);

    Debug_printx("---------- Part 8 Done ----------\r\n");
    return;

cleanup_comprehensive:
    /* 错误处理: destroy 会释放树中所有剩余节点 */
    RedBlackTree_destroy(&root, my_free_fn, NULL);
    free(items);
    Debug_printx("---------- Part 8 Done (with errors) ----------\r\n");
}


/* ================================================================== */
/*                                                                    */
/*     main - 主函数                                                   */
/*                                                                    */
/* ================================================================== */

/**
 * @func         main
 * @brief        测试程序主函数
 * @details      依次执行8个测试模块，覆盖 RedBlackTree.h 全部API
 *
 * @param[in]    argc  参数个数
 * @param[in]    argv  参数字符串
 * @return       0正常退出
 */
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    Debug_printx("========================================");
    Debug_printx("  RedBlackTree Test Program Start");
    Debug_printx("========================================");

    test_basic_operations();        /* Part 1: 基础操作 */
    test_query_operations();        /* Part 2: 查询操作 */
    test_iteration_operations();    /* Part 3: 迭代操作 */
    test_replace_operation();       /* Part 4: 替换操作 */
    test_traverse_macros();         /* Part 5: 遍历宏 */
    test_static_init();             /* Part 6: 静态初始化 */
    test_batch_operations();        /* Part 7: 批量操作 */
    test_comprehensive();           /* Part 8: 综合测试 */

    Debug_printx("========================================");
    Debug_printx("  RedBlackTree Test Program End");
    Debug_printx("========================================");

    Debug_printx("Program exit");
    return 0;
}

/**
 * @file        main.c
 * @brief       KernelLinkedList 内核风格侵入式双向循环链表 - 测试程序
 * @details     本程序演示内核链表的完整使用流程，覆盖所有API:
 *
 *              Part 1:  基础操作 - 初始化、添加、删除、遍历
 *              Part 2:  查询操作 - empty/singular/first/last/head/count
 *              Part 3:  替换与移动 - replace/swap/move
 *              Part 4:  拼接操作 - splice/splice_tail/splice_init
 *              Part 5:  旋转操作 - rotate_left/rotate_to_front
 *              Part 6:  截取操作 - cut_before
 *              Part 7:  静态初始化 - LIST_HEAD/LIST_HEAD_INIT
 *              Part 8:  安全遍历删除 - list_for_each_entry_safe
 *
 * @author      zlzksrl
 * @Version     V1.0.0
 * @date        2026-05-07
 * @copyright   copyright (C) 2026
 */

/**
 * @date        2026-05-07
 * @Version     V1.0.0
 * @brief       创建文件，KernelLinkedList 全套API测试
 * @author      zlzksrl
 */
#include <stdlib.h>
#include <stdio.h>

#include "../include/KernelLinkedList.h"


/* ========================== 调试宏 ========================== */

#if 1
/**
 * @def   Debug_printx
 * @brief 调试打印宏，输出格式: [Debug]-[#####]-[用户信息##@line:[行号]@func:[函数名]]
 *        将 #if 1 改为 #if 0 可关闭所有调试输出
 */
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
 * @brief       测试用数据结构，包含一个整数值和链表节点
 */
struct my_data {
    int value;                  /**< 数据值 */
    struct list_head node;      /**< 链表节点 */
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
        INIT_LIST_HEAD(&data->node);
    }
    return data;
}

/**
 * @func        print_list
 * @brief       打印链表中所有节点的值
 * @param[in]   head:   链表头指针
 * @param[in]   label:  打印标签
 */
static void print_list(struct list_head *head, const char *label)
{
    struct my_data *pos;
    printf("  %s: [", label);
    list_for_each_entry(pos, head, node) {
        printf("%d", pos->value);
        if (&pos->node != head->prev)
            printf(", ");
    }
    printf("]\r\n");
}

/**
 * @func        free_all_entries
 * @brief       安全释放链表中所有节点
 * @param[in]   head: 链表头指针
 */
static void free_all_entries(struct list_head *head)
{
    struct my_data *pos, *n;
    list_for_each_entry_safe(pos, n, head, node) {
        list_del(&pos->node);
        free(pos);
    }
}


/* ================================================================== */
/*                                                                    */
/*     Part 1: 基础操作 - 初始化、添加、删除、遍历                       */
/*                                                                    */
/* ================================================================== */

/**
 * @func         test_basic_operations
 * @brief        测试链表基础操作
 * @details      测试 INIT_LIST_HEAD, list_add, list_add_tail, list_del,
 *               list_del_init, list_for_each_entry 等基础API
 */
static void test_basic_operations(void)
{
    Debug_printx("========== Part 1: Basic Operations ==========");

    struct list_head head;
    INIT_LIST_HEAD(&head);

    /* 测试空链表 */
    TEST_ASSERT(list_empty(&head), "INIT_LIST_HEAD - empty list",
                "list should be empty after init");

    /* list_add: 在头部添加 1, 2, 3 → 链表顺序: 3, 2, 1 */
    struct my_data *d1 = create_data(1);
    struct my_data *d2 = create_data(2);
    struct my_data *d3 = create_data(3);
    list_add(&d1->node, &head);
    list_add(&d2->node, &head);
    list_add(&d3->node, &head);

    TEST_ASSERT(!list_empty(&head), "list_add - list not empty",
                "list should not be empty after add");
    TEST_ASSERT(list_count_nodes(&head) == 3, "list_add - count is 3",
                "expected 3 nodes");

    /* 验证 list_for_each_entry 正向遍历（应为 3, 2, 1）*/
    {
        int expected[] = {3, 2, 1};
        int i = 0;
        struct my_data *pos;
        list_for_each_entry(pos, &head, node) {
            if (pos->value != expected[i]) {
                TEST_FAIL("list_for_each_entry - order check",
                          "value mismatch");
                goto next_basic;
            }
            i++;
        }
        TEST_PASS("list_for_each_entry - forward order (3,2,1)");
    }
    next_basic:

    print_list(&head, "After list_add(1), list_add(2), list_add(3)");

    /* list_add_tail: 在尾部添加 4 → 链表顺序: 3, 2, 1, 4 */
    struct my_data *d4 = create_data(4);
    list_add_tail(&d4->node, &head);
    TEST_ASSERT(list_count_nodes(&head) == 4, "list_add_tail - count is 4",
                "expected 4 nodes");

    print_list(&head, "After list_add_tail(4)");

    /* list_del: 删除节点 2 → 链表顺序: 3, 1, 4 */
    list_del(&d2->node);
    TEST_ASSERT(list_count_nodes(&head) == 3, "list_del - count is 3",
                "expected 3 nodes after del");
    TEST_ASSERT(d2->node.next == NULL && d2->node.prev == NULL,
                "list_del - node pointers NULL",
                "deleted node should have NULL pointers");
    free(d2);

    print_list(&head, "After list_del(2)");

    /* list_del_init: 删除并重新初始化节点 3 */
    list_del_init(&d3->node);
    TEST_ASSERT(list_count_nodes(&head) == 2, "list_del_init - count is 2",
                "expected 2 nodes");
    /* 重新初始化后 prev/next 指向自身 */
    TEST_ASSERT(d3->node.next == &d3->node && d3->node.prev == &d3->node,
                "list_del_init - node re-initialized",
                "node should be self-referencing after del_init");

    /* 将 d3 重新链入尾部 → 链表顺序: 1, 4, 3 */
    list_add_tail(&d3->node, &head);
    print_list(&head, "After list_del_init(3) then list_add_tail(3)");

    /* list_for_each_entry_reverse: 反向遍历（应为 3, 4, 1）*/
    {
        int expected[] = {3, 4, 1};
        int i = 0;
        struct my_data *pos;
        list_for_each_entry_reverse(pos, &head, node) {
            if (pos->value != expected[i]) {
                TEST_FAIL("list_for_each_entry_reverse - order check",
                          "value mismatch");
                goto next_reverse;
            }
            i++;
        }
        TEST_PASS("list_for_each_entry_reverse - reverse order (3,4,1)");
    }
    next_reverse:

    free_all_entries(&head);
    Debug_printx("---------- Part 1 Done ----------\r\n");
}


/* ================================================================== */
/*                                                                    */
/*     Part 2: 查询操作                                                */
/*                                                                    */
/* ================================================================== */

/**
 * @func         test_query_operations
 * @brief        测试链表查询操作
 * @details      测试 list_empty, list_is_singular, list_is_first,
 *               list_is_last, list_is_head, list_count_nodes
 */
static void test_query_operations(void)
{
    Debug_printx("========== Part 2: Query Operations ==========");

    LIST_HEAD(head);

    /* 空链表测试 */
    TEST_ASSERT(list_empty(&head) == 1, "list_empty - empty",
                "new list should be empty");
    TEST_ASSERT(list_count_nodes(&head) == 0, "list_count_nodes - 0",
                "empty list should have 0 nodes");
    TEST_ASSERT(list_is_singular(&head) == 0, "list_is_singular - empty",
                "empty list is not singular");
    TEST_ASSERT(list_is_head(&head, &head) == 1, "list_is_head - head is head",
                "head should be head");

    /* 添加一个节点 */
    struct my_data *d1 = create_data(10);
    list_add(&d1->node, &head);

    TEST_ASSERT(list_empty(&head) == 0, "list_empty - not empty",
                "list with 1 node should not be empty");
    TEST_ASSERT(list_is_singular(&head) == 1, "list_is_singular - 1 node",
                "list with 1 node should be singular");
    TEST_ASSERT(list_count_nodes(&head) == 1, "list_count_nodes - 1",
                "expected 1 node");
    TEST_ASSERT(list_is_first(&d1->node, &head) == 1,
                "list_is_first - d1 is first",
                "d1 should be first");
    TEST_ASSERT(list_is_last(&d1->node, &head) == 1,
                "list_is_last - d1 is last",
                "d1 should be last (only node)");

    /* 添加第二个节点 */
    struct my_data *d2 = create_data(20);
    list_add_tail(&d2->node, &head);

    TEST_ASSERT(list_is_singular(&head) == 0, "list_is_singular - 2 nodes",
                "list with 2 nodes is not singular");
    TEST_ASSERT(list_count_nodes(&head) == 2, "list_count_nodes - 2",
                "expected 2 nodes");
    TEST_ASSERT(list_is_first(&d1->node, &head) == 1,
                "list_is_first - d1 still first",
                "d1 should still be first");
    TEST_ASSERT(list_is_last(&d2->node, &head) == 1,
                "list_is_last - d2 is last",
                "d2 should be last");
    TEST_ASSERT(list_is_first(&d2->node, &head) == 0,
                "list_is_first - d2 not first",
                "d2 should not be first");
    TEST_ASSERT(list_is_last(&d1->node, &head) == 0,
                "list_is_last - d1 not last",
                "d1 should not be last");

    /* list_entry 测试 */
    struct my_data *entry = list_entry(&d1->node, struct my_data, node);
    TEST_ASSERT(entry == d1 && entry->value == 10,
                "list_entry - get host struct",
                "list_entry should return correct host struct");

    /* list_first_entry / list_last_entry */
    struct my_data *first = list_first_entry(&head, struct my_data, node);
    struct my_data *last  = list_last_entry(&head, struct my_data, node);
    TEST_ASSERT(first->value == 10, "list_first_entry - value 10",
                "first entry should have value 10");
    TEST_ASSERT(last->value == 20, "list_last_entry - value 20",
                "last entry should have value 20");

    free_all_entries(&head);
    Debug_printx("---------- Part 2 Done ----------\r\n");
}


/* ================================================================== */
/*                                                                    */
/*     Part 3: 替换与移动操作                                           */
/*                                                                    */
/* ================================================================== */

/**
 * @func         test_replace_move_operations
 * @brief        测试链表替换和移动操作
 * @details      测试 list_replace, list_replace_init, list_swap,
 *               list_move, list_move_tail
 */
static void test_replace_move_operations(void)
{
    Debug_printx("========== Part 3: Replace & Move Operations ==========");

    LIST_HEAD(head);

    struct my_data *d1 = create_data(1);
    struct my_data *d2 = create_data(2);
    struct my_data *d3 = create_data(3);
    list_add_tail(&d1->node, &head);   /* 1 */
    list_add_tail(&d2->node, &head);   /* 1, 2 */
    list_add_tail(&d3->node, &head);   /* 1, 2, 3 */

    print_list(&head, "Initial: 1, 2, 3");

    /* list_replace: 用新节点(99)替换 d2 */
    struct my_data *d99 = create_data(99);
    list_replace(&d2->node, &d99->node);
    TEST_ASSERT(list_count_nodes(&head) == 3, "list_replace - count still 3",
                "replace should keep node count");
    free(d2);

    print_list(&head, "After list_replace(2 → 99): 1, 99, 3");

    /* list_replace_init: 用新节点(88)替换 d99，并将 d99 重新初始化 */
    struct my_data *d88 = create_data(88);
    list_replace_init(&d99->node, &d88->node);
    TEST_ASSERT(d99->node.next == &d99->node, "list_replace_init - old node re-init",
                "old node should be re-initialized");
    free(d99);

    print_list(&head, "After list_replace_init(99 → 88): 1, 88, 3");

    /* list_swap: 交换 d1 和 d3 */
    list_swap(&d1->node, &d3->node);
    print_list(&head, "After list_swap(1, 3): 3, 88, 1");

    /* list_move: 将 d1 移动到链表头部 */
    list_move(&d1->node, &head);
    print_list(&head, "After list_move(d1 to head): 1, 3, 88");

    /* list_move_tail: 将 d1 移动到链表尾部 */
    list_move_tail(&d1->node, &head);
    print_list(&head, "After list_move_tail(d1 to tail): 3, 88, 1");

    /* 验证最终顺序: 3, 88, 1 */
    {
        int expected[] = {3, 88, 1};
        int i = 0;
        struct my_data *pos;
        list_for_each_entry(pos, &head, node) {
            if (pos->value != expected[i]) {
                TEST_FAIL("replace/move - final order check",
                          "value mismatch");
                goto cleanup_replace;
            }
            i++;
        }
        TEST_PASS("replace/move - final order correct (3, 88, 1)");
    }

cleanup_replace:
    free_all_entries(&head);
    Debug_printx("---------- Part 3 Done ----------\r\n");
}


/* ================================================================== */
/*                                                                    */
/*     Part 4: 拼接操作                                                */
/*                                                                    */
/* ================================================================== */

/**
 * @func         test_splice_operations
 * @brief        测试链表拼接操作
 * @details      测试 list_splice, list_splice_tail,
 *               list_splice_init, list_splice_tail_init
 */
static void test_splice_operations(void)
{
    Debug_printx("========== Part 4: Splice Operations ==========");

    LIST_HEAD(head1);
    LIST_HEAD(head2);

    struct my_data *a1 = create_data(1);
    struct my_data *a2 = create_data(2);
    struct my_data *b1 = create_data(10);
    struct my_data *b2 = create_data(20);
    struct my_data *b3 = create_data(30);

    list_add_tail(&a1->node, &head1);  /* head1: 1 */
    list_add_tail(&a2->node, &head1);  /* head1: 1, 2 */

    list_add_tail(&b1->node, &head2);  /* head2: 10 */
    list_add_tail(&b2->node, &head2);  /* head2: 10, 20 */
    list_add_tail(&b3->node, &head2);  /* head2: 10, 20, 30 */

    print_list(&head1, "head1: 1, 2");
    print_list(&head2, "head2: 10, 20, 30");

    /* list_splice: 将 head2 拼接到 head1 头部 */
    list_splice(&head2, &head1);
    print_list(&head1, "After list_splice(head2 to head1 head): 10, 20, 30, 1, 2");

    {
        int expected[] = {10, 20, 30, 1, 2};
        int i = 0;
        struct my_data *pos;
        list_for_each_entry(pos, &head1, node) {
            if (pos->value != expected[i]) {
                TEST_FAIL("list_splice - order check", "value mismatch");
                goto cleanup_splice1;
            }
            i++;
        }
        TEST_PASS("list_splice - correct order (10, 20, 30, 1, 2)");
    }

cleanup_splice1:
    free_all_entries(&head1);
    /* head2 现在是无效状态（节点已转移到 head1 并被释放），重新初始化 */
    INIT_LIST_HEAD(&head2);

    /* list_splice_tail_init: 拼接到尾部并重新初始化源链表 */
    struct my_data *c1 = create_data(100);
    struct my_data *c2 = create_data(200);
    list_add_tail(&c1->node, &head1);  /* head1: 100 */
    list_add_tail(&c2->node, &head1);  /* head1: 100, 200 */

    struct my_data *d1 = create_data(500);
    struct my_data *d2 = create_data(600);
    list_add_tail(&d1->node, &head2);  /* head2: 500 */
    list_add_tail(&d2->node, &head2);  /* head2: 500, 600 */

    print_list(&head1, "head1: 100, 200");
    print_list(&head2, "head2: 500, 600");

    list_splice_tail_init(&head2, &head1);
    print_list(&head1, "After list_splice_tail_init: 100, 200, 500, 600");
    TEST_ASSERT(list_empty(&head2), "list_splice_tail_init - head2 re-empty",
                "head2 should be empty after splice_init");

    {
        int expected[] = {100, 200, 500, 600};
        int i = 0;
        struct my_data *pos;
        list_for_each_entry(pos, &head1, node) {
            if (pos->value != expected[i]) {
                TEST_FAIL("list_splice_tail_init - order check",
                          "value mismatch");
                goto cleanup_splice2;
            }
            i++;
        }
        TEST_PASS("list_splice_tail_init - correct order (100, 200, 500, 600)");
    }

cleanup_splice2:
    free_all_entries(&head1);
    Debug_printx("---------- Part 4 Done ----------\r\n");
}


/* ================================================================== */
/*                                                                    */
/*     Part 5: 旋转操作                                                */
/*                                                                    */
/* ================================================================== */

/**
 * @func         test_rotate_operations
 * @brief        测试链表旋转操作
 * @details      测试 list_rotate_left, list_rotate_to_front
 */
static void test_rotate_operations(void)
{
    Debug_printx("========== Part 5: Rotate Operations ==========");

    LIST_HEAD(head);

    struct my_data *d1 = create_data(1);
    struct my_data *d2 = create_data(2);
    struct my_data *d3 = create_data(3);
    struct my_data *d4 = create_data(4);
    list_add_tail(&d1->node, &head);   /* 1, 2, 3, 4 */
    list_add_tail(&d2->node, &head);
    list_add_tail(&d3->node, &head);
    list_add_tail(&d4->node, &head);

    print_list(&head, "Initial: 1, 2, 3, 4");

    /* list_rotate_left: 将第一个节点旋转到末尾 */
    list_rotate_left(&head);
    print_list(&head, "After list_rotate_left: 2, 3, 4, 1");

    {
        int expected[] = {2, 3, 4, 1};
        int i = 0;
        struct my_data *pos;
        list_for_each_entry(pos, &head, node) {
            if (pos->value != expected[i]) {
                TEST_FAIL("list_rotate_left - order check", "value mismatch");
                goto cleanup_rotate;
            }
            i++;
        }
        TEST_PASS("list_rotate_left - correct order (2, 3, 4, 1)");
    }

    /* list_rotate_to_front: 将 d3 旋转到头部 */
    list_rotate_to_front(&d3->node, &head);
    print_list(&head, "After list_rotate_to_front(d3): 3, 2, 4, 1");

    {
        int expected[] = {3, 2, 4, 1};
        int i = 0;
        struct my_data *pos;
        list_for_each_entry(pos, &head, node) {
            if (pos->value != expected[i]) {
                TEST_FAIL("list_rotate_to_front - order check",
                          "value mismatch");
                goto cleanup_rotate;
            }
            i++;
        }
        TEST_PASS("list_rotate_to_front - correct order (3, 2, 4, 1)");
    }

cleanup_rotate:
    free_all_entries(&head);
    Debug_printx("---------- Part 5 Done ----------\r\n");
}


/* ================================================================== */
/*                                                                    */
/*     Part 6: 截取操作                                                */
/*                                                                    */
/* ================================================================== */

/**
 * @func         test_cut_operation
 * @brief        测试链表截取操作
 * @details      测试 list_cut_before
 */
static void test_cut_operation(void)
{
    Debug_printx("========== Part 6: Cut Operation ==========");

    LIST_HEAD(head);
    LIST_HEAD(new_head);

    struct my_data *d1 = create_data(1);
    struct my_data *d2 = create_data(2);
    struct my_data *d3 = create_data(3);
    struct my_data *d4 = create_data(4);
    struct my_data *d5 = create_data(5);
    list_add_tail(&d1->node, &head);   /* 1, 2, 3, 4, 5 */
    list_add_tail(&d2->node, &head);
    list_add_tail(&d3->node, &head);
    list_add_tail(&d4->node, &head);
    list_add_tail(&d5->node, &head);

    print_list(&head, "Original head: 1, 2, 3, 4, 5");

    /* list_cut_before: 在 d3 之前截断，d3及之后的节点移到 new_head */
    list_cut_before(&new_head, &head, &d3->node);

    print_list(&head,     "After cut - head: 1, 2");
    print_list(&new_head, "After cut - new_head: 3, 4, 5");

    TEST_ASSERT(list_count_nodes(&head) == 2, "list_cut_before - head has 2",
                "head should have 2 nodes");
    TEST_ASSERT(list_count_nodes(&new_head) == 3, "list_cut_before - new_head has 3",
                "new_head should have 3 nodes");

    {
        int expected_head[] = {1, 2};
        int i = 0;
        struct my_data *pos;
        list_for_each_entry(pos, &head, node) {
            if (pos->value != expected_head[i]) {
                TEST_FAIL("list_cut_before - head order", "value mismatch");
                goto cleanup_cut;
            }
            i++;
        }
        TEST_PASS("list_cut_before - head correct (1, 2)");
    }

    {
        int expected_new[] = {3, 4, 5};
        int i = 0;
        struct my_data *pos;
        list_for_each_entry(pos, &new_head, node) {
            if (pos->value != expected_new[i]) {
                TEST_FAIL("list_cut_before - new_head order", "value mismatch");
                goto cleanup_cut;
            }
            i++;
        }
        TEST_PASS("list_cut_before - new_head correct (3, 4, 5)");
    }

cleanup_cut:
    free_all_entries(&head);
    free_all_entries(&new_head);
    Debug_printx("---------- Part 6 Done ----------\r\n");
}


/* ================================================================== */
/*                                                                    */
/*     Part 7: 静态初始化                                              */
/*                                                                    */
/* ================================================================== */

/**
 * @func         test_static_init
 * @brief        测试静态初始化宏
 * @details      测试 LIST_HEAD 宏和 LIST_HEAD_INIT 宏
 */
static void test_static_init(void)
{
    Debug_printx("========== Part 7: Static Init ==========");

    /* LIST_HEAD 宏 */
    LIST_HEAD(static_head);
    TEST_ASSERT(list_empty(&static_head), "LIST_HEAD - empty",
                "LIST_HEAD should create empty list");
    TEST_ASSERT(static_head.next == &static_head && static_head.prev == &static_head,
                "LIST_HEAD - self-referencing",
                "empty list should point to itself");

    /* LIST_HEAD_INIT 宏 */
    struct list_head init_head = LIST_HEAD_INIT(init_head);
    TEST_ASSERT(list_empty(&init_head), "LIST_HEAD_INIT - empty",
                "LIST_HEAD_INIT should create empty list");
    TEST_ASSERT(init_head.next == &init_head && init_head.prev == &init_head,
                "LIST_HEAD_INIT - self-referencing",
                "empty list should point to itself");

    /* 在静态初始化的链表上操作 */
    struct my_data *d1 = create_data(42);
    list_add(&d1->node, &static_head);
    TEST_ASSERT(list_count_nodes(&static_head) == 1, "static list - add 1 node",
                "should have 1 node");
    TEST_ASSERT(list_first_entry(&static_head, struct my_data, node)->value == 42,
                "static list - first entry value 42",
                "first entry should be 42");

    free_all_entries(&static_head);
    Debug_printx("---------- Part 7 Done ----------\r\n");
}


/* ================================================================== */
/*                                                                    */
/*     Part 8: 安全遍历删除                                             */
/*                                                                    */
/* ================================================================== */

/**
 * @func         test_safe_iteration
 * @brief        测试安全遍历和删除操作
 * @details      测试 list_for_each_entry_safe, list_for_each_entry_safe_reverse,
 *               list_for_each_safe, list_for_each_prev_safe,
 *               list_for_each_entry_continue, list_for_each_entry_from,
 *               list_next_entry, list_prev_entry
 */
static void test_safe_iteration(void)
{
    Debug_printx("========== Part 8: Safe Iteration & Delete ==========");

    LIST_HEAD(head);

    struct my_data *d1 = create_data(1);
    struct my_data *d2 = create_data(2);
    struct my_data *d3 = create_data(3);
    struct my_data *d4 = create_data(4);
    struct my_data *d5 = create_data(5);
    list_add_tail(&d1->node, &head);   /* 1, 2, 3, 4, 5 */
    list_add_tail(&d2->node, &head);
    list_add_tail(&d3->node, &head);
    list_add_tail(&d4->node, &head);
    list_add_tail(&d5->node, &head);

    print_list(&head, "Initial: 1, 2, 3, 4, 5");

    /* list_for_each_entry_safe: 删除所有偶数值节点 */
    {
        struct my_data *pos, *n;
        int deleted = 0;
        list_for_each_entry_safe(pos, n, &head, node) {
            if (pos->value % 2 == 0) {
                Debug_printx("Safe iter delete: [%d]", pos->value);
                list_del(&pos->node);
                free(pos);
                deleted++;
            }
        }
        TEST_ASSERT(deleted == 2, "list_for_each_entry_safe - deleted 2 even",
                    "should delete 2 even numbers (2, 4)");
    }

    print_list(&head, "After deleting evens: 1, 3, 5");

    /* list_for_each_entry_safe_reverse: 反向安全删除全部 */
    {
        struct my_data *pos, *n;
        int deleted = 0;
        int expected[] = {5, 3, 1};
        list_for_each_entry_safe_reverse(pos, n, &head, node) {
            if (pos->value != expected[deleted]) {
                TEST_FAIL("list_for_each_entry_safe_reverse - order",
                          "value mismatch");
                /* 清理剩余 */
                list_for_each_entry_safe(pos, n, &head, node) {
                    list_del(&pos->node);
                    free(pos);
                }
                goto next_safe;
            }
            list_del(&pos->node);
            free(pos);
            deleted++;
        }
        TEST_ASSERT(deleted == 3, "safe_reverse - deleted 3",
                    "should delete all 3 nodes");
        TEST_ASSERT(list_empty(&head), "safe_reverse - list empty",
                    "list should be empty");
        TEST_PASS("list_for_each_entry_safe_reverse - correct order (5, 3, 1)");
    }

next_safe:

    /* list_for_each_safe: 使用 list_head 指针遍历 */
    {
        INIT_LIST_HEAD(&head);
        struct my_data *a1 = create_data(10);
        struct my_data *a2 = create_data(20);
        list_add_tail(&a1->node, &head);
        list_add_tail(&a2->node, &head);

        struct list_head *lh_pos, *lh_n;
        int count = 0;
        list_for_each_safe(lh_pos, lh_n, &head) {
            struct my_data *d = list_entry(lh_pos, struct my_data, node);
            Debug_printx("list_for_each_safe: [%d]", d->value);
            count++;
        }
        TEST_ASSERT(count == 2, "list_for_each_safe - count 2",
                    "should iterate 2 nodes");

        free_all_entries(&head);
    }

    /* list_for_each_prev_safe: 反向使用 list_head 指针遍历 */
    {
        INIT_LIST_HEAD(&head);
        struct my_data *a1 = create_data(100);
        struct my_data *a2 = create_data(200);
        struct my_data *a3 = create_data(300);
        list_add_tail(&a1->node, &head);
        list_add_tail(&a2->node, &head);
        list_add_tail(&a3->node, &head);

        struct list_head *lh_pos, *lh_n;
        int values[3];
        int idx = 0;
        list_for_each_prev_safe(lh_pos, lh_n, &head) {
            struct my_data *d = list_entry(lh_pos, struct my_data, node);
            values[idx++] = d->value;
        }
        TEST_ASSERT(values[0] == 300 && values[1] == 200 && values[2] == 100,
                    "list_for_each_prev_safe - reverse order",
                    "expected 300, 200, 100");

        free_all_entries(&head);
    }

    /* list_for_each_entry_continue: 从指定位置继续遍历 */
    {
        INIT_LIST_HEAD(&head);
        struct my_data *a1 = create_data(1);
        struct my_data *a2 = create_data(2);
        struct my_data *a3 = create_data(3);
        struct my_data *a4 = create_data(4);
        list_add_tail(&a1->node, &head);
        list_add_tail(&a2->node, &head);
        list_add_tail(&a3->node, &head);
        list_add_tail(&a4->node, &head);

        /* 从 a2 之后继续遍历，应得到 3, 4 */
        int expected[] = {3, 4};
        int i = 0;
        struct my_data *pos = a2; /* 设置起始位置 */
        list_for_each_entry_continue(pos, &head, node) {
            if (pos->value == expected[i]) {
                i++;
            }
        }
        TEST_ASSERT(i == 2, "list_for_each_entry_continue - 2 nodes after a2",
                    "should iterate 3, 4");

        /* list_for_each_entry_from: 从 a3 开始（包含 a3）*/
        pos = a3;
        i = 0;
        int expected_from[] = {3, 4};
        list_for_each_entry_from(pos, &head, node) {
            if (pos->value == expected_from[i]) {
                i++;
            }
        }
        TEST_ASSERT(i == 2, "list_for_each_entry_from - 2 nodes from a3",
                    "should iterate 3, 4");

        free_all_entries(&head);
    }

    /* list_next_entry / list_prev_entry */
    {
        INIT_LIST_HEAD(&head);
        struct my_data *a1 = create_data(10);
        struct my_data *a2 = create_data(20);
        struct my_data *a3 = create_data(30);
        list_add_tail(&a1->node, &head);
        list_add_tail(&a2->node, &head);
        list_add_tail(&a3->node, &head);

        struct my_data *next = list_next_entry(a1, node);
        struct my_data *prev = list_prev_entry(a3, node);
        TEST_ASSERT(next->value == 20, "list_next_entry - a1 next is 20",
                    "next of a1 should be a2 (value 20)");
        TEST_ASSERT(prev->value == 20, "list_prev_entry - a3 prev is 20",
                    "prev of a3 should be a2 (value 20)");

        free_all_entries(&head);
    }

    Debug_printx("---------- Part 8 Done ----------\r\n");
}


/* ================================================================== */
/*                                                                    */
/*     main - 主函数                                                   */
/*                                                                    */
/* ================================================================== */

/**
 * @func         main
 * @brief        测试程序主函数
 * @details      依次执行8个测试模块，覆盖 KernelLinkedList.h 全部API
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
    Debug_printx("  KernelLinkedList Test Program Start");
    Debug_printx("========================================");

    test_basic_operations();        /* Part 1: 基础操作 */
    test_query_operations();        /* Part 2: 查询操作 */
    test_replace_move_operations(); /* Part 3: 替换与移动 */
    test_splice_operations();       /* Part 4: 拼接操作 */
    test_rotate_operations();       /* Part 5: 旋转操作 */
    test_cut_operation();           /* Part 6: 截取操作 */
    test_static_init();             /* Part 7: 静态初始化 */
    test_safe_iteration();          /* Part 8: 安全遍历删除 */

    Debug_printx("========================================");
    Debug_printx("  KernelLinkedList Test Program End");
    Debug_printx("========================================");

    Debug_printx("Program exit");
    return 0;
}

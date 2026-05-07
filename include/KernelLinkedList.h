/**
 * @file        KernelLinkedList.h
 * @brief       LinuxARM-PublicLib-内核风格侵入式双向循环链表-公共API头文件
 * @details     IMX6ULL平台
 *              本文件提供 Linux 内核风格的侵入式双向循环链表实现，
 *              移植自 Linux Kernel list.h，适用于嵌入式 C 项目。
 *
 *              核心特性:
 *              - 侵入式设计: 链表节点嵌入用户数据结构中，无需额外分配节点内存
 *              - 双向循环链表: 支持前向/后向遍历，O(1) 插入/删除
 *              - 类型安全: 通过 container_of 宏从链表节点反查宿主结构体
 *              - 零依赖: 纯宏 + static inline 实现，无库依赖
 *              - 非线程安全: 多线程环境需调用者自行加锁保护
 *
 *              使用示例:
 *              @code
 *              struct my_data {
 *                  int value;
 *                  struct list_head node;   // 链表节点嵌入
 *              };
 *
 *              struct list_head head;
 *              INIT_LIST_HEAD(&head);
 *
 *              struct my_data *data = malloc(sizeof(*data));
 *              data->value = 42;
 *              list_add_tail(&data->node, &head);
 *
 *              struct my_data *pos;
 *              list_for_each_entry(pos, &head, node) {
 *                  printf("value = %d\n", pos->value);
 *              }
 *              @endcode
 *
 * @author      zlzksrl
 * @Version     V1.0.0
 * @date        2026-05-07
 * @copyright   copyright (C) 2026
 */

/**
 * @date        2026-05-07
 * @Version     V1.0.0
 * @brief       创建文件，提供 Linux 内核风格侵入式双向循环链表全套API
 * @author      zlzksrl
 */
#ifndef KERNELLINKEDLIST_H_
#define KERNELLINKEDLIST_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif


/* ================================================================== */
/*                                                                    */
/*     基础宏定义 - offsetof / container_of                            */
/*                                                                    */
/*  说明:                                                              */
/*    offsetof    - 获取结构体成员在结构体中的偏移量                      */
/*    container_of - 通过结构体成员指针反查宿主结构体指针                  */
/*                                                                    */
/*  注意:                                                              */
/*    container_of 依赖 GCC typeof 扩展，需使用 GCC 编译器               */
/*                                                                    */
/* ================================================================== */

/**
 * @macro       offsetof
 * @brief       获取结构体成员在结构体中的字节偏移量
 * @details     利用编译器内置 __builtin_offsetof（如果可用），
 *              否则使用指针运算 ((size_t) &((TYPE *)0)->MEMBER)。
 * @param       TYPE:   结构体类型名
 * @param       MEMBER: 结构体成员名
 * @return      成员的字节偏移量 (size_t)
 */
#ifndef offsetof
#define offsetof(TYPE, MEMBER) __builtin_offsetof(TYPE, MEMBER)
#endif

/**
 * @macro       container_of
 * @brief       通过结构体成员指针反查宿主结构体指针
 * @details     给定一个指向结构体成员的指针，计算出包含该成员的宿主结构体的首地址。
 *              利用 typeof 扩展进行类型检查，确保 ptr 类型与 member 类型一致。
 * @param       ptr:    指向结构体成员的指针
 * @param       type:   宿主结构体类型名
 * @param       member: 成员在宿主结构体中的名称
 * @return      指向宿主结构体的指针 (type*)
 * @warning     依赖 GCC typeof 扩展，需使用 GCC 兼容编译器
 * @note        如需移植到不支持 typeof 的编译器，可使用 C11 _Generic 或
 *              直接强制类型转换替代
 */
#ifndef container_of
#define container_of(ptr, type, member) ({                          \
    const typeof( ((type *)0)->member ) *__mptr = (ptr);           \
    (type *)( (char *)__mptr - offsetof(type, member) ); })
#endif


/* ================================================================== */
/*                                                                    */
/*     链表节点定义与初始化                                             */
/*                                                                    */
/*  说明:                                                              */
/*    list_head 是双向循环链表的核心节点结构体，                        */
/*    仅包含 prev/next 两个指针，不承载数据。                           */
/*    用户将此结构体嵌入自己的数据结构中来使用链表功能。                   */
/*                                                                    */
/*  初始化方式:                                                        */
/*    1. 静态初始化: LIST_HEAD(name) 或 LIST_HEAD_INIT(var)           */
/*    2. 动态初始化: INIT_LIST_HEAD(ptr)                              */
/*                                                                    */
/* ================================================================== */

/**
 * @struct      list_head
 * @brief       双向循环链表节点结构体
 * @details     内核风格链表的核心数据结构，仅包含前驱和后继指针。
 *              用户应将此结构体作为成员嵌入到自定义数据结构中。
 *              链表头也是一个 list_head，其 prev 指向尾节点，next 指向首节点。
 *              空链表的头节点 prev/next 均指向自身。
 */
struct list_head {
    struct list_head *next;     /**< 后继节点指针 */
    struct list_head *prev;     /**< 前驱节点指针 */
};

/**
 * @macro       LIST_HEAD_INIT
 * @brief       静态初始化 list_head 的初始化器
 * @details     用于在变量声明时进行静态初始化，使 prev/next 均指向自身。
 * @param       name: list_head 变量名
 * @return      初始化器 { &(name), &(name) }
 * @note        通常使用 LIST_HEAD 宏代替直接使用此宏
 */
#define LIST_HEAD_INIT(name) { &(name), &(name) }

/**
 * @macro       LIST_HEAD
 * @brief       定义并静态初始化一个链表头
 * @details     声明一个 list_head 变量并静态初始化为空链表。
 * @param       name: 链表头变量名
 * @note        等价于 struct list_head name = LIST_HEAD_INIT(name)
 * @par 使用示例:
 * @code
 *   LIST_HEAD(my_list);  // 定义并初始化一个空链表
 * @endcode
 */
#define LIST_HEAD(name) \
    struct list_head name = LIST_HEAD_INIT(name)

/**
 * @func        INIT_LIST_HEAD
 * @brief       动态初始化链表头
 * @details     将 list_head 的 prev/next 指针均指向自身，初始化为空链表。
 *              适用于在运行时初始化链表头（如动态分配的结构体成员）。
 * @param[in]   list: 链表头指针
 * @warning     list 不能为 NULL
 * @par 使用示例:
 * @code
 *   struct list_head head;
 *   INIT_LIST_HEAD(&head);
 * @endcode
 */
static inline void INIT_LIST_HEAD(struct list_head *list)
{
    list->next = list;
    list->prev = list;
}


/* ================================================================== */
/*                                                                    */
/*     内部辅助函数 (Internal Helper)                                  */
/*                                                                    */
/*  说明:                                                              */
/*    list_add_raw / list_del_raw 为底层指针操作函数，                  */
/*    不做参数校验，仅供 list_add / list_del 等公共API内部调用。         */
/*    外部不应直接调用这些函数。                                        */
/*                                                                    */
/* ================================================================== */

/**
 * @func        list_add_raw
 * @brief       在两个已知节点之间插入新节点（内部辅助函数）
 * @details     将 new_node 插入到 prev 和 next 之间。
 *              调用前需保证 prev->next == next 且 next->prev == prev。
 * @param[in]   new_node: 待插入的新节点
 * @param[in]   prev:     前驱节点
 * @param[in]   next:     后继节点
 * @warning     仅供内部调用，不做参数校验
 */
static inline void list_add_raw(struct list_head *new_node,
                                struct list_head *prev,
                                struct list_head *next)
{
    next->prev = new_node;
    new_node->next = next;
    new_node->prev = prev;
    prev->next = new_node;
}

/**
 * @func        list_del_raw
 * @brief       通过前驱/后继指针删除节点（内部辅助函数）
 * @details     将 prev->next 指向 next，next->prev 指向 prev，
 *              使中间节点从链表中脱离。
 * @param[in]   prev: 待删除节点的前驱
 * @param[in]   next: 待删除节点的后继
 * @warning     仅供内部调用，不修改被删除节点的指针
 */
static inline void list_del_raw(struct list_head *prev, struct list_head *next)
{
    next->prev = prev;
    prev->next = next;
}


/* ================================================================== */
/*                                                                    */
/*     节点添加操作                                                    */
/*                                                                    */
/*  API列表:                                                          */
/*    list_add        - 在链表头部（head->next）插入新节点               */
/*    list_add_tail   - 在链表尾部（head->prev）插入新节点               */
/*                                                                    */
/*  时间复杂度: O(1)                                                   */
/*                                                                    */
/* ================================================================== */

/**
 * @func        list_add
 * @brief       在链表头部插入新节点
 * @details     将 new_node 插入到 head 之后（即链表的第一个位置）。
 *              常用于实现栈（LIFO）结构。
 * @param[in]   new_node: 待插入的新节点指针
 * @param[in]   head:     链表头指针
 * @warning     new_node 必须是未被链入其他链表的独立节点，否则会导致链表损坏
 * @par 使用示例:
 * @code
 *   list_add(&data->node, &head);  // data 成为链表第一个元素
 * @endcode
 */
static inline void list_add(struct list_head *new_node, struct list_head *head)
{
    list_add_raw(new_node, head, head->next);
}

/**
 * @func        list_add_tail
 * @brief       在链表尾部插入新节点
 * @details     将 new_node 插入到 head 之前（即链表的最后一个位置）。
 *              常用于实现队列（FIFO）结构。
 * @param[in]   new_node: 待插入的新节点指针
 * @param[in]   head:     链表头指针
 * @warning     new_node 必须是未被链入其他链表的独立节点
 * @par 使用示例:
 * @code
 *   list_add_tail(&data->node, &head);  // data 成为链表最后一个元素
 * @endcode
 */
static inline void list_add_tail(struct list_head *new_node, struct list_head *head)
{
    list_add_raw(new_node, head->prev, head);
}


/* ================================================================== */
/*                                                                    */
/*     节点删除操作                                                    */
/*                                                                    */
/*  API列表:                                                          */
/*    list_del        - 删除节点并置空指针（不可再使用）                  */
/*    list_del_init   - 删除节点并重新初始化（可再次链入）                */
/*                                                                    */
/*  时间复杂度: O(1)                                                   */
/*  注意: 删除操作不会释放宿主结构体内存，需调用者自行释放                 */
/*                                                                    */
/* ================================================================== */

/**
 * @macro       LIST_POISON1
 * @brief       list_del 后 next 指针的毒值
 * @details     默认为 NULL。可在包含本头文件前定义为非零值（如 (void*)0x100）
 *              以便在调试时识别已删除节点的野指针访问。
 */
#ifndef LIST_POISON1
#define LIST_POISON1  ((struct list_head *)0)
#endif

/**
 * @macro       LIST_POISON2
 * @brief       list_del 后 prev 指针的毒值
 * @details     默认为 NULL。可在包含本头文件前定义为非零值（如 (void*)0x200）
 *              以便在调试时识别已删除节点的野指针访问。
 */
#ifndef LIST_POISON2
#define LIST_POISON2  ((struct list_head *)0)
#endif

/**
 * @func        list_del
 * @brief       从链表中删除节点
 * @details     将 entry 从所在链表中移除，并将 entry 的 prev/next 置为毒值。
 *              删除后 entry 不属于任何链表，不可再对其使用链表操作。
 * @param[in]   entry: 待删除的节点指针
 * @warning     删除后需由调用者负责释放 entry 所属宿主结构体的内存
 * @note        如果删除后需要重新链入，建议使用 list_del_init
 * @par 使用示例:
 * @code
 *   list_del(&data->node);
 *   free(data);
 * @endcode
 */
static inline void list_del(struct list_head *entry)
{
    list_del_raw(entry->prev, entry->next);
    entry->next = LIST_POISON1;
    entry->prev = LIST_POISON2;
}

/**
 * @func        list_del_init
 * @brief       从链表中删除节点并重新初始化
 * @details     将 entry 从所在链表中移除，然后重新初始化为独立的空节点。
 *              删除后 entry 可以立即再次链入其他链表。
 * @param[in]   entry: 待删除并重新初始化的节点指针
 * @note        适用于需要将节点从一个链表移动到另一个链表的场景
 * @par 使用示例:
 * @code
 *   list_del_init(&data->node);   // 从原链表移除
 *   list_add(&data->node, &head2); // 链入新链表
 * @endcode
 */
static inline void list_del_init(struct list_head *entry)
{
    list_del_raw(entry->prev, entry->next);
    INIT_LIST_HEAD(entry);
}


/* ================================================================== */
/*                                                                    */
/*     节点替换与移动操作                                               */
/*                                                                    */
/*  API列表:                                                          */
/*    list_replace      - 用新节点替换旧节点                             */
/*    list_replace_init - 替换旧节点并将旧节点重新初始化                   */
/*    list_swap         - 交换两个节点的位置                              */
/*    list_move         - 将节点移动到另一个链表头部                      */
/*    list_move_tail    - 将节点移动到另一个链表尾部                      */
/*                                                                    */
/*  时间复杂度: O(1)                                                   */
/*                                                                    */
/* ================================================================== */

/**
 * @func        list_replace
 * @brief       用新节点替换链表中的旧节点
 * @details     将 old_node 从链表中移除，并将 new_node 插入到 old_node 原来的位置。
 *              old_node 的指针不会被修改。
 * @param[in]   old_node: 被替换的旧节点
 * @param[in]   new_node: 替换的新节点
 * @warning     old_node 必须在链表中；new_node 必须不在任何链表中
 */
static inline void list_replace(struct list_head *old_node,
                                struct list_head *new_node)
{
    if (old_node == new_node) {
        return;
    }
    new_node->next = old_node->next;
    new_node->next->prev = new_node;
    new_node->prev = old_node->prev;
    new_node->prev->next = new_node;
}

/**
 * @func        list_replace_init
 * @brief       用新节点替换旧节点，并将旧节点重新初始化
 * @details     等价于 list_replace + INIT_LIST_HEAD(old_node)。
 *              替换后 old_node 变为独立的空节点。
 * @param[in]   old_node: 被替换的旧节点（替换后被重新初始化）
 * @param[in]   new_node: 替换的新节点
 */
static inline void list_replace_init(struct list_head *old_node,
                                     struct list_head *new_node)
{
    list_replace(old_node, new_node);
    INIT_LIST_HEAD(old_node);
}

/**
 * @func        list_swap
 * @brief       交换链表中两个节点的位置
 * @details     交换 entry1 和 entry2 在链表中的位置。
 *              如果两个节点在同一个链表中，交换后链表结构保持正确。
 *              如果两个节点在不同链表中，效果等同于互相移动。
 * @param[in]   entry1: 第一个节点
 * @param[in]   entry2: 第二个节点
 */
static inline void list_swap(struct list_head *entry1,
                             struct list_head *entry2)
{
    struct list_head *pos;

    if (entry1 == entry2) {
        return;
    }

    pos = entry2->prev;
    list_del(entry2);
    list_add(entry2, entry1);
    if (pos == entry1) {
        pos = entry2;
    }
    list_del(entry1);
    list_add(entry1, pos);
}

/**
 * @func        list_move
 * @brief       将节点从当前链表移动到目标链表的头部
 * @details     先从当前链表中移除 list，再将其插入到 head 之后。
 * @param[in]   list: 待移动的节点
 * @param[in]   head: 目标链表头
 */
static inline void list_move(struct list_head *list, struct list_head *head)
{
    list_del_raw(list->prev, list->next);
    list_add(list, head);
}

/**
 * @func        list_move_tail
 * @brief       将节点从当前链表移动到目标链表的尾部
 * @details     先从当前链表中移除 list，再将其插入到 head 之前。
 * @param[in]   list: 待移动的节点
 * @param[in]   head: 目标链表头
 */
static inline void list_move_tail(struct list_head *list,
                                  struct list_head *head)
{
    list_del_raw(list->prev, list->next);
    list_add_tail(list, head);
}


/* ================================================================== */
/*                                                                    */
/*     链表查询操作                                                    */
/*                                                                    */
/*  API列表:                                                          */
/*    list_is_first     - 判断节点是否为链表第一个节点                    */
/*    list_is_last      - 判断节点是否为链表最后一个节点                  */
/*    list_empty        - 判断链表是否为空                               */
/*    list_is_singular  - 判断链表是否只有一个节点                       */
/*    list_is_head      - 判断节点是否为链表头                            */
/*    list_count_nodes  - 统计链表中的节点数量                            */
/*                                                                    */
/* ================================================================== */

/**
 * @func        list_is_first
 * @brief       判断节点是否为链表的第一个节点
 * @details     检查 list 是否紧接在 head 之后（即 head->next == list）。
 * @param[in]   list: 待检查的节点
 * @param[in]   head: 链表头
 * @return      int
 * @retval      1: 是第一个节点
 * @retval      0: 不是第一个节点
 */
static inline int list_is_first(const struct list_head *list,
                                const struct list_head *head)
{
    return list->prev == head;
}

/**
 * @func        list_is_last
 * @brief       判断节点是否为链表的最后一个节点
 * @details     检查 list 是否紧接在 head 之前（即 head->prev == list）。
 * @param[in]   list: 待检查的节点
 * @param[in]   head: 链表头
 * @return      int
 * @retval      1: 是最后一个节点
 * @retval      0: 不是最后一个节点
 */
static inline int list_is_last(const struct list_head *list,
                               const struct list_head *head)
{
    return list->next == head;
}

/**
 * @func        list_empty
 * @brief       判断链表是否为空
 * @details     当 head->next == head 时，链表为空（无任何数据节点）。
 * @param[in]   head: 链表头
 * @return      int
 * @retval      1: 链表为空
 * @retval      0: 链表不为空
 */
static inline int list_empty(const struct list_head *head)
{
    return head->next == head;
}

/**
 * @func        list_is_singular
 * @brief       判断链表是否只有一个节点
 * @details     链表不为空且 head->next == head->prev 时返回 1。
 * @param[in]   head: 链表头
 * @return      int
 * @retval      1: 链表只有一个节点
 * @retval      0: 链表为空或有多个节点
 */
static inline int list_is_singular(const struct list_head *head)
{
    return !list_empty(head) && (head->next == head->prev);
}

/**
 * @func        list_is_head
 * @brief       判断节点是否为链表头
 * @details     检查 list 是否就是 head 本身（通过地址比较）。
 *              在遍历中常用于判断是否回到链表头（结束条件）。
 * @param[in]   list: 待检查的节点
 * @param[in]   head: 链表头
 * @return      int
 * @retval      1: list 就是链表头
 * @retval      0: list 不是链表头
 */
static inline int list_is_head(const struct list_head *list,
                               const struct list_head *head)
{
    return list == head;
}

/**
 * @func        list_count_nodes
 * @brief       统计链表中的节点数量
 * @details     从 head->next 开始遍历，直到回到 head，统计节点个数。
 * @param[in]   head: 链表头
 * @return      int 链表中的节点数量
 * @retval      >=0: 节点数量
 * @warning     时间复杂度 O(n)，频繁调用可能影响性能
 */
static inline unsigned int list_count_nodes(const struct list_head *head)
{
    const struct list_head *pos;
    unsigned int count = 0;

    for (pos = head->next; pos != head; pos = pos->next)
        count++;

    return count;
}


/* ================================================================== */
/*                                                                    */
/*     链表拼接操作                                                    */
/*                                                                    */
/*  API列表:                                                          */
/*    list_splice           - 将 list 拼接到 head 之后                   */
/*    list_splice_tail      - 将 list 拼接到 head 之前                   */
/*    list_splice_init      - 拼接到 head 之后并重新初始化 list           */
/*    list_splice_tail_init - 拼接到 head 之前并重新初始化 list           */
/*                                                                    */
/*  说明:                                                              */
/*    list 是一个待合并链表的头节点，拼接后 list 的所有节点被转移到        */
/*    head 所在的链表中，list 变为空链表（_init 版本）。                  */
/*                                                                    */
/* ================================================================== */

/**
 * @func        list_splice
 * @brief       将 list 链表拼接到 head 链表的头部
 * @details     将 list 链表中的所有节点插入到 head 之后。
 *              拼接后 list 链表头变为未使用状态（但仍指向原节点），
 *              调用者不应再使用 list 作为有效链表头，建议使用
 *              list_splice_init 以自动重新初始化 list。
 * @param[in]   list: 待拼接的链表头（其节点将被合并到 head 链表），不能为 NULL
 * @param[in]   head: 目标链表头
 * @warning     如果 list 为空，不做任何操作
 */
static inline void list_splice(const struct list_head *list,
                               struct list_head *head)
{
    if (list != NULL && !list_empty(list)) {
        struct list_head *first = list->next;
        struct list_head *last  = list->prev;
        struct list_head *at    = head->next;

        first->prev = head;
        head->next  = first;

        last->next = at;
        at->prev   = last;
    }
}

/**
 * @func        list_splice_tail
 * @brief       将 list 链表拼接到 head 链表的尾部
 * @details     将 list 链表中的所有节点插入到 head 之前。
 *              等价于将 list 的节点追加到 head 链表的末尾。
 *              拼接后 list 链表头变为未使用状态，建议使用
 *              list_splice_tail_init 以自动重新初始化 list。
 * @param[in]   list: 待拼接的链表头，不能为 NULL
 * @param[in]   head: 目标链表头
 * @warning     如果 list 为空，不做任何操作
 */
static inline void list_splice_tail(const struct list_head *list,
                                    struct list_head *head)
{
    if (list != NULL && !list_empty(list)) {
        struct list_head *first = list->next;
        struct list_head *last  = list->prev;
        struct list_head *at    = head->prev;

        first->prev = at;
        at->next    = first;

        last->next = head;
        head->prev = last;
    }
}

/**
 * @func        list_splice_init
 * @brief       将 list 链表拼接到 head 链表头部，并重新初始化 list
 * @details     等价于 list_splice + INIT_LIST_HEAD(list)。
 *              拼接后 list 被重新初始化为空链表。
 * @param[in]   list: 待拼接的链表头（拼接后被重新初始化）
 * @param[in]   head: 目标链表头
 */
static inline void list_splice_init(struct list_head *list,
                                    struct list_head *head)
{
    if (!list_empty(list)) {
        list_splice(list, head);
        INIT_LIST_HEAD(list);
    }
}

/**
 * @func        list_splice_tail_init
 * @brief       将 list 链表拼接到 head 链表尾部，并重新初始化 list
 * @details     等价于 list_splice_tail + INIT_LIST_HEAD(list)。
 *              拼接后 list 被重新初始化为空链表。
 * @param[in]   list: 待拼接的链表头（拼接后被重新初始化）
 * @param[in]   head: 目标链表头
 */
static inline void list_splice_tail_init(struct list_head *list,
                                         struct list_head *head)
{
    if (!list_empty(list)) {
        list_splice_tail(list, head);
        INIT_LIST_HEAD(list);
    }
}


/* ================================================================== */
/*                                                                    */
/*     链表旋转操作                                                    */
/*                                                                    */
/*  API列表:                                                          */
/*    list_rotate_left   - 将链表第一个节点旋转到末尾                     */
/*    list_rotate_to_front - 将指定节点旋转到链表头部                    */
/*                                                                    */
/* ================================================================== */

/**
 * @func        list_rotate_left
 * @brief       将链表的第一个节点旋转到末尾
 * @details     取出链表的第一个节点（head->next），将其移动到链表尾部。
 *              如果链表为空或只有一个节点，不做任何操作。
 * @param[in]   head: 链表头
 */
static inline void list_rotate_left(struct list_head *head)
{
    struct list_head *first;

    if (!list_empty(head)) {
        first = head->next;
        list_move_tail(first, head);
    }
}

/**
 * @func        list_rotate_to_front
 * @brief       将指定节点旋转到链表头部
 * @details     将 list 从当前位置移除，插入到 head 之后成为第一个节点。
 * @param[in]   list: 要旋转到头部的节点
 * @param[in]   head: 链表头
 */
static inline void list_rotate_to_front(struct list_head *list,
                                        struct list_head *head)
{
    list_move(list, head);
}


/* ================================================================== */
/*                                                                    */
/*     链表截取操作                                                    */
/*                                                                    */
/*  API列表:                                                          */
/*    list_cut_before    - 在指定节点前截断链表                           */
/*                                                                    */
/*  说明:                                                              */
/*    将 head 链表从 entry 之前截断，entry 及其之后的所有节点              */
/*    被移动到 list 链表中。                                            */
/*                                                                    */
/* ================================================================== */

/**
 * @func        list_cut_before
 * @brief       在指定节点前截断链表
 * @details     将 head 链表从 entry 之前截断。
 *              entry 及其之后的所有节点被移动到 list 链表中。
 *              截断后 head 链表只保留 entry 之前的节点。
 * @param[in]   list:  新链表头，用于接收截取的节点
 * @param[in]   head:  原链表头
 * @param[in]   entry: 截断位置，此节点及之后的节点被移到 list
 * @warning     list 必须是空链表；entry 必须在 head 链表中
 */
static inline void list_cut_before(struct list_head *list,
                                   struct list_head *head,
                                   struct list_head *entry)
{
    struct list_head *old_last;

    if (head->next == entry) {
        INIT_LIST_HEAD(list);
        return;
    }

    old_last = entry->prev;

    list->next       = entry;
    list->prev       = head->prev;
    list->prev->next = list;
    entry->prev      = list;

    head->prev       = old_last;
    old_last->next   = head;
}


/* ================================================================== */
/*                                                                    */
/*     入口获取宏 (Entry Access Macros)                                */
/*                                                                    */
/*  说明:                                                              */
/*    通过 list_head 指针获取包含它的宿主结构体指针。                     */
/*    这些宏是内核链表的核心优势所在，实现了侵入式设计。                   */
/*                                                                    */
/*  API列表:                                                          */
/*    list_entry        - 从 list_head 指针获取宿主结构体指针             */
/*    list_first_entry  - 获取链表第一个节点的宿主结构体指针              */
/*    list_last_entry   - 获取链表最后一个节点的宿主结构体指针            */
/*    list_next_entry   - 获取下一个节点的宿主结构体指针                  */
/*    list_prev_entry   - 获取上一个节点的宿主结构体指针                  */
/*    list_safe_reset_next - 安全地获取下一个节点（用于删除场景）          */
/*                                                                    */
/* ================================================================== */

/**
 * @macro       list_entry
 * @brief       从 list_head 指针获取包含它的宿主结构体指针
 * @details     container_of 的封装，通过成员指针反查宿主结构体首地址。
 * @param       ptr:    指向 struct list_head 的指针
 * @param       type:   宿主结构体类型
 * @param       member: list_head 在宿主结构体中的成员名
 * @return      指向宿主结构体的指针 (type*)
 * @par 使用示例:
 * @code
 *   struct my_data *data = list_entry(node_ptr, struct my_data, node);
 * @endcode
 */
#define list_entry(ptr, type, member) \
    container_of(ptr, type, member)

/**
 * @macro       list_first_entry
 * @brief       获取链表第一个节点的宿主结构体指针
 * @details     等价于 list_entry(head->next, type, member)。
 *              调用前应确保链表不为空。
 * @param       ptr:    链表头指针 (struct list_head *)
 * @param       type:   宿主结构体类型
 * @param       member: list_head 在宿主结构体中的成员名
 * @return      指向第一个节点的宿主结构体指针 (type*)
 * @warning     如果链表为空，返回的是包含 head 的结构体（未定义行为）
 */
#define list_first_entry(ptr, type, member) \
    list_entry((ptr)->next, type, member)

/**
 * @macro       list_last_entry
 * @brief       获取链表最后一个节点的宿主结构体指针
 * @details     等价于 list_entry(head->prev, type, member)。
 *              调用前应确保链表不为空。
 * @param       ptr:    链表头指针 (struct list_head *)
 * @param       type:   宿主结构体类型
 * @param       member: list_head 在宿主结构体中的成员名
 * @return      指向最后一个节点的宿主结构体指针 (type*)
 * @warning     如果链表为空，返回的是包含 head 的结构体（未定义行为）
 */
#define list_last_entry(ptr, type, member) \
    list_entry((ptr)->prev, type, member)

/**
 * @macro       list_next_entry
 * @brief       获取当前节点下一个节点的宿主结构体指针
 * @details     用于遍历中获取下一个节点。
 * @param       pos:    当前节点的宿主结构体指针
 * @param       member: list_head 在宿主结构体中的成员名
 * @return      下一个节点的宿主结构体指针
 */
#define list_next_entry(pos, member) \
    list_entry((pos)->member.next, typeof(*(pos)), member)

/**
 * @macro       list_prev_entry
 * @brief       获取当前节点上一个节点的宿主结构体指针
 * @details     用于反向遍历中获取上一个节点。
 * @param       pos:    当前节点的宿主结构体指针
 * @param       member: list_head 在宿主结构体中的成员名
 * @return      上一个节点的宿主结构体指针
 */
#define list_prev_entry(pos, member) \
    list_entry((pos)->member.prev, typeof(*(pos)), member)

/**
 * @macro       list_safe_reset_next
 * @brief       安全重置 next 指针（用于 list_for_each_entry_safe 中的删除场景）
 * @details     当 pos 被删除后，使用此宏可以安全地获取下一个节点。
 * @param       pos:    当前节点（已被删除）
 * @param       n:      临时变量，用于存储下一个节点
 * @param       member: list_head 在宿主结构体中的成员名
 */
#define list_safe_reset_next(pos, n, member) \
    (n) = list_next_entry(pos, member)


/* ================================================================== */
/*                                                                    */
/*     链表遍历宏 (Iteration Macros)                                   */
/*                                                                    */
/*  说明:                                                              */
/*    提供多种遍历方式，分为基础遍历和安全遍历两类:                       */
/*    - 基础遍历: 遍历期间不可删除当前节点                               */
/*    - 安全遍历 (_safe): 预存下一个节点，允许删除当前节点                 */
/*                                                                    */
/*  API列表:                                                          */
/*    list_for_each               - 基础正向遍历 (list_head 指针)        */
/*    list_for_each_prev          - 基础反向遍历                          */
/*    list_for_each_safe          - 安全正向遍历（可删除当前节点）         */
/*    list_for_each_prev_safe     - 安全反向遍历（可删除当前节点）         */
/*    list_for_each_entry         - 正向遍历宿主结构体                    */
/*    list_for_each_entry_reverse - 反向遍历宿主结构体                    */
/*    list_for_each_entry_safe    - 安全正向遍历宿主结构体（可删除）       */
/*    list_for_each_entry_safe_reverse - 安全反向遍历宿主结构体（可删除） */
/*    list_for_each_entry_continue - 从当前节点继续正向遍历               */
/*    list_for_each_entry_continue_reverse - 从当前节点继续反向遍历       */
/*    list_for_each_entry_from    - 从指定节点开始遍历                    */
/*    list_prepare_entry          - 准备 entry 用于 continue 类遍历       */
/*                                                                    */
/*  注意:                                                              */
/*    所有遍历宏中，head 参数是链表头（不在链表中），                     */
/*    遍历到 head 时表示遍历结束。                                      */
/*                                                                    */
/* ================================================================== */

/**
 * @macro       list_for_each
 * @brief       正向遍历链表（使用 list_head 指针）
 * @details     从 head->next 开始，直到回到 head。
 *              遍历期间不可删除当前节点 pos。
 * @param       pos:  循环变量，struct list_head 指针
 * @param       head: 链表头
 * @par 使用示例:
 * @code
 *   struct list_head *pos;
 *   list_for_each(pos, &head) {
 *       struct my_data *data = list_entry(pos, struct my_data, node);
 *       // 使用 data ...
 *   }
 * @endcode
 */
#define list_for_each(pos, head) \
    for (pos = (head)->next; pos != (head); pos = pos->next)

/**
 * @macro       list_for_each_prev
 * @brief       反向遍历链表（使用 list_head 指针）
 * @details     从 head->prev 开始向前遍历，直到回到 head。
 *              遍历期间不可删除当前节点 pos。
 * @param       pos:  循环变量，struct list_head 指针
 * @param       head: 链表头
 */
#define list_for_each_prev(pos, head) \
    for (pos = (head)->prev; pos != (head); pos = pos->prev)

/**
 * @macro       list_for_each_safe
 * @brief       安全正向遍历链表（可删除当前节点）
 * @details     预存下一个节点到 n，允许在循环体内安全删除 pos。
 * @param       pos:  循环变量，当前节点
 * @param       n:    临时变量，预存下一个节点（struct list_head 指针）
 * @param       head: 链表头
 * @par 使用示例:
 * @code
 *   struct list_head *pos, *n;
 *   list_for_each_safe(pos, n, &head) {
 *       struct my_data *data = list_entry(pos, struct my_data, node);
 *       list_del(pos);
 *       free(data);
 *   }
 * @endcode
 */
#define list_for_each_safe(pos, n, head) \
    for (pos = (head)->next, n = pos->next; pos != (head); \
         pos = n, n = pos->next)

/**
 * @macro       list_for_each_prev_safe
 * @brief       安全反向遍历链表（可删除当前节点）
 * @details     预存前一个节点到 n，允许在循环体内安全删除 pos。
 * @param       pos:  循环变量，当前节点
 * @param       n:    临时变量，预存前一个节点
 * @param       head: 链表头
 */
#define list_for_each_prev_safe(pos, n, head) \
    for (pos = (head)->prev, n = pos->prev; pos != (head); \
         pos = n, n = pos->prev)

/**
 * @macro       list_for_each_entry
 * @brief       正向遍历链表，直接获取宿主结构体指针
 * @details     从链表第一个节点开始，依次获取宿主结构体指针 pos。
 *              这是最常用的遍历宏。
 *              遍历期间不可删除当前节点 pos。
 * @param       pos:    循环变量，宿主结构体指针
 * @param       head:   链表头
 * @param       member: list_head 在宿主结构体中的成员名
 * @par 使用示例:
 * @code
 *   struct my_data *pos;
 *   list_for_each_entry(pos, &head, node) {
 *       printf("value = %d\n", pos->value);
 *   }
 * @endcode
 */
#define list_for_each_entry(pos, head, member)                     \
    for (pos = list_first_entry(head, typeof(*pos), member);       \
         &pos->member != (head);                                   \
         pos = list_next_entry(pos, member))

/**
 * @macro       list_for_each_entry_reverse
 * @brief       反向遍历链表，直接获取宿主结构体指针
 * @details     从链表最后一个节点开始，依次向前获取宿主结构体指针。
 *              遍历期间不可删除当前节点 pos。
 * @param       pos:    循环变量，宿主结构体指针
 * @param       head:   链表头
 * @param       member: list_head 在宿主结构体中的成员名
 */
#define list_for_each_entry_reverse(pos, head, member)             \
    for (pos = list_last_entry(head, typeof(*pos), member);        \
         &pos->member != (head);                                   \
         pos = list_prev_entry(pos, member))

/**
 * @macro       list_prepare_entry
 * @brief       准备一个 entry 指针用于 continue 类遍历
 * @details     如果 pos 已知不为链表头，直接使用 pos；否则获取第一个 entry。
 *              通常与 list_for_each_entry_continue 配合使用。
 * @param       pos:    宿主结构体指针（可能为链表头所在的无效结构体）
 * @param       head:   链表头
 * @param       member: list_head 在宿主结构体中的成员名
 * @return      有效的起始 pos 或第一个 entry
 */
/* 注意: 使用了 GCC 条件表达式扩展 (?: 省略中间操作数) */
#define list_prepare_entry(pos, head, member) \
    ((pos) ? : list_entry(head, typeof(*(pos)), member))

/**
 * @macro       list_for_each_entry_continue
 * @brief       从当前节点之后继续正向遍历
 * @details     从 pos 的下一个节点开始遍历，直到回到 head。
 *              pos 本身不会被处理。
 * @param       pos:    起始位置（从此节点之后开始遍历）
 * @param       head:   链表头
 * @param       member: list_head 在宿主结构体中的成员名
 */
#define list_for_each_entry_continue(pos, head, member)            \
    for (pos = list_next_entry(pos, member);                       \
         &pos->member != (head);                                   \
         pos = list_next_entry(pos, member))

/**
 * @macro       list_for_each_entry_continue_reverse
 * @brief       从当前节点之前继续反向遍历
 * @details     从 pos 的上一个节点开始反向遍历，直到回到 head。
 * @param       pos:    起始位置（从此节点之前开始遍历）
 * @param       head:   链表头
 * @param       member: list_head 在宿主结构体中的成员名
 */
#define list_for_each_entry_continue_reverse(pos, head, member)    \
    for (pos = list_prev_entry(pos, member);                       \
         &pos->member != (head);                                   \
         pos = list_prev_entry(pos, member))

/**
 * @macro       list_for_each_entry_from
 * @brief       从指定节点开始正向遍历（包含当前节点）
 * @details     从 pos 开始遍历，pos 会被处理。
 * @param       pos:    起始位置（包含此节点）
 * @param       head:   链表头
 * @param       member: list_head 在宿主结构体中的成员名
 */
#define list_for_each_entry_from(pos, head, member)                \
    for (; &pos->member != (head);                                 \
         pos = list_next_entry(pos, member))

/**
 * @macro       list_for_each_entry_safe
 * @brief       安全正向遍历链表，直接获取宿主结构体指针（可删除当前节点）
 * @details     预存下一个宿主结构体指针到 n，允许在循环体内安全删除 pos。
 *              这是最常用的安全遍历宏。
 * @param       pos:    循环变量，当前节点的宿主结构体指针
 * @param       n:      临时变量，预存下一个节点的宿主结构体指针
 * @param       head:   链表头
 * @param       member: list_head 在宿主结构体中的成员名
 * @par 使用示例:
 * @code
 *   struct my_data *pos, *n;
 *   list_for_each_entry_safe(pos, n, &head, node) {
 *       list_del(&pos->node);
 *       free(pos);
 *   }
 * @endcode
 */
#define list_for_each_entry_safe(pos, n, head, member)             \
    for (pos = list_first_entry(head, typeof(*pos), member),       \
         n = list_next_entry(pos, member);                         \
         &pos->member != (head);                                   \
         pos = n, n = list_next_entry(n, member))

/**
 * @macro       list_for_each_entry_safe_reverse
 * @brief       安全反向遍历链表（可删除当前节点）
 * @details     预存前一个宿主结构体指针到 n，允许在循环体内安全删除 pos。
 * @param       pos:    循环变量，当前节点的宿主结构体指针
 * @param       n:      临时变量，预存前一个节点的宿主结构体指针
 * @param       head:   链表头
 * @param       member: list_head 在宿主结构体中的成员名
 */
#define list_for_each_entry_safe_reverse(pos, n, head, member)     \
    for (pos = list_last_entry(head, typeof(*pos), member),        \
         n = list_prev_entry(pos, member);                         \
         &pos->member != (head);                                   \
         pos = n, n = list_prev_entry(n, member))

/**
 * @macro       list_for_each_entry_continue_safe
 * @brief       从当前节点之后安全继续正向遍历（可删除当前节点）
 * @details     结合 continue 和 safe 语义，从 pos 之后开始遍历，
 *              并预存下一个节点以支持删除操作。
 * @param       pos:    起始位置
 * @param       n:      临时变量
 * @param       head:   链表头
 * @param       member: list_head 在宿主结构体中的成员名
 */
#define list_for_each_entry_continue_safe(pos, n, head, member)    \
    for (pos = list_next_entry(pos, member),                       \
         n = list_next_entry(pos, member);                         \
         &pos->member != (head);                                   \
         pos = n, n = list_next_entry(n, member))

/**
 * @macro       list_for_each_entry_from_safe
 * @brief       从指定节点开始安全正向遍历（可删除当前节点）
 * @details     结合 from 和 safe 语义，从 pos 开始遍历（包含 pos），
 *              并预存下一个节点以支持删除操作。
 * @param       pos:    起始位置（包含此节点）
 * @param       n:      临时变量
 * @param       head:   链表头
 * @param       member: list_head 在宿主结构体中的成员名
 */
#define list_for_each_entry_from_safe(pos, n, head, member)        \
    for (n = list_next_entry(pos, member);                         \
         &pos->member != (head);                                   \
         pos = n, n = list_next_entry(n, member))


#ifdef __cplusplus
}
#endif

#endif
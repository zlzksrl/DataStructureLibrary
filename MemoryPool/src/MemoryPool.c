/**
 * @file        MemoryPool.c
 * @brief       LinuxARM-PublicLib-内存池-核心实现文件
 * @details     IMX6ULL平台
 *              本文件实现内存池(MemoryPool)的全部公共API。
 *              固定大小对象池：预分配槽位、空闲链表(LIFO 内嵌 next)、循环复用、零 malloc。
 *              三种池满策略：DROP(返回NULL) / GROW(动态扩容) / BLOCK(阻塞等待)。
 *              mutex 保护 free_list；BLOCK 用 cond(CLOCK_MONOTONIC)，Free signal 唤醒。
 *
 * @author      zlzksrl
 * @Version     V1.0.0
 * @date        2026-07-10
 * @copyright   copyright (C) 2026
 */

/**
 * @date        2026-07-10
 * @Version     V1.0.0
 * @brief       创建文件，实现内存池全套API
 * @author      zlzksrl
 */
#include "../include/MemoryPool.h"
#include "MemoryPool_Main.h"
#include "MemoryPool_Maketime.h"


/* ========================== 内部辅助函数 ========================== */

/**
 * @func         mp_align_up
 * @brief        将 size 向上补齐到 MEMPOOL_ALIGN 的倍数
 */
static int mp_align_up(int size)
{
    return (size + MEMPOOL_ALIGN - 1) / MEMPOOL_ALIGN * MEMPOOL_ALIGN;
}

/**
 * @func         mp_size_overflow
 * @brief        检查 count × align_size 是否会溢出 size_t（32位平台防堆溢出）
 * @return       1=会溢出, 0=安全
 */
static int mp_size_overflow(int count, int align_size)
{
    return (count > 0 && align_size > 0 &&
            (size_t)count > (size_t)-1 / (size_t)align_size);
}

/**
 * @func         mp_set_name
 * @brief        拷贝名称到固定缓冲（截断保护）
 */
static void mp_set_name(char *dst, const char *src)
{
    size_t len = strlen(src);
    if(len < MAX_MEMORYPOOLNAME_LEN)
    {
        strncpy(dst, src, len);
        dst[len] = '\0';
    }
    else
    {
        strncpy(dst, src, MAX_MEMORYPOOLNAME_LEN);
        dst[MAX_MEMORYPOOLNAME_LEN] = '\0';
    }
}

/**
 * @func         mp_chunk_to_freelist
 * @brief        把 chunk 的 count 个槽位逐个头插串入 free_list（LIFO）
 * @details      调用者须持有 mux（或单线程 Init 阶段）。每个槽位开头存当前 free_list 头。
 */
static void mp_chunk_to_freelist(T_MemPool *p, T_MemPoolChunk *ch)
{
    int i;
    for(i = 0; i < ch->count; i++)
    {
        void *slot = ch->mem + (size_t)i * (size_t)p->align_size;
        *(void **)slot = p->free_list;   /* 槽位开头存当前链头 */
        p->free_list = slot;             /* 本槽位变新链头 */
    }
    p->free_count += ch->count;
}

/**
 * @func         mp_calc_deadline
 * @brief        计算条件变量绝对超时（CLOCK_MONOTONIC 当前时间 + ms 毫秒）
 */
static void mp_calc_deadline(struct timespec *ts, int ms)
{
    long add_ns;
    long nsec;
    clock_gettime(CLOCK_MONOTONIC, ts);
    add_ns = (long)(ms % 1000) * 1000000L;
    nsec   = ts->tv_nsec + add_ns;
    ts->tv_sec  += ms / 1000 + nsec / 1000000000L;
    ts->tv_nsec  = nsec % 1000000000L;
}

/**
 * @func         mp_update_peak
 * @brief        更新峰值已用槽位数（调用者须持锁）
 */
static void mp_update_peak(T_MemPool *p)
{
    int used = p->total_count - p->free_count;
    if(used > p->peak_used)
    {
        p->peak_used = used;
    }
}


/* ========================== 生命周期 ========================== */

/**
 * @func         MemPoolAPI_Init
 * @brief        初始化内存池
 */
int MemPoolAPI_Init(T_MemPool **pp, const T_MemPoolConfig *cfg, const char *name)
{
    int element_size, init_count, grow_count, block_timeo, align_size;
    MemPoolMode mode;
    T_MemPool *pt;
    T_MemPoolChunk *ch;

    /* ---- 参数合法性检查 ---- */
    if(NULL == pp)
    {
        printf("NULL == pp fail ##%s->%d\n", __FUNCTION__, __LINE__);
        return -1;
    }
    if(NULL != *pp)
    {
        printf("NULL != *pp fail (already init?) ##%s->%d\n", __FUNCTION__, __LINE__);
        return -1;
    }
    if(NULL == name)
    {
        printf("NULL == name fail ##%s->%d\n", __FUNCTION__, __LINE__);
        return -1;
    }

    /* ---- 解析配置（NULL 或 0 用默认） ---- */
    element_size = (cfg && cfg->element_size > 0) ? cfg->element_size : 64;
    init_count   = (cfg && cfg->init_count   > 0) ? cfg->init_count   : 64;
    mode         = cfg ? cfg->mode         : MEMPOOL_MODE_DROP;
    grow_count   = cfg ? cfg->grow_count   : 0;
    block_timeo  = cfg ? cfg->block_timeo  : 0;

    /* ---- GROW 模式 grow_count 必须 >0 ---- */
    if(mode == MEMPOOL_MODE_GROW && grow_count <= 0)
    {
        printf("GROW mode need grow_count > 0 fail ##%s->%d\n", __FUNCTION__, __LINE__);
        return -1;
    }
    /* ---- mode 取值范围校验 ---- */
    if(mode < 0 || mode > MEMPOOL_MODE_BLOCK)
    {
        printf("mode %d invalid fail ##%s->%d\n", mode, __FUNCTION__, __LINE__);
        return -1;
    }
    /* ---- block_timeo 负值拒绝（避免静默无限挂起，统一负值=失败）---- */
    if(block_timeo < 0)
    {
        printf("block_timeo < 0 fail ##%s->%d\n", __FUNCTION__, __LINE__);
        return -1;
    }

    /* ---- 对齐：向上补齐到 MEMPOOL_ALIGN，且保证 >= sizeof(void*) ---- */
    align_size = mp_align_up(element_size);
    if(align_size < (int)sizeof(void *))
    {
        align_size = (int)sizeof(void *);
    }

    /* ---- 乘法溢出校验（32位平台防 count×align_size 溢出致堆溢出）---- */
    if(mp_size_overflow(init_count, align_size))
    {
        printf("init_count*align_size overflow fail ##%s->%d\n", __FUNCTION__, __LINE__);
        return -1;
    }

    /* ---- 打印库版本 ---- */
    printf("MemoryPoolLibVision = [%s]\n", MemoryPool_PROJECT_MAKETIME);

    /* ---- 分配结构体 ---- */
    pt = (T_MemPool *)malloc(sizeof(T_MemPool));
    if(NULL == pt)
    {
        printf("malloc fail struct ##%s->%d\n", __FUNCTION__, __LINE__);
        return -1;
    }
    memset(pt, 0, sizeof(T_MemPool));

    /* ---- 分配首 chunk ---- */
    ch = (T_MemPoolChunk *)malloc(sizeof(T_MemPoolChunk));
    if(NULL == ch)
    {
        printf("malloc fail chunk ##%s->%d\n", __FUNCTION__, __LINE__);
        free(pt);
        return -1;
    }
    ch->mem = (unsigned char *)malloc((size_t)init_count * (size_t)align_size);
    if(NULL == ch->mem)
    {
        printf("malloc fail chunk mem ##%s->%d\n", __FUNCTION__, __LINE__);
        free(ch);
        free(pt);
        return -1;
    }
    ch->count = init_count;
    ch->next  = NULL;

    /* ---- 初始化字段 ---- */
    pt->element_size = element_size;
    pt->align_size   = align_size;
    pt->init_count   = init_count;
    pt->mode         = mode;
    pt->grow_count   = grow_count;
    pt->block_timeo  = block_timeo;
    pt->chunks       = ch;
    pt->free_list    = NULL;
    pt->free_count   = 0;
    pt->total_count  = init_count;

    pthread_mutex_init(&pt->mux, NULL);
    {
        /* cond 用 CLOCK_MONOTONIC，超时不受系统时间跳变影响 */
        pthread_condattr_t attr;
        pthread_condattr_init(&attr);
        pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
        pthread_cond_init(&pt->cond, &attr);
        pthread_condattr_destroy(&attr);
    }

    /* 切槽位串入 free_list（Init 阶段单线程，无需加锁） */
    mp_chunk_to_freelist(pt, ch);

    pt->init_done = 1;
    mp_set_name(pt->name, name);

    *pp = pt;
    return 0;
}

/**
 * @func         MemPoolAPI_Destroy
 * @brief        销毁内存池，释放所有资源
 * @details      遍历 chunks 链表逐个释放(mem + 节点)，销毁 mutex/cond，释放结构体。
 *               幂等：*pp==NULL 返回 -1。
 */
int MemPoolAPI_Destroy(T_MemPool **pp)
{
    T_MemPool *pt;
    T_MemPoolChunk *ch;

    if(NULL == pp || NULL == *pp)
    {
        return -1;
    }
    if(1 != (*pp)->init_done)
    {
        return -1;
    }

    pt = *pp;
    /* 释放所有 chunk（含 GROW 扩容的） */
    ch = pt->chunks;
    while(ch != NULL)
    {
        T_MemPoolChunk *next = ch->next;
        free(ch->mem);
        free(ch);
        ch = next;
    }
    pthread_mutex_destroy(&pt->mux);
    pthread_cond_destroy(&pt->cond);
    free(pt);
    *pp = NULL;
    return 0;
}


/* ========================== 分配（内部三策略） ========================== */

/**
 * @func         mp_alloc_drop
 * @brief        内部-DROP 策略：池满返回 NULL
 */
static void *mp_alloc_drop(T_MemPool *p)
{
    void *slot;
    pthread_mutex_lock(&p->mux);
    slot = p->free_list;
    if(slot == NULL)
    {
        p->total_drop++;
        pthread_mutex_unlock(&p->mux);
        return NULL;
    }
    p->free_list = *(void **)slot;   /* next */
    p->free_count--;
    p->total_alloc++;
    mp_update_peak(p);
    pthread_mutex_unlock(&p->mux);
    return slot;
}

/**
 * @func         mp_alloc_grow
 * @brief        内部-GROW 策略：池满则 malloc 新 chunk 扩容
 */
static void *mp_alloc_grow(T_MemPool *p)
{
    void *slot;
    pthread_mutex_lock(&p->mux);
    slot = p->free_list;
    if(slot == NULL)
    {
        /* 扩容 */
        T_MemPoolChunk *ch;
        if(p->grow_count <= 0)
        {
            p->total_drop++;
            pthread_mutex_unlock(&p->mux);
            return NULL;
        }
        if(mp_size_overflow(p->grow_count, p->align_size))
        {
            p->total_drop++;
            pthread_mutex_unlock(&p->mux);
            return NULL;
        }
        ch = (T_MemPoolChunk *)malloc(sizeof(T_MemPoolChunk));
        if(ch == NULL)
        {
            p->total_drop++;              /* 扩容失败计入丢弃 */
            pthread_mutex_unlock(&p->mux);
            return NULL;
        }
        ch->mem = (unsigned char *)malloc((size_t)p->grow_count * (size_t)p->align_size);
        if(ch->mem == NULL)
        {
            free(ch);
            p->total_drop++;              /* 扩容失败计入丢弃 */
            pthread_mutex_unlock(&p->mux);
            return NULL;
        }
        ch->count = p->grow_count;
        ch->next  = p->chunks;
        p->chunks = ch;
        mp_chunk_to_freelist(p, ch);          /* 切槽串入 free_list */
        p->total_count += ch->count;
        p->total_grow  += (unsigned long)ch->count;
        slot = p->free_list;
    }
    p->free_list = *(void **)slot;
    p->free_count--;
    p->total_alloc++;
    mp_update_peak(p);
    pthread_mutex_unlock(&p->mux);
    return slot;
}

/**
 * @func         mp_alloc_block
 * @brief        内部-BLOCK 策略：池满阻塞等 Free 归还（timeo=0 无限，>0 超时返 NULL）
 */
static void *mp_alloc_block(T_MemPool *p, int timeo)
{
    void *slot;
    if(timeo < 0)
    {
        return NULL;
    }
    pthread_mutex_lock(&p->mux);
    if(timeo == 0)
    {
        /* 无限等待 */
        while(p->free_list == NULL)
        {
            pthread_cond_wait(&p->cond, &p->mux);
        }
    }
    else
    {
        /* 带超时 */
        struct timespec ts;
        mp_calc_deadline(&ts, timeo);
        while(p->free_list == NULL)
        {
            int r = pthread_cond_timedwait(&p->cond, &p->mux, &ts);
            if(r == ETIMEDOUT)
            {
                /* 信号与超时可能并发：持锁再确认，仍有空闲则正常分配 */
                if(p->free_list == NULL)
                {
                    p->total_drop++;      /* 确实超时无槽位 */
                    pthread_mutex_unlock(&p->mux);
                    return NULL;
                }
                break;                    /* 谓词已真，跳出走正常分配 */
            }
            else if(r != 0 && r != EINTR)  /* 非 ETIMEDOUT/EINTR 的意外错误：避免忙等空转 */
            {
                p->total_drop++;
                pthread_mutex_unlock(&p->mux);
                return NULL;
            }
        }
    }
    slot = p->free_list;
    p->free_list = *(void **)slot;
    p->free_count--;
    p->total_alloc++;
    mp_update_peak(p);
    pthread_mutex_unlock(&p->mux);
    return slot;
}


/* ========================== 分配（公共） ========================== */

/**
 * @func         MemPoolAPI_Alloc
 * @brief        分配一个槽位（默认模式，按 cfg.mode）
 */
void *MemPoolAPI_Alloc(T_MemPool *p)
{
    if(NULL == p || !p->init_done)
    {
        return NULL;
    }
    switch(p->mode)
    {
        case MEMPOOL_MODE_DROP:  return mp_alloc_drop(p);
        case MEMPOOL_MODE_GROW:  return mp_alloc_grow(p);
        case MEMPOOL_MODE_BLOCK: return mp_alloc_block(p, p->block_timeo);
        default:                 return NULL;
    }
}

/**
 * @func         MemPoolAPI_AllocDrop
 * @brief        分配（满则返回 NULL）
 */
void *MemPoolAPI_AllocDrop(T_MemPool *p)
{
    if(NULL == p || !p->init_done)
    {
        return NULL;
    }
    return mp_alloc_drop(p);
}

/**
 * @func         MemPoolAPI_AllocGrow
 * @brief        分配（满则动态扩容）
 */
void *MemPoolAPI_AllocGrow(T_MemPool *p)
{
    if(NULL == p || !p->init_done)
    {
        return NULL;
    }
    return mp_alloc_grow(p);
}

/**
 * @func         MemPoolAPI_AllocBlock
 * @brief        分配（满则阻塞等待）
 */
void *MemPoolAPI_AllocBlock(T_MemPool *p, int timeo)
{
    if(NULL == p || !p->init_done)
    {
        return NULL;
    }
    return mp_alloc_block(p, timeo);
}


/* ========================== 释放 ========================== */

/**
 * @func         MemPoolAPI_Free
 * @brief        归还一个槽位（LIFO 头插，唤醒 BLOCK 等待者）
 * @details      不校验 elem 是否本池 Alloc 出的（错指针 UB，同 free）。
 */
int MemPoolAPI_Free(T_MemPool *p, void *elem)
{
    if(NULL == p || NULL == elem || !p->init_done)
    {
        return -1;
    }
    pthread_mutex_lock(&p->mux);
    *(void **)elem = p->free_list;   /* 槽位开头存当前链头 */
    p->free_list = elem;             /* 本槽位变新链头 */
    p->free_count++;
    p->total_free++;
    pthread_cond_signal(&p->cond);   /* 唤醒一个 BLOCK 等待者 */
    pthread_mutex_unlock(&p->mux);
    return 0;
}


/* ========================== 查询与统计 ========================== */

/**
 * @func         MemPoolAPI_GetFreeCount
 * @brief        获取当前空闲槽位数
 */
int MemPoolAPI_GetFreeCount(T_MemPool *p)
{
    int n;
    if(NULL == p || !p->init_done)
    {
        return -1;
    }
    pthread_mutex_lock(&p->mux);
    n = p->free_count;
    pthread_mutex_unlock(&p->mux);
    return n;
}

/**
 * @func         MemPoolAPI_GetUsedCount
 * @brief        获取当前已用槽位数(= 总量 - 空闲)
 */
int MemPoolAPI_GetUsedCount(T_MemPool *p)
{
    int used;
    if(NULL == p || !p->init_done)
    {
        return -1;
    }
    pthread_mutex_lock(&p->mux);
    used = p->total_count - p->free_count;
    pthread_mutex_unlock(&p->mux);
    return used;
}

/**
 * @func         MemPoolAPI_StatsGet
 * @brief        获取运行统计信息
 */
int MemPoolAPI_StatsGet(T_MemPool *p, T_MemPoolStats *st)
{
    if(NULL == p || NULL == st || !p->init_done)
    {
        return -1;
    }
    pthread_mutex_lock(&p->mux);
    st->ulTotalAlloc = p->total_alloc;
    st->ulTotalFree  = p->total_free;
    st->ulTotalDrop  = p->total_drop;
    st->ulTotalGrow  = p->total_grow;
    st->iPeakUsed    = p->peak_used;
    st->iCapacity    = p->total_count;
    pthread_mutex_unlock(&p->mux);
    return 0;
}

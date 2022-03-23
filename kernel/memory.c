#include "memory.h"
#include "stdint.h"
#include "print.h"
#include "global.h"
#include "debug.h"
#include "string.h"
#include "sync.h"
#include "interrupt.h"

#define PG_SIZE 4096

#define MEM_BITMAP_BASE 0xc009a000 // 位图位置 栈顶0xc009f000 内核主线程pcb 0xc009e000 + 1 + 0xfff
// 一个页框大小位图 128M (4k * 4096字节 * 8位) 支持4个页框 0xc009e000 - 0x4000(1024 * 4页) = 0xc009a000

// 堆起始地址 0xc0000000 是 内核从虚拟地址 3G 起
// 0x100000 低端1M 绕过使其连续
#define K_HEAP_START 0xc0100000

#define PDE_IDX(addr) ((addr & 0xffc00000) >> 22)
#define PTE_IDX(addr) ((addr & 0x003ff000) >> 12)

/* 内存池结构，生成两个实例用于管理内核内存池和用户内存池 */
struct pool { // 内存池
    struct bitmap pool_bitmap; //本内存池用到的位图结构，用于管理物理内存
    uint32_t phy_addr_start;  // 本内存池所管理物理内存的起始地址
    uint32_t pool_size;      // 本内存池字节容量
    struct lock lock; // 申请内存用于互斥
};

// 内存仓库
struct arena {
    struct mem_block_desc* desc;
    uint32_t cnt;
    bool large; // large == true cnt为页框数 else cnt 为 空闲block的数量
};
struct mem_block_desc k_block_descs[DESC_CNT];

struct pool kernel_pool, user_pool; // 生成内核内存池和用户内存池
struct virtual_addr kernel_vaddr; // 给内核分配虚拟地址

//  在pf虚拟内存池中 申请 pg_cnt 个虚拟页
static void* vaddr_get(enum pool_flags pf, uint32_t pg_cnt) {
    int vaddr_start = 0, bit_idx_start = -1;
    uint32_t cnt = 0;
    if (pf == PF_KERNEL) {  // 内核池
        bit_idx_start = bitmap_scan(&kernel_vaddr.vaddr_bitmap, pg_cnt);
        if (bit_idx_start == -1) {
            return NULL;
        }
        while (cnt < pg_cnt) {
            bitmap_set(&kernel_vaddr.vaddr_bitmap, bit_idx_start + cnt++, 1);
        }
        vaddr_start = kernel_vaddr.vaddr_start + bit_idx_start * PG_SIZE;
    } else {  // 用户内存池
        struct task_struct* cur = running_thread();
        bit_idx_start = bitmap_scan(&cur->userprog_vaddr.vaddr_bitmap, pg_cnt);
        if (bit_idx_start == -1) { return NULL; }
        while (cnt < pg_cnt) {
            bitmap_set(&cur->userprog_vaddr.vaddr_bitmap, bit_idx_start + cnt++, 1);
        }
        vaddr_start = cur->userprog_vaddr.vaddr_start + bit_idx_start * PG_SIZE;
        // (0xc0000000 - PG_SIZE) 作为用户3级栈已经在start_process 被分配
        ASSERT((uint32_t)vaddr_start < (0xc0000000 - PG_SIZE));
    }
    return (void*)vaddr_start;
}

/* 得到虚拟地址 vaddr 对应的 pte 指针*/
uint32_t* pte_ptr(uint32_t vaddr) {
   uint32_t* pte = (uint32_t*)(0xffc00000 + \
     ((vaddr & 0xffc00000) >> 10) + \
     PTE_IDX(vaddr) * 4);
     return pte;
}

uint32_t* pde_ptr(uint32_t vaddr) {
    uint32_t* pde = (uint32_t*)((0xfffff000) + PDE_IDX(vaddr) * 4);
    return pde;
}

// 在 m_pool 物理内存池中分配一个物理页 成功则返回页框的物理地址，失败则返回 NULL
static void* palloc(struct pool* m_pool) {
    int bit_idx = bitmap_scan(&m_pool->pool_bitmap, 1);
    if (bit_idx == -1) { return NULL; }
    bitmap_set(&m_pool->pool_bitmap, bit_idx, 1);
    uint32_t page_phyaddr = ((bit_idx * PG_SIZE) + m_pool->phy_addr_start);
    return (void*)page_phyaddr;
}

// 在页表中添加虚拟地址 和 物理地址的映射
static void page_table_add(void* _vaddr, void* _page_phyaddr) {
    uint32_t vaddr = (uint32_t)_vaddr, page_phyaddr = (uint32_t)_page_phyaddr;
    uint32_t* pde = pde_ptr(vaddr);
    uint32_t* pte = pte_ptr(vaddr);

    if (*pde & 0x00000001) {
        ASSERT(!(*pte & 0x00000001));
        if (!(*pte & 0x00000001)) {
            *pte = (page_phyaddr | PG_US_U | PG_RW_W | PG_P_1);
        } else {
            // pte 已存在
            PANIC("pte repeat"); // 2022年02月27日11:22:03
            *pte = (page_phyaddr | PG_US_U | PG_RW_W | PG_P_1);
        }
    } else {
        uint32_t pde_phyaddr = (uint32_t)palloc(&kernel_pool);
        *pde = (pde_phyaddr | PG_US_U | PG_RW_W | PG_P_1);
        memset((void*)((int)pte & 0xfffff000), 0, PG_SIZE);  
        // 初始化页表也要用虚拟地址，不能用pde_phyaddr物理地址
        ASSERT(!(*pte & 0x00000001));
        *pte = (page_phyaddr | PG_US_U | PG_RW_W | PG_P_1);
    }
}
// 分配 pg_cnt 页空间
void* malloc_page(enum pool_flags pf, uint32_t pg_cnt) {
    // 3840 = 15 * 1024 * 1024 / 4096 假设只能用到15MB
    ASSERT(pg_cnt > 0 && pg_cnt < 3840);
    void* vaddr_start = vaddr_get(pf, pg_cnt);
    if (vaddr_start == NULL) { return NULL; }
    uint32_t vaddr = (uint32_t)vaddr_start, cnt = pg_cnt;
    struct pool* mem_pool = pf & PF_KERNEL ? &kernel_pool : &user_pool;
    /* 因为虚拟地址是连续的，但物理地址可以是不连续的，所以逐个做映射*/
    while (cnt-- > 0) {
        void* page_phyaddr = palloc(mem_pool);
        if (page_phyaddr == NULL) { return NULL; }//失败时要将曾经已申请的虚拟地址和
        page_table_add((void*)vaddr, page_phyaddr); // 在页表中做映射
        vaddr += PG_SIZE;  // 下一个虚拟页
    }
    return vaddr_start;
}

/* 从内核物理内存池中申请 1 页内存，成功则返回其虚拟地址，失败则返回 NULL */
void* get_kernel_pages(uint32_t pg_cnt) {
    lock_acquire(&kernel_pool.lock);
    void* vaddr = malloc_page(PF_KERNEL, pg_cnt);
    if (vaddr != NULL) {
        memset(vaddr, 0, pg_cnt * PG_SIZE);
    }
    lock_release(&kernel_pool.lock);
    return vaddr;
}

void* get_user_pages(uint32_t pg_cnt) {
    lock_acquire(&user_pool.lock);
    void* vaddr = malloc_page(PF_USER, pg_cnt);
    if (vaddr != NULL) {
        memset(vaddr, 0, pg_cnt * PG_SIZE);
    }
    lock_release(&user_pool.lock);
    return vaddr;
}

// 将vaddr和pf池物理地址关联 仅支持一页
void* get_a_page(enum pool_flags pf, uint32_t vaddr) {
    struct pool* mem_pool = pf & PF_KERNEL ? &kernel_pool : &user_pool;
    lock_acquire(&mem_pool->lock);
    struct task_struct* cur = running_thread();
    int32_t bit_idx = -1;

    if (cur->pgdir != NULL && pf == PF_USER) {
        bit_idx = (vaddr - cur->userprog_vaddr.vaddr_start) / PG_SIZE;
        ASSERT(bit_idx > 0);
        bitmap_set(&cur->userprog_vaddr.vaddr_bitmap, bit_idx, 1);
    } else if (cur->pgdir == NULL && pf == PF_KERNEL) {
        bit_idx = (vaddr - kernel_vaddr.vaddr_start) / PG_SIZE;
        ASSERT(bit_idx > 0);
        bitmap_set(&kernel_vaddr.vaddr_bitmap, bit_idx, 1);
    }
    void* page_phyaddr = palloc(mem_pool);
    if (page_phyaddr == NULL) { return NULL; }
    page_table_add((void*)vaddr, page_phyaddr);
    lock_release(&mem_pool->lock);
    return (void*)vaddr;
}

// 得到虚拟地址映射到的物理地址
uint32_t addr_v2p(uint32_t vaddr) {
    uint32_t* pte = pte_ptr(vaddr);
    return ((*pte & 0xfffff000) + (vaddr & 0x000000ff));
}

static void mem_pool_init(uint32_t all_mem) {
    put_str("   mem_pool_init start\n");
    uint32_t page_table_size = PG_SIZE * 256; // 第0和第768 769~1022 PDE
    uint32_t used_mem = page_table_size + 0x100000; // 低端1MB
    uint32_t free_mem = all_mem - used_mem;
    // 页表大小 = 1 页的页目录表 + 第 0 和第 768 个页目录项指向同一个页表 + 
    // 第 769～1022 个页目录项共指向 254 个页表，共 256 个页框
    uint16_t all_free_pages = free_mem / PG_SIZE;
    uint16_t kernel_free_pages = all_free_pages / 2;
    uint16_t user_free_pages = all_free_pages - kernel_free_pages;
    /* 为简化位图操作，余数不处理，坏处是这样做会丢内存。
    好处是不用做内存的越界检查，因为位图表示的内存少于实际物理内存 */
    uint32_t kbm_length = kernel_free_pages / 8;
    uint32_t ubm_length = user_free_pages / 8;

    uint32_t kp_start = used_mem; // Kernel pool start
    uint32_t up_start = kp_start + kernel_free_pages * PG_SIZE;

    kernel_pool.phy_addr_start = kp_start;
    user_pool.phy_addr_start = up_start;
    kernel_pool.pool_size = kernel_free_pages * PG_SIZE;
    user_pool.pool_size = user_free_pages * PG_SIZE;
    
    kernel_pool.pool_bitmap.btmp_bytes_len = kbm_length;
    user_pool.pool_bitmap.btmp_bytes_len = ubm_length;
    // 内核内存池的位图先定在 MEM_BITMAP_BASE(0xc009a000)处
    kernel_pool.pool_bitmap.bits = (void*)MEM_BITMAP_BASE;
    user_pool.pool_bitmap.bits = (void*)(MEM_BITMAP_BASE + kbm_length);

    put_str("       kernel_pool_bitmap_start:");
    put_int((int)kernel_pool.pool_bitmap.bits);
    put_str("   kernel_pool_phy_addr_start:");
    put_int(kernel_pool.phy_addr_start);
    put_str("\n");
    put_str("user_pool_bitmap_start:");
    put_int((int)user_pool.pool_bitmap.bits);
    put_str("   user_pool_phy_addr_start:");
    put_int(user_pool.phy_addr_start);
    put_str("\n");

    bitmap_init(&kernel_pool.pool_bitmap);
    bitmap_init(&user_pool.pool_bitmap);

    lock_init(&kernel_pool.lock);
    lock_init(&user_pool.lock);

    kernel_vaddr.vaddr_bitmap.btmp_bytes_len = kbm_length;

    kernel_vaddr.vaddr_bitmap.bits = (void*)(MEM_BITMAP_BASE + kbm_length + ubm_length);
    kernel_vaddr.vaddr_start = K_HEAP_START;
    bitmap_init(&kernel_vaddr.vaddr_bitmap);
    put_str("   mem_pool_init done\n");
}

// 初始化内存块描述符
void block_desc_init(struct mem_block_desc* desc) {
    uint16_t desc_idx, block_size = 16;
    for (desc_idx = 0; desc_idx < DESC_CNT; ++desc_idx) {
        desc[desc_idx].block_size = block_size;
        desc[desc_idx].blocks_per_arena = (PG_SIZE - sizeof(struct arena)) / block_size;
        list_init(&desc[desc_idx].free_list);
        block_size *= 2;
    }
}

// 返回 arena idx个内存块的地址
static struct mem_block* arena2block(struct arena* a, uint32_t idx) {
    return (struct mem_block*)((uint32_t)a + sizeof(struct arena) + idx * a->desc->block_size);
}
// 返回内存块b所在的arena地址
static struct arena* block2arena(struct mem_block* b) {
    return (struct arena*)((uint32_t)b & 0xfffff000);
}

void* sys_malloc(uint32_t size) {
    enum pool_flags PF;
    struct pool* mem_pool;
    uint32_t pool_size;
    struct mem_block_desc* descs;
    struct task_struct* cur_thread = running_thread();
    
    if (cur_thread->pgdir == NULL) {
        PF = PF_KERNEL;
        pool_size = kernel_pool.pool_size;
        mem_pool = &kernel_pool;
        descs = k_block_descs;
    } else {
        PF = PF_USER;
        pool_size = user_pool.pool_size;
        mem_pool = &user_pool;
        descs = cur_thread->u_block_desc;
    }
    if (!(size > 0 && size < pool_size)) {
        return NULL;
    }
    struct arena* a;
    struct mem_block* b;
    lock_acquire(&mem_pool->lock);
    if (size > 1024) {
        uint32_t page_cnt = DIV_ROUND_UP(size + sizeof(struct arena), PG_SIZE); // 取整
        a = malloc_page(PF, page_cnt);
        if (a != NULL) {
            memset(a, 0, page_cnt * PG_SIZE);
            a->desc = NULL;
            a->cnt = page_cnt;
            a->large = true;
            lock_release(&mem_pool->lock);
            return (void*)(a + 1); // 跨过 arena
        } else {
            lock_release(&mem_pool->lock);
            return NULL;
        }
    } else {
        uint8_t desc_idx;
        for (desc_idx = 0; desc_idx < DESC_CNT; ++desc_idx) {
            if (size <= descs[desc_idx].block_size) {
                break;
            }
        }
        if (list_empty(&descs[desc_idx].free_list)) {
            a = malloc_page(PF, 1);
            if (a == NULL) {
                lock_release(&mem_pool->lock);
                return NULL;
            }
            memset(a, 0, PG_SIZE);
            a->desc = &descs[desc_idx];
            a->large = false;
            a->cnt = descs[desc_idx].blocks_per_arena;
            uint32_t block_idx;

            enum intr_status old_status = intr_disable();
            // 将 arena 拆分
            for (block_idx = 0; block_idx < descs[desc_idx].blocks_per_arena; ++block_idx) {
                b = arena2block(a, block_idx);
                ASSERT(!elem_find(&a->desc->free_list, &b->free_elem));
                list_append(&a->desc->free_list, &b->free_elem);
            }
            intr_set_status(old_status);
        }
        b = elem2entry(struct mem_block, free_elem, list_pop(&(descs[desc_idx].free_list)));
        memset(b, 0, descs[desc_idx].block_size);
        a = block2arena(b);
        a->cnt--;
        lock_release(&mem_pool->lock);
        return (void*)b;
    }
}

void mem_init() {
    put_str("mem_init start\n");
    uint32_t mem_bytes_total = (*(uint32_t*)(0xb00)); // BIOS 0x15 中断得到的内存所在地址
    mem_pool_init(mem_bytes_total);
    block_desc_init(k_block_descs);
    put_str("mem_init done\n");
}
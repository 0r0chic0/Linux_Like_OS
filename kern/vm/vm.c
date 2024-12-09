#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <cpu.h>
#include <spinlock.h>
#include <proc.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include <uio.h>
#include <stat.h>
#include <vnode.h>
#include <vfs.h>
#include <bitmap.h>
#include <kern/fcntl.h>

static paddr_t memory_start, memory_end;
struct coremap_page *coremap;
static int allocated_pages_count = 0;
static struct spinlock coremap_lock = SPINLOCK_INITIALIZER;
static struct spinlock swap_lock = SPINLOCK_INITIALIZER;
struct swap_disk swap;
static unsigned int eviction_pointer;

static void as_zero_region(paddr_t paddr, unsigned npages) {
    bzero((void *)PADDR_TO_KVADDR(paddr), npages * PAGE_SIZE);
}

void coremap_bootstrap(void) {
    // Retrieve memory boundaries
    memory_end = ram_getsize();
    memory_start = ram_getfirstfree();

    // Align memory_start to the next page boundary
    if (memory_start % PAGE_SIZE != 0) {
        memory_start = (memory_start + PAGE_SIZE - 1) & PAGE_FRAME;
    }

    // Calculate the total number of pages in physical memory
    unsigned int total_pages = memory_end / PAGE_SIZE;

    // Initialize the coremap structure
    coremap = (struct coremap_page *)PADDR_TO_KVADDR(memory_start);

    // Compute the number of pages required for the coremap itself
    unsigned int coremap_pages = sizeof(struct coremap_page) * total_pages;
    coremap_pages = (coremap_pages + PAGE_SIZE - 1) / PAGE_SIZE;

    // Adjust memory_start to account for the coremap
    memory_start += coremap_pages * PAGE_SIZE;
    KASSERT(memory_start % PAGE_SIZE == 0);

    // Determine the number of pages already used
    unsigned int used_pages = memory_start / PAGE_SIZE;

    // Loop through the entire coremap and initialize each page's metadata
    for (unsigned int page_index = 0; page_index < total_pages; page_index++) {
        coremap[page_index].chunk_size = 0;

        // Mark pages as either "fixed" (used) or "free"
        if (page_index < used_pages) {
            coremap[page_index].state = fixed;
        } else {
            coremap[page_index].state = free;
        }
    }

    // Set the eviction pointer to the first free page
    eviction_pointer = used_pages;

    // Reset the count of allocated pages
    allocated_pages_count = 0;
}

void vm_bootstrap(void) {
    struct vnode *disk_node = NULL;
    struct stat disk_info;
    char disk_path[] = "lhd0raw:";

    // Open swap disk and validate
    if (vfs_open(disk_path, O_RDWR, 0, &disk_node)) {
        swap.vnode = NULL;
        swap.bitmap = NULL;
        swap.swap_disk_present = false;
        return;
    }

    // Fetch disk statistics and validate
    if (VOP_STAT(disk_node, &disk_info) || (disk_info.st_size % PAGE_SIZE != 0)) {
        vfs_close(disk_node);
        swap.vnode = NULL;
        swap.bitmap = NULL;
        swap.swap_disk_present = false;
        return;
    }

    // Initialize the swap bitmap
    swap.bitmap = bitmap_create(disk_info.st_size / PAGE_SIZE);
    if (!swap.bitmap) {
        vfs_close(disk_node);
        swap.vnode = NULL;
        swap.swap_disk_present = false;
        return;
    }

    // Swap structure setup
    swap.vnode = disk_node;
    swap.swap_disk_present = true;
}

static paddr_t allocate_kernel_pages(unsigned long npages) {
    paddr_t allocated_addr = 0;

    spinlock_acquire(&coremap_lock);

    unsigned int start_idx = memory_start / PAGE_SIZE;
    unsigned int end_idx = memory_end / PAGE_SIZE;
    unsigned int contiguous_free_pages = 0;

    // Search for a contiguous block of free pages
    for (unsigned int i = start_idx; i < end_idx; i++) {
        if (coremap[i].state == free) {
            contiguous_free_pages++;
            if (contiguous_free_pages == npages) {
                allocated_addr = (i - npages + 1) * PAGE_SIZE;
                break;
            }
        } else {
            contiguous_free_pages = 0;
        }
    }

    if (!allocated_addr) {
        spinlock_release(&coremap_lock);
        return 0; // No available block
    }

    // Update metadata for the allocated block
    for (unsigned int i = 0; i < npages; i++) {
        coremap[(allocated_addr / PAGE_SIZE) + i].state = fixed;
        coremap[(allocated_addr / PAGE_SIZE) + i].chunk_size = (i == 0) ? npages : 0;
    }

    allocated_pages_count += npages;
    as_zero_region(allocated_addr, npages);

    spinlock_release(&coremap_lock);
    return allocated_addr;
}

paddr_t allocate_user_page(unsigned long pages, struct addrspace *as, vaddr_t vpage_addr, bool copy_call) {
    KASSERT(pages == 1); // Single-page allocations only

    paddr_t allocated_addr = 0;

    spinlock_acquire(&coremap_lock);

    // Search for a free page
    for (unsigned int i = memory_start / PAGE_SIZE; i < memory_end / PAGE_SIZE; i++) {
        if (coremap[i].state == free) {
            allocated_addr = i * PAGE_SIZE;
            coremap[i] = (struct coremap_page){
                .state = used,
                .owner_addrspace = as,
                .owner_vaddr = vpage_addr,
                .ref_bit = !copy_call,
                .chunk_size = 1
            };
            break;
        }
    }

    if (!allocated_addr) {
        spinlock_release(&coremap_lock);
        return 0; // No available pages
    }

    allocated_pages_count++;
    as_zero_region(allocated_addr, pages);

    spinlock_release(&coremap_lock);
    return allocated_addr;
}

vaddr_t alloc_kpages(unsigned npages) {
    paddr_t kernel_pages = allocate_kernel_pages(npages);
    return kernel_pages ? PADDR_TO_KVADDR(kernel_pages) : 0;
}

void free_kpages(vaddr_t addr) {
    paddr_t physical_addr = addr - MIPS_KSEG0;

    spinlock_acquire(&coremap_lock);

    KASSERT(physical_addr % PAGE_SIZE == 0);

    unsigned int start_index = physical_addr / PAGE_SIZE;
    unsigned int chunk_size = coremap[start_index].chunk_size;

    for (unsigned int i = 0; i < chunk_size; i++) {
        coremap[start_index + i] = (struct coremap_page){ .state = free, .chunk_size = 0 };
    }

    allocated_pages_count -= chunk_size;
    spinlock_release(&coremap_lock);
}

int release_physical_page(paddr_t page_paddr) {
    spinlock_acquire(&coremap_lock);

    KASSERT(page_paddr % PAGE_SIZE == 0);

    unsigned int page_idx = page_paddr / PAGE_SIZE;

    if (coremap[page_idx].state == in_eviction) {
        spinlock_release(&coremap_lock);
        return 1;
    }

    coremap[page_idx] = (struct coremap_page){ .state = free, .chunk_size = 0, .owner_addrspace = NULL, .owner_vaddr = 0 };

    allocated_pages_count--;
    spinlock_release(&coremap_lock);

    return 0;
}


void tlb_invalidate_entry(vaddr_t remove_vaddr) {
    int old_spl = splhigh();
    int tlb_index = tlb_probe(remove_vaddr, 0);
    if (tlb_index >= 0) {
        tlb_write(TLBHI_INVALID(tlb_index), TLBLO_INVALID(), tlb_index);
    }
    splx(old_spl);
}

int vm_fault(int faulttype, vaddr_t faultaddress) {
    (void)faulttype;

    struct addrspace *as = proc_getas();
    if (as == NULL) {
        return EFAULT; // No address space
    }

    faultaddress &= PAGE_FRAME; // Align faultaddress to page boundary
    vaddr_t stackbase = USERSTACK - VM_STACKPAGES * PAGE_SIZE;
    vaddr_t stacktop = USERSTACK;
    bool valid_address = false;

    // Validate the fault address
    if ((faultaddress >= stackbase && faultaddress < stacktop) || 
        (faultaddress >= as->heap_start && faultaddress < as->heap_end)) {
        valid_address = true;
    } else {
        for (struct region *region = as->start_region; region != NULL; region = region->next) {
            if (faultaddress >= region->start && faultaddress < region->start + region->size) {
                valid_address = true;
                break;
            }
        }
    }

    if (!valid_address) {
        return EFAULT; // Invalid address
    }

    struct page_table_entry *pte = as->start_page_table;
    struct page_table_entry *prev_pte = NULL;
    paddr_t physical_page = 0;

    // Search for the page table entry corresponding to faultaddress
    while (pte != NULL) {
        if (pte->as_vpage == faultaddress) {
            lock_acquire(pte->lock);

            if (pte->state == SWAPPED) { // Handle swapped page
                physical_page = allocate_user_page(1, as, faultaddress, false);
                if (!physical_page) {
                    lock_release(pte->lock);
                    return ENOMEM; // Allocation failed
                }
                if (read_swap_disk(physical_page, pte->diskpage_location, true)) {
                    panic("Swap read failed");
                }
                pte->as_ppage = physical_page;
                pte->state = MAPPED;
            } else if (pte->state == MAPPED) { // Already mapped
                physical_page = pte->as_ppage;
            }

            lock_release(pte->lock);
            break;
        }

        prev_pte = pte;
        pte = pte->next;
    }

    // If no existing PTE, create a new one
    if (!pte) {
        pte = kmalloc(sizeof(*pte));
        if (!pte) {
            return ENOMEM; // Memory allocation failed
        }

        pte->lock = lock_create("pte_lock");
        if (!pte->lock) {
            kfree(pte);
            return ENOMEM;
        }

        lock_acquire(pte->lock);
        pte->as_vpage = faultaddress;
        pte->state = UNMAPPED;
        pte->next = NULL;

        if (prev_pte) {
            prev_pte->next = pte;
        } else {
            as->start_page_table = pte;
        }

        physical_page = allocate_user_page(1, as, faultaddress, false);
        if (!physical_page) {
            lock_release(pte->lock);
            lock_destroy(pte->lock);
            kfree(pte);
            return ENOMEM;
        }

        pte->as_ppage = physical_page;
        pte->state = MAPPED;
        lock_release(pte->lock);
    }

    // Insert the mapping into the TLB
    int spl = splhigh();
    for (int i = 0; i < NUM_TLB; i++) {
        uint32_t ehi, elo;
        tlb_read(&ehi, &elo, i);

        if (elo & TLBLO_VALID) {
            continue; // Skip valid entries
        }

        tlb_write(faultaddress, physical_page | TLBLO_DIRTY | TLBLO_VALID, i);
        splx(spl);

        coremap[physical_page / PAGE_SIZE].ref_bit = true;
        return 0;
    }

    // If no free slot is available, use a random TLB entry
    tlb_random(faultaddress, physical_page | TLBLO_DIRTY | TLBLO_VALID);
    splx(spl);

    coremap[physical_page / PAGE_SIZE].ref_bit = true;

    return 0;
}


unsigned int coremap_memory_usage(void) {
    return allocated_pages_count * PAGE_SIZE;
}

int read_swap_disk(paddr_t page_paddr, unsigned int disk_index, bool unmark) {
    // Ensure the bitmap entry for the disk index is set
    if (!bitmap_isset(swap.bitmap, disk_index)) {
        return EINVAL; // Invalid disk index
    }

    // Initialize a UIO structure for reading
    struct iovec io_vector;
    struct uio kernel_uio;
    uio_kinit(&io_vector, &kernel_uio, (void *)PADDR_TO_KVADDR(page_paddr),
              PAGE_SIZE, disk_index * PAGE_SIZE, UIO_READ);

    // Perform the read operation
    int result = VOP_READ(swap.vnode, &kernel_uio);
    if (result) {
        return result; // Return the error code if read fails
    }

    // Optionally unmark the bitmap if the unmark flag is true
    if (unmark) {
        spinlock_acquire(&swap_lock);
        bitmap_unmark(swap.bitmap, disk_index);
        spinlock_release(&swap_lock);
    }

    return 0; // Success
}

int write_swap_disk(paddr_t page_paddr, unsigned int *disk_index) {
    // Allocate a free index in the swap bitmap
    unsigned int free_index;
    spinlock_acquire(&swap_lock);
    int allocation_result = bitmap_alloc(swap.bitmap, &free_index);
    spinlock_release(&swap_lock);

    if (allocation_result) {
        return allocation_result; // Allocation failed, return the error code
    }

    // Initialize a UIO structure for writing
    struct iovec io_vector;
    struct uio kernel_uio;
    uio_kinit(&io_vector, &kernel_uio, (void *)PADDR_TO_KVADDR(page_paddr),
              PAGE_SIZE, free_index * PAGE_SIZE, UIO_WRITE);

    // Perform the write operation
    int result = VOP_WRITE(swap.vnode, &kernel_uio);
    if (result) {
        return result; // Return the error code if write fails
    }

    // Store the allocated index
    *disk_index = free_index;
    return 0; // Success
}

void unmark_swap_bitmap(unsigned int index) {
    // Acquire the lock before modifying the bitmap
    spinlock_acquire(&swap_lock);

    // Unmark the specified index in the bitmap
    if (bitmap_isset(swap.bitmap, index)) {
        bitmap_unmark(swap.bitmap, index);
    }

    spinlock_release(&swap_lock);
}

paddr_t evict_page(void) {
    struct addrspace *evicted_as;
    vaddr_t evicted_vaddr;
    paddr_t evicted_paddr;
    struct page_table_entry *pte = NULL;
    unsigned int disk_block_index;
    int error;

    KASSERT(spinlock_do_i_hold(&coremap_lock));

    // Identify a page to evict using the clock algorithm
    while (true) {
        struct coremap_page *current_page = &coremap[eviction_pointer];

        if (current_page->state == used && !current_page->ref_bit) {
            break; // Found a page to evict
        }

        // Reset the reference bit if the page is in use
        if (current_page->state == used) {
            current_page->ref_bit = false;
        }

        // Increment eviction pointer and wrap around if needed
        eviction_pointer = (eviction_pointer + 1) % (memory_end / PAGE_SIZE);
    }

    // Gather information about the page being evicted
    struct coremap_page *evict_page = &coremap[eviction_pointer];
    KASSERT(evict_page->state == used);

    evicted_as = evict_page->owner_addrspace;
    evicted_vaddr = evict_page->owner_vaddr;
    evicted_paddr = eviction_pointer * PAGE_SIZE;

    evict_page->state = in_eviction;

    // Move the eviction pointer forward
    eviction_pointer = (eviction_pointer + 1) % (memory_end / PAGE_SIZE);

    spinlock_release(&coremap_lock);

    // Locate the page table entry corresponding to the evicted page
    for (pte = evicted_as->start_page_table; pte != NULL; pte = pte->next) {
        if (pte->as_vpage == evicted_vaddr) {
            lock_acquire(pte->lock);
            KASSERT(pte->as_ppage == evicted_paddr);
            KASSERT(pte->state == MAPPED);
            break;
        }
    }
    KASSERT(pte != NULL);

    // Write the evicted page to the swap disk
    tlb_invalidate_entry(evicted_vaddr);
    error = write_swap_disk(evicted_paddr, &disk_block_index);
    if (error) {
        panic("Unable to write to swap disk");
    }

    // Update the page table entry state
    pte->diskpage_location = disk_block_index;
    pte->state = SWAPPED;
    lock_release(pte->lock);

    spinlock_acquire(&coremap_lock);

    return evicted_paddr;
}


void vm_tlbshootdown(const struct tlbshootdown * ts) {
	(void)ts;
}

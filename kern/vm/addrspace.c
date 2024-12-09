/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <types.h>
#include <kern/errno.h>
#include <spl.h>
#include <lib.h>
#include <addrspace.h>
#include <vm.h>
#include <proc.h>
#include <mips/tlb.h>
#include <synch.h>
/*
 * Note! If OPT_DUMBVM is set, as is the case until you start the VM
 * assignment, this file is not compiled or linked or in any way
 * used. The cheesy hack versions in dumbvm.c are used instead.
 */

struct addrspace *
as_create(void)
{
	struct addrspace *as;

	as = kmalloc(sizeof(struct addrspace));
	if (as == NULL) {
		return NULL;
	}

	/*
	 * Initialize as needed.
	 */
	as->start_region = NULL;
	as->start_page_table = NULL;
	as->heap_start = 0;
	as->heap_end = 0;
	return as;
}

int as_copy(struct addrspace *old, struct addrspace **ret) {
    struct addrspace *newas = as_create();
    if (newas == NULL) {
        return ENOMEM;
    }

    struct page_table_entry *old_pte = old->start_page_table;
    struct page_table_entry **new_pte_next = &newas->start_page_table;

    // Copy page table entries
    while (old_pte != NULL) {
        struct page_table_entry *new_pte = kmalloc(sizeof(struct page_table_entry));
        if (new_pte == NULL) {
            as_destroy(newas);
            return ENOMEM;
        }

        new_pte->lock = lock_create("pte_lock");
        if (new_pte->lock == NULL) {
            kfree(new_pte);
            as_destroy(newas);
            return ENOMEM;
        }

        lock_acquire(new_pte->lock);
        new_pte->as_vpage = old_pte->as_vpage;
        new_pte->vpage_permission = old_pte->vpage_permission;
        new_pte->state = UNMAPPED;
        new_pte->next = NULL;

        paddr_t new_ppage = allocate_user_page(1, newas, new_pte->as_vpage, true);
        if (new_ppage == 0) {
            lock_release(new_pte->lock);
            lock_destroy(new_pte->lock);
            as_destroy(newas);
            return ENOMEM;
        }

        lock_acquire(old_pte->lock);
        if (old_pte->state == SWAPPED) {
            if (read_swap_disk(new_ppage, old_pte->diskpage_location, false)) {
                panic("Cannot read from swap disk");
            }
        } else {
            memmove((void *)PADDR_TO_KVADDR(new_ppage),
                    (const void *)PADDR_TO_KVADDR(old_pte->as_ppage), PAGE_SIZE);
        }
        lock_release(old_pte->lock);

        new_pte->as_ppage = new_ppage;
        new_pte->state = MAPPED;
        lock_release(new_pte->lock);

        *new_pte_next = new_pte;
        new_pte_next = &new_pte->next;
        old_pte = old_pte->next;
    }

    // Copy regions
    struct region *old_region = old->start_region;
    struct region **new_region_next = &newas->start_region;

    while (old_region != NULL) {
        struct region *new_region = kmalloc(sizeof(struct region));
        if (new_region == NULL) {
            as_destroy(newas);
            return ENOMEM;
        }

        *new_region_next = new_region;
        new_region->start = old_region->start;
        new_region->size = old_region->size;
        new_region->npages = old_region->npages;
        new_region->read = old_region->read;
        new_region->write = old_region->write;
        new_region->execute = old_region->execute;
        new_region->next = NULL;

        new_region_next = &new_region->next;
        old_region = old_region->next;
    }

    // Copy heap information
    newas->heap_start = old->heap_start;
    newas->heap_end = old->heap_end;

    *ret = newas;
    return 0;
}

void as_destroy(struct addrspace *as) {
    KASSERT(as != NULL);

    // Clean up page table entries
    struct page_table_entry *current_pte = as->start_page_table;
    struct page_table_entry *next_pte;

    while (current_pte != NULL) {
        lock_acquire(current_pte->lock);

        if (current_pte->state == SWAPPED) {
            // Unmark the swapped page in the swap bitmap
            unmark_swap_bitmap(current_pte->diskpage_location);
        } else if (current_pte->state == MAPPED) {
            // Release the physical page
            int err = release_physical_page(current_pte->as_ppage);
            if (err) {
                // If release failed, skip cleanup for this PTE
                lock_release(current_pte->lock);
                current_pte = current_pte->next;
                continue;
            }
        }

        lock_release(current_pte->lock);

        // Destroy the lock and free the PTE
        next_pte = current_pte->next;
        lock_destroy(current_pte->lock);
        kfree(current_pte);
        current_pte = next_pte;
    }

    // Clean up region list
    struct region *current_region = as->start_region;
    struct region *next_region;

    while (current_region != NULL) {
        next_region = current_region->next;
        kfree(current_region);
        current_region = next_region;
    }

    // Free the address space itself
    kfree(as);
}

void as_activate(void) {
    // Get the current address space
    struct addrspace *current_as = proc_getas();
    if (current_as == NULL) {
        return; // No address space; kernel thread does not require TLB updates
    }

    // Temporarily disable interrupts
    int spl = splhigh();

    // Clear the TLB entries using an efficient loop
    for (int i = 0; i < NUM_TLB; i++) {
        tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
    }

    // Re-enable interrupts
    splx(spl);
}


void
as_deactivate(void)
{
	/*
	 * Write this. For many designs it won't need to actually do
	 * anything. See proc.c for an explanation of why it (might)
	 * be needed.
	 */
}

/*
 * Set up a segment at virtual address VADDR of size MEMSIZE. The
 * segment in memory extends from VADDR up to (but not including)
 * VADDR+MEMSIZE.
 *
 * The READABLE, WRITEABLE, and EXECUTABLE flags are set if read,
 * write, or execute permission should be set on the segment. At the
 * moment, these are ignored. When you write the VM system, you may
 * want to implement them.
 */
int as_define_region(struct addrspace *as, vaddr_t vaddr, size_t memsize,
                     int readable, int writeable, int executable) {
    KASSERT(as != NULL);

    // Align base address and size
    vaddr &= PAGE_FRAME;
    memsize = (memsize + (vaddr & ~(vaddr_t)PAGE_FRAME) + PAGE_SIZE - 1) & PAGE_FRAME;

    size_t npages = memsize / PAGE_SIZE;

    // Create a new region
    struct region *new_region = kmalloc(sizeof(struct region));
    if (new_region == NULL) {
        return ENOMEM;
    }

    // Initialize region properties
    new_region->start = vaddr;
    new_region->size = memsize;
    new_region->npages = npages;
    new_region->read = (readable & 4) != 0;
    new_region->write = (writeable & 2) != 0;
    new_region->execute = (executable & 1) != 0;
    new_region->next = NULL;

    // Insert the region into the list
    if (as->start_region == NULL) {
        as->start_region = new_region;
    } else {
        struct region *last = as->start_region;
        while (last->next != NULL) {
            last = last->next;
        }
        last->next = new_region;
    }

    return 0;
}

int as_prepare_load(struct addrspace *as) {
    KASSERT(as != NULL);

    vaddr_t heap_start = 0;

    // Find the end of the largest region
    for (struct region *current = as->start_region; current != NULL; current = current->next) {
        vaddr_t region_end = current->start + current->size;
        if (region_end > heap_start) {
            heap_start = region_end;
        }
    }

    // Align heap start and initialize heap end
    as->heap_start = (heap_start + PAGE_SIZE - 1) & PAGE_FRAME;
    as->heap_end = as->heap_start;

    KASSERT(as->heap_start % PAGE_SIZE == 0);

    return 0;
}


int
as_complete_load(struct addrspace *as)
{
	/*
	 * Write this.
	 */

	(void)as;
	return 0;
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
	/*
	 * Write this.
	 */

	/* Initial user-level stack pointer */
	(void)as;
	*stackptr = USERSTACK;

	return 0;
}

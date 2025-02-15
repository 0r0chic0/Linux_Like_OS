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

#ifndef _VM_H_
#define _VM_H_

#include <machine/vm.h>
#include <synch.h>

/* Fault-type arguments to vm_fault() */
#define VM_FAULT_READ        0    /* A read was attempted */
#define VM_FAULT_WRITE       1    /* A write was attempted */
#define VM_FAULT_READONLY    2    /* A write to a readonly page was attempted*/

#define VM_STACKPAGES 128

/* Coremap initialization start */

typedef enum {
    free,
    fixed,
    used,
    in_eviction
} page_state;

struct coremap_page {
    int chunk_size;
    page_state state;
    struct addrspace *owner_addrspace;
    vaddr_t owner_vaddr;
    bool ref_bit;
};

struct swap_disk {
    struct bitmap *bitmap;
    struct vnode *vnode;
    bool swap_disk_present;
};

extern struct swap_disk swap;

void coremap_bootstrap(void);  /* Renamed from coremap_load */

/* Coremap initialization end */

/* Initialization function */
void vm_bootstrap(void);

/* Fault handling function called by trap code */
int vm_fault(int faulttype, vaddr_t faultaddress);

paddr_t allocate_user_page(unsigned long pages, struct addrspace *as, vaddr_t vpage_addr, bool copy_call); 
vaddr_t alloc_kpages(unsigned npages);
void free_kpages(vaddr_t addr);
int release_physical_page(paddr_t page_paddr);
void tlb_invalidate_entry(vaddr_t remove_vaddr);
int read_swap_disk(paddr_t ppage_addr, unsigned int index, bool unmark); 
int write_swap_disk(paddr_t ppage_addr, unsigned int *index);   
void unmark_swap_bitmap(unsigned int index);                           
paddr_t evict_page(void);                                              

/*
 * Return amount of memory (in bytes) used by allocated coremap pages.
 * If there are ongoing allocations, this value could change after it
 * is returned to the caller. But it should have been correct at some
 * point in time.
 */
unsigned int coremap_memory_usage(void); /* Renamed from coremap_used_bytes */

/* TLB shootdown handling called from interprocessor_interrupt */
void vm_tlbshootdown(const struct tlbshootdown *);

#endif /* _VM_H_ */

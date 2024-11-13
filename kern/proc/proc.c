/*
 * Copyright (c) 2013
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

/*
 * Process support.
 *
 * There is (intentionally) not much here; you will need to add stuff
 * and maybe change around what's already present.
 *
 * p_lock is intended to be held when manipulating the pointers in the
 * proc structure, not while doing any significant work with the
 * things they point to. Rearrange this (and/or change it to be a
 * regular lock) as needed.
 *
 * Unless you're implementing multithreaded user processes, the only
 * process that will have more than one thread is the kernel process.
 */

#include <types.h>
#include <spl.h>
#include <proc.h>
#include <current.h>
#include <addrspace.h>
#include <vnode.h>
#include <synch.h>
#include <kern/fcntl.h>
#include <kern/errno.h>
#include <vfs.h>
#include <proc_table.h>
#include <wchan.h>

/*
 * The process for the kernel; this holds all the kernel-only threads.
 */
struct proc *kproc;
struct proc_table *processes;

/*
 * Create a proc structure.
 */
static
struct proc *
proc_create(const char *name)
{
	struct proc *proc;
	int i = 0;

	proc = kmalloc(sizeof(*proc));
	if (proc == NULL) {
		return NULL;
	}
	proc->p_name = kstrdup(name);
	if (proc->p_name == NULL) {
		kfree(proc);
		return NULL;
	}

	for(i=0 ; i < OPEN_MAX ; ++i)
	{
		proc->file_table[i] = NULL;
	}

	proc->children = array_create();
	if (proc->children == NULL) {
		kfree(proc->p_name);
		kfree(proc);
		return NULL;
	}

	threadarray_init(&proc->p_threads);
	spinlock_init(&proc->p_lock);

	/* VM fields */
	proc->p_addrspace = NULL;

	/* VFS fields */
	proc->p_cwd = NULL;

	proc->pid = 1;
	
	return proc;
}

void
clear_pid(pid_t pid)
{
	KASSERT(pid >= PID_MIN && pid <= PID_MAX);

	processes->pid_available++;
	processes->proc[pid] = NULL;
	processes->status[pid] = READY;
	processes->waitcode[pid] = (int) NULL;
}

/* Adds a given process to the proc_table at the given index */
static
void
add_pid(pid_t pid, struct proc *proc)
{
	KASSERT(proc != NULL);

	processes->proc[pid] = proc;
	processes->status[pid] = RUNNING;
	processes->waitcode[pid] = (int) NULL;
	processes->pid_available--;
}

int
proc_create_fork(const char *name, struct proc **new_proc)
{
	int ret;
	struct proc *proc;
	struct proc *c = curproc;
	(void)c;
	int i;

	proc = proc_create(name);
	if (proc == NULL) {
		return ENOMEM;
	}

	ret = proc_table_add(proc, &proc->pid);
	if (ret){
		proc_destroy(proc);
		return ret;
	}

	ret = as_copy(curproc->p_addrspace, &proc->p_addrspace);
	if (ret) {
		proc_destroy(proc);
		return ret;
	}

	spinlock_acquire(&curproc->p_lock);
	if (curproc->p_cwd != NULL) {
		VOP_INCREF(curproc->p_cwd);
		proc->p_cwd = curproc->p_cwd;
	}
	spinlock_release(&curproc->p_lock);
	
	spinlock_acquire(&curproc->p_lock);
	for (i = 0; i < OPEN_MAX; i++) {
		if (curproc->file_table[i] != NULL) {
        curproc->file_table[i]->d_count++;
        proc->file_table[i] = curproc->file_table[i];
		}
	}
	spinlock_release(&curproc->p_lock);


	*new_proc = proc;
	return 0;
}

/*
 * Destroy a proc structure.
 *
 * Note: nothing currently calls this. Your wait/exit code will
 * probably want to do so.
 */
void
proc_destroy(struct proc *proc)
{
	/*
	 * You probably want to destroy and null out much of the
	 * process (particularly the address space) at exit time if
	 * your wait/exit design calls for the process structure to
	 * hang around beyond process exit. Some wait/exit designs
	 * do, some don't.
	 */

	KASSERT(proc != NULL);
	KASSERT(proc != kproc);

	int destroy_counter = -1;

	/*
	 * We don't take p_lock in here because we must have the only
	 * reference to this structure. (Otherwise it would be
	 * incorrect to destroy it.)
	 */

	/* VFS fields */
	if (proc->p_cwd) {
		VOP_DECREF(proc->p_cwd);
		proc->p_cwd = NULL;
	}

	int children_size = array_num(proc->children);
	for (int i = 0; i < children_size; i++){
		array_remove(proc->children, 0);
	}

	/* VM fields */
	if (proc->p_addrspace) {

		struct addrspace *as;

		/*if (proc == curproc) {
			as = proc_setas(NULL);
			as_deactivate();
		}
		else {
			as = proc->p_addrspace;
			proc->p_addrspace = NULL;
		}*/
		as = proc->p_addrspace;
		as_destroy(as);
	}

	int threadarray_size = threadarray_num(&proc->p_threads);
	for (int i = 0; i < threadarray_size; i++){
		threadarray_remove(&proc->p_threads, 0);
	}

	threadarray_cleanup(&proc->p_threads);
	spinlock_cleanup(&proc->p_lock);

	for(int i=0 ; i < OPEN_MAX ; ++i)
	{
		if(proc->file_table[i] != NULL)
		{
			lock_acquire(proc->file_table[i]->lock);
			destroy_counter = proc->file_table[i]->d_count--;
			if(destroy_counter > 0)
			{
			    lock_release(proc->file_table[i]->lock);
				proc->file_table[i] = NULL;
			}
			else{
				lock_release(proc->file_table[i]->lock);
				lock_destroy(proc->file_table[i]->lock);
			    vfs_close(proc->file_table[i]->vnode);
				kfree(proc->file_table[i]);
				proc->file_table[i] = NULL;
			}
		}
	}
    array_destroy(proc->children);
	kfree(proc->p_name);
	kfree(proc);
}

struct proc *
get_pid(pid_t pid)
{
	KASSERT(pid >= PID_MIN && pid <= PID_MAX);

	struct proc *proc;
	bool acquired = lock_do_i_hold(processes->lock);

	if (!acquired) {
		lock_acquire(processes->lock);
	}

	proc = processes->proc[pid];

	if (!acquired) {
		lock_release(processes->lock);
	}

	return proc;
}

/* Removes a given PID from the proc_table. Used for failed forks. */
void
proc_table_freepid(pid_t pid)
{
	KASSERT(pid >= PID_MIN && pid <= PID_MAX);

	lock_acquire(processes->lock);
	clear_pid(pid);
	lock_release(processes->lock);
}

void
proc_table_bootstrap(void) {
/* Set up the proc_tables */
	processes = kmalloc(sizeof(struct proc_table));
	if (processes == NULL) {
		panic("Unable to initialize PID table.\n");
	}

	processes->lock = lock_create("locK");
	if (processes->lock == NULL) {
		panic("Unable to intialize PID table's lock.\n");
	}

	processes->cv = cv_create("pidtable cv");
	if (processes->cv == NULL) {
		panic("Unable to intialize PID table's cv.\n");
	}

	/* Set the kernel thread parameters */
	processes->pid_available = 1; /* One space for the kernel process */
	processes->pid_next = PID_MIN;
	add_pid(kproc->pid, kproc);

	/* Create space for more pids within the table */
	for (int i = PID_MIN; i < PID_MAX; i++){
		clear_pid(i);
	}
}
/*
 * Create the process structure for the kernel.
 */
void
proc_bootstrap(void)
{
	kproc = proc_create("[kernel]");
	if (kproc == NULL) {
		panic("proc_create for kproc failed\n");
	}
}
/*
 * Create a fresh proc for use by runprogram.
 *
 * It will have no address space and will inherit the current
 * process's (that is, the kernel menu's) current directory.
 */
struct proc *
proc_create_runprogram(const char *name)
{
	struct proc *newproc;
	newproc = proc_create(name);
	if (newproc == NULL) {
		return NULL;
	}

	/* VM fields */

	newproc->p_addrspace = NULL;

	/* VFS fields */

	/*
	 * Lock the current process to copy its current directory.
	 * (We don't need to lock the new process, though, as we have
	 * the only reference to it.)
	 */
	spinlock_acquire(&curproc->p_lock);
	if (curproc->p_cwd != NULL) {
		VOP_INCREF(curproc->p_cwd);
		newproc->p_cwd = curproc->p_cwd;
	}
	spinlock_release(&curproc->p_lock);

	int ret = proc_table_add(newproc, &newproc->pid);
	if(ret){
		proc_destroy(newproc);
		return NULL;
	}

	 newproc->file_table[0] = initialize_console("con:", O_RDONLY, "STDIN");
    if (newproc->file_table[0] == NULL) {
        goto cleanup;
    }

    newproc->file_table[1] = initialize_console("con:", O_WRONLY, "STDOUT");
    if (newproc->file_table[1] == NULL) {
        goto cleanup;
    }

    newproc->file_table[2] = initialize_console("con:", O_WRONLY, "STDERR");
    if (newproc->file_table[2] == NULL) {
        goto cleanup;
    }

    return newproc;

	cleanup:
    if (newproc->file_table[0]) {
        lock_destroy(newproc->file_table[0]->lock);
        vfs_close(newproc->file_table[0]->vnode);
        kfree(newproc->file_table[0]);
    }
    if (newproc->file_table[1]) {
        lock_destroy(newproc->file_table[1]->lock);
        vfs_close(newproc->file_table[1]->vnode);
        kfree(newproc->file_table[1]);
    }
    if (newproc->file_table[2]) {
        lock_destroy(newproc->file_table[2]->lock);
        vfs_close(newproc->file_table[2]->vnode);
        kfree(newproc->file_table[2]);
    }
    proc_destroy(newproc);
    return NULL;
     
}

struct file_handler *initialize_console(const char *con_name, int flags, const char *lock_name) {
    int err;
    struct file_handler *fh =(struct file_handler *)kmalloc(sizeof(struct file_handler));
    if (fh == NULL) {
        return NULL;
    }

    char *con = kstrdup(con_name);
    if (con == NULL) {
        kfree(fh);
        return NULL;
    }

    err = vfs_open(con, flags, 0, &fh->vnode);
    kfree(con);
    if (err) {
        kfree(fh);
        return NULL;
    }

    fh->offset = 0;
    fh->lock = lock_create(lock_name);
    if (fh->lock == NULL) {
        vfs_close(fh->vnode);
        kfree(fh);
        return NULL;
    }

    fh->d_count = 1;
    fh->mode = flags & O_ACCMODE;
    return fh;
}

/*
 * Add a thread to a process. Either the thread or the process might
 * or might not be current.
 *
 * Turn off interrupts on the local cpu while changing t_proc, in
 * case it's current, to protect against the as_activate call in
 * the timer interrupt context switch, and any other implicit uses
 * of "curproc".
 */
int
proc_addthread(struct proc *proc, struct thread *t)
{
	int result;
	int spl;

	KASSERT(t->t_proc == NULL);

	spinlock_acquire(&proc->p_lock);
	result = threadarray_add(&proc->p_threads, t, NULL);
	spinlock_release(&proc->p_lock);
	if (result) {
		return result;
	}
	spl = splhigh();
	t->t_proc = proc;
	splx(spl);
	return 0;
}

/*
 * Remove a thread from its process. Either the thread or the process
 * might or might not be current.
 *
 * Turn off interrupts on the local cpu while changing t_proc, in
 * case it's current, to protect against the as_activate call in
 * the timer interrupt context switch, and any other implicit uses
 * of "curproc".
 */
void
proc_remthread(struct thread *t)
{
	struct proc *proc;
	unsigned i, num;
	int spl;

	proc = t->t_proc;
	KASSERT(proc != NULL);

	spinlock_acquire(&proc->p_lock);
	/* ugh: find the thread in the array */
	num = threadarray_num(&proc->p_threads);
	for (i=0; i<num; i++) {
		if (threadarray_get(&proc->p_threads, i) == t) {
			threadarray_remove(&proc->p_threads, i);
			spinlock_release(&proc->p_lock);
			spl = splhigh();
			t->t_proc = NULL;
			splx(spl);
			return;
		}
	}
	/* Did not find it. */
	spinlock_release(&proc->p_lock);
	panic("Thread (%p) has escaped from its process (%p)\n", t, proc);
}

/*
 * Fetch the address space of (the current) process.
 *
 * Caution: address spaces aren't refcounted. If you implement
 * multithreaded processes, make sure to set up a refcount scheme or
 * some other method to make this safe. Otherwise the returned address
 * space might disappear under you.
 */
struct addrspace *
proc_getas(void)
{
	struct addrspace *as;
	struct proc *proc = curproc;

	if (proc == NULL) {
		return NULL;
	}

	spinlock_acquire(&proc->p_lock);
	as = proc->p_addrspace;
	spinlock_release(&proc->p_lock);
	return as;
}

/*
 * Change the address space of (the current) process. Return the old
 * one for later restoration or disposal.
 */
struct addrspace *
proc_setas(struct addrspace *newas)
{
	struct addrspace *oldas;
	struct proc *proc = curproc;

	KASSERT(proc != NULL);

	spinlock_acquire(&proc->p_lock);
	oldas = proc->p_addrspace;
	proc->p_addrspace = newas;
	spinlock_release(&proc->p_lock);
	return oldas;
}

int
proc_table_add(struct proc *proc, int32_t *retval)
{
	int next;
	int output = 0;

	KASSERT(proc != NULL);

	lock_acquire(processes->lock);

	if (processes->pid_available < 1){
		lock_release(processes->lock);
		return ENPROC;
	}

	array_add(curproc->children, proc, NULL);

	next = processes->pid_next;
	*retval = next;

	add_pid(next, proc);

	/* Find the next available PID */
	if(processes->pid_available > 0){
		for (int i = next; i < PID_MAX; i++){
			if (processes->status[i] == READY){
				processes->pid_next = i;
				break;
			}
		}
	}
	/* Put an out-of-bounds value to signify a full table */
	else{
		processes->pid_next = PID_MAX + 1;
	}

	lock_release(processes->lock);

	return output;
}


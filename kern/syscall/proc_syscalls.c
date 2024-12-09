#include <types.h>
#include <proc.h>
#include <current.h>
#include <addrspace.h>
#include <kern/errno.h>
#include <machine/trapframe.h>
#include <kern/fcntl.h>
#include <vfs.h>
#include <syscall.h>
#include <copyinout.h>
#include <proc_table.h>
#include <wchan.h>


void
enter_usermode(void *data1, unsigned long data2)
{
	(void) data2;
	void *tf = (void *) curthread->t_stack + 16;

	memcpy(tf, (const void *) data1, sizeof(struct trapframe));
	kfree((struct trapframe *) data1);

	as_activate();
	mips_usermode(tf);
}

/*
 Function which forks the current process to create a child.
 */
int
sys_fork(struct trapframe *tf, int32_t *retval)
{
	struct proc *new_proc;
	int ret;

	ret = proc_create_fork("new_proc", &new_proc);
	if(ret) {
		return ret;
	}

	struct trapframe *new_tf;
	new_tf = kmalloc(sizeof(struct trapframe));
	if (new_tf == NULL) {
		return ENOMEM;
	}

	memcpy(new_tf, tf, sizeof(struct trapframe));
	new_tf->tf_v0 = 0;
	new_tf->tf_v1 = 0;
	new_tf->tf_a3 = 0;      /* signal no error */
	new_tf->tf_epc += 4;

	*retval = new_proc->pid;
	ret = thread_fork("new_thread", new_proc, enter_usermode, new_tf, 1);
	if (ret) {
		pid_t pid = new_proc->pid;
		proc_destroy(new_proc);
		proc_table_freepid(pid);
		kfree(new_tf);
		return ret;
	}

	return 0;
}

/*
 Gets the PID of the current process.
 */
int
sys_getpid(int32_t *retval)
{
	lock_acquire(processes->lock);

	*retval = curproc->pid;

	lock_release(processes->lock);
	return 0;
}

/*
 Function called by a parent process to wait until a child process exits.
 */
int
sys_waitpid(pid_t pid, int32_t *retval, int32_t options)
{
	int status; // The status of the process which is being waited upon
	int waitcode; // The reason for process exit as defined in wait.h

	if (pid < PID_MIN || pid > PID_MAX || processes->status[pid] == READY){
		return ESRCH;
	}
	
	if (options != 0){
		return EINVAL;
	}

	/* Check that the pid being called is a child of the current process */
	bool ischild = false;
	
	struct proc *child = processes->proc[pid];
	
	int size = array_num(curproc->children);
	for (int i = 0; i < size; i++){
		if (child == array_get(curproc->children, i)){
			ischild =true;
			break;
		}
	}
	if(!ischild){
		return ECHILD;
	}

	lock_acquire(processes->lock);

	status = processes->status[pid];
	while(status != ZOMBIE){
		cv_wait(processes->cv, processes->lock);
		status = processes->status[pid];
	}
	waitcode = processes->waitcode[pid];

	lock_release(processes->lock);

	/* A NULL retval0 indicates that nothing is to be returned. */
	if(retval != NULL){
		int ret = copyout(&waitcode, (userptr_t) retval, sizeof(int32_t));
		if (ret){
			return ret;
		}
	}

	return 0;
}

/* Will update the status of children to either ORPHAN or ZOMBIE. */
static
void
proc_table_update_children(struct proc *proc)
{
	KASSERT(lock_do_i_hold(processes->lock));
	KASSERT(proc != NULL);

	int num_child = array_num(proc->children);

	for(int i = num_child-1; i >= 0; i--){

		struct proc *child = array_get(proc->children, i);
		int child_pid = child->pid;

		if(processes->status[child_pid] == RUNNING){
			processes->status[child_pid] = ORPHAN;
		}
		else if (processes->status[child_pid] == ZOMBIE){
			/* Update the next pid indicator */
			if(child_pid < processes->pid_next){
				processes->pid_next = child_pid;
			}
			proc_destroy(child);
			clear_pid(child_pid);
		}
		else{
			panic("Tried to modify a child that did not exist.\n");
		}
	}
}

/*
 Exits the current process and stores the waitcode as defined in <kern/wait.h>.
 The supplied waitcode to this funtion is assumed to be already encoded properly.
 */
void
sys__exit(int32_t waitcode)
{
	struct proc *proc = curproc;
	KASSERT(proc != NULL);

	lock_acquire(processes->lock);

	proc_table_update_children(proc);

	/* Case: Signal the parent that the child ended with waitcode given. */
	if(processes->status[proc->pid] == RUNNING){
		processes->status[proc->pid] = ZOMBIE;
		processes->waitcode[proc->pid] = waitcode;
	}
	/* Case: Parent already exited. Reset the current pidtable spot for later use. */
	else if(processes->status[proc->pid] == ORPHAN){
		pid_t pid = proc->pid;
		proc_destroy(curproc);
		clear_pid(pid);
	}
	else{
		panic("Tried to remove a bad process.\n");
	}

	/* Broadcast to any waiting processes. There is no guarentee that the processes on the cv are waiting for us */
	cv_broadcast(processes->cv, processes->lock);

	lock_release(processes->lock);

	thread_exit();
}

int
sys_execv(const char *prog, char **args)
{
	int ret;

	// Check for NULL arguments
	if (prog == NULL || args == NULL) {
		return EFAULT;
	}

	
	// Copy progname from user space to kernel space 
	char *progname;
	size_t copy_size = PATH_MAX;
	copy_size++;
	progname = kmalloc(copy_size * sizeof(char));
	size_t *path_len = kmalloc(sizeof(int));
	ret = copyinstr((const_userptr_t)prog, progname, copy_size, path_len);
	if (ret) {
		kfree(progname);
		kfree(path_len);
		return ret;
	}
	kfree(path_len);

	
	// Count arguments 
	int argc = 0;
	char *next_arg;
	do {
		argc++;
		ret = copyin((const_userptr_t) &args[argc], (void *) &next_arg, (size_t) sizeof(char *));
		if (ret) {
			kfree(progname);
			return ret;
		}
	} while (next_arg != NULL && argc < ARG_MAX);

	if (next_arg != NULL) {
		kfree(progname);
		return E2BIG;
	}


	// Allocate memory for kernel args and sizes
	char **args_in = kmalloc(argc * sizeof(char *));
	int *size = kmalloc(argc * sizeof(int));
	if (args_in == NULL || size == NULL) {
		kfree(progname);
		kfree(args_in);
		kfree(size);
		return ENOMEM;
	}

	// Copy arguments from user space to kernel space 
	int arg_size_left = ARG_MAX;
	size_t cur_size;
	for (int i = 0; i < argc; i++) {
		// string length check
		int j = -1;
		char next_char;
		do {
			j++;
			ret = copyin((const_userptr_t) &args[i][j], (void *) &next_char, (size_t) sizeof(char));
			if (ret) {
				kfree(progname);
				for (int k = 0; k < i; k++) {
					kfree(args_in[k]);
				}
				kfree(args_in);
				kfree(size);
				return ret;
			}
		} while (next_char != 0 && j < arg_size_left - 1);

		if (next_char != 0) {
			kfree(progname);
			for (int k = 0; k < i; k++) {
				kfree(args_in[k]);
			}
			kfree(args_in);
			kfree(size);
			return E2BIG;
		}

		cur_size = j;
		arg_size_left -= (cur_size + 1);
		size[i] = (int) cur_size + 1;

		// copy string in
		copy_size = cur_size + 1;
		args_in[i] = kmalloc(copy_size * sizeof(char));
		size_t *arg_len = kmalloc(sizeof(int));
		ret = copyinstr((const_userptr_t) args[i], args_in[i], copy_size, arg_len);
		if (ret) {
			kfree(progname);
			kfree(arg_len);
			for (int k = 0; k <= i; k++) {
				kfree(args_in[k]);
			}
			kfree(args_in);
			kfree(size);
			return ret;
		}
		kfree(arg_len);
	}

	
	// Open the program file
	struct vnode *v;
	vaddr_t entrypoint, stackptr;
	ret = vfs_open(progname, O_RDONLY, 0, &v);
	kfree(progname);
	if (ret) {
		for (int i = 0; i < argc; i++) {
			kfree(args_in[i]);
		}
		kfree(args_in);
		kfree(size);
		return ret;
	}

	
	// Create a new address space
	struct addrspace *as_new = as_create();
	if (as_new == NULL) {
		vfs_close(v);
		for (int i = 0; i < argc; i++) {
			kfree(args_in[i]);
		}
		kfree(args_in);
		kfree(size);
		return ENOMEM;
	}
	struct addrspace *as_old = proc_setas(as_new);
	as_destroy(as_old);
	as_activate();

	
	// Load the ELF executable
	ret = load_elf(v, &entrypoint);
	vfs_close(v);
	if (ret) {
		as_destroy(as_new);
		for (int i = 0; i < argc; i++) {
			kfree(args_in[i]);
		}
		kfree(args_in);
		kfree(size);
		return ret;
	}

	
	// Define the user stack in the new address space
	ret = as_define_stack(as_new, &stackptr);
	if (ret) {
		as_destroy(as_new);
		for (int i = 0; i < argc; i++) {
			kfree(args_in[i]);
		}
		kfree(args_in);
		kfree(size);
		return ret;
	}

	
	// Copy arguments onto the stack
	userptr_t arg_addr = (userptr_t)(stackptr - argc * sizeof(userptr_t *) - sizeof(NULL));
	userptr_t *args_out = (userptr_t *)(stackptr - argc * sizeof(userptr_t *) - sizeof(NULL));
	for (int i = 0; i < argc; i++) {
		arg_addr -= size[i];
		ret = copyoutstr(args_in[i], arg_addr, (size_t)size[i], NULL);
		if (ret) {
			as_destroy(as_new);
			for (int j = 0; j < argc; j++) {
				kfree(args_in[j]);
			}
			kfree(args_in);
			kfree(size);
			return ret;
		}
		*args_out = arg_addr;
		args_out++;
	}

	*args_out = NULL;
	userptr_t args_out_addr = (userptr_t)(stackptr - argc * sizeof(int) - sizeof(NULL));
	arg_addr -= (int)arg_addr % sizeof(void *);
	stackptr = (vaddr_t)arg_addr;

	
	// Free kernel args
	for (int i = 0; i < argc; i++) {
		kfree(args_in[i]);
	}
	kfree(args_in);
	kfree(size);

	
	// Enter the new process
	enter_new_process(argc, args_out_addr, NULL, stackptr, entrypoint);

	
	// enter_new_process does not return
	panic("enter_new_process returned\n");
	return EINVAL;
}

int sys_sbrk(intptr_t increment, vaddr_t *retval) {
    struct addrspace *addr_space;
    long old_heap_end, new_heap_end;
    int num_pages, err;
    vaddr_t remove_vaddr;
    struct page_table_entry *current_pte, *previous_pte;

    addr_space = proc_getas();
    KASSERT(addr_space != NULL);

    old_heap_end = (long)addr_space->heap_end;

    if (increment == 0) {
        *retval = addr_space->heap_end;
        return 0;
    }

    // Calculate the new heap end
    new_heap_end = old_heap_end + increment;

    // Validate the new heap end
    if ((increment > 0 && new_heap_end < old_heap_end) || (increment < 0 && new_heap_end < (long)addr_space->heap_start)) {
        return (increment > 0) ? ENOMEM : EINVAL;
    }
    if (increment > 0 && new_heap_end >= (long)(USERSTACK - VM_STACKPAGES * PAGE_SIZE)) {
        return ENOMEM;
    }
    if (increment % PAGE_SIZE != 0) {
        return EINVAL;
    }

    // Calculate the number of pages to add/remove
    num_pages = increment / PAGE_SIZE;
    if (num_pages == 0) {
        *retval = addr_space->heap_end;
        return 0;
    }

    if (increment > 0) {
        addr_space->heap_end = (vaddr_t)new_heap_end;
    } else {
        for (int i = 0; i < -num_pages; i++) {
            remove_vaddr = old_heap_end - (i + 1) * PAGE_SIZE;
            current_pte = addr_space->start_page_table;
            previous_pte = NULL;

            while (current_pte != NULL) {
                if (current_pte->as_vpage == remove_vaddr) {
                    lock_acquire(current_pte->lock);

                    if (current_pte->state == SWAPPED) {
                        unmark_swap_bitmap(current_pte->diskpage_location);
                    } else {
                        err = release_physical_page(current_pte->as_ppage);
                        if (err) {
                            lock_release(current_pte->lock);
                            continue;
                        }
                        tlb_invalidate_entry(remove_vaddr);
                    }

                    lock_release(current_pte->lock);

                    if (current_pte == addr_space->start_page_table) {
                        addr_space->start_page_table = current_pte->next;
                    } else {
                        previous_pte->next = current_pte->next;
                    }

                    lock_destroy(current_pte->lock);
                    kfree(current_pte);
                    break;
                }

                previous_pte = current_pte;
                current_pte = current_pte->next;
            }
        }
        addr_space->heap_end = (vaddr_t)new_heap_end;
    }

    *retval = (vaddr_t)old_heap_end;
    return 0;
}

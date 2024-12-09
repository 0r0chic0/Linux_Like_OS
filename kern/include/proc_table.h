
#ifndef _PROC_TABLE_H_
#define _PROC_TABLE_H_

#include <spinlock.h>
#include <proc.h>
#include <limits.h>
#include <mips/trapframe.h>
#define READY 0     /* Index available for process */
#define RUNNING 1   /* Process running */
#define ZOMBIE 2    /* Process waiting to be reaped */
#define ORPHAN 3    /* Process running and parent exited */


struct proc_table {
    struct proc *proc[32 + 1] ;
    int status[32 + 1];
    int waitcode[32 + 1];
    struct lock *lock;   
    struct cv *cv;
    int pid_available;
	int pid_next;
};

extern struct proc_table *processes;

int sys_getpid(int32_t *);
int sys_fork(struct trapframe *, int32_t *);
int sys_waitpid(pid_t, int32_t *, int32_t);
void sys__exit(int32_t);
int sys_execv(const char *, char **);
int sys_sbrk(intptr_t amount, vaddr_t *retval);

/* Creating and entering a new process */
void enter_usermode(void *, unsigned long);

void proc_table_bootstrap(void);
int proc_create_fork(const char *, struct proc **);
struct proc *get_pid(pid_t);
int proc_table_add(struct proc *, int32_t *);
void proc_table_freepid(pid_t);

#endif 

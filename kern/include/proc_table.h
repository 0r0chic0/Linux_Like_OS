#ifndef _PROC_TABLE_H_
#define _PROC_TABLE_H_

#include <spinlock.h>
#include <proc.h>
#include <limits.h>

struct proc_table {
    struct proc *proc[OPEN_MAX] ;
    struct spinlock lock;   
};

extern struct proc_table *processes;

#endif 

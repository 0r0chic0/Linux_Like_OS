#ifndef _FILE_HANDLER_H_
#define _FILE_HANDLER_H_

#include <vnode.h>
#include <synch.h>

struct file_handler {
struct vnode *vnode;
int d_count;
int mode;
off_t offset;
bool config;
struct lock *lock;
};

struct file_handler *initialize_console(const char *con_name, int flags, const char *lock_name);

#endif /* _FILE_HANDLER_H_ */
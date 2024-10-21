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

int sys_open(const char *filename, int flags, int32_t *retval);
int sys_read(int fd, void *buf, size_t bufflen, int32_t *retval);
int sys_write(int fd, const void *buff, size_t bufflen, int32_t *ret);
int sys_close(int fd);
int sys_lseek(int fd, off_t offset, int32_t whence,off_t *retval);
int sys_dup2(int oldfd, int newfd, int *retval);
int sys__getcwd(char *buf, size_t buflen, int32_t *retval);
int sys_chdir (const char *path);

#endif /* _FILE_HANDLER_H_ */
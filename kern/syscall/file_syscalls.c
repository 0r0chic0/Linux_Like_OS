#include <types.h>
#include <proc.h>
#include <current.h>
#include <vnode.h>
#include <vfs.h>
#include <uio.h>
#include <synch.h>
#include <kern/errno.h>
#include <kern/fcntl.h>
#include <kern/seek.h>
#include <copyinout.h>
#include <limits.h>
#include <kern/unistd.h>
#include <endian.h>
#include <stat.h>
#include <lib.h>
#include <file_handler.h>

int sys_open(const char *filename, int flags, int32_t *retval) {
    char cin_filename[PATH_MAX];
    int err = copyinstr((const_userptr_t)filename, cin_filename, PATH_MAX, NULL);
    if (err) return err;

    int i = 3;
    while (i < OPEN_MAX && curproc->file_table[i] != NULL) i++;
    if (i == OPEN_MAX) return EMFILE;

    struct file_handler *fh = kmalloc(sizeof(*fh));
    if (!fh) return ENOMEM;

    err = vfs_open(cin_filename, flags, 0, &fh->vnode);
    if (err) {
        kfree(fh);
        return err;
    }

    // Set the offset based on O_APPEND or start at 0.
    if (flags & O_APPEND) {
        struct stat statbuf;
        err = VOP_STAT(fh->vnode, &statbuf);
        if (err) {
            vfs_close(fh->vnode);
            kfree(fh);
            return err;
        }
        fh->offset = statbuf.st_size;
    } else {
        fh->offset = 0;
    }

    fh->d_count = 1;
    fh->config = false;
    fh->mode = flags & O_ACCMODE;
    fh->lock = lock_create("filehandle_lock");
    if (!fh->lock) {
        vfs_close(fh->vnode);
        kfree(fh);
        return ENOMEM;
    }

    curproc->file_table[i] = fh;
    *retval = i;
    return 0;
}


int sys_read(int fd, void *buf, size_t bufflen, int32_t *retval) {
    if (fd < 0 || fd >= OPEN_MAX || !curproc->file_table[fd] || curproc->file_table[fd]->mode == O_WRONLY)
        return EBADF;

    struct file_handler *fh = curproc->file_table[fd];
    char *kbuf = kmalloc(bufflen);
    if (!kbuf) return ENOMEM;

    struct iovec iov;
    struct uio kuio;
    lock_acquire(fh->lock);
    uio_kinit(&iov, &kuio, kbuf, bufflen, fh->offset, UIO_READ);

    int err = VOP_READ(fh->vnode, &kuio);
    if (!err) {
        *retval = kuio.uio_offset - fh->offset;
        fh->offset = kuio.uio_offset;
        err = copyout(kbuf, buf, *retval);
    }
    lock_release(fh->lock);
    kfree(kbuf);
    return err;
}

int sys_write(int fd, const void *buff, size_t bufflen, int32_t *retval) {
    if (fd < 0 || fd >= OPEN_MAX || !curproc->file_table[fd] || curproc->file_table[fd]->mode == O_RDONLY)
        return EBADF;

    struct file_handler *fh = curproc->file_table[fd];
    char *kbuf = kmalloc(bufflen);
    if (!kbuf) return ENOMEM;

    int err = copyin((const_userptr_t)buff, kbuf, bufflen);
    if (err) {
        kfree(kbuf);
        return err;
    }

    struct iovec iov;
    struct uio kuio;
    lock_acquire(fh->lock);
    uio_kinit(&iov, &kuio, kbuf, bufflen, fh->offset, UIO_WRITE);

    err = VOP_WRITE(fh->vnode, &kuio);
    if (!err) {
        *retval = kuio.uio_offset - fh->offset;
        fh->offset = kuio.uio_offset;
    }
    lock_release(fh->lock);
    kfree(kbuf);
    return err;
}

int sys_close(int fd) {
    if (fd < 0 || fd >= OPEN_MAX || !curproc->file_table[fd]) return EBADF;

    struct file_handler *fh = curproc->file_table[fd];
    lock_acquire(fh->lock);
    if (--fh->d_count > 0) {
        lock_release(fh->lock);
    } else {
        lock_release(fh->lock);
        lock_destroy(fh->lock);
        vfs_close(fh->vnode);
        kfree(fh);
        curproc->file_table[fd] = NULL;
    }
    return 0;
}

int sys_lseek(int fd, off_t offset, int32_t whence, off_t *retval) {
    struct stat info;
    struct file_handler *file;
    int result;

    // Validate the file descriptor and retrieve the file handle.
    if (fd < 0 || fd >= OPEN_MAX || curproc->file_table[fd] == NULL) {
        return EBADF;
    }
    file = curproc->file_table[fd];

    // Acquire the lock for thread-safe access.
    lock_acquire(file->lock);

    // Check if the file descriptor refers to a special file or pipe.
    if (file->config) {
        lock_release(file->lock);
        return ESPIPE; // Cannot seek on pipes or special files.
    }

    // Check if the vnode associated with the file is seekable.
    if (!VOP_ISSEEKABLE(file->vnode)) {
        lock_release(file->lock);
        return ESPIPE;
    }

    // Calculate the new offset based on 'whence'.
    switch (whence) {
        case SEEK_SET:
            if (offset < 0) {
                lock_release(file->lock);
                return EINVAL;
            }
            *retval = offset;
            break;

        case SEEK_CUR:
            if (file->offset + offset < 0) {
                lock_release(file->lock);
                return EINVAL;
            }
            *retval = file->offset + offset;
            break;

        case SEEK_END:
            // Get the size of the file using VOP_STAT.
            result = VOP_STAT(file->vnode, &info);
            if (result) {
                lock_release(file->lock);
                return result;
            }

            if (info.st_size + offset < 0) {
                lock_release(file->lock);
                return EINVAL;
            }
            *retval = info.st_size + offset;
            break;

        default:
            lock_release(file->lock);
            return EINVAL;
    }

    // Update the file's current offset after validation.
    file->offset = *retval;

    // Release the lock after updating.
    lock_release(file->lock);

    return 0;
}


int sys_dup2(int oldfd, int newfd, int *retval) {

	struct file_handler *temp = NULL;
	bool newfdflag = false;
	if(oldfd < 0 || oldfd >= OPEN_MAX || newfd < 0 || newfd >= OPEN_MAX || curproc->file_table[oldfd] == NULL) {
		return EBADF;		
	}

	if(oldfd == newfd){
		*retval = newfd;
		return 0;
	}

	if(curproc->file_table[newfd] != NULL){
		temp = curproc->file_table[newfd];
		newfdflag = true;
	}
	
	lock_acquire(curproc->file_table[oldfd]->lock);
	if(curproc->file_table[oldfd]->d_count == 0) {
		lock_release(curproc->file_table[oldfd]->lock);
		return EBADF;
	}
	curproc->file_table[newfd] = curproc->file_table[oldfd];
	KASSERT(curproc->file_table[oldfd]->d_count > 0);
	curproc->file_table[oldfd]->d_count++;
	lock_release(curproc->file_table[oldfd]->lock);

	if(newfdflag) {
		lock_acquire(temp->lock);	
		KASSERT(temp->d_count > 0);
		temp->d_count--;
		if(temp->d_count > 0) {
			lock_release(temp->lock);
			temp = NULL;
		} else {
			lock_release(temp->lock);
			KASSERT(temp->d_count == 0);
			lock_destroy(temp->lock);
        	        vfs_close(temp->vnode);
        	        kfree(temp);
			temp = NULL;
		}
	}
	*retval = newfd;

	return 0;
}

int sys__getcwd(char *buf, size_t buflen, int32_t *retval){
	char *buffer = (char *)kmalloc(sizeof(*buf)*buflen);
    struct uio kuio;
    struct iovec iov;
    
    uio_kinit(&iov, &kuio, buffer, buflen, 0, UIO_READ);
    int err = vfs_getcwd(&kuio);
    if (err){
		return err;
	}

	off_t bytes = kuio.uio_offset ;
	*retval = (int32_t)bytes;
	if(bytes != 0){
		err = copyout(buffer, (userptr_t)buf, *retval);
		if(err) {
			kfree(buffer);
			return err;
		}
	}
	return 0;
}

int sys_chdir (const char *path) { 
    size_t len = PATH_MAX;
	size_t got;
	char kpath[PATH_MAX]; 
    int err;
	int copyinside = copyinstr((const_userptr_t)path, kpath, len, &got);
	if (copyinside) {
		return copyinside;
	}
    err = vfs_chdir(kpath);
    if(err) {
        return err;
    }
    return 0;
}
    



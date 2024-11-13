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
#include <filesyscalls.h>

// Open a file with specified filename and flags, and return a file descriptor.
int sys_open(const char *filename, int flags, int32_t *retval) {
    char cin_filename[PATH_MAX];
    // Copy the filename from user space to kernel space.
    int err = copyinstr((const_userptr_t)filename, cin_filename, PATH_MAX, NULL);
    if (err) return err;

    // Find an available file descriptor slot.
    int i = 3;
    while (i < OPEN_MAX && curproc->file_table[i] != NULL) i++;
    if (i == OPEN_MAX) return EMFILE; // No available file descriptors.

    // Allocate and initialize a file handler.
    struct file_handler *fh = (struct file_handler *)kmalloc(sizeof(struct file_handler));
    if (!fh) return ENOMEM;

    // Open the file using the VFS layer.
    err = vfs_open(cin_filename, flags, 0, &fh->vnode);
    if (err) {
        kfree(fh);
        return err;
    }

    // Set the offset to the end if O_APPEND is set; otherwise, start at 0.
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

    // Set other fields for the file handler.
    fh->d_count = 1;
    fh->config = false;
    fh->mode = flags & O_ACCMODE;
    fh->lock = lock_create("filehandle_lock");
    if (!fh->lock) {
        vfs_close(fh->vnode);
        kfree(fh);
        return ENOMEM;
    }

    // Store the file handler in the process's file table.
    curproc->file_table[i] = fh;
    *retval = i; // Return the new file descriptor.
    return 0;
}

// Read data from an open file descriptor into a user-provided buffer.
int sys_read(int fd, void *buf, size_t bufflen, int32_t *retval) {
    if (fd < 0 || fd >= OPEN_MAX || curproc->file_table[fd] == NULL || curproc->file_table[fd]->mode == O_WRONLY) {
        return EBADF; // Invalid file descriptor or write-only file.
    }

    struct file_handler *fh = curproc->file_table[fd];
    struct iovec iov;
    struct uio kuio;
    
    lock_acquire(fh->lock);
    // Initialize a uio structure for reading.
    uio_kinit(&iov, &kuio, buf, bufflen, fh->offset, UIO_READ);
    kuio.uio_segflg = UIO_USERSPACE;
    kuio.uio_space = curproc->p_addrspace;
    iov.iov_ubase = (userptr_t)buf;

    int err = VOP_READ(fh->vnode, &kuio); // Perform the read operation.
    if (err) {
        lock_release(fh->lock);
        return err;
    }

    // Calculate the number of bytes read and update the file offset.
    off_t bytes = kuio.uio_offset - fh->offset;
    *retval = (int32_t)bytes;
    fh->offset = kuio.uio_offset;

    lock_release(fh->lock);
    return 0;
}

// Write data from a user buffer to an open file descriptor.
int sys_write(int fd, const void *buff, size_t bufflen, int32_t *retval) {
    if (fd < 0 || fd >= OPEN_MAX || !curproc->file_table[fd]|| curproc->file_table[fd]->mode == O_RDONLY) {
        return EBADF; // Invalid file descriptor or read-only file.
    }

    struct file_handler *fh = curproc->file_table[fd];
    struct iovec iov;
    struct uio kuio;
    lock_acquire(fh->lock);
    // Initialize a uio structure for writing.
    uio_kinit(&iov, &kuio, (char *)buff, bufflen, fh->offset, UIO_WRITE);
     kuio.uio_segflg = UIO_USERSPACE;
    kuio.uio_space = curproc->p_addrspace;
    iov.iov_ubase = (userptr_t)buff;

    // Perform the write operation.
    int err = VOP_WRITE(fh->vnode, &kuio);
    if (!err) {
        *retval = kuio.uio_offset - fh->offset; // Set the number of bytes written.
        fh->offset = kuio.uio_offset; // Update the file offset.
    }

    lock_release(fh->lock);
    return err;
}

// Close an open file descriptor, releasing its resources.
int sys_close(int fd) {
    if (fd < 0 || fd >= OPEN_MAX || !curproc->file_table[fd]) return EBADF;

    struct file_handler *fh = curproc->file_table[fd];
    lock_acquire(fh->lock);

    // Decrease the reference count and close if it reaches zero.
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
// Duplicate a file descriptor.
int sys_dup2(int oldfd, int newfd, int *retval) {
    // Validate file descriptors and ensure oldfd is open.
    if (oldfd < 0 || oldfd >= OPEN_MAX || newfd < 0 || newfd >= OPEN_MAX ||
        curproc->file_table[oldfd] == NULL) {
        return EBADF;
    }

    // If oldfd and newfd are the same, no duplication is needed.
    if (oldfd == newfd) {
        *retval = newfd;
        return 0;
    }

    // Retrieve the file handlers.
    struct file_handler *oldfh = curproc->file_table[oldfd];
    struct file_handler *newfh = curproc->file_table[newfd];

    // If newfd is open, close it first.
    if (newfh != NULL) {
        lock_acquire(newfh->lock);
        newfh->d_count--;
        if (newfh->d_count == 0) {
            lock_destroy(newfh->lock);
            vfs_close(newfh->vnode);
            kfree(newfh);
        } else {
            lock_release(newfh->lock);
        }
    }

    // Point newfd to the same file as oldfd.
    lock_acquire(oldfh->lock);
    curproc->file_table[newfd] = oldfh;
    oldfh->d_count++;
    lock_release(oldfh->lock);

    *retval = newfd; // Return the duplicated file descriptor.
    return 0;
}

// Get the current working directory.
int sys__getcwd(char *buf, size_t buflen, int32_t *retval) {
    char *buffer = (char *)kmalloc(sizeof(*buf) * buflen);
    struct uio kuio;
    struct iovec iov;

    // Initialize a uio structure for reading the current working directory.
    uio_kinit(&iov, &kuio, buffer, buflen, 0, UIO_READ);
    int err = vfs_getcwd(&kuio);
    if (err) return err;

    // Get the number of bytes read and copy them to the user space buffer.
    off_t bytes = kuio.uio_offset;
    *retval = (int32_t)bytes;
    if (bytes != 0) {
        err = copyout(buffer, (userptr_t)buf, *retval);
        if (err) {
            kfree(buffer);
            return err;
        }
    }
    kfree(buffer);
    return 0;
}

// Change the current working directory.
int sys_chdir(const char *path) {
    char kpath[PATH_MAX];
    int err = copyinstr((const_userptr_t)path, kpath, PATH_MAX, NULL);
    if (err) return err;

    // Change the current working directory using vfs_chdir.
    err = vfs_chdir(kpath);
    return err;
}

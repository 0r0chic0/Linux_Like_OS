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
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS `AS IS'' AND
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
#include <kern/syscall.h>
#include <lib.h>
#include <mips/trapframe.h>
#include <thread.h>
#include <current.h>
#include <syscall.h>
#include <file_handler.h>
#include <copyinout.h>
#include <filesyscalls.h>
#include <proc_table.h>
#include <addrspace.h>
#include <kern/wait.h>


/*
 * System call dispatcher.
 *
 * A pointer to the trapframe created during exception entry (in
 * exception-*.S) is passed in.
 *
 * The calling conventions for syscalls are as follows: Like ordinary
 * function calls, the first 4 32-bit arguments are passed in the 4
 * argument registers a0-a3. 64-bit arguments are passed in *aligned*
 * pairs of registers, that is, either a0/a1 or a2/a3. This means that
 * if the first argument is 32-bit and the second is 64-bit, a1 is
 * unused.
 *
 * This much is the same as the calling conventions for ordinary
 * function calls. In addition, the system call number is passed in
 * the v0 register.
 *
 * On successful return, the return value is passed back in the v0
 * register, or v0 and v1 if 64-bit. This is also like an ordinary
 * function call, and additionally the a3 register is also set to 0 to
 * indicate success.
 *
 * On an error return, the error code is passed back in the v0
 * register, and the a3 register is set to 1 to indicate failure.
 * (Userlevel code takes care of storing the error code in errno and
 * returning the value -1 from the actual userlevel syscall function.
 * See src/user/lib/libc/arch/mips/syscalls-mips.S and related files.)
 *
 * Upon syscall return the program counter stored in the trapframe
 * must be incremented by one instruction; otherwise the exception
 * return code will restart the "syscall" instruction and the system
 * call will repeat forever.
 *
 * If you run out of registers (which happens quickly with 64-bit
 * values) further arguments must be fetched from the user-level
 * stack, starting at sp+16 to skip over the slots for the
 * registerized values, with copyin().
 */
void
syscall(struct trapframe *tf)
{
    int callno;
    int32_t retval;
    int err;
    off_t ret_offset; // for lseek 64-bit return values

    KASSERT(curthread != NULL);
    KASSERT(curthread->t_curspl == 0);
    KASSERT(curthread->t_iplhigh_count == 0);

    callno = tf->tf_v0;

    /*
     * Initialize retval to 0. Many of the system calls don't
     * really return a value, just 0 for success and -1 on
     * error. Since retval is the value returned on success,
     * initialize it to 0 by default; thus it's not necessary to
     * deal with it except for calls that return other values,
     * like write.
     */

    retval = 0;

    switch (callno) {
        case SYS_reboot:
            err = sys_reboot(tf->tf_a0);
            break;

        case SYS___time:
            err = sys___time((userptr_t)tf->tf_a0,
                             (userptr_t)tf->tf_a1);
            break;

        case SYS_open:
            err = sys_open((const char *)tf->tf_a0, (int)tf->tf_a1, &retval);
            break;

        case SYS_read:
            err = sys_read((int)tf->tf_a0, (void *)tf->tf_a1, (size_t)tf->tf_a2, &retval);
            break;

        case SYS_write:
            err = sys_write((int)tf->tf_a0, (void *)tf->tf_a1, (size_t)tf->tf_a2, &retval);
            break;

        case SYS_close:
            err = sys_close((int)tf->tf_a0);
            break;

        case SYS_lseek: {
            // Retrieve the 64-bit offset from a2/a3
            off_t offset = ((off_t)tf->tf_a2 << 32) | tf->tf_a3;

            // Retrieve 'whence' from the user stack (sp+16)
            int32_t whence;
            err = copyin((const_userptr_t)(tf->tf_sp + 16), &whence, sizeof(whence));
            if (err) {
                break;
            }

            // Call sys_lseek with the 64-bit offset.
            err = sys_lseek((int)tf->tf_a0, offset, whence, &ret_offset);

            if (!err) {
                // Split the 64-bit offset into v0 and v1 for returning.
                tf->tf_v0 = (uint32_t)(ret_offset >> 32); // High 32 bits.
                tf->tf_v1 = (uint32_t)ret_offset;         // Low 32 bits.
            }
            break;
        }

        case SYS_dup2:
            err = sys_dup2((int)tf->tf_a0, (int)tf->tf_a1, &retval);
            break;

        case SYS___getcwd:
            err = sys__getcwd((char *)tf->tf_a0, (size_t)tf->tf_a1, &retval);
            break;

        case SYS_chdir:
            err = sys_chdir((const char *)tf->tf_a0);
            break;

        case SYS_fork:
            err = sys_fork(tf, &retval);
            break;

        case SYS_getpid:
            err = sys_getpid(&retval);
            break;

        case SYS_execv:
            err = sys_execv((const char *)tf->tf_a0, (char **)tf->tf_a1);
            break;
        
        case SYS_waitpid:
            err = sys_waitpid((pid_t)tf->tf_a0, (int32_t *) tf->tf_a1, (int32_t) tf->tf_a2);
            break;
        
        case SYS__exit: ;
		int waitcode = (int) _MKWAIT_EXIT(tf->tf_a0);
		sys__exit(waitcode);
		panic("The exit syscall should never return");
		break;

        case SYS_sbrk:
		err = sys_sbrk((intptr_t)tf->tf_a0, (vaddr_t *)&retval);
		break;


        default:
            kprintf("Unknown syscall %d\n", callno);
            err = ENOSYS;
            break;
    }

    // Handle the return value based on success or failure.
    if (err) {
        // Return the error code.
        tf->tf_v0 = err;
        tf->tf_a3 = 1; // Signal an error.
    }
    else if (callno != SYS_lseek) {
        // Success, not lseek, return simple retval.
        tf->tf_v0 = retval;
        tf->tf_a3 = 0; // Signal no error.
    } else {
        // lseek succeeded and set tf_v0 and tf_v1.
        tf->tf_a3 = 0;
    }

    // Advance the program counter to avoid restarting the syscall.
    tf->tf_epc += 4;

    // Make sure the syscall code didn't forget to lower spl.
    KASSERT(curthread->t_curspl == 0);
    // ...or leak any spinlocks.
    KASSERT(curthread->t_iplhigh_count == 0);
}

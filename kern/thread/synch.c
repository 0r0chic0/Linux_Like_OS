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

/*
 * Synchronization primitives.
 * The specifications of the functions are in synch.h.
 */

#include <types.h>
#include <lib.h>
#include <spinlock.h>
#include <wchan.h>
#include <thread.h>
#include <current.h>
#include <synch.h>

////////////////////////////////////////////////////////////
//
// Semaphore.

struct semaphore *
sem_create(const char *name, unsigned initial_count)
{
        struct semaphore *sem;

        sem = kmalloc(sizeof(struct semaphore));
        if (sem == NULL) {
                return NULL;
        }

        sem->sem_name = kstrdup(name);
        if (sem->sem_name == NULL) {
                kfree(sem);
                return NULL;
        }

	sem->sem_wchan = wchan_create(sem->sem_name);
	if (sem->sem_wchan == NULL) {
		kfree(sem->sem_name);
		kfree(sem);
		return NULL;
	}

	spinlock_init(&sem->sem_lock);
        sem->sem_count = initial_count;

        return sem;
}

void
sem_destroy(struct semaphore *sem)
{
        KASSERT(sem != NULL);

	/* wchan_cleanup will assert if anyone's waiting on it */
	spinlock_cleanup(&sem->sem_lock);
	wchan_destroy(sem->sem_wchan);
        kfree(sem->sem_name);
        kfree(sem);
}

void
P(struct semaphore *sem)
{
        KASSERT(sem != NULL);

        /*
         * May not block in an interrupt handler.
         *
         * For robustness, always check, even if we can actually
         * complete the P without blocking.
         */
        KASSERT(curthread->t_in_interrupt == false);

	/* Use the semaphore spinlock to protect the wchan as well. */
	spinlock_acquire(&sem->sem_lock);
        while (sem->sem_count == 0) {
		/*
		 *
		 * Note that we don't maintain strict FIFO ordering of
		 * threads going through the semaphore; that is, we
		 * might "get" it on the first try even if other
		 * threads are waiting. Apparently according to some
		 * textbooks semaphores must for some reason have
		 * strict ordering. Too bad. :-)
		 *
		 * Exercise: how would you implement strict FIFO
		 * ordering?
		 */
		wchan_sleep(sem->sem_wchan, &sem->sem_lock);
        }
        KASSERT(sem->sem_count > 0);
        sem->sem_count--;
	spinlock_release(&sem->sem_lock);
}

void
V(struct semaphore *sem)
{
        KASSERT(sem != NULL);

	spinlock_acquire(&sem->sem_lock);

        sem->sem_count++;
        KASSERT(sem->sem_count > 0);
	wchan_wakeone(sem->sem_wchan, &sem->sem_lock);

	spinlock_release(&sem->sem_lock);
}

////////////////////////////////////////////////////////////
//
// Lock.

struct lock *
lock_create(const char *name)
{
<<<<<<< HEAD
    struct lock *lock;

    lock = kmalloc(sizeof(struct lock));
    if (lock == NULL) {
        return NULL;
    }

    lock->lk_name = kstrdup(name);
    if (lock->lk_name == NULL) {
        kfree(lock);
        return NULL;
    }

       // Create a wait channel for threads waiting on this lock
    lock->lk_wchan = wchan_create(lock->lk_name);
    if (lock->lk_wchan == NULL) {
        kfree(lock->lk_name);
        kfree(lock);
        return NULL;
    }
    

    // Initialize the lock as free
    lock->lk_lock = 0;

    // Initialize the spinlock for protecting this lock
    spinlock_init(&lock->lk_spinlock);

    return lock;
}


=======
        struct lock *lock;

        lock = kmalloc(sizeof(struct lock));
        if (lock == NULL) {
                return NULL;
        }

        lock->lk_name = kstrdup(name);
        if (lock->lk_name == NULL) {
                kfree(lock);
                return NULL;
        }

        // add stuff here as needed

        return lock;
}

>>>>>>> instructor/synchprobs
void
lock_destroy(struct lock *lock)
{
        KASSERT(lock != NULL);
<<<<<<< HEAD
        
        spinlock_cleanup(&lock->lk_spinlock);
	wchan_destroy(lock->lk_wchan);
=======

        // add stuff here as needed

>>>>>>> instructor/synchprobs
        kfree(lock->lk_name);
        kfree(lock);
}

void
lock_acquire(struct lock *lock)
{
<<<<<<< HEAD
    KASSERT(lock != NULL); // Ensure lock is not NULL

    // Acquire the spinlock to ensure atomic access to the lock state
    spinlock_acquire(&lock->lk_spinlock);

    // Check if the lock is already held
    while (lock->lk_lock == 1) {

        wchan_sleep(lock->lk_wchan, &lock->lk_spinlock); // Lock is held, so we sleep on the wait channel
    }

    // Lock is free, we can acquire it
    lock->lk_lock = 1;  // Set the lock as held
    lock->lk_holder = curthread; // Set to current thread value
    

    // Release the spinlock
    spinlock_release(&lock->lk_spinlock);
=======
        // Write this

        (void)lock;  // suppress warning until code gets written
>>>>>>> instructor/synchprobs
}

void
lock_release(struct lock *lock)
{
<<<<<<< HEAD
    KASSERT(lock != NULL); // Ensure lock is not NULL
    KASSERT(lock->lk_lock == 1 ); // Ensure lock is held
    KASSERT(lock->lk_holder == curthread); // Ensure thread value matches
    
    spinlock_acquire(&lock->lk_spinlock);

    lock->lk_lock = 0;  // Set the lock as free
    lock->lk_holder = NULL;  // Set the current CPU as NULL

    wchan_wakeone(lock->lk_wchan, &lock->lk_spinlock);  // Lock is free, so we can wake up one thread on the wait channel

    // Release the spinlock
    spinlock_release(&lock->lk_spinlock);
       
=======
        // Write this

        (void)lock;  // suppress warning until code gets written
>>>>>>> instructor/synchprobs
}

bool
lock_do_i_hold(struct lock *lock)
{
<<<<<<< HEAD
    KASSERT(lock != NULL);  // Ensure lock is not NULL
    
    bool result = false;

    spinlock_acquire(&lock->lk_spinlock); 
    
    if(lock->lk_lock == 1 && lock->lk_holder == curthread) {
		result = true;
	} else {
		result = false;
	}

    spinlock_release(&lock->lk_spinlock);

    return result;
}


////////////////////////////////////////////////////////////
//
// CV
struct cv *
cv_create(const char *name)
{
    struct cv *cv;

    cv = kmalloc(sizeof(struct cv));
    if (cv == NULL) {
        return NULL;
    }

    cv->cv_name = kstrdup(name);
    if (cv->cv_name == NULL) {
        kfree(cv);
        return NULL;
    }

    cv->cv_wchan = wchan_create(cv->cv_name);
    if (cv->cv_wchan == NULL) {
        kfree(cv->cv_name);
        kfree(cv);
        return NULL;
    }

    spinlock_init(&cv->cv_spinlock); // Initialize spinlock for protecting cv

    return cv;
}


=======
        // Write this

        (void)lock;  // suppress warning until code gets written

        return true; // dummy until code gets written
}

////////////////////////////////////////////////////////////
//
// CV


struct cv *
cv_create(const char *name)
{
        struct cv *cv;

        cv = kmalloc(sizeof(struct cv));
        if (cv == NULL) {
                return NULL;
        }

        cv->cv_name = kstrdup(name);
        if (cv->cv_name==NULL) {
                kfree(cv);
                return NULL;
        }

        // add stuff here as needed

        return cv;
}

>>>>>>> instructor/synchprobs
void
cv_destroy(struct cv *cv)
{
        KASSERT(cv != NULL);

<<<<<<< HEAD
        spinlock_cleanup(&cv->cv_spinlock);
	wchan_destroy(cv->cv_wchan);
=======
        // add stuff here as needed
>>>>>>> instructor/synchprobs

        kfree(cv->cv_name);
        kfree(cv);
}

void
cv_wait(struct cv *cv, struct lock *lock)
{
<<<<<<< HEAD
    KASSERT(cv != NULL); // Ensure condition variable is not NULL
    KASSERT(lock != NULL); // Ensure lock is not NULL
    KASSERT(lock_do_i_hold(lock)); // Ensure the current thread holds the lock

    // Acquire the spinlock to protect the wait channel
    spinlock_acquire(&cv->cv_spinlock);

    // Release the associated lock before sleeping
    lock_release(lock);

    // Sleep on the wait channel and release the spinlock
    wchan_sleep(cv->cv_wchan, &cv->cv_spinlock);
    spinlock_release(&cv->cv_spinlock);

    // Reacquire the lock after waking up
    lock_acquire(lock);
=======
        // Write this
        (void)cv;    // suppress warning until code gets written
        (void)lock;  // suppress warning until code gets written
>>>>>>> instructor/synchprobs
}

void
cv_signal(struct cv *cv, struct lock *lock)
{
<<<<<<< HEAD
    KASSERT(cv != NULL); // Ensure condition variable is not NULL

    KASSERT(lock_do_i_hold(lock) == true);

    // Acquire the spinlock to protect the wait channel
    spinlock_acquire(&cv->cv_spinlock);

    // Wake up one thread waiting on the condition variable
    wchan_wakeone(cv->cv_wchan, &cv->cv_spinlock);

    // Release the spinlock
    spinlock_release(&cv->cv_spinlock);
}

void
cv_broadcast(struct cv *cv,struct lock *lock)
{
    KASSERT(cv != NULL); // Ensure condition variable is not NULL

    KASSERT(lock_do_i_hold(lock) == true);

    // Acquire the spinlock to protect the wait channel
    spinlock_acquire(&cv->cv_spinlock);

    // Wake up all threads waiting on the condition variable
    wchan_wakeall(cv->cv_wchan,&cv->cv_spinlock);

    // Release the spinlock
    spinlock_release(&cv->cv_spinlock);
=======
        // Write this
	(void)cv;    // suppress warning until code gets written
	(void)lock;  // suppress warning until code gets written
}

void
cv_broadcast(struct cv *cv, struct lock *lock)
{
	// Write this
	(void)cv;    // suppress warning until code gets written
	(void)lock;  // suppress warning until code gets written
>>>>>>> instructor/synchprobs
}

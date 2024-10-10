#include <types.h>
#include <lib.h>
#include <thread.h>
#include <test.h>
#include <synch.h>

#define N_LORD_FLOWERKILLER 8
#define NROPES 16

static volatile int ropes_left = NROPES;

// Rope structure to store individual lock for each rope
struct rope {
    int stake;
    int hook;
    bool cut;
    struct lock *rope_lock; // Each rope will have its own lock
};

// Array of ropes
static struct rope ropelist[NROPES];

// Lock for protecting access to ropes_left
static struct lock *ropes_left_lock;

// Semaphores to signal thread completion
static struct semaphore *dandelion_done;
static struct semaphore *marigold_done;
static struct semaphore *balloon_done;
static struct semaphore *flowerkiller_done;

// Function to initialize mappings and synchronization primitives
static void init_mappings(void) {
    for (int i = 0; i < NROPES; i++) {
        ropelist[i].stake = i;
        ropelist[i].hook = i;
        ropelist[i].cut = false;
        ropelist[i].rope_lock = lock_create("rope_lock");
    }
    
    ropes_left_lock = lock_create("ropes_left_lock");

    dandelion_done = sem_create("dandelion_done", 0);
    marigold_done = sem_create("marigold_done", 0);
    balloon_done = sem_create("balloon_done", 0);
    flowerkiller_done = sem_create("flowerkiller_done", 0);
}

// Function to destroy resources
static void destroy_resources(void) {
    for (int i = 0; i < NROPES; i++) {
        lock_destroy(ropelist[i].rope_lock); // Destroy individual rope locks
    }
    
    lock_destroy(ropes_left_lock); // Destroy the lock for ropes_left
    sem_destroy(dandelion_done);
    sem_destroy(marigold_done);
    sem_destroy(balloon_done);
    sem_destroy(flowerkiller_done);
}

// Thread function: Dandelion
static void dandelion(void *p, unsigned long arg) {
    (void)p;
    (void)arg;

    kprintf("Dandelion thread starting\n");
    thread_yield();

    while (ropes_left > 0) {
        int random_rope = random() % NROPES;

        lock_acquire(ropelist[random_rope].rope_lock);
        if (!ropelist[random_rope].cut) {
            kprintf("Dandelion severed rope %d\n", random_rope);
            ropelist[random_rope].cut = true;

            // Protect access to ropes_left with a lock
            lock_acquire(ropes_left_lock);
            ropes_left--;
            lock_release(ropes_left_lock);
        }
        lock_release(ropelist[random_rope].rope_lock);

        thread_yield();  // Yield to allow other threads to run
    }

    kprintf("Dandelion thread done\n");
    V(dandelion_done); // Signal the semaphore
    thread_exit();
}

// Thread function: Marigold
static void marigold(void *p, unsigned long arg) {
    (void)p;
    (void)arg;

    kprintf("Marigold thread starting\n");
    thread_yield();

    while (ropes_left > 0) {
        int random_rope = random() % NROPES;

        lock_acquire(ropelist[random_rope].rope_lock);
        if (!ropelist[random_rope].cut) {
            kprintf("Marigold severed rope %d from stake %d\n", random_rope, ropelist[random_rope].stake);
            ropelist[random_rope].cut = true;

            // Protect access to ropes_left with a lock
            lock_acquire(ropes_left_lock);
            ropes_left--;
            lock_release(ropes_left_lock);
        }
        lock_release(ropelist[random_rope].rope_lock);

        thread_yield();  // Yield to allow other threads to run
    }

    kprintf("Marigold thread done\n");
    V(marigold_done); // Signal the semaphore
    thread_exit();
}

// Thread function: Lord FlowerKiller
static void flowerkiller(void *p, unsigned long arg) {
    (void)p;
    (void)arg;

    kprintf("Lord FlowerKiller thread starting\n");
    thread_yield();

    while (ropes_left > 1) {
        int rope1 = random() % NROPES;
        int rope2 = random() % NROPES;

        if (rope1 != rope2) {
            // Ensure rope1 < rope2 to prevent circular wait
            if (rope1 > rope2) {
                int temp = rope1;
                rope1 = rope2;
                rope2 = temp;
            }

            // Acquire locks in consistent order: first rope1, then rope2
            lock_acquire(ropelist[rope1].rope_lock);
            lock_acquire(ropelist[rope2].rope_lock);

            if (!ropelist[rope1].cut && !ropelist[rope2].cut) {
                kprintf("Lord FlowerKiller switched rope %d from stake %d to stake %d\n", rope1, ropelist[rope1].stake, ropelist[rope2].stake);
                kprintf("Lord FlowerKiller switched rope %d from stake %d to stake %d\n", rope2, ropelist[rope2].stake, ropelist[rope1].stake);

                // Swap stakes
                int temp = ropelist[rope1].stake;
                ropelist[rope1].stake = ropelist[rope2].stake;
                ropelist[rope2].stake = temp;
            }

            // Release locks in the same order
            lock_release(ropelist[rope1].rope_lock);
            lock_release(ropelist[rope2].rope_lock);
        }

        thread_yield();  // Yield to allow other threads to run
    }

    kprintf("Lord FlowerKiller thread done\n");
    V(flowerkiller_done);
    thread_exit();
}

// Thread function: Balloon
static void balloon(void *p, unsigned long arg) {
    (void)p;
    (void)arg;

    kprintf("Balloon thread starting\n");
    while (ropes_left > 0) {
        thread_yield();  // Yield to allow other threads to run
    }

    kprintf("Balloon freed and Prince Dandelion escapes!\n");
    kprintf("Balloon thread done\n");
    V(balloon_done); // Signal the semaphore
    thread_exit();
}

int airballoon(int nargs, char **args) {
    int err = 0;

    (void)nargs;
    (void)args;

    init_mappings();

    err = thread_fork("Marigold Thread", NULL, marigold, NULL, 0);
    if (err) goto panic;

    err = thread_fork("Dandelion Thread", NULL, dandelion, NULL, 0);
    if (err) goto panic;

    for (int i = 0; i < N_LORD_FLOWERKILLER; i++) {
        err = thread_fork("Lord FlowerKiller Thread", NULL, flowerkiller, NULL, 0);
        if (err) goto panic;
    }

    err = thread_fork("Air Balloon", NULL, balloon, NULL, 0);
    if (err) goto panic;

    // Wait for all threads to complete
    P(dandelion_done);
    P(marigold_done);

    for (int i = 0; i < N_LORD_FLOWERKILLER; i++) {
        P(flowerkiller_done);
    }

    P(balloon_done);  // Wait for Balloon to finish

    ropes_left = NROPES;

    // Clean up resources
    destroy_resources();

    kprintf("Main thread done\n");

    goto done;

panic:
    panic("airballoon: thread_fork failed: %s)\n", strerror(err));

done:
    return 0;
}

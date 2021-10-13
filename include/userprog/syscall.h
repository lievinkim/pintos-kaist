#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

void syscall_init (void);

/* Proj 2-4. file descriptor */
/* prevent simultaneous read, write (race condition prevention) */
struct lock file_rw_lock;               

#endif /* userprog/syscall.h */

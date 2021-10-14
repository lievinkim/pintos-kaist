#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H
#include <list.h>

void syscall_init (void);

/* Proj 2-4. file descriptor */
/* prevent simultaneous read, write (race condition prevention) */
/* 단순 read, write prevention이 아니라 syscall에 대한 lock으로 확장 */
/* file_rw_lock -> filesys_lock으로 네이밍 변경 */
struct lock filesys_lock;               

#endif /* userprog/syscall.h */

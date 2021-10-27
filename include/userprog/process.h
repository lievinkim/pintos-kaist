#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"

tid_t process_create_initd (const char *file_name);
tid_t process_fork (const char *name, struct intr_frame *if_);
int process_exec (void *f_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (struct thread *next);

/* Proj 2-3. wait syscall */
struct thread *get_child_with_pid(int pid);

/* Proj 2-7. Extra */
/* child fork 시 이미 복제된 파일인지 확인하는 Map */
struct MapElem
{
	uintptr_t key;
	uintptr_t value;
};

/* Project 3. MMF : 신규 생성 함수 선언 */
struct file *process_get_file(int fd);

#endif /* userprog/process.h */

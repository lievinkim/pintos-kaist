#ifndef VM_FILE_H
#define VM_FILE_H
#include "filesys/file.h"
#include "vm/vm.h"

struct page;
enum vm_type;

/* Project 3. MMF : do_mmap 함수를 위해 file_page 구조체 생성*/
struct file_page {
	
	struct file* file;		// 페이지에 할당한 파일에 대한 정보
	off_t size;				// real written size except zero bytes
	off_t ofs;				// 시작되는 위치

};

/* Project 3. MMF : do_mmap 함수를 위해 mmap_info 구조체 생성*/
struct mmap_info{
	struct file* file;		// 파일 정보
	off_t offset;			// 오프셋 정보
	size_t read_bytes;		// 저장 길이
};

void vm_file_init (void);
bool file_backed_initializer (struct page *page, enum vm_type type, void *kva);
void *do_mmap(void *addr, size_t length, int writable,
		struct file *file, off_t offset);
void do_munmap (void *va);
#endif

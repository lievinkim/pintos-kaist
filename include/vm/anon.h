#ifndef VM_ANON_H
#define VM_ANON_H
#include "vm/vm.h"

/* Project 3. Swap In/Out : 어나니머스 페이지 초기화 시 스왑 정보 입력을 위한 헤더 및 정의 추가 */
#include "devices/disk.h"
#define INVALID_SLOT_IDX SIZE_MAX   // MAX 값으로 초기화

struct page;
enum vm_type;

struct anon_page {
    struct thread* owner;           // Project 3. AP : anon_initializer 수정에 따른 멤버 추가

    /* Project 3. Swap In/Out : 어나니머스 페이지에 스왑 정보 입력을 위한 멤버 추가 */
    size_t swap_slot_idx;

};

void vm_anon_init (void);
bool anon_initializer (struct page *page, enum vm_type type, void *kva);

#endif
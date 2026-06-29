//
// User-space helpers for kernel threads.
//
// thread_create() and thread_join() are thin wrappers over the clone()
// and join() system calls.  They mirror the spirit of pthread_create()
// and pthread_join():
#include "kernel/types.h"
#include "user/uthread.h"
#include "kernel/riscv.h"
#include "user/user.h"

#define CLONE_VM 0x0100

int thread_create(void (*fn)(void *), void *arg, int n_pages) {
  // user stack을 malloc으로 할당
  void *stack = malloc(n_pages * PGSIZE);
  if (stack == 0)
    return -1;

  // clone syscall 호출 — CLONE_VM 플래그로 thread 경로
  int tid = clone(fn, arg, stack, n_pages, CLONE_VM);

  if (tid < 0) {
    free(stack);    // 실패 시 stack 회수
    return -1;
  }
  return tid;
}

int thread_join(void) { 
  void *stack = 0;

  // join syscall 호출 — kernel이 ZOMBIE thread의 ustack을 채워 줌
  int tid = join(&stack);

  if (tid < 0)
    return -1;

  // thread가 쓰던 stack을 해제
  if (stack)
    free(stack);

  return tid;
 }

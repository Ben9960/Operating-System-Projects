#include "types.h"     
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void proc_mapstacks(pagetable_t kpgtbl) {
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if (pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int)(p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table.
void procinit(void) {
  struct proc *p;

  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for (p = proc; p < &proc[NPROC]; p++) {
    initlock(&p->lock, "proc");
    p->state = UNUSED;
    p->kstack = KSTACK((int)(p - proc));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int cpuid() {
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu *mycpu(void) {
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc *myproc(void) {
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int allocpid() {
  int pid;

  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

struct mm_struct *mm_alloc(void) {
  struct mm_struct *mm = (struct mm_struct *)kalloc();
  if (mm == 0)
    return 0;
  mm->pagetable = 0;
  mm->sz = 0;
  mm->refcount = 1;
  initlock(&mm->lock, "mm");
  return mm;
}

void mm_get(struct mm_struct *mm) {
  acquire(&mm->lock);
  if (mm->refcount < 1)
    panic("mm_get_panic");
  mm->refcount++;
  release(&mm->lock);
}

void mm_put(struct mm_struct *mm) {
  acquire(&mm->lock);
  if (mm->refcount < 1)
    panic("mm_put_panic");
  else {
    mm->refcount--;
    if (mm->refcount != 0) { // mm을 쓰는 다른 thread가 있으니 page table은
      release(&mm->lock);    // 건드리지 않고, 락만 풀고 리턴
      return;
    }
  }
  release(&mm->lock);

  // 이 코드까지 도달하면, 마지막 참조자임 (mm->refcount == 0)
  // 락 밖에서 page table mm 자체를 해제

  if (mm->pagetable) {
    proc_freepagetable(mm->pagetable,mm->sz); 
    // mm이 가리키는 pagetable 내용을 해제함
  }

  kfree((void *)mm);
}

uint64 find_free_tf_va(pagetable_t pagetable) {
  // NTREAD 개의 슬롯을 순회하며 비어있는 첫 VA를 반환
  // slot 0 = TRAPFRAME (group leader가 이미 사용 중일 것)
  // slot i = TRAPFRAME - i * PGSIZE

  for (int i = 0; i < NTHREAD; i++) {
    uint64 va = TRAPFRAME - i * PGSIZE;
    if (ismapped(pagetable, va) == 0) { // 매핑되지 않았다면
      return va;
    }
  }
  return 0;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc *allocproc(void) {
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;

  // Default thread fields: a single-task process is its own group leader.
  p->mm = 0;
  p->group_leader = p;
  p->tgid = p->pid;
  p->tf_va = 0;
  p->ustack = 0;

  // Allocate a trapframe page (per-task; never shared).
  if ((p->trapframe = (struct trapframe *)kalloc()) == 0) {
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}

// free a proc structure and its per-task resources.
// p->lock must be held.
static void freeproc(struct proc *p) {
  // leader(tf_va == TRAPFRAME)와 슬롯 미할당(tf_va == 0) 제외
  // tf_va == 0인 채로 uvmunmap을 호출하면 0번지 text page를 날리는 버그가 생김
  if (p->mm &&  p->mm->pagetable && p->tf_va !=0 && p->tf_va != TRAPFRAME) {  
    acquire(&p->mm->lock);
    uvmunmap(p->mm->pagetable, p->tf_va, 1, 0);
    release(&p->mm->lock);
    // uvmunmap 인자
    // 세번째 인자는 언매핑할 페이지 수
    // 네번쨰 인자(dofree)는 0이면 pte만 지우고 1이면 free까지함
  }

  if (p->trapframe) {
    kfree((void *)p->trapframe);
    p->trapframe = 0;
  }
  p->tf_va = 0;

  if (p->mm) {
    mm_put(p->mm);
    p->mm = 0;
  }

  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
  p->group_leader = 0;
  p->tgid = 0;
  p->ustack = 0;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.  Sets p->tf_va = TRAPFRAME
// (slot 0 — used by group leaders / single-threaded processes).
pagetable_t proc_pagetable(struct proc *p) {
  pagetable_t pagetable;

  pagetable = uvmcreate();
  if (pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if (mappages(pagetable, TRAMPOLINE, PGSIZE, (uint64)trampoline,
               PTE_R | PTE_X) < 0) {
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if (mappages(pagetable, TRAPFRAME, PGSIZE, (uint64)(p->trapframe),
               PTE_R | PTE_W) < 0) {
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  p->tf_va = TRAPFRAME;
  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void proc_freepagetable(pagetable_t pagetable, uint64 sz) {
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// Set up first user process.
void userinit(void) {
  struct proc *p;

  p = allocproc();
  initproc = p;

  if ((p->mm = mm_alloc()) == 0)
    panic("userinit: mm_alloc");
  if ((p->mm->pagetable = proc_pagetable(p)) == 0)
    panic("userinit: proc_pagetable");
  p->mm->sz = 0;

  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Returns 0 on success, -1 on failure.
int growproc(int n) {
  struct proc *p = myproc();
  struct mm_struct *mm = p->mm;
  uint64 sz;

  acquire(&mm->lock);
  sz = mm->sz;
  if (n > 0) {
    if (sz + n > USERTOP) {
      release(&mm->lock);
      return -1;
    }
    if ((sz = uvmalloc(mm->pagetable, sz, sz + n, PTE_W)) == 0) {
      release(&mm->lock);
      return -1;
    }
  } else if (n < 0) {
    sz = uvmdealloc(mm->pagetable, sz, sz + n);
  }
  mm->sz = sz;
  release(&mm->lock);
  return 0;
}

int kclone(uint64 fn, uint64 arg, uint64 stack, int n_pages, int flags) {
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  if ((np = allocproc()) == 0)
    return -1;

  if (flags & CLONE_VM) {
    // === thread path ===
    np->mm = p->mm;
    mm_get(np->mm);
    
    acquire(&np->mm->lock);
    uint64 tf_va = find_free_tf_va(np->mm->pagetable);
    if(tf_va == 0){
      release(&np->mm->lock);
      goto fail;
    }
    if(mappages(np->mm->pagetable, tf_va, PGSIZE, (uint64)np->trapframe, PTE_R | PTE_W) < 0){
      release(&np->mm->lock);
      goto fail;
    }
    np->tf_va = tf_va;
    release(&np->mm->lock);

    np->group_leader = p->group_leader;
    np->tgid = p->tgid;

    np->ustack = (void *) stack;

    memset(np->trapframe, 0, sizeof(*np->trapframe));
    np->trapframe->epc = fn;
    np->trapframe->sp = stack + (uint64)n_pages * PGSIZE;  
    np->trapframe->a0 = arg; // fn에 전달할 인자

  
  } else {
    // === fork path ===
    if ((np->mm = mm_alloc()) == 0)
      goto fail;
    if ((np->mm->pagetable = proc_pagetable(np)) == 0)
      goto fail;
    if (uvmcopy(p->mm->pagetable, np->mm->pagetable, p->mm->sz) < 0)
      goto fail;
    np->mm->sz = p->mm->sz;
    np->group_leader = np;
    np->tgid = np->pid;

    // copy parent's user registers; child resumes after the syscall.
    *(np->trapframe) = *(p->trapframe);
    np->trapframe->a0 = 0; // child's clone() returns 0
  }
  // === common: files, cwd, name ===
  for (i = 0; i < NOFILE; i++)
    if (p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);

  return pid;

fail:
  freeproc(np);
  release(&np->lock);
  return -1;
}

int kjoin(uint64 stack_addr) {
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);
 
  for(;;){
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->group_leader != p->group_leader || pp == p) ///tgid로 sibling임을 검증하면 안됨 (자기자신도 포함되므로)
        continue;
    
      acquire(&pp->lock);
      havekids = 1;
    if(pp->state == ZOMBIE){
      pid = pp->pid;

      if(stack_addr != 0){
        uint64 ustack_val = (uint64)pp->ustack;
        if(copyout(p->mm->pagetable, stack_addr, (char *)&ustack_val, sizeof(ustack_val)) < 0){
          //copy out으로 user에게 ustack_val 전달 실패시 정리
          release(&pp->lock);
          release(&wait_lock);
          return -1;
          }
        }
        freeproc(pp);
        release(&pp->lock);
        release(&wait_lock);
        return pid;
    }
    release(&pp->lock);
  }

  if(!havekids || killed(p)){
    release(&wait_lock);
    return -1;
  }

  sleep(p, &wait_lock);
  }
}
// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void reparent(struct proc *p) {
  struct proc *pp;

  for (pp = proc; pp < &proc[NPROC]; pp++) {
    if (pp->parent == p) {
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current task.  Does not return.
// An exited task remains in the zombie state until its parent calls
// wait() (for processes) or join() (for threads).
void kexit(int status) {
  // leader가 exit: 그룹 전체가 끝나야 함 
  // -> sibling들에게 killed=1 세팅 후 그들이 ZOMBIE가 될떄까지 기다린 다음 reap한다(free)

  // thread가 exit: 자기자신만 ZOMBIE가 되고, sibling은 그대로 계속 실행
  // ->leader가 나중에 kjoin으로 거두어 감

  struct proc *p = myproc();

  if (p == initproc)
    panic("init exiting");

  // Close all open files.
  for (int fd = 0; fd < NOFILE; fd++) {
    if (p->ofile[fd]) {
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }


  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // 추가: leader라면 sibling들을 drain
  if(p->group_leader == p){
      // p->pid== p->tgid도 동작은 하지만
      // group leaader== p가 조금더 맞는 득
      // 모든 sibling들에 killed = 1 표시 + SLEEPING은 깨움
      struct proc * pp;
      for(pp = proc; pp < &proc[NPROC]; pp++){
        if(pp->group_leader == p && pp != p){
          acquire(&pp->lock);
          pp->killed = 1;
          if(pp->state == SLEEPING)
            pp->state = RUNNABLE;
          release(&pp->lock);
        }
      }
  

  // sibing이 모두 ZOMBIE/UNUSED가 될 때까지 기다리며 reap
  for(;;){
    int alive = 0;
    for(pp = proc; pp< &proc[NPROC]; pp++){
      if(pp->group_leader == p && pp != p){
        acquire(&pp->lock);
        if(pp->state == ZOMBIE){
          freeproc(pp);
        } else if (pp->state != UNUSED) {
          alive = 1;
        }
        release(&pp->lock);
      }
    }
    if(!alive)
      break;
    
      //아직 살아있는 sibling들이 있다 -> 꺠어날 때까지 잠든다
    sleep(p, &wait_lock);  
  }
  }


  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  // And the group leader might be sleeping in join().
  wakeup(p->parent);
  if (p->group_leader != p)
    wakeup(p->group_leader);

  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE; 
  // p는 여기서 ZOMBIE가 되므로  
  // 부모의 kwait이 나중에 leader를 거두어 감

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process (group leader) to exit and return its pid.
// Returns -1 if this proc has no such children.
//
// A sibling thread is NOT a child here — for that, see kjoin().
int kwait(uint64 addr) { 
  //원래 내 자식인가만 봤지만 이제 thread도 proc table에 있으므로 thread를 자식으로 오해할 수 있음
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for (;;) {
    // Scan through table looking for exited children.
    havekids = 0;
    for (pp = proc; pp < &proc[NPROC]; pp++) {
      if (pp->parent != p)
        continue;

      if(pp->group_leader != pp)
        continue;
      // make sure the child isn't still in exit() or swtch().
      acquire(&pp->lock);

      havekids = 1;
      if (pp->state == ZOMBIE) {
        // Found one.
        pid = pp->pid;
        if (addr != 0 && copyout(p->mm->pagetable, addr, (char *)&pp->xstate,
                                 sizeof(pp->xstate)) < 0) {
          release(&pp->lock);
          release(&wait_lock);
          return -1;
        }
        freeproc(pp);
        release(&pp->lock);
        release(&wait_lock);
        return pid;
      }
      release(&pp->lock);
    }

    // No point waiting if we don't have any children.
    if (!havekids || killed(p)) {
      release(&wait_lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep(p, &wait_lock); // DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void scheduler(void) {
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;
  for (;;) {
    // The most recent process to run may have had interrupts
    // turned off; enable them to avoid a deadlock if all
    // processes are waiting. Then turn them back off
    // to avoid a possible race between an interrupt
    // and wfi.
    intr_on();
    intr_off();

    int found = 0;
    for (p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if (p->state == RUNNABLE) {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        p->state = RUNNING;
        c->proc = p;
        swtch(&c->context, &p->context);

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
        found = 1;
      }
      release(&p->lock);
    }
    if (found == 0) {
      // nothing to run; stop running on this core until an interrupt.
      asm volatile("wfi");
    }
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void sched(void) {
  int intena;
  struct proc *p = myproc();

  if (!holding(&p->lock))
    panic("sched p->lock");
  if (mycpu()->noff != 1)
    panic("sched locks");
  if (p->state == RUNNING)
    panic("sched RUNNING");
  if (intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void yield(void) {
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void forkret(void) {
  extern char userret[];
  static int first = 1;
  struct proc *p = myproc();

  // Still holding p->lock from scheduler.
  release(&p->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    fsinit(ROOTDEV);

    first = 0;
    // ensure other cores see first=0.
    __sync_synchronize();

    // We can invoke kexec() now that file system is initialized.
    // Put the return value (argc) of kexec into a0.
    p->trapframe->a0 = kexec("/init", (char *[]){"/init", 0});
    if (p->trapframe->a0 == -1) {
      panic("exec");
    }
  }

  // return to user space, mimicing usertrap()'s return.
  prepare_return();
  uint64 satp = MAKE_SATP(p->mm->pagetable);
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
}

// Sleep on channel chan, releasing condition lock lk.
// Re-acquires lk when awakened.
void sleep(void *chan, struct spinlock *lk) {
  struct proc *p = myproc();

  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock); // DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on channel chan.
// Caller should hold the condition lock.
void wakeup(void *chan) {
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    if (p != myproc()) {
      acquire(&p->lock);
      if (p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;
      }
      release(&p->lock);
    }
  }
}

// Kill the process containing pid.
// In the original xv6 this is one task;
// with thread groups it should be the whole group.
int kkill(int pid) {
  struct proc *p;       // 원래는 pid 하나만 저정해서 killed를 세팅
  struct proc *target = 0;  // 이제는 같은 그룹의 모든 task에 표시를 남겨야 함
  int tgid = 0;           // 만약 leader만 죽이면 thread들이 살아있는 좀비 그룹이 됨

  // pid에 해당하는 task를 찾아 tgid를 얻음
  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->pid == pid) {
      target = p;
      tgid = p->tgid;
      release(&p->lock);
      break;
    }
    release(&p->lock);
  }

  if(target == 0) //target 못 찾으면  -1 반환
    return -1;    //UNUSED 상태인 slot은 pid == 0이라 자연스럽게 매칭되지 않음

  // 같은 tgid의 모든 task에 killed = 1을 표시 + SLEEPING은 꺠움
  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->tgid == tgid){
      p->killed = 1;
      if(p->state == SLEEPING){
        p->state = RUNNABLE;
      }
    }
    release(&p->lock);
  }
  return 0;
}

void setkilled(struct proc *p) {
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int killed(struct proc *p) {
  int k;

  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int either_copyout(int user_dst, uint64 dst, void *src, uint64 len) {
  struct proc *p = myproc();
  if (user_dst) {
    return copyout(p->mm->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int either_copyin(void *dst, int user_src, uint64 src, uint64 len) {
  struct proc *p = myproc();
  if (user_src) {
    return copyin(p->mm->pagetable, dst, src, len);
  } else {
    memmove(dst, (char *)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void procdump(void) {
  static char *states[] = {
      [UNUSED] "unused",   [USED] "used",      [SLEEPING] "sleep ",
      [RUNNABLE] "runble", [RUNNING] "run   ", [ZOMBIE] "zombie"};
  struct proc *p;
  char *state;

  printf("\n");
  for (p = proc; p < &proc[NPROC]; p++) {
    if (p->state == UNUSED)
      continue;
    if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d (tgid=%d tf_va=%lx) %s %s", p->pid, p->tgid, p->tf_va, state,
           p->name);
    printf("\n");
  }
}

void
drain_siblings(struct proc *p)
{
  struct proc *pp;

  acquire(&wait_lock);

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->group_leader == p && pp != p){
      acquire(&pp->lock);
      pp->killed = 1;
      if(pp->state == SLEEPING)
        pp->state = RUNNABLE;
      release(&pp->lock);
    }
  }

  for(;;){
    int alive = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->group_leader == p && pp != p){
        acquire(&pp->lock);
        if(pp->state == ZOMBIE){
          freeproc(pp);
        } else if(pp->state != UNUSED){
          alive = 1;
        }
        release(&pp->lock);
      }
    }

    if(!alive) 
      break;
    sleep(p, &wait_lock);
  }

  release(&wait_lock);
}
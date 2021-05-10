#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
// #include "spinlock.h"
#include "semaphore.h"
#include "proc.h"
#include "defs.h"

int is_valid_sigmask(uint);
void sigkill_handler(int);
void sigstop_handler(int);
void sigcont_handler(int);
void sigign_handler(int);
void sigdfl_handler(int);

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
int nexttid = 1;
struct spinlock pid_lock;
struct spinlock tid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);
void freethread(struct proc *p, struct thread *t);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;
struct spinlock join_lock;
struct semaphore_table semaphore_t;


static handler *def_handlers[] = {
	[SIGSTOP]  sigstop_handler,
	[SIGKILL]  sigkill_handler,
	[SIG_IGN]  sigign_handler,
	[SIG_DFL]  sigkill_handler,
	[SIGCONT]  sigcont_handler
};

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl) {
	struct proc *p;
	
	for(p = proc; p < &proc[NPROC]; p++) {
		char *pa = kalloc();
		if(pa == 0)
			panic("kalloc");
		uint64 va = KSTACK((int) (p - proc));
		kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
	}
}

// initialize the proc table at boot time.
void
procinit(void)
{
	struct proc *p;
	
	initlock(&pid_lock, "nextpid");
	initlock(&tid_lock, "nexttid");
	initlock(&wait_lock, "wait_lock");
	initlock(&join_lock, "join_lock");
	initlock(&semaphore_t.lock, "semaphore_t_lock");
	int i = 0;
	for(struct semaphore *s = semaphore_t.sems; s < &semaphore_t.sems[MAX_BSEM]; s++){
		s->state = UNUSEDS;
		s->descriptor = i;
		initsleeplock(&s->sl, "sem");
		i++;
	}

	for(p = proc; p < &proc[NPROC]; p++) {
		initlock(&p->lock, "proc");
		struct thread* main_thread = &p->threads[0];
		for(struct thread* t = p->threads; t < &p->threads[NTHREAD]; t++){
			initlock(&t->lock, "thread");
		}
		main_thread->kstack = KSTACK((int) (p - proc));
	}
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
	int id = r_tp();
	return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void) {
	int id = cpuid();
	struct cpu *c = &cpus[id];
	return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void) {
	push_off();
	struct cpu *c = mycpu();
	struct proc *p = c->proc;
	pop_off();
	return p;
}

struct thread*
mythread(void){
	push_off();
	struct cpu *c = mycpu();
	struct thread *t = c->thread;
	pop_off();
	return t;
}

int
allocpid() {
	int pid;
	
	acquire(&pid_lock);
	pid = nextpid;
	nextpid = nextpid + 1;
	release(&pid_lock);

	return pid;
}

int
alloctid() {
	int tid;
	
	acquire(&tid_lock);
	tid = nexttid;
	nexttid = nexttid + 1;
	release(&tid_lock);

	return tid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
	struct proc *p;
	for(p = proc; p < &proc[NPROC]; p++) {
		acquire(&p->lock);
		p->lock.called_function = "allocproc";
		if(p->state == UNUSED) {
			goto found;
		} else {
			release(&p->lock);
		}
	}
	return 0;

found:
	p->pid = allocpid();
	p->state = USED;

	// Allocate a trapframe page.
	void* start;
	if((start = kalloc()) == 0){
		freeproc(p);
		release(&p->lock);
		return 0;
	}
	int i = 0;
	for(struct thread* t = p->threads; t < &p->threads[NTHREAD]; t++){
		t->trapframe = start + i*sizeof(struct trapframe);
		i++;
		t->state = UNUSEDT;
		t->tid = alloctid();
	}

	// An empty user page table.
	p->pagetable = proc_pagetable(p);
	if(p->pagetable == 0){
		freeproc(p);
		release(&p->lock);
		return 0;
	}

	// Set up new context to start executing at forkret,
	// which returns to user space.
	struct thread* main_thread = &p->threads[0];
	memset(&(main_thread->context), 0, sizeof(struct context));
	main_thread->context.ra = (uint64)forkret;
	main_thread->context.sp = main_thread->kstack + PGSIZE;

	// Allocate a backup trapframe page.
	if((p->trapframe_backup = (struct trapframe *)kalloc()) == 0){
		freeproc(p);
		release(&p->lock);
		return 0;
	}
	
	for (int i = 0; i < SIGNALS_COUNT; i++){
		if(i == SIG_DFL 
		|| i == SIG_IGN 
		|| i == SIGKILL 
		|| i == SIGSTOP 
		|| i == SIGCONT){
			p->signal_handlers[i] = def_handlers[i];
		}
		else{
			p->signal_handlers[i] = sigkill_handler;
		}
		p->signal_handlers_masks[i] = 0;
	}

	return p;
}
void
freethread(struct proc* p, struct thread* t)
{
	if(t->tid != p->threads[0].tid && t->kstack){
		kfree((void*)t->kstack);
	}
	memset(&(t->trapframe), 0, sizeof(struct trapframe));
	t->chan = 0;
	t->name[0] = 0;
	t->state = UNUSEDT;
	t->is_killed = 0;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
	struct thread* t = &p->threads[0];
	if(t->trapframe)
		kfree((void*)t->trapframe);
	if(p->trapframe_backup)
		kfree((void*)p->trapframe_backup);

	for(struct thread* tt = p->threads; tt < &p->threads[NTHREAD]; tt++){
		freethread(p, tt);
	}

	if(p->pagetable)
		proc_freepagetable(p->pagetable, p->sz);
	p->pagetable = 0;
	p->sz = 0;
	p->pid = 0;
	p->parent = 0;
	p->name[0] = 0;
	p->killed = 0;
	p->xstate = 0;
	p->state = UNUSED;
	p->pending_signals = 0;
	p->signal_mask = 0;
}

// Create a user page table for a given process,
// with no user memory, but with trampoline pages.
pagetable_t
proc_pagetable(struct proc *p)
{
	struct thread* t = &p->threads[0];
	pagetable_t pagetable;

	// An empty page table.
	pagetable = uvmcreate();
	if(pagetable == 0)
		return 0;

	// map the trampoline code (for system call return)
	// at the highest user virtual address.
	// only the supervisor uses it, on the way
	// to/from user space, so not PTE_U.
	if(mappages(pagetable, TRAMPOLINE, PGSIZE,
							(uint64)trampoline, PTE_R | PTE_X) < 0){
		uvmfree(pagetable, 0);
		return 0;
	}

	// map the trapframe just below TRAMPOLINE, for trampoline.S.
	if(mappages(pagetable, TRAPFRAME, PGSIZE,
							(uint64)(t->trapframe), PTE_R | PTE_W) < 0){
		uvmunmap(pagetable, TRAMPOLINE, 1, 0);
		uvmfree(pagetable, 0);
		return 0;
	}

	return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
	uvmunmap(pagetable, TRAMPOLINE, 1, 0);
	uvmunmap(pagetable, TRAPFRAME, 1, 0);
	uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// od -t xC initcode
uchar initcode[] = {
	0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
	0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
	0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
	0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
	0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
	0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
void
userinit(void)
{
	struct proc *p;
	p = allocproc();
	initproc = p;
	struct thread* t = &p->threads[0];
	// allocate one user page and copy init's instructions
	// and data into it.
	uvminit(p->pagetable, initcode, sizeof(initcode));
	p->sz = PGSIZE;

	// prepare for the very first "return" from kernel to user.
	t->trapframe->epc = 0;      // user program counter
	t->trapframe->sp = PGSIZE;  // user stack pointer

	safestrcpy(p->name, "initcode", sizeof(p->name));
	p->cwd = namei("/");

	p->state = USED;
	t->state = RUNNABLE;

	release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
	uint sz;
	struct proc *p = myproc();
	acquire(&p->lock);
	p->lock.called_function = "growproc";
	sz = p->sz;
	if(n > 0){
		if((sz = uvmalloc(p->pagetable, sz, sz + n)) == 0) {
            release(&p->lock); //TODO: check why this worked before adding threads
			return -1;
		}
	} else if(n < 0){
		sz = uvmdealloc(p->pagetable, sz, sz + n);
	}
	p->sz = sz;
	release(&p->lock);
	return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
	int i, pid;
	struct proc *np;
	struct proc *p = myproc();
	struct thread* t = mythread();
	// Allocate process.
	if((np = allocproc()) == 0){
		return -1;
	}
	struct thread* t_np = &np->threads[0];
	// Copy user memory from parent to child.
	if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
		freeproc(np);
		release(&np->lock);
		return -1;
	}
	np->sz = p->sz;

	// copy saved user registers.
	*(t_np->trapframe) = *(t->trapframe);

	// Cause fork to return 0 in the child.
	t_np->trapframe->a0 = 0;

	// increment reference counts on open file descriptors.
	for(i = 0; i < NOFILE; i++)
		if(p->ofile[i])
			np->ofile[i] = filedup(p->ofile[i]);
	np->cwd = idup(p->cwd);

	safestrcpy(np->name, p->name, sizeof(p->name));

	pid = np->pid;
	np->signal_mask = p->signal_mask;
	for (int i = 0; i < SIGNALS_COUNT; i++){
		np->signal_handlers[i] = p->signal_handlers[i];
	}

	release(&np->lock);

	acquire(&wait_lock);
	np->parent = p;
	release(&wait_lock);

	acquire(&np->lock);
	np->state = RUNNABLE;
	t_np->state = RUNNABLE;
	release(&np->lock);
	return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
	struct proc *pp;

	for(pp = proc; pp < &proc[NPROC]; pp++){
		if(pp->parent == p){
			pp->parent = initproc;
			wakeup(initproc);
		}
	}
}

uint 
sigprocmask(uint sigprocmask)
{
	struct proc *p = myproc();
	uint old_sigprocmask;
	acquire(&p->lock);
	p->lock.called_function = "sigprocmask";
	old_sigprocmask = p->signal_mask;
	p->signal_mask = sigprocmask;
	release(&p->lock);
	return old_sigprocmask;
}

//int signum, const struct sigaction *act, struct sigaction *oldact

int
sigaction(int signum, uint64 act_addr, uint64 old_act_addr)
{	
	if(signum > SIGNALS_COUNT || signum < 0){
		return -1;
	}
	struct proc *p = myproc();
	struct sigaction old_act;
	struct sigaction new_act;
	acquire(&p->lock);
	p->lock.called_function = "sigaction";
	old_act.sa_handler = p->signal_handlers[signum];
	old_act.sigmask = p->signal_handlers_masks[signum];
	if(old_act_addr != 0){
		if(copyout(p->pagetable, old_act_addr, (char *)&old_act, sizeof(old_act)) < 0){
			release(&p->lock);
			return -1;
		}
	}
	if(act_addr != 0){
		if(signum == SIGKILL || signum == SIGSTOP){
			release(&p->lock);
			return -1;
		}
		if(copyin(p->pagetable, (char *)&new_act, act_addr, sizeof(new_act)) < 0){
			release(&p->lock);
			return -1;
		}
	}
	else{
		return -1;
	}
	if(is_valid_sigmask(new_act.sigmask) < 0){
		release(&p->lock);
		return -1;
	}
	int sa_handler_num = (uint64)new_act.sa_handler;
	if(sa_handler_num == SIG_DFL
	|| sa_handler_num == SIGSTOP
	|| sa_handler_num == SIG_IGN
	|| sa_handler_num == SIGCONT
	|| sa_handler_num == SIGKILL)
	{
		p->signal_handlers[signum] = def_handlers[sa_handler_num];
	}
	else{
		p->signal_handlers[signum] = new_act.sa_handler;
	}
	p->signal_handlers_masks[signum] = new_act.sigmask;
	release(&p->lock);
	return 0;
}

void
sigret(){
	
	struct proc* p = myproc();
	struct thread* t = mythread();
	acquire(&p->lock);
	p->lock.called_function = "sigret";
	*(t->trapframe) = *(p->trapframe_backup);
	p->signal_mask = p->signal_mask_backup;
	p->is_handling_signal = 0;
	release(&p->lock);
}

int 
is_valid_sigmask(uint sigmask){
	int sigkill = 1 << SIGKILL;
	int sigstop = 1 << SIGSTOP;
	if(sigmask < 0 || sigkill & sigmask || sigstop & sigmask){
		return -1;
	}
	return 0;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
	struct proc *p = myproc();
	struct thread *my_t = mythread();

	if(p == initproc)
		panic("init exiting");

	wakeup(my_t);
	acquire(&p->lock);
	if(my_t->is_killed || my_t->state == ZOMBIET || my_t->state == UNUSEDT){
		sched();
	}
	for(struct thread* t = p->threads; t < &p->threads[NTHREAD]; t++){
		if(t->tid != my_t->tid){
			t->is_killed = 1;
		}
	}
	release(&p->lock);
	int found;
	for(;;){
		found = 0;
		acquire(&p->lock);
		for(struct thread* t = p->threads; t < &p->threads[NTHREAD]; t++){
			if(t->tid != my_t->tid && t->state != ZOMBIET && t->state != UNUSEDT){
				found = 1;
			}
		}
		release(&p->lock);
		if(found){
			yield();
		}
		else{
			break;
		}
	}

	// Close all open files.
	for(int fd = 0; fd < NOFILE; fd++){
		if(p->ofile[fd]){
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

	// Give any children to init.
	reparent(p);

	// Parent might be sleeping in wait().
	wakeup(p->parent);

	acquire(&p->lock);
	p->lock.called_function = "exit";
	my_t->state = UNUSEDT;
	p->xstate = status;
	p->state = ZOMBIE;
	release(&wait_lock);

	// Jump into the scheduler, never to return.
	sched();
	if(p->state == ZOMBIE){
		panic("zombie exit");
	}
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
	struct proc *np;
	int havekids, pid;
	struct proc *p = myproc();

	acquire(&wait_lock);

	for(;;){
		// Scan through table looking for exited children.
		havekids = 0;
		for(np = proc; np < &proc[NPROC]; np++){
			if(np->parent == p){
				// make sure the child isn't still in exit() or swtch().
				acquire(&np->lock);
				np->lock.called_function = "wait";
				havekids = 1;
				if(np->state == ZOMBIE){
					// Found one.
					pid = np->pid;
					if(addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
																	sizeof(np->xstate)) < 0) {
						release(&np->lock);
						release(&wait_lock);
						return -1;
					}
					freeproc(np);
					release(&np->lock);
					release(&wait_lock);
					return pid;
				}
				release(&np->lock);
			}
		}

		// No point waiting if we don't have any children.
		if(!havekids || p->killed){
			release(&wait_lock);
			return -1;
		}
		
		// Wait for a child to exit.
		sleep(p, &wait_lock);  //DOC: wait-sleep
	}
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
	struct proc *p;
	struct cpu *c = mycpu();
	
	c->proc = 0;
	c->thread = 0;
	for(;;){
		// Avoid deadlock by ensuring that devices can interrupt.
		intr_on();

		for(p = proc; p < &proc[NPROC]; p++) {
			acquire(&p->lock);
			p->lock.called_function = "scheduler";
			int is_released = 0;
			if(p->state == USED) {
				// Switch to chosen process.  It is the process's job
				// to release its lock and then reacquire it
				// before jumping back to us.
				for(struct thread* t = p->threads; t < &p->threads[NTHREAD]; t++){
					if(t->state == RUNNABLE){
						if(p->is_stopped){
							int is_blocked = (1 << SIGCONT & p->signal_mask);
							int is_set = (1 << SIGCONT & p->pending_signals);
							if(is_blocked || !is_set){
								release(&p->lock);
								is_released = 1;
								break;
							}
						}
						//TODO: need to know why thread does not release process lock after swtch
						t->state = RUNNING;
						c->proc = p;
						c->thread = t;
						swtch(&c->context, &t->context);
						c->thread = 0;
						c->proc = 0;
					}
				}

				// Process is done running for now.
				// It should have changed its p->state before coming back.

			}
			if(!is_released){
				release(&p->lock);
			}
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
void
sched(void)
{
	int intena;
	struct proc *p = myproc();
	struct thread* t = mythread();
	if(!holding(&p->lock))
		panic("sched p->lock");
	if(mycpu()->noff != 1)
		panic("sched locks");
	if(t->state == RUNNING)
		panic("sched running");
	if(intr_get())
		panic("sched interruptible");

	intena = mycpu()->intena;
	swtch(&t->context, &mycpu()->context);
	mycpu()->intena = intena;

}

// Give up the CPU for one scheduling round.
void
yield(void)
{
	struct proc *p = myproc();
	struct thread* t = mythread();
	acquire(&p->lock);
	t->state = RUNNABLE;
	sched();
	release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
	static int first = 1;

	// Still holding p->lock from scheduler.
	release(&myproc()->lock);

	if (first) {
		// File system initialization must be run in the context of a
		// regular process (e.g., because it calls sleep), and thus cannot
		// be run from main().
		first = 0;
		fsinit(ROOTDEV);
	}

	usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
	struct proc *p = myproc();
	struct thread* t = mythread();
	// Must acquire p->lock in order to
	// change p->state and then call sched.
	// Once we hold p->lock, we can be
	// guaranteed that we won't miss any wakeup
	// (wakeup locks p->lock),
	// so it's okay to release lk.

	acquire(&p->lock);  //DOC: sleeplock1
	release(lk);

	// Go to sleep.
	t->chan = chan;
	t->state = SLEEPING;
	sched();

	// Tidy up. -_- :^)
	t->chan = 0;

	// Reacquire original lock.
	release(&p->lock);
	acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void *chan)
{
	struct proc *p;

	for(p = proc; p < &proc[NPROC]; p++) {
		acquire(&p->lock);
		for(struct thread* t = p->threads; t < &p->threads[NTHREAD]; t++){
			if(t != mythread()){
				if(t->state == SLEEPING && t->chan == chan) {
					t->state = RUNNABLE;
				}
			}
		}
		release(&p->lock);
	}
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid, int signum)
{
	if (signum < 0 || signum > SIGNALS_COUNT){
		return -1;
	}
	struct proc *p;
	for(p = proc; p < &proc[NPROC]; p++){
		acquire(&p->lock);
		if(p->pid == pid){
			p->pending_signals = 1 << signum | p -> pending_signals;
			release(&p->lock);
			return 0;
		}
		release(&p->lock);
	}
	return -1;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
	struct proc *p = myproc();
	if(user_dst){
		return copyout(p->pagetable, dst, src, len);
	} else {
		memmove((char *)dst, src, len);
		return 0;
	}
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
	struct proc *p = myproc();
	if(user_src){
		return copyin(p->pagetable, dst, src, len);
	} else {
		memmove(dst, (char*)src, len);
		return 0;
	}
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
	static char *states[] = {
	[UNUSED]    "unused",
	[SLEEPING]  "sleep ",
	[RUNNABLE]  "runble",
	[RUNNING]   "run   ",
	[ZOMBIE]    "zombie"
	};
	struct proc *p;
	char *state;

	printf("\n");
	for(p = proc; p < &proc[NPROC]; p++){
		if(p->state == UNUSED)
			continue;
		if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
			state = states[p->state];
		else
			state = "???";
		printf("%d %s %s", p->pid, state, p->name);
		printf("\n");
	}
}

void
sigkill_handler(int signum)
{
	struct proc* p = myproc();
	p->killed = 1;
	for(struct thread* t = p->threads; t < &p->threads[NTHREAD]; t++){
		if(t->state == SLEEPING) {
			t->state = RUNNABLE;
			return;
		}
	}
}

void
sigstop_handler(int signum)
{
	struct proc* p = myproc();
	p->is_stopped = 1;
}

void
sigcont_handler(int signum)
{
	struct proc* p = myproc();
	p->is_stopped = 0;
}

void
sigign_handler(int signum)
{
}

int allocthread(void* start_func , void* stack)
{
	struct proc* p = myproc();
	struct thread* my_t = mythread();
	struct thread* new_thread = 0;

	
	int found = 0;
	for(struct thread* t = p->threads; t < &p->threads[NTHREAD]; t++){
		if(t->state == ZOMBIET){
			t->state = UNUSEDT;
		}
		if (t->state == UNUSEDT && !found){
			new_thread = t;
			found = 1;
		}
	}
	if(new_thread == 0){
		return -1;
	}
	new_thread->kstack = (uint64)kalloc();
	memset(&(new_thread->context), 0, sizeof(struct context));
	new_thread->context.ra = (uint64)forkret;
	new_thread->context.sp = new_thread->kstack + PGSIZE;
	new_thread->state = RUNNABLE;
	*(new_thread->trapframe) = *(my_t->trapframe);
	new_thread->trapframe->sp = (uint64)(stack + MAX_STACK_SIZE - 16);
	// int kthread_exit_size = threadretend - threadret;
	// new_thread->trapframe->sp -= kthread_exit_size;
	// if(copyout(p->pagetable, new_thread->trapframe->sp, (char *)threadret, kthread_exit_size) < 0){
	// 	return -1;
	// }
	// new_thread->trapframe->ra = new_thread->trapframe->sp;
	new_thread->is_killed = 0;
	new_thread->trapframe->epc = (uint64)start_func;
	
	
	return new_thread->tid;
}

int
kthread_create(uint64 start_func, uint64 stack){
	struct proc* p = myproc();
	acquire(&p->lock);
	int result = allocthread((void*)start_func, (void*)stack);
	release(&p->lock);
	return result;
}

int
kthread_id(){
	struct thread* t = mythread();
	return t->tid;
}

void
kthread_exit(int status){
	struct proc* p = myproc();
	struct thread* my_t = mythread();
	int found = 0;
	acquire(&p->lock);
	for(struct thread* t = p->threads; t < &p->threads[NTHREAD]; t++){
		if(t->tid != my_t->tid && t->state != ZOMBIET && t->state != UNUSEDT){
			found = 1;
			break;
		}
	}
	my_t->xstate = status;
	release(&p->lock);
	wakeup(my_t);
	if(!found){
		exit(status);
	}
	else{
		acquire(&p->lock);
		my_t->state = ZOMBIET;
		sched();
	}
}

int
kthread_join(int thread_id, uint64 status){
	struct proc* p = myproc();
	if (thread_id == mythread()->tid){
		return -1;
	}
	struct thread* t;
	for(t = p->threads; t < &p->threads[NTHREAD]; t++){
		if(t->tid == thread_id){
			break;
		}
	}
	if(t->state != ZOMBIET || t->state != UNUSEDT){
		acquire(&join_lock);
		sleep(t, &join_lock);
		release(&join_lock);
	}

	if(t->state == ZOMBIET){
		freethread(p, t);
	}

	if(copyout(p->pagetable, status, (char *)&t->xstate, sizeof(int)) < 0){
		return -1;
	}

	return 0;
}

int is_in_range_desc(int descriptor){
	if (descriptor < 0 || descriptor >= MAX_BSEM){
		return 0;
	}
	return 1;
}

int bsem_alloc()
{
	acquire(&semaphore_t.lock);
	for(struct semaphore *s = semaphore_t.sems; s < &semaphore_t.sems[MAX_BSEM]; s++){
		if (s->state == UNUSEDS){
			s->state = USEDS;
			release(&semaphore_t.lock);
			return s->descriptor;
		}
	}
	release(&semaphore_t.lock);
	return -1;
}
void bsem_free(int descriptor)
{
	if(!is_in_range_desc(descriptor)){
		return;
	}
	struct semaphore *s = &semaphore_t.sems[descriptor];
	acquire(&semaphore_t.lock);
	s->state = UNUSEDS;
	release(&semaphore_t.lock);
}
void bsem_down(int descriptor)
{
	if(!is_in_range_desc(descriptor)){
		return;
	}
	struct semaphore *s = &semaphore_t.sems[descriptor];
	acquire(&semaphore_t.lock);
	if (s->state != USEDS){
		release(&semaphore_t.lock);
		return;
	}
	release(&semaphore_t.lock);
	acquiresleep(&s->sl);
}
void bsem_up(int descriptor)
{
	if(!is_in_range_desc(descriptor)){
		return;
	}
	struct semaphore *s = &semaphore_t.sems[descriptor];
	acquire(&semaphore_t.lock);
	if (s->state != USEDS){
		release(&semaphore_t.lock);
		return;
	}
	release(&semaphore_t.lock);
	releasesleep(&s->sl);
}
/*
 * Copyright (c) 2013
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
 * Process support.
 *
 * There is (intentionally) not much here; you will need to add stuff
 * and maybe change around what's already present.
 *
 * p_lock is intended to be held when manipulating the pointers in the
 * proc structure, not while doing any significant work with the
 * things they point to. Rearrange this (and/or change it to be a
 * regular lock) as needed.
 *
 * Unless you're implementing multithreaded user processes, the only
 * process that will have more than one thread is the kernel process.
 */

#include <types.h>
#include <spl.h>
#include <proc.h>
#include <current.h>
#include <addrspace.h>
#include <vnode.h>
#include <syscall.h>
#include <kern/unistd.h>
#include <vfs.h>
#include <synch.h>

#if OPT_SHELL
#define MAX_PROC 100
static struct _processTable {
  // int active;           /* initial value 0 */
  struct proc *proc[MAX_PROC+1]; /* [0] not used. pids are >= 1 */
  int last_i;           /* index of last allocated pid */
  struct spinlock lk;	/* Lock for this table */
  bool is_full;
} processTable;

struct lock *ft_copy_lock;
#endif

/*
 * The process for the kernel; this holds all the kernel-only threads.
 */
struct proc *kproc;

#if OPT_SHELL
/* Return the value of processTable.is_full */
bool
is_proc_table_full(void){
	bool tmp;
	spinlock_acquire(&processTable.lk);
	tmp = processTable.is_full;
	spinlock_release(&processTable.lk);
	return tmp;
}

/* Remove the link to the parent (if it exits) from children processes */
void proc_rm_parent_link(pid_t pid) {
	int i;
	struct proc *p;

	KASSERT(!(pid < PID_MIN || pid > MAX_PROC));

	spinlock_acquire(&processTable.lk);
	for (i = 1; i <= MAX_PROC; i++) {
		/* 	If the pointer to the parent process of the current element
			has the same pid of the parameter, it must be a children */
		p = processTable.proc[i];
		if (p != NULL && p->parent_proc != NULL && p->parent_proc->p_pid == pid)
			p->parent_proc = NULL;
	}
	spinlock_release(&processTable.lk);
}
/*
 * G.Cabodi - 2019
 * Initialize support for pid/waitpid.
 */
struct proc *
proc_search_pid(pid_t pid) {
  struct proc *p;
  
  // Check if the pid argument is valid (pid 0 is not used)
  // The upper bound is MAX_PROC > PID_MAX
  if (pid < PID_MIN || pid > MAX_PROC)
	return NULL;

  // KASSERT(pid>=0 && pid<MAX_PROC);

  spinlock_acquire(&processTable.lk);
  p = processTable.proc[pid];
  spinlock_release(&processTable.lk);
  if(p!=NULL)
  	KASSERT(p->p_pid==pid);

  return p;
}

/*
 * G.Cabodi - 2019
 * Initialize support for pid/waitpid.
 */
static void
proc_init_waitpid(struct proc *proc, const char *name) {
  /* search a free index in table using a circular strategy */
  int i;
  spinlock_acquire(&processTable.lk);
  i = processTable.last_i+1;
  proc->p_pid = 0;
  if (i>MAX_PROC) i=1;
  while (i!=processTable.last_i) {
    if (processTable.proc[i] == NULL) {
      processTable.proc[i] = proc;
      processTable.last_i = i;
      proc->p_pid = i;
      break;
    }
    i++;
    if (i>MAX_PROC) i=1;
  }
  if (proc->p_pid==0) {
    // panic("too many processes. proc table is full\n");
	processTable.is_full = true;
	spinlock_release(&processTable.lk);
	return;
  }
  spinlock_release(&processTable.lk);
  proc->p_status = 0;
#if USE_SEMAPHORE_FOR_WAITPID
  proc->p_sem = sem_create(name, 0);
#else
  proc->p_cv = cv_create(name);
  proc->p_cv_lock = lock_create(name);
#endif
}

/*
 * G.Cabodi - 2019
 * Terminate support for pid/waitpid.
 */
static void
proc_end_waitpid(struct proc *proc) {
  /* remove the process from the table */
  int i;
  spinlock_acquire(&processTable.lk);
  i = proc->p_pid;
  KASSERT(i>0 && i<=MAX_PROC);
  processTable.proc[i] = NULL;
  processTable.is_full = false;
  spinlock_release(&processTable.lk);

#if USE_SEMAPHORE_FOR_WAITPID
  sem_destroy(proc->p_sem);
#else
  cv_destroy(proc->p_cv);
  lock_destroy(proc->p_cv_lock);
#endif
}
#endif
/*
 * Create a proc structure.
 */
static
struct proc *
proc_create(const char *name)
{
	struct proc *proc;

	proc = kmalloc(sizeof(*proc));
	if (proc == NULL) {
		return NULL;
	}
	proc->p_name = kstrdup(name);
	if (proc->p_name == NULL) {
		kfree(proc);
		return NULL;
	}

	proc->p_numthreads = 0;
	spinlock_init(&proc->p_lock);

	/* VM fields */
	proc->p_addrspace = NULL;
	
	/* VFS fields */
	proc->p_cwd = NULL;

#if OPT_SHELL
	// New fields
	proc->p_exited = 0;
	proc->parent_proc = NULL;
	proc->ft_lock = lock_create(proc->p_name);

	proc_init_waitpid(proc,name);
	if(is_proc_table_full()){
		kfree(proc->p_name);
		spinlock_cleanup(&proc->p_lock);
		lock_destroy(proc->ft_lock);
		kfree(proc);
		return NULL;
	}
    bzero(proc->fileTable,OPEN_MAX*sizeof(struct openfile *));	
#endif

	return proc;
}

/*
 * Destroy a proc structure.
 *
 * Note: nothing currently calls this. Your wait/exit code will
 * probably want to do so.
 */
void
proc_destroy(struct proc *proc)
{
	/*
	 * You probably want to destroy and null out much of the
	 * process (particularly the address space) at exit time if
	 * your wait/exit design calls for the process structure to
	 * hang around beyond process exit. Some wait/exit designs
	 * do, some don't.
	 */

	KASSERT(proc != NULL);
	KASSERT(proc != kproc);

	/*
	 * We don't take p_lock in here because we must have the only
	 * reference to this structure. (Otherwise it would be
	 * incorrect to destroy it.)
	 */

	/* VFS fields */
	if (proc->p_cwd) {
		VOP_DECREF(proc->p_cwd);
		proc->p_cwd = NULL;
	}

	/* VM fields */
	if (proc->p_addrspace) {
		/*
		 * If p is the current process, remove it safely from
		 * p_addrspace before destroying it. This makes sure
		 * we don't try to activate the address space while
		 * it's being destroyed.
		 *
		 * Also explicitly deactivate, because setting the
		 * address space to NULL won't necessarily do that.
		 *
		 * (When the address space is NULL, it means the
		 * process is kernel-only; in that case it is normally
		 * ok if the MMU and MMU- related data structures
		 * still refer to the address space of the last
		 * process that had one. Then you save work if that
		 * process is the next one to run, which isn't
		 * uncommon. However, here we're going to destroy the
		 * address space, so we need to make sure that nothing
		 * in the VM system still refers to it.)
		 *
		 * The call to as_deactivate() must come after we
		 * clear the address space, or a timer interrupt might
		 * reactivate the old address space again behind our
		 * back.
		 *
		 * If p is not the current process, still remove it
		 * from p_addrspace before destroying it as a
		 * precaution. Note that if p is not the current
		 * process, in order to be here p must either have
		 * never run (e.g. cleaning up after fork failed) or
		 * have finished running and exited. It is quite
		 * incorrect to destroy the proc structure of some
		 * random other process while it's still running...
		 */
		struct addrspace *as;

		if (proc == curproc) {
			as = proc_setas(NULL);
			as_deactivate();
		}
		else {
			as = proc->p_addrspace;
			proc->p_addrspace = NULL;
		}
		as_destroy(as);
	}

	KASSERT(proc->p_numthreads == 0);
	spinlock_cleanup(&proc->p_lock);

	#if OPT_SHELL
	proc_end_waitpid(proc);
	for (int fd=0; fd<OPEN_MAX; fd++) {
		struct openfile *of = proc->fileTable[fd];
		if (of != NULL) {
			lock_acquire(of->of_lock);
			if (--of->countRef == 0){
				if (of->vn != NULL){
					vfs_close(of->vn);
				}
				lock_release(of->of_lock);
				lock_destroy(of->of_lock);
			}
			else{
				lock_release(of->of_lock);
			}
		}
		
		proc->fileTable[fd] = NULL;
	}
	
	lock_destroy(proc->ft_lock);
	#endif

	kfree(proc->p_name);
	kfree(proc);
}

/*
 * Create the process structure for the kernel.
 */
void
proc_bootstrap(void)
{
	kproc = proc_create("[kernel]");
	if (kproc == NULL) {
		panic("proc_create for kproc failed\n");
	}
#if OPT_SHELL
	spinlock_init(&processTable.lk);
	processTable.is_full = false;
	/* kernel process is not registered in the table */
	// processTable.active = 1;
	sft_init();
	ft_copy_lock = lock_create("File Table Copy");
#endif
}


/*
 * Create a fresh proc for use by runprogram.
 *
 * It will have no address space and will inherit the current
 * process's (that is, the kernel menu's) current directory.
 */
struct proc *
proc_create_runprogram(const char *name)
{
	struct proc *newproc;

	newproc = proc_create(name);
	if (newproc == NULL) {
		return NULL;
	}
	/* VM fields */

	newproc->p_addrspace = NULL;
		
	/* VFS fields */

	/*
	 * Lock the current process to copy its current directory.
	 * (We don't need to lock the new process, though, as we have
	 * the only reference to it.)
	 */
	spinlock_acquire(&curproc->p_lock);
	if (curproc->p_cwd != NULL) {
		VOP_INCREF(curproc->p_cwd);
		newproc->p_cwd = curproc->p_cwd;
	}
	spinlock_release(&curproc->p_lock);

	return newproc;
}

/*
 * Add a thread to a process. Either the thread or the process might
 * or might not be current.
 *
 * Turn off interrupts on the local cpu while changing t_proc, in
 * case it's current, to protect against the as_activate call in
 * the timer interrupt context switch, and any other implicit uses
 * of "curproc".
 */
int
proc_addthread(struct proc *proc, struct thread *t)
{
	int spl;

	KASSERT(t->t_proc == NULL);

	spinlock_acquire(&proc->p_lock);
	proc->p_numthreads++;
	spinlock_release(&proc->p_lock);

	spl = splhigh();
	t->t_proc = proc;
	splx(spl);

	return 0;
}

/*
 * Remove a thread from its process. Either the thread or the process
 * might or might not be current.
 *
 * Turn off interrupts on the local cpu while changing t_proc, in
 * case it's current, to protect against the as_activate call in
 * the timer interrupt context switch, and any other implicit uses
 * of "curproc".
 */
void
proc_remthread(struct thread *t)
{
	struct proc *proc;
	int spl;

	proc = t->t_proc;
	KASSERT(proc != NULL);

	spinlock_acquire(&proc->p_lock);
	KASSERT(proc->p_numthreads > 0);
	proc->p_numthreads--;
	spinlock_release(&proc->p_lock);

	spl = splhigh();
	t->t_proc = NULL;
	splx(spl);
}

/*
 * Fetch the address space of (the current) process.
 *
 * Caution: address spaces aren't refcounted. If you implement
 * multithreaded processes, make sure to set up a refcount scheme or
 * some other method to make this safe. Otherwise the returned address
 * space might disappear under you.
 */
struct addrspace *
proc_getas(void)
{
	struct addrspace *as;
	struct proc *proc = curproc;

	if (proc == NULL) {
		return NULL;
	}

	spinlock_acquire(&proc->p_lock);
	as = proc->p_addrspace;
	spinlock_release(&proc->p_lock);
	return as;
}

/*
 * Change the address space of (the current) process. Return the old
 * one for later restoration or disposal.
 */
struct addrspace *
proc_setas(struct addrspace *newas)
{
	struct addrspace *oldas;
	struct proc *proc = curproc;

	KASSERT(proc != NULL);

	spinlock_acquire(&proc->p_lock);
	oldas = proc->p_addrspace;
	proc->p_addrspace = newas;
	spinlock_release(&proc->p_lock);
	return oldas;
}

#if OPT_SHELL
    /* G.Cabodi - 2019 - support for waitpid */
int 
proc_wait(struct proc *proc)
{
    int return_status;
    /* NULL and kernel proc forbidden */
	KASSERT(proc != NULL);
	KASSERT(proc != kproc);

    /* wait on semaphore or condition variable */ 
#if USE_SEMAPHORE_FOR_WAITPID
    P(proc->p_sem);
#else
    lock_acquire(proc->p_cv_lock);
    cv_wait(proc->p_cv);
    lock_release(proc->p_cv_lock);
#endif
    return_status = proc->p_status;
    proc_destroy(proc);
    return return_status;
}


/* G.Cabodi - 2019 - support for waitpid */
void
proc_signal_end(struct proc *proc)
{
#if USE_SEMAPHORE_FOR_WAITPID
      V(proc->p_sem);
#else
      lock_acquire(proc->p_cv_lock);
      cv_signal(proc->p_cv);
      lock_release(proc->p_cv_lock);
#endif
}

void 
proc_file_table_copy(struct proc *psrc, struct proc *pdest) {
  int fd;
  lock_acquire(ft_copy_lock);
  lock_acquire(psrc->ft_lock);
  lock_acquire(pdest->ft_lock);
  for (fd=0; fd<OPEN_MAX; fd++) {
    struct openfile *of = psrc->fileTable[fd];
    pdest->fileTable[fd] = of;
    if (of != NULL) {
      /* incr reference count */
	  VOP_INCREF(of->vn);
      openfileIncrRefCount(of);
    }
  }
  lock_release(pdest->ft_lock);
  lock_release(psrc->ft_lock);
  lock_release(ft_copy_lock);
}

#endif

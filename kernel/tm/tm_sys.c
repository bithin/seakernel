#include <kernel.h>
#include <memory.h>
#include <task.h>
#include <tqueue.h>

/* Low-level memory allocator implementation */
int sys_sbrk(long inc)
{
	assert(current_task);
	if(inc < 0 && current_task->heap_start < current_task->heap_end) {
		int dec = -inc;
		addr_t new_end = current_task->heap_end - dec;
		if(new_end < current_task->heap_start)
			new_end = current_task->heap_start;
		addr_t old_end = current_task->heap_end;
		addr_t free_start = (new_end&PAGE_MASK) + PAGE_SIZE;
		addr_t free_end = old_end&PAGE_MASK;
		while(free_start <= free_end) {
			if(vm_getmap(free_start, 0))
				vm_unmap(free_start);
			free_start += PAGE_SIZE;
		}
		current_task->heap_end = new_end;
		assert(new_end + dec == old_end);
		return old_end;
	}
	if(!inc)
		return current_task->heap_end;
	addr_t end = current_task->heap_end;
	assert(end);
	if(end + inc >= TOP_TASK_MEM)
		send_signal(current_task->pid, SIGSEGV);
	current_task->heap_end += inc;
	current_task->he_red = end + inc;
	addr_t page = end & PAGE_MASK;
	for(;page <=(current_task->heap_end&PAGE_MASK);page += PAGE_SIZE)
		user_map_if_not_mapped(page);
	return end;
}

int sys_isstate(int pid, int state)
{
	task_t *task = get_task_pid(pid);
	if(!task) return -ESRCH;
	return (task->state == state) ? 1 : 0;
}

int sys_gsetpriority(int set, int which, int id, int val)
{
	if(set)
		return -ENOSYS;
	return current_task->priority;
}

void __sys_nice_search_action(task_t *t, int val)
{
	t->priority = val;
}

int sys_nice(int which, int who, int val, int flags)
{
	if(!flags || which == PRIO_PROCESS)
	{
		if(who && (unsigned)who != current_task->pid)
			return -ENOTSUP;
		if(!flags && val < 0 && current_task->thread->uid != 0)
			return -EPERM;
		/* Yes, this is correct */
		if(!flags)
			current_task->priority += -val;
		else
			current_task->priority = (-val)+1;
		return 0;
	}
	val=-val;
	val++; /* our default is 1, POSIX default is 0 */
	task_t *t = (task_t *)kernel_task;
	int c=0;
	if(which == PRIO_USER)
		search_tqueue(primary_queue, TSEARCH_UID | TSEARCH_EUID | TSEARCH_FINDALL | TSEARCH_EXCLUSIVE, who, __sys_nice_search_action, val, &c);
	return c ? 0 : -ESRCH;
}

int sys_setsid(int ex, int cmd)
{
	if(cmd) {
		return -ENOTSUP;
	}
	current_task->tty=0;
	return 0;
}

int sys_setpgid(int a, int b)
{
	return -ENOSYS;
}

int get_pid()
{
	return current_task->pid;
}

int sys_getppid()
{
	return current_task->parent->pid;
}

int set_gid(int n)
{
	current_task->thread->_gid = current_task->thread->gid;
	current_task->thread->gid = n;
	return 0;
}

int set_uid(int n)
{
	current_task->thread->_uid = current_task->thread->uid;
	current_task->thread->uid = n;
	return 0;
}

int get_gid()
{
	return current_task->thread->gid;
}

int get_uid()
{
	return current_task->thread->uid;
}

void do_task_stat(struct task_stat *s, task_t *t)
{
	assert(s && t);
	s->stime = t->stime;
	s->utime = t->utime;
	s->state = t->state;
	if(s->state != TASK_DEAD) {
		s->uid = t->thread->uid;
		s->gid = t->thread->gid;
	}
	if(t->parent) s->ppid = t->parent->pid;
	s->system = t->system;
	s->tty = t->tty;
	s->argv = t->argv;
	s->pid = t->pid;
	strncpy(s->cmd, (char *)t->command, 128);
	s->mem_usage = t->pid ? t->phys_mem_usage * 4 : 0;
}

int task_pstat(unsigned int pid, struct task_stat *s)
{
	if(!s) return -EINVAL;
	task_t *t=get_task_pid(pid);
	if(!t)
		return -ESRCH;
	do_task_stat(s, t);
	return 0;
}

int task_stat(unsigned int num, struct task_stat *s)
{
	if(!s) return -EINVAL;
	task_t *t = search_tqueue(primary_queue, TSEARCH_ENUM, num, 0, 0, 0);
	if(!t) 
		return -ESRCH;
	do_task_stat(s, t);
	return 0;
}

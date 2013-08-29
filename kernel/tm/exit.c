/* Functions for exiting processes, killing processes, and cleaning up resources.
* Copyright (c) 2012 Daniel Bittman
*/
#include <kernel.h>
#include <memory.h>
#include <task.h>
#include <cpu.h>
#include <atomic.h>

extern struct llist *kill_queue;
extern unsigned running_processes;
void clear_resources(task_t *t)
{
	clear_mmfiles(t, (t->flags&TF_EXITING) ? 1 : 0);
}

void set_as_dead(task_t *t)
{
	assert(t);
	sub_atomic(&running_processes, 1);
	tqueue_remove(((cpu_t *)t->cpu)->active_queue, t->activenode);
	kfree(t->activenode);
	kfree(t->blocknode);
	sub_atomic(&(((cpu_t *)t->cpu)->numtasks), 1);
	t->state = TASK_DEAD;
}

void move_task_to_kill_queue(task_t *t, int locked)
{
	if(locked)
		tqueue_remove_nolock(primary_queue, t->listnode);
	else
		tqueue_remove(primary_queue, t->listnode);
	raise_task_flag(t, TF_KILLREADY);
}

int __KT_try_releasing_tasks()
{
	struct llistnode *cur;
	rwlock_acquire(&kill_queue->rwl, RWL_READER);
	if(ll_is_empty(kill_queue))
	{
		rwlock_release(&kill_queue->rwl, RWL_READER);
		return 0;
	}
	task_t *t=0;
	ll_for_each_entry(kill_queue, cur, task_t *, t)
	{
		if(t->flags & TF_BURIED) {
			if(t->parent == 0 || t->parent->state == TASK_DEAD || (t->parent->flags & TF_KTASK))
				move_task_to_kill_queue(t, 0);
			if(t->flags & TF_KILLREADY)
				break;
		}
	}
	rwlock_release(&kill_queue->rwl, RWL_READER);
	if(!t) return 0;
	if(!((t->flags & TF_BURIED) && (t->flags & TF_KILLREADY))) return 0;
	assert(t != current_task);
	assert(cur->entry == t);
	release_task(t);
	ll_remove(kill_queue, cur);
	return ll_is_empty(kill_queue) ? 0 : 1;
}

void release_task(task_t *p)
{
	destroy_task_page_directory(p);
	kfree(p->listnode);
	kfree((void *)p->kernel_stack);
	kfree((void *)p);
}

void task_suicide()
{
	exit(-9);
}

void kill_task(unsigned int pid)
{
	if(pid == 0) return;
	task_t *task = get_task_pid(pid);
	if(!task) {
		printk(KERN_WARN, "kill_task recieved invalid PID\n");
		return;
	}
	task->state = TASK_SUICIDAL;
	task->sigd = 0; /* fuck your signals */
	if(task == current_task)
	{
		for(;;) schedule();
	}
}

void exit(int code)
{
	if(!current_task || current_task->pid == 0) 
		panic(PANIC_NOSYNC, "kernel tried to exit");
	task_t *t = (task_t *)current_task;
	/* Get ready to exit */
	ll_insert(kill_queue, (void *)t);
	raise_flag(TF_EXITING);
	if(code != -9) 
		t->exit_reason.cause = 0;
	t->exit_reason.ret = code;
	t->exit_reason.pid = t->pid;
	/* Clear out system resources */
	free_thread_specific_directory();
	clear_resources(t);
	/* tell our parent that we're dead */
	if(t->parent)
		do_send_signal(t->parent->pid, SIGCHILD, 1);
	if(!sub_atomic(&t->thread->count, 1))
	{
		/* we're the last thread to share this data. Clean it up */
		close_all_files(t);
		if(t->thread->root)iput(t->thread->root);
		if(t->thread->pwd) iput(t->thread->pwd);
		mutex_destroy(&t->thread->files_lock);
		kfree(t->thread);
		t->thread = 0;
	}
	raise_flag(TF_DYING);
	/* don't do this while the state is dead, as we may step on the toes of waitpid.
	 * this fixes all tasks that are children of current_task, or are waiting
	 * on current_task. For those waiting, it signals the task. For those that
	 * are children, it fixes the 'parent' pointer. */
	search_tqueue(primary_queue, TSEARCH_EXIT_PARENT | TSEARCH_EXIT_WAITING, 0, 0, 0, 0);
	t->state = TASK_DEAD;
	set_as_dead(t);
	char flag_last_page_dir_task=0;
	mutex_acquire(&pd_cur_data->lock);
	flag_last_page_dir_task = (sub_atomic(&pd_cur_data->count, 1) == 0) ? 1 : 0;
	mutex_release(&pd_cur_data->lock);
	if(flag_last_page_dir_task) {
		/* no one else is referencing this directory. Clean it up... */
		free_thread_shared_directory();
		vm_unmap(PDIR_DATA);
		raise_flag(TF_LAST_PDIR);
	}
	for(;;) schedule();
}

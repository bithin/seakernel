/* Code to keep track of file handles */
#include <kernel.h>
#include <task.h>
#include <fs.h>
#include <atomic.h>
#include <file.h>

struct file_ptr *get_file_handle(task_t *t, int n)
{
	if(n >= FILP_HASH_LEN) return 0;
	struct file_ptr *f = t->thread->filp[n];
	return f;
}

struct file *get_file_pointer(task_t *t, int n)
{
	mutex_acquire(&t->thread->files_lock);
	struct file_ptr *fp = get_file_handle(t, n);
	if(fp && !fp->fi)
		panic(PANIC_NOSYNC, "found empty file handle in task %d pointer list", t->pid);
	struct file *ret=0;
	if(fp) {
		add_atomic(&fp->count, 1);
		assert(ret = fp->fi);
	}
	mutex_release(&t->thread->files_lock);
	return ret;
}

void remove_file_pointer(task_t *t, int n)
{
	if(n > FILP_HASH_LEN) return;
	if(!t || !t->thread->filp)
		return;
	struct file_ptr *f = get_file_handle(t, n);
	if(!f)
		return;
	t->thread->filp[n] = 0;
	sub_atomic(&f->fi->count, 1);
	if(!f->fi->count)
		kfree(f->fi);
	kfree(f);
}

void fput(task_t *t, int fd, char flags)
{
	mutex_acquire(&t->thread->files_lock);
	struct file_ptr *fp = get_file_handle(t, fd);
	assert(fp);
	int r = sub_atomic(&fp->count, (flags & FPUT_CLOSE) ? 2 : 1);
	if(!r)
		remove_file_pointer(t, fd);
	mutex_release(&t->thread->files_lock);
}

/* Here we find an unused filedes, and add it to the list. We rely on the 
 * list being sorted, and since this is the only function that adds to it, 
 * we can assume it is. This allows for relatively efficient determining of 
 * a filedes without limit. */
int add_file_pointer_do(task_t *t, struct file_ptr *f, int after)
{
	assert(t && f);
	while(after < FILP_HASH_LEN && t->thread->filp[after])
		after++;
	if(after >= FILP_HASH_LEN) {
		printk(1, "[vfs]: task %d ran out of files (syscall=%d). killed.\n", 
				t->pid, t == current_task ? (int)t->system : -1);
		kill_task(t->pid);
	}
	t->thread->filp[after] = f;
	f->num = after;
	return after;
}

int add_file_pointer(task_t *t, struct file *f)
{
	struct file_ptr *fp = (struct file_ptr *)kmalloc(sizeof(struct file_ptr));
	mutex_acquire(&t->thread->files_lock);
	fp->fi = f;
	int r = add_file_pointer_do(t, fp, 0);
	fp->num = r;
	fp->count = 2; /* once for being open, once for being used by the function that calls this */
	mutex_release(&t->thread->files_lock);
	return r;
}

int add_file_pointer_after(task_t *t, struct file *f, int x)
{
	struct file_ptr *fp = (struct file_ptr *)kmalloc(sizeof(struct file_ptr));
	mutex_acquire(&t->thread->files_lock);
	fp->fi = f;
	int r = add_file_pointer_do(t, fp, x);
	fp->num = r;
	fp->count = 2; /* once for being open, once for being used by the function that calls this */
	mutex_release(&t->thread->files_lock);
	return r;
}

void copy_file_handles(task_t *p, task_t *n)
{
	if(!p || !n)
		return;
	int c=0;
	while(c < FILP_HASH_LEN) {
		if(p->thread->filp[c]) {
			struct file_ptr *fp = (void *)kmalloc(sizeof(struct file_ptr));
			fp->num = c;
			fp->fi = p->thread->filp[c]->fi;
			fp->count = p->thread->filp[c]->count;
			add_atomic(&fp->fi->count, 1);
			struct inode *i = fp->fi->inode;
			assert(i && i->count && i->f_count);
			add_atomic(&i->count, 1);
			add_atomic(&i->f_count, 1);
			if(i->pipe && !i->pipe->type) {
				add_atomic(&i->pipe->count, 1);
				if(fp->fi->flags & _FWRITE)
					add_atomic(&i->pipe->wrcount, 1);
				task_unblock_all(i->pipe->read_blocked);
				task_unblock_all(i->pipe->write_blocked);
			}
			n->thread->filp[c] = fp;
		}
		c++;
	}
}

void close_all_files(task_t *t)
{
	int q=0;
	for(;q<FILP_HASH_LEN;q++)
	{
		if(t->thread->filp[q]) {
			sys_close(q);
			assert(!t->thread->filp[q]);
		}
	}
}

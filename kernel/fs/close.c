/* Closes an open file descriptor. If its a pipe, we shutdown the pipe too */
#include <kernel.h>
#include <memory.h>
#include <task.h>
#include <fs.h>
#include <dev.h>
#include <sys/fcntl.h>
#include <block.h>
#include <char.h>
#include <rwlock.h>
#include <atomic.h>
#include <file.h>
int sys_close(int fp)
{
	struct file *f = get_file_pointer((task_t *) current_task, fp);
	if(!f)
		return -EBADF;
	assert(f->inode && f->inode->f_count);
	if(f->inode->pipe)
	{
		/* okay, its a pipe. We need to do some special things
		 * to close a pipe, like decrement the various counts.
		 * If the counts reach zero, we free it */
		mutex_acquire(f->inode->pipe->lock);
		if(f->inode->pipe->count)
			sub_atomic(&f->inode->pipe->count, 1);
		if(f->flags & _FWRITE && f->inode->pipe->wrcount 
				&& f->inode->pipe->type != PIPE_NAMED)
			sub_atomic(&f->inode->pipe->wrcount, 1);
		if(!f->inode->pipe->count && f->inode->pipe->type != PIPE_NAMED) {
			assert(!f->inode->pipe->read_blocked->num);
			assert(!f->inode->pipe->write_blocked->num);
			free_pipe(f->inode);
			f->inode->pipe = 0;
		} else {
			task_unblock_all(f->inode->pipe->read_blocked);
			task_unblock_all(f->inode->pipe->write_blocked);
			mutex_release(f->inode->pipe->lock);
		}
	}
	/* close devices */
	if(S_ISCHR(f->inode->mode) && !fp)
		char_rw(CLOSE, f->inode->dev, 0, 0);
	else if(S_ISBLK(f->inode->mode) && !fp)
		block_device_rw(CLOSE, f->inode->dev, 0, 0, 0);
	if(!sub_atomic(&f->inode->f_count, 1) && f->inode->marked_for_deletion)
		do_unlink(f->inode);
	else
		iput(f->inode);
	fput((task_t *)current_task, fp, FPUT_CLOSE);
	return 0;
}

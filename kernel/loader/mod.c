
#include <kernel.h>
#if (CONFIG_MODULES)
#include <fs.h>
#include <task.h>
#include <memory.h>
#include <module.h>
#include <elf.h>
#include <symbol.h>
module_t *modules=0;
int load_deps(char *);
mutex_t mod_mutex;
mutex_t sym_mutex;
kernel_symbol_t export_syms[MAX_SYMS];
void init_kernel_symbols(void)
{
	uint32_t i;
	for(i = 0; i < MAX_SYMS; i++)
		export_syms[i].ptr = 0;
	mutex_create(&sym_mutex, 0);
	/* symbol functions */
	add_kernel_symbol(find_kernel_function);
	add_kernel_symbol(remove_kernel_symbol);
	add_kernel_symbol(_add_kernel_symbol);
	/* basic kernel functions */
	add_kernel_symbol(panic_assert);
	add_kernel_symbol(panic);
	_add_kernel_symbol((addr_t)&kernel_state_flags, "kernel_state_flags");
	add_kernel_symbol(printk);
	add_kernel_symbol(kprintf);
	add_kernel_symbol(sprintf);
	add_kernel_symbol(memset);
	add_kernel_symbol(memcpy);
	add_kernel_symbol(_strcpy);
	add_kernel_symbol(inb);
	add_kernel_symbol(outb);
	add_kernel_symbol(inw);
	add_kernel_symbol(outw);
	add_kernel_symbol(inl);
	add_kernel_symbol(outl);
	add_kernel_symbol(mutex_create);
	add_kernel_symbol(mutex_destroy);
	add_kernel_symbol(__mutex_release);
	add_kernel_symbol(__mutex_acquire);
	add_kernel_symbol(__rwlock_acquire);
	add_kernel_symbol(rwlock_release);
	add_kernel_symbol(__rwlock_escalate);
	add_kernel_symbol(rwlock_create);
	add_kernel_symbol(rwlock_destroy);
	
	/* these systems export these, but have no initialization function */
	add_kernel_symbol(get_epoch_time);
	add_kernel_symbol(allocate_dma_buffer);
}

void _add_kernel_symbol(const intptr_t func, const char * funcstr)
{
	uint32_t i;
	if(func < (addr_t)&kernel_start)
		panic(0, "tried to add invalid symbol %x:%s\n", func, funcstr);
	mutex_acquire(&sym_mutex);
	for(i = 0; i < MAX_SYMS; i++)
	{
		if(!export_syms[i].ptr)
			break;
	}
	if(i >= MAX_SYMS)
		panic(0, "ran out of space on symbol table");
	export_syms[i].name = funcstr;
	export_syms[i].ptr = func;
	mutex_release(&sym_mutex);
}

intptr_t find_kernel_function(char * unres)
{
	uint32_t i;
	mutex_acquire(&sym_mutex);
	for(i = 0; i < MAX_SYMS; i++)
	{
		if(export_syms[i].ptr && 
			strlen(export_syms[i].name) == strlen(unres) &&
			!memcmp((uint8_t*)export_syms[i].name, (uint8_t*)unres, 
					(int)strlen(unres))) {
			mutex_release(&sym_mutex);
			return export_syms[i].ptr;
		}
	}
	mutex_release(&sym_mutex);
	return 0;
}

int remove_kernel_symbol(char * unres)
{
	uint32_t i;
	mutex_acquire(&sym_mutex);
	for(i = 0; i < MAX_SYMS; i++)
	{
		if(export_syms[i].ptr && 
			strlen(export_syms[i].name) == strlen(unres) &&
			!memcmp((uint8_t*)export_syms[i].name, (uint8_t*)unres, 
					(int)strlen(unres)))
		{
			export_syms[i].ptr=0;
			mutex_release(&sym_mutex);
			return 1;
		}
	}
	mutex_release(&sym_mutex);
	return 0;
}

int is_loaded(char *name)
{
	mutex_acquire(&mod_mutex);
	module_t *m = modules;
	while(m) {
		if(!strcmp(m->name, name))
		{
			mutex_release(&mod_mutex);
			return 1;
		}
		m=m->next;
	}
	mutex_release(&mod_mutex);
	return 0;
}

int load_module(char *path, char *args, int flags)
{
	if(!path)
		return -EINVAL;
	if(!(flags & 2)) printk(KERN_DEBUG, "[mod]: Loading Module '%s'\n", path);
	int i, pos=-1;
	module_t *tmp = (module_t *)kmalloc(sizeof(module_t));
	char *r = strrchr(path, '/');
	if(r) r++; else r = path;
	strncpy(tmp->name, r, 128);
	strncpy(tmp->path, path, 128);
	if(is_loaded(tmp->name)) {
		kfree(tmp);
		return -EEXIST;
	}
	if(flags & 2) {
		kfree(tmp);
		return 0;
	}
	/* Open and test */
	int desc=sys_open(path, O_RDWR);
	if(desc < 0)
	{
		kfree(tmp);
		return -ENOENT;
	}
	/* Determine the length */
	struct stat sf;
	sys_fstat(desc, &sf);
	int len = sf.st_size;
	/* Allocate the space and read into it */
	char *mem = (char *)kmalloc(len);
	sys_read(desc, 0, mem, len);
	sys_close(desc);
	/* Fill out the slot info */
	tmp->base=mem;
	tmp->length=len;
	tmp->exiter=0;
	tmp->deps[0]=0;
	/* Time to decode the module header */
	if(!(*mem == 'M' && *(mem + 1) == 'O' && *(mem+2) == 'D'))
	{
		kfree(tmp);
		kfree(mem);
		return -EINVAL;
	}
	/* Call the elf parser */
	int res = parse_elf_module(tmp, (unsigned char *)mem+4, path, flags & 1);
	if(res == _MOD_FAIL || res == _MOD_AGAIN)
	{
		kfree(mem);
		kfree(tmp);
		/* try again? Maybe we loaded a dependency and need to retry */
		if(res == _MOD_AGAIN)
			return load_module(path, args, flags);
		return -ENOEXEC;
	}
	mutex_acquire(&mod_mutex);
	module_t *old = modules;
	modules = tmp;
	tmp->next = old;
	mutex_release(&mod_mutex);
	printk(0, "[mod]: loaded module '%s' @[%x - %x]\n", path, tmp->base, tmp->base + len);
	return ((int (*)(char *))tmp->entry)(args);
}

/* This checks if module 'm' depends on module 'yo' */
int do_it_depend_on(module_t *yo, module_t *m)
{
	char *d = m->deps;
	if(!*d) return 0;
	char *mnext, *current;
	current = d;
	int count=0;
	while(current)
	{
		mnext = strchr(current, ',');
		if(mnext)
		{
			*mnext=0;
			mnext++;
		}
		if(*current == ':')
			break;
		if(!strcmp(yo->name, current))
			return 1;
		current = mnext;
	}
	return 0;
}

/* This makes sure module 'i' can unload and not break dependencies */
module_t *canweunload(module_t *i)
{
	module_t *mq = modules;
	while(mq) {
		if(mq != i && do_it_depend_on(i, mq))
			return mq;
		mq=mq->next;
	}
	return 0;
}

int do_unload_module(char *name, int flags)
{
	/* Is it going to work? */
	mutex_acquire(&mod_mutex);
	module_t *mq = modules;
	while(mq) {
		if((!strcmp(mq->name, name) || !strcmp(mq->path, name)))
			break;
		mq = mq->next;
	}
	
	if(!mq) {
		mutex_release(&mod_mutex);
		return -ENOENT;
	}
	/* Determine if are being depended upon or if we can unload */
	module_t *mo;
	if(!(flags & 1) && (mo = canweunload(mq))) {
		mutex_release(&mod_mutex);
		return -EINVAL;
	}
	mutex_release(&mod_mutex);
	/* Call the unloader */
	printk(KERN_INFO, "[mod]: Unloading Module '%s'\n", name);
	int ret = 0;
	if(mq->exiter)
		ret = ((int (*)())mq->exiter)();
	/* Clear out the resources */
	kfree(mq->base);
	mutex_acquire(&mod_mutex);
	module_t *a = modules;
	if(a == mq)
		modules = mq->next;
	else
	{
		while(a && a->next != mq)a=a->next;
		assert(a);
		a->next = mq->next;
	}
	mutex_release(&mod_mutex);
	kfree(mq);
	return ret;
}

int unload_module(char *name)
{
	if(!name)
		return -EINVAL;
	return do_unload_module(name, 0);
}

void unload_all_modules()
{
	/* Unload all loaded modules */
	int todo=1;
	int pass=1;
	while(todo--) {
		if(pass == 10) {
			kprintf("[mod]: Unloading modules...pass 10...fuck it.\n");
			return;
		}
		kprintf("[mod]: Unloading modules pass #%d...\n", pass++);
		int i;
		module_t *mq = modules;
		while(mq) {
			int r = do_unload_module(mq->name, 0);
			if(r < 0 && r != -ENOENT) {
				todo++;
				mq = mq->next;
			} else
				mq = modules;
		}
	}
}

void init_module_system()
{
	mutex_create(&mod_mutex, 0);
}

int sys_load_module(char *path, char *args, int flags)
{
	if(current_task->thread->uid)
		return -EPERM;
	return load_module(path, args, flags);
}

int sys_unload_module(char *path, int flags)
{
	if(current_task->thread->uid)
		return -EPERM;
	return do_unload_module(path, flags);
}
#endif

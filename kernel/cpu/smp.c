#include <config.h>
#if CONFIG_SMP
#include <task.h>
#include <cpu.h>

void smp_cpu_task_idle(task_t *me)
{
	((cpu_t *)(me->cpu))->flags |= CPU_TASK;
	me->system = -1;
	set_int(1);
	/* wait until we have tasks to run */
	for(;;) 
		schedule();
}

#endif

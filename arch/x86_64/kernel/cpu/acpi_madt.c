#include <config.h>
#if CONFIG_SMP
#include <kernel.h>
#include <acpi.h>
#include <cpu.h>
#include <pmap.h>
struct pmap apic_pmap;

void acpi_madt_parse_processor(void *ent, int boot)
{
	struct {
		uint8_t type, length, acpi_processor_id, apicid;
		uint32_t flags;
	} *proc = ent;
	num_cpus++;
	cpu_t new_cpu;
	if(!(proc->flags & 1))
	{
		printk(0, "[acpi]: detected disabled processor #%d (%d)\n", proc->apicid, proc->acpi_processor_id);
		return;
	}
	memset(&new_cpu, 0, sizeof(cpu_t));
	new_cpu.apicid = proc->apicid;
	new_cpu.flags=0;
	cpu_t *cp = add_cpu(&new_cpu);
	if(boot) {
		primary_cpu = &cpu_array[proc->apicid];
		return;
	}
	if(!cp)
	{
		printk(2, "[smp]: refusing to initialize CPU %d\n", proc->apicid);
		return;
	}
	int ver = APIC_VERSION(LAPIC_READ(LAPIC_VER));
	printk(0, "[acpi]: booting CPU %d %x\n", proc->apicid, ver);
	int re = boot_cpu(proc->apicid, ver);
	if(!re) {
		cp->flags |= CPU_ERROR;
		num_failed_cpus++;
	} else
		num_booted_cpus++;
}

void acpi_madt_parse_ioapic(void *ent)
{
	struct {
		uint8_t type, length, apicid, __reserved;
		uint32_t address, int_start;
	} *st = ent;
	add_ioapic(pmap_get_mapping(&apic_pmap, st->address), st->apicid, st->int_start);
}

int parse_acpi_madt()
{
	struct {
		uint8_t type;
		uint8_t length;
	} *ent;
	pmap_create(&apic_pmap, 0);
	int length;
	void *ptr = acpi_get_table_data("APIC", &length);
	if(!ptr) return 0;
	
	uint64_t controller_address = *(uint32_t *)ptr;
	uint32_t flags = *(uint32_t *)((uint32_t *)ptr + 1);
	lapic_addr = pmap_get_mapping(&apic_pmap, controller_address);
	void *tmp = (void *)((addr_t)ptr + 8);
	/* the ACPI MADT specification says that we may assume
	 * that the boot processor is the first processor listed
	 * in the table. */
	int boot_cpu = 1;
	while((addr_t)tmp < (addr_t)ptr+length)
	{
		ent = tmp;
		if(ent->type == 0)
			acpi_madt_parse_processor(ent, boot_cpu);
		else if(ent->type == 1)
			acpi_madt_parse_ioapic(ent);
		boot_cpu = 0;
		tmp = (void *)((addr_t)tmp + ent->length);
	}
	return 1;
}

#endif

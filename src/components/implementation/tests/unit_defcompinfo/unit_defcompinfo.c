/*
 * Copyright 2016, Phani Gadepalli and Gabriel Parmer, GWU, gparmer@gwu.edu.
 *
 * This uses a two clause BSD License.
 */

#include <stdio.h>
#include <string.h>
#include <cos_component.h>
#include <cobj_format.h>
#include <cos_defkernel_api.h>

#undef assert
#define assert(node) do { if (unlikely(!(node))) { debug_print("assert error in @ "); *((int *)0) = 0; } } while (0)
#define PRINT_FN prints
#define debug_print(str) (PRINT_FN(str __FILE__ ":" STR(__LINE__) ".\n"))
#define BUG() do { debug_print("BUG @ "); *((int *)0) = 0; } while (0);
#define SPIN() do { while (1) ; } while (0)

#define CHILD_COMP_COUNT   2
#define CHILD_UNTYPED_SIZE (1<<24)
#define CHILD_SCHED_ID     0
#define CHILD_SCHED_CYCS   TCAP_RES_INF 
#define CHILD_SCHED_PRIO   TCAP_PRIO_MAX

int is_booter = 1;
extern vaddr_t cos_upcall_entry;
struct cos_defcompinfo child_defci[CHILD_COMP_COUNT];
static cycles_t cycs_per_usec;

static void
cos_llprint(char *s, int len)
{ call_cap(PRINT_CAP_TEMP, (int)s, len, 0, 0); }

int
prints(char *s)
{
	int len = strlen(s);

	cos_llprint(s, len);

	return len;
}

int __attribute__((format(printf,1,2)))
printc(char *fmt, ...)
{
	  char s[128];
	  va_list arg_ptr;
	  int ret, len = 128;

	  va_start(arg_ptr, fmt);
	  ret = vsnprintf(s, len, fmt, arg_ptr);
	  va_end(arg_ptr);
	  cos_llprint(s, ret);

	  return ret;
}

#define TEST_NAEPS    3
#define TEST_AEP_CYCS 400000
#define TEST_AEP_PRIO TCAP_PRIO_MAX
struct cos_aep_info test_aep[TEST_NAEPS];

static void
aep_thd_fn(arcvcap_t rcv, void *data)
{
	printc("\tSwitched to aep %d\n", (int)data);
	while (1) {
		cos_rcv(rcv);
	}
}

static void
test_aeps(void)
{
	int                  i, ret;
	int                  blocked;
	cycles_t             cycs;
	thdid_t              tid;
	struct cos_compinfo *ci = cos_compinfo_get(cos_defcompinfo_curr_get());

	memset(&test_aep, 0, sizeof(struct cos_aep_info) * TEST_NAEPS);

	printc("Test creating AEPS\n");
	for (i = 0 ; i < TEST_NAEPS ; i ++ ) {
		asndcap_t snd;

		printc("\tCreating AEP [%d]\n", i);
		ret = cos_aep_tcap_alloc(&(test_aep[i]), BOOT_CAPTBL_SELF_INITTCAP_BASE, aep_thd_fn, (void *)i);
		assert(ret == 0);

		snd = cos_asnd_alloc(ci, test_aep[i].rcv, ci->captbl_cap);
		assert(snd);
			
		ret = cos_tcap_delegate(snd, BOOT_CAPTBL_SELF_INITTCAP_BASE, TEST_AEP_CYCS, TEST_AEP_PRIO, TCAP_DELEG_YIELD);
		assert(ret == 0);

		while (cos_sched_rcv(BOOT_CAPTBL_SELF_INITRCV_BASE, &tid, &blocked, &cycs));
	}

	printc("Done.\n");
}

static void
test_childcomps(void)
{
	int id, ret;

	printc("Test switching to new components\n");
	for (id = 0 ; id < CHILD_COMP_COUNT ; id ++ ) {
		int      blocked;
		cycles_t cycs;
		thdid_t  tid;

		while (cos_sched_rcv(BOOT_CAPTBL_SELF_INITRCV_BASE, &tid, &blocked, &cycs));
		printc("\tSwitching to [%d] component\n", id);
		if (id == CHILD_SCHED_ID) {
			ret = cos_switch(child_defci[id].sched_aep.thd, child_defci[id].sched_aep.tc, CHILD_SCHED_PRIO, TCAP_TIME_NIL, 
					BOOT_CAPTBL_SELF_INITRCV_BASE, cos_sched_sync());
			assert(ret == 0);
		} else {
			cycles_t    now;
			tcap_time_t timer;

			rdtscll(now);
			timer = tcap_cyc2time(now + 100 * cycs_per_usec);

			ret = cos_defswitch(child_defci[id].sched_aep.thd, timer, CHILD_SCHED_PRIO, cos_sched_sync());
			assert(ret == 0);
		}
	}

	printc("Done.\n");
}

void
cos_init(void)
{
	cycs_per_usec = (cycles_t)cos_hw_cycles_per_usec(BOOT_CAPTBL_SELF_INITHW_BASE);

	if (is_booter) {
		int                     id, ret;
		struct cos_defcompinfo *defci = cos_defcompinfo_curr_get();
		struct cos_compinfo    *ci    = cos_compinfo_get(defci);
		
		is_booter = 0;
		printc("Unit-test for defcompinfo API\n");
		cos_meminfo_init(&(ci->mi), BOOT_MEM_KM_BASE, COS_MEM_KERN_PA_SZ, BOOT_CAPTBL_SELF_UNTYPED_PT);
		cos_defcompinfo_init();

		for (id = 0 ; id < CHILD_COMP_COUNT ; id ++ ) {
			vaddr_t              vm_range, addr;
			pgtblcap_t           child_utpt;
			int                  is_sched = ((id == CHILD_SCHED_ID) ? 1 : 0);
			struct cos_compinfo *child_ci = cos_compinfo_get(&child_defci[id]);

			printc("\tCreating new %s component [%d]\n", is_sched ? "scheduler" : "simple", id);
			child_utpt = cos_pgtbl_alloc(ci);
			assert(child_utpt);

			cos_meminfo_init(&(child_ci->mi), BOOT_MEM_KM_BASE, CHILD_UNTYPED_SIZE, child_utpt);
			cos_defcompinfo_child_alloc(&child_defci[id], (vaddr_t)&cos_upcall_entry, (vaddr_t)BOOT_MEM_VM_BASE, BOOT_CAPTBL_FREE, is_sched);

			printc("\t\tCopying new capabilities\n");
			ret = cos_cap_cpy_at(child_ci, BOOT_CAPTBL_SELF_CT, ci, child_ci->captbl_cap);
			assert(ret == 0);
			ret = cos_cap_cpy_at(child_ci, BOOT_CAPTBL_SELF_PT, ci, child_ci->pgtbl_cap);
			assert(ret == 0);
			ret = cos_cap_cpy_at(child_ci, BOOT_CAPTBL_SELF_UNTYPED_PT, ci, child_utpt);
			assert(ret == 0);
			ret = cos_cap_cpy_at(child_ci, BOOT_CAPTBL_SELF_COMP, ci, child_ci->comp_cap);
			assert(ret == 0);

			ret = cos_cap_cpy_at(child_ci, BOOT_CAPTBL_SELF_INITTHD_BASE, ci, child_defci[id].sched_aep.thd);
			assert(ret == 0);
			ret = cos_cap_cpy_at(child_ci, BOOT_CAPTBL_SELF_INITHW_BASE, ci, BOOT_CAPTBL_SELF_INITHW_BASE);
			assert(ret == 0);

			if (is_sched) {
				printc("\t\tCopying scheduler capabilities\n");
				ret = cos_cap_cpy_at(child_ci, BOOT_CAPTBL_SELF_INITTCAP_BASE, ci, child_defci[id].sched_aep.tc);
				assert(ret == 0);
				ret = cos_cap_cpy_at(child_ci, BOOT_CAPTBL_SELF_INITRCV_BASE, ci, child_defci[id].sched_aep.rcv);
				assert(ret == 0);

				ret = cos_tcap_transfer(child_defci[id].sched_aep.rcv, BOOT_CAPTBL_SELF_INITTCAP_BASE, CHILD_SCHED_CYCS, CHILD_SCHED_PRIO);
				assert(ret == 0);
			}

			printc("\t\tMapping in the booter address space\n");
			vm_range = (vaddr_t)cos_get_heap_ptr() - BOOT_MEM_VM_BASE;
			assert(vm_range > 0);
			for (addr = 0 ; addr < vm_range ; addr += PAGE_SIZE) {
				vaddr_t src_pg = (vaddr_t)cos_page_bump_alloc(ci), dst_pg;

				assert(src_pg);
				memcpy((void *)src_pg, (void *)(BOOT_MEM_VM_BASE + addr), PAGE_SIZE);

				dst_pg = cos_mem_alias(child_ci, ci, src_pg);
				assert(dst_pg);
			}

			printc("\t\tReserving untyped memory\n");
			cos_meminfo_alloc(child_ci, BOOT_MEM_KM_BASE, CHILD_UNTYPED_SIZE);

			printc("\tDone.\n");
		}

		/* TEST CREATING AEPS */
		test_aeps();

		/* TEST SWITCHING TO CHILD COMPS */
		test_childcomps();

		printc("Unit-test done.\n");

		SPIN();
	} else {
		struct cos_defcompinfo *defci = cos_defcompinfo_curr_get();
		struct cos_compinfo    *ci    = cos_compinfo_get(defci);

		printc("Component started\n");
		cos_meminfo_init(&(ci->mi), BOOT_MEM_KM_BASE, CHILD_UNTYPED_SIZE, BOOT_CAPTBL_SELF_UNTYPED_PT);
		cos_defcompinfo_init();

		/* TEST BLOCKING */
		/* TODO: Challenge - how does a component know at runtime if can call cos_rcv or not? - It does not at runtime. */
		cos_rcv(BOOT_CAPTBL_SELF_INITRCV_BASE);
		printc("\tThis is a simple component\n");

		SPIN();
	}

	return;
}

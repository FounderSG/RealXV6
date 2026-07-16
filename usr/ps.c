#include "unix.h"

/*
 *	ps - process status
 *	examine and print certain things about processes
 *
 *	The privileged work is done by the kernel: psinfo() copies out proc[i]
 *	plus a 512-byte image of the top of that process's user stack (the
 *	argument vector exec leaves at USTACK).  ps never reads /dev/mem or
 *	/dev/kmem and never needs to know the physical memory layout.
 */

#include "../h/param.h"
#include "../h/proc.h"

struct psbuf {
	struct proc pr;
	char stk[512];          /* image of virtual [USTACK-512, USTACK) */
} info;

#define SBASE (USTACK - 512)    /* virtual address that maps to stk[0] */

void prcom(void);

/* read a word from the captured stack image; caller checks inrange() first */
int sword(int v)
{
	return *(int *)(info.stk + (v - SBASE));
}

int inrange(int v)
{
	return v >= SBASE && v <= USTACK - 2;
}

main()
{
	int i;

	printf(" F S UID   PID   PRI ADDR  WCHAN COMMAND\n");
	for (i = 0; i < NPROC; i++) {
		psinfo(i, &info);
		if (info.pr.p_stat == 0)
			continue;
		printf("%2x %c%4d", info.pr.p_flag,
			"0SWRIZT"[info.pr.p_stat], info.pr.p_uid & 0377);
		printf("%6u", info.pr.p_pid);
		printf("%6d%5x", info.pr.p_pri, info.pr.p_addr);
		if (info.pr.p_wchan)
			printf("%7x", info.pr.p_wchan);
		else
			printf("       ");
		if (info.pr.p_stat == 5)
			printf(" <defunct>");
		else if (info.pr.p_pid == 0)
			printf(" swaper");
		else if (info.pr.p_pid == 1)
			printf(" init");
		else if (info.pr.p_tsize != 0)
			prcom();
		printf("\n");
	}
	exit();
}

void prcom(void)
{
	int sp, argc, argv, ai;
	int j, n, k;
	char argbuf[17];

	sp = sword(USTACK - 2);
	if (!inrange(sp) || !inrange(sp + 2))
		return;
	argc = sword(sp);
	argv = sword(sp + 2);
	printf(" ");
	for (j = 0; j < argc && j < 8; j++) {
		if (!inrange(argv + j * 2))
			break;
		ai = sword(argv + j * 2);
		if (ai < SBASE || ai >= USTACK)
			break;
		k = ai - SBASE;
		for (n = 0; n < 16 && k < 512 && info.stk[k]; n++)
			argbuf[n] = info.stk[k++];
		argbuf[n] = 0;
		printf("%s", argbuf);
		if (j < argc - 1)
			printf(" ");
	}
}

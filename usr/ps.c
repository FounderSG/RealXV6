#include "unix.h"

/*
 *	ps - process status
 *	examine and print certain things about processes
 *
 *	The privileged work is done by the kernel: psinfo() fills in the struct
 *	below -- the fields ps prints, plus an image of the argument frame exec
 *	leaves on that process's stack.  ps never reads /dev/mem or /dev/kmem,
 *	and needs no kernel header: stkbase says where the image came from, or
 *	is 0 when the kernel captured no frame, and the loop stops when psinfo
 *	reports the end of the proc table.
 */

/*
 * The psinfo() contract.  The kernel declares the same fields in the same
 * order ahead of psinfo() in ken/sys4.c, stopping at stkbase: it copies the
 * frame image out separately, straight into stk[].  Keep the two in step,
 * and keep any new field ahead of stk[].
 */
#define STKSIZ	512

struct psbuf {
	int	p_stat;		/* 0 = free slot, else index into "0SWRIZT" */
	int	p_flag;
	int	p_pri;		/* priority, negative is high */
	int	p_uid;
	int	p_pid;
	int	p_ppid;
	int	p_addr;
	int	p_wchan;
	int	stkbase;	/* user address stk[0] came from; 0 = no frame */
	char	stk[STKSIZ];	/* image of the argument frame */
} info;

void prcom(void);

/*
 * Frame addresses are turned into offsets into stk[] and never compared
 * against an absolute top: the frame can sit high enough that stkbase +
 * STKSIZ wraps to 0, which would make every absolute test misfire.  An
 * address outside the frame wraps to a huge offset and fails the same test.
 *
 * exec leaves the frame's top word unused and stores the initial SP in the
 * word below it, so the SP always sits at SPOFF into the captured image
 * whatever address the frame has on this port.
 */
#define SPOFF	(STKSIZ - 4)
uint stkoff(uint v)
{
	return v - (uint)info.stkbase;
}

/* read a word from the captured stack image; caller checks inrange() first */
int sword(uint v)
{
	return *(int *)(info.stk + stkoff(v));
}

int inrange(uint v)
{
	return stkoff(v) <= STKSIZ - 2;
}

main()
{
	int i;

	printf(" F S UID   PID   PRI ADDR  WCHAN COMMAND\n");
	for (i = 0; psinfo(i, &info) >= 0; i++) {
		if (info.p_stat == 0)
			continue;
		printf("%2x %c%4d", info.p_flag,
			"0SWRIZT"[info.p_stat], info.p_uid);
		printf("%6u", info.p_pid);
		printf("%6d%5x", info.p_pri, info.p_addr);
		if (info.p_wchan)
			printf("%7x", info.p_wchan);
		else
			printf("       ");
		if (info.p_stat == 5)
			printf(" <defunct>");
		else if (info.p_pid == 0)
			printf(" swaper");
		else if (info.p_pid == 1)
			printf(" init");
		else if (info.stkbase != 0)
			prcom();
		printf("\n");
	}
	exit();
}

void prcom(void)
{
	uint sp, argv, ai, k;
	int argc, j, n;
	char argbuf[17];

	sp = (uint)sword((uint)info.stkbase + SPOFF);
	if (!inrange(sp) || !inrange(sp + 2))
		return;
	argc = sword(sp);
	argv = (uint)sword(sp + 2);
	printf(" ");
	for (j = 0; j < argc && j < 8; j++) {
		if (!inrange(argv + j * 2))
			break;
		ai = (uint)sword(argv + j * 2);
		k = stkoff(ai);
		if (k >= STKSIZ)
			break;
		for (n = 0; n < 16 && k < STKSIZ && info.stk[k]; n++)
			argbuf[n] = info.stk[k++];
		argbuf[n] = 0;
		printf("%s", argbuf);
		if (j < argc - 1)
			printf(" ");
	}
}

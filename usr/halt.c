#include "unix.h"

/*
 * halt -- take the system down cleanly and stop the processor.
 *
 * Modelled on Peter Collinson's killunix, the program his site ran from 1976
 * to take V6 down, recoded for simh as "halt" in its halt/ directory:
 *
 *	https://github.com/pcollinson/unixv6-extras
 *
 * Kill every other process so nothing can dirty a buffer behind the flush,
 * then make one system call that flushes the cache, waits for the disk to go
 * quiet and stops the CPU without returning to user mode in between.
 * sync(2) is not that guarantee -- it only starts the writes: update() hands
 * the delayed-write buffers to the driver B_ASYNC and returns with the
 * transfers still in flight -- so a program that synced and then stopped the
 * machine would leave behind exactly the corruption a clean shutdown is meant
 * to prevent.  The sync() below is there only to get the bulk of the cache
 * onto the disk while the system is still ordinary, so the uninterruptible
 * part of halt(2) has little left to do.
 * halt() in ken/sys4.c is the other half, and prints "safe to poweroff".
 *
 * halt(2) returns only on failure: EPERM if not root, EBUSY if another
 * update() holds updlock -- which would make halt's own update() a no-op and
 * stop the machine on a cache nobody flushed.  EBUSY is worth retrying, and
 * the retry is the reason this is a program rather than one line of shell.
 */

#define TRIES	15		/* ~15 seconds of a busy updater */

/*
 * The psinfo() contract, as usr/ps.c carries it: the kernel declares the same
 * fields in the same order ahead of psinfo() in ken/sys4.c and copies the
 * argument frame out separately, straight into stk[].  halt reads only p_stat
 * and p_pid, but stk[] still has to be here for that copy to land in.
 */
#define STKSIZ	512

struct psbuf {
	int	p_stat;		/* 0 = free slot, 5 = SZOMB */
	int	p_flag;
	int	p_pri;
	int	p_uid;
	int	p_pid;
	int	p_ppid;
	int	p_addr;
	int	p_wchan;
	int	stkbase;
	char	stk[STKSIZ];
} info;

/*
 * Deliberately the same test halt(2) will make, and not the "root, real or
 * effective" of the V6 halt: the kernel's suser() looks at the effective uid
 * alone, which getuid() returns in the high byte.  Accepting a real uid of 0
 * as well would let a caller past this check and still be refused by halt(2)
 * -- after killall() had already taken the machine's processes down with it.
 */
int suser(void)
{
	return ((getuid() >> 8) & 0377) == 0;
}

/*
 * Kill everything but the swapper (pid 0), the zombies (already dead) and
 * ourselves (kill skips self anyway).  psinfo() is what says where the proc
 * table ends -- it fails on the first index past it -- so the walk carries no
 * NPROC of its own and nothing here has to be revisited when the kernel's
 * table grows.  Killing as we go rather than collecting a list first is what
 * lets it stay that way: there is no array to size.
 *
 * init goes first, before the walk reaches it.  It respawns the shell from
 * wait(), so a shell killed while init has no signal pending only earns us a
 * shell we never saw; killed first, init takes the SIGKIL on its way out of
 * wait() instead, whichever child dies first.  It need not have died by the
 * time the shell is killed -- the pending signal is enough.
 */
void killall(void)
{
	int i, mypid;

	mypid = getpid();
	printf("halt: killing 1");
	kill(1, SIGKIL);

	for (i = 0; psinfo(i, &info) >= 0; i++) {
		if (info.p_stat == 0 || info.p_stat == 5)
			continue;		/* free slot, or a zombie already */
		if (info.p_pid <= 1 || info.p_pid == mypid)
			continue;		/* swapper, init (above), ourselves */
		printf(" %d", info.p_pid);
		kill(info.p_pid, SIGKIL);
	}
	printf("\n");
}

main()
{
	int i;

	if (!suser()) {
		printf("halt: must be root\n");
		exit();
	}

	/* early sync: sync() returns at once if one is already running, so
	 * give that one a moment to finish before we lean on halt(2) */
	sync();
	sleep(1);

	killall();
	sleep(3);		/* let them die.  init is gone, so they stay zombies */

	for (i = 0; i < TRIES; i++) {
		if (halt() != EBUSY)	/* returns only if it could not */
			break;
		printf("halt: disks busy\n");
		sleep(1);
	}
	printf("halt: cannot halt the system -- please stop by hand\n");
	exit();
}

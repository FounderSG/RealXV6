/*
 * fsck command
 *
 * Fsck audits UNIX file systems for consistency and corrects
 * any discrepancies.  Since these corrections will, in general,
 * result in a loss of data, the program will request operator
 * concurrence for each correction.
 * Fsck has the following checks for file consistency:
 *
 * Phase 1: Check block pointers for bad or duplicate blocks
 *          also check file sizes
 * Phase 1b: Check for other duplicate blocks
 * Phase 2: Check link counts
 * Phase 3: Correct link counts
 * Phase 4: Check free list
 * Phase 5: Correct free list
 *
 * fsck [-sdev-code] [-i] [-u] [-r] [-y] [-n] [filesystem] ...
 *
 *	s ... correct free list
 *		dev-code:
 *			3 ... rp03
 *			4 ... rp04
 *			blocks-per-cylinder:Blocks-to-skip (for everything else)
 *      q ... read answer from the operator for each prompt (default)
 *      u ... open the file system for updating (default)
 *      r ... open the file system read only
 *	y ... assume a yes answer to each prompt
 *	n ... assume a no answer to each prompt
 *
 *	filesystem
 *		filesystem to check
 *
 *	If no filesystem is given a list of filesystems
 *		is read from /etc/checklist
 *
 * Ported to UNIX V6 by Peter Collinson
 * June 2023
 *
 * a) Use full paths in include, <> not supported
 * b) remove 'unsigned' values, most can be ints for UnixV6
 *
 * all changes marked with +++pc
 */

/*
 * Ported to this 8086 real-mode V6 by adapting Peter Collinson's V6 port to
 * the Watcom C dialect and this system's libc.  Changes marked +++rx.
 */
#include "unix.h"

#define EOF -1		/* end of file on read */

#define	DSTATE	002	/* inode is a directory */
#define	FSTATE	001	/* inode is a file */
#define	CLEAR	( DSTATE | FSTATE )	/* inode should be cleared */
#define LSTATE	2	/* bits per inode state */
char	m[] = { CLEAR, CLEAR<< (1*LSTATE), CLEAR<< (2*LSTATE),
		CLEAR<< (3*LSTATE) };

#define	ALLINUM	inum=1;inum<=imax;inum++
#define	ALLAP	ap= &inode->i_addr[0];ap<&inode->i_addr[8];ap++) if (*ap

/* +++rx userspace has no path to the kernel headers, so the two on-disk
 * structures are spelled out here, exactly as h/ino.h and h/filsys.h have
 * them (mkfs.c carries its own copies for the same reason) */
struct	inode {		/* inode as it appears on the disk */
	int	i_mode;
	char	i_nlink;
	char	i_uid;
	char	i_gid;
	char	i_size0;
	uint	i_size1;
	int	i_addr[8];
	int	i_atime[2];
	int	i_mtime[2];
};

#define	IALLOC	0100000
#define	IFMT	060000
#define		IFDIR	040000
#define		IFCHR	020000
#define		IFBLK	060000
#define	ILARG	010000
#define	ISUID	04000
#define	ISGID	02000
#define	IREAD	0400
#define	IWRITE	0200
#define	IEXEC	0100

struct	inode	*inode;	/* pointer to the current inode */
#define	NINODE	(512/sizeof(*inode))	/* inodes per block (512) */
struct	inode	INODE[NINODE];

struct	filsys {	/* super block */
	int	s_isize;	/* size in blocks of I list */
	int	s_fsize;	/* size in blocks of entire volume */
	int	s_nfree;	/* number of in core free blocks (0-100) */
	int	s_free[100];	/* in core free blocks */
	int	s_ninode;	/* number of in core I nodes (0-100) */
	int	s_inode[100];	/* in core free I nodes */
	char	s_flock;
	char	s_ilock;
	char	s_fmod;
	char	s_ronly;
	int	s_time[2];
	int	pad[48];	/* rounds the super block out to 512 bytes */
};
struct	filsys	sblock;

struct DE {		/* directory structure */
	int	dnum;   /* pc: was unsigned */
	char	dname[14];
};

/* +++rx this compiler will not infer these; without them a call reached
 * before the definition is taken to return int */
struct	inode	*ginode();
char		*getcore();
char		*alloc();
char		*locv();
extern uint	ldiv(uint ah, uint al, uint bl, uint *rem);

#define	ALLOC	(inode->i_mode&IALLOC)
#define	LARGE	(inode->i_mode&ILARG)
#define	DIR	((inode->i_mode&IFMT)==IFDIR)
#define NOTSPL	((inode->i_mode&(IFCHR&IFBLK))==0)

#define	DUPTABLESIZE	100	/* number of duplicate blocks we will remember */
#define	MAXDUP	10	/* number of duplicate blocks we will look at */
#define	MAXBAD	300	/* number of bad blocks we will look at */

char
	*bitmap,	/* pointer to primary bitmap for block allocation */
	*freebit,	/* pointer to secondary bitmap for block allocation */
	*state,		/* pointer to table for inode state */
	*lc,		/* pointer to link count for each inode */
	pathname[200],
	*pp,		/* pointer to pathname */
	*name,		/* pointer to pathname for .. */
	sflag,		/* salvage the free block list */
	nflag,		/* always assume a no response */
	rflag,		/* open the file system read only */
	yflag,		/* always assume a yes response */
	*dev,		/* pointer to file system pathname being checked */
	*checklist = "/etc/checklist";

int
	n_free,		/* number of free blocks */
	sb_tfree,	/* +++pc free count that's stored in superblock - we don't have */
	n_blks,		/* number of blocks used */
	n_files,	/* number of files seen */
	fixfree,	/* is the free list corrupted */
	diskr,		/* file descriptor for reading the filesystem */
	diskw,		/* file descriptor for writing the filesystem */
	dups[DUPTABLESIZE],	/* duplicate block list */
	*dc,		/* pointer to singly duplicated block list */
	*el,		/* pointer to multiply duplicated block list */
	imod,		/* was the inode modified since it was read in */
	mod,		/* was the filesystem modified */
	dmod,		/* was the disk block modified since it was read */
	buf[256],	/* scratch buffer for processing indirect blocks */
	cylsiz,		/* number of blocks per cylinder */
	stepsize,	/* number of blocks for spacing purposes */
	mountdev,	/* is this a mounted device */
	blkdev,		/* is this a block device */
	chardev,	/* is this a character device */
	size,		/* number of blocks we have thus far traversed */
	badblk,		/* number of bad blocks */
	dupblk,		/* number of duplicate blocks */
	dupexcess,	/* excessive number of dups seen */
	dupfree,	/* number of duplicate blocks in free list */
	badfree	/* number of bad blocks in free list */
;

/* +++pc - these values are int in V6
 * unsigned	int
 */
int
	year,  		/* today's date minus 245 days (six months) */
	fmin,		/* block number of the first data block */
	fmax,		/* number of blocks in the volume */
	inum,		/* inode we are currently working on */
	imax,		/* number of inodes */
	lblock		/* last block number we looked at */
;

struct buf {		/* default checklist buffer */
	int fildes;
	int nleft;
	char *nextp;
	char buff[512];
} checkbuf ;
struct buf *iobuf = &checkbuf;	/* pointer to buffer for default checklist */

main(argn,argv)
int	argn;
char	*argv[];
{
	char
		c,	/* temporary character for option check */
		lsflag,	/* salvage flag */
		lnflag,	/* always no flag */
		lrflag,	/* open the file system read only */
		lyflag	/* always yes flag */
	;

	int
		i,	/* temporary */
		ch,	/* +++rx character from the checklist file */
		/* +++pc setting values in declarations not available
		checked	=0,	/* did we ever check anything */
		checked
	;

	struct	inode	today;

	/* +++pc - set the value */
        checked = 0;

/*
 * *** find out what day 6 months ago was ***
 */
	time(today.i_mtime);
	year = today.i_mtime[0] - 245;
/*
 * *** reset ***
 */
	lyflag = lnflag = lsflag = lrflag = 0;
	sync();
/*
 * *** let's look at the arguements ***
 */
	while (--argn) {
		argv++;
		if(**argv == '-') {
			switch(c = (*argv)[1]) {
			case 's':	/* salvage flag */
			case 'S':
				*argv += 2;
				stype(*argv);
				lsflag++;
				continue;
			case 'q':	/* default interactive answer flag */
			case 'Q':
				lnflag=0;
				lyflag=0;
				continue;
			case 'u':	/* open file system for updating */
			case 'U':
				lrflag=0;
				continue;
			case 'r':	/* open file system for reading */
			case 'R':
				lrflag++;
				continue;
			case 'n':	/* default no answer flag */
			case 'N':
				lnflag++;
				lyflag=0;
				continue;
			case 'y':	/* default yes answer flag */
			case 'Y':
				lyflag++;
				lnflag=0;
				continue;
			default:
				printf("%c option?\n",c);
				exit1(1);
			}
		} else {	/* let's look at the file system given */
			sflag = lsflag;
			nflag = lnflag;
			yflag = lyflag;
			rflag = lrflag;
			dev = *argv;
			checked++;
			check();
		}
	}
/*
 * *** see if we use the default list ***
 */
	if(checked == 0) {
		if((fopen(checklist,iobuf)) == -1){
			printf("Can NOT open default checklist file %s\n",
				checklist);
			return;
		}
		dev = pathname;
		/* +++rx char is unsigned here (-j), so EOF has to be caught
		 * in an int before it is stored into the path */
		for ( i=0 ; EOF != (ch = getc(iobuf)) ; i++ )
			if( (pathname[i] = ch) == '\n' ) {
				pathname[i] = 0;
				sflag = lsflag;
				nflag = lnflag;
				yflag = lyflag;
				rflag = lrflag;
				check();
				i = -1;
			}
	}
/*
 * *** exit the program ***
 */
	exit1(0);
}
/*
 * check the filesystem
 *
 * This is the main controlling routine for the fsck program.
 * It guides the program through the various phases of file system checking.
 * It checks file system (dev), either always answering yes to all
 * questions (yflag) or always answering no to all questions (nflag).
 * If a salvage is to be done (sflag) it prompts the operator for the
 * type of disk that the free list is to be rebuilt for.
 *
 * return:	nothing
 */
check()
{
	register int
		*ap,	/* fast int pointers */
		*bp	/* fast int pointers */
	;

	char	*tmparg;	/* pointer into scratch buffer */

/* +++pc Storage for block numbers
 *	register unsigned int blk;
 */
	register int blk;

	int
		a,	/* temporary */
		b,	/* temporary */
		*cp	/* temporary pointer */
	;

/*
 * *** see if a mounted device ***
 */
	if(stat("/", &buf[0]) == -1 || stat(dev, &buf[1]) == -1) {
		printf("CAN NOT GET STATISTICS %s\n", dev);
		return;
	}
	mountdev = buf[0] == buf[1+6];		/* hot root */
	blkdev = ((buf[1+2]&060000) == 060000);
	chardev = ((buf[1+2]&020000) == 020000);
/*
 * *** is it really a file system ***
 */
	if(blkdev == 0 && chardev == 0) {
		printf("%s IS NOT A BLOCK OR CHARACTER DEVICE\n", dev);
		return;
	}
/*
 * *** open file system ***
 */
	if((diskr = open(dev,0)) == -1) {
		printf("CAN NOT OPEN %s\n",dev);
		return;
	}
	printf("\n%25s",dev);
	if( rflag || (diskw = open(dev,1)) == -1) {
		rflag = 1;
		printf("(NO WRITE)");
	}
	printf("\n");
/*
 * *** setup ***
 */
	bread(&sblock,1,512);
	if((sblock.s_isize+2) > sblock.s_fsize ||
	sblock.s_isize < 0 || sblock.s_isize > 4096) {
		printf("SIZE CHECK: FSIZE %d, ISIZE %d\n",
		sblock.s_fsize, sblock.s_isize);
		return;
	}
	/* +++pc these values not in V6
	 * printf("\tFile Name: %.6s\tVolume Name: %.6s\n\n", sblock.s_fname,
	 *	sblock.s_fpack);
	 */
	imax = 16 * sblock.s_isize;
	fmin = 2+sblock.s_isize;
	fmax = sblock.s_fsize;
	el = dc = dups;
	n_files = n_blks = n_free = *dc = mod = imod = dmod = 0;
	pp = pathname;
	pathname[0] = '/';
	pathname[1] = 0;
/*
 * *** check for mounted file systems ***
 */
/* +++pc we don't have ustat
	if(blkdev) {
		if(ustat(buf[1+6], buf[0]) != -1) {
			printf("WARNING ... FILE SYSTEM IS MOUNTED\n\n");
			mountdev++;
		}
	}
*/
/*
 * *** allocate bit maps and inode list ***
 */
	bitmap = getcore(((fmax>>3)&017777)+1);
	state = getcore((imax/(8/LSTATE))+1);
	lc = getcore(imax+1);
/*
 * *** Phase 1 ***
 */
	printf("Phase 1 - Check Blocks and Sizes\n");

	for(ALLINUM) {
		ginode();
		if(ALLOC) {
			n_files++;
			set(DIR ? DSTATE : FSTATE);
			if(DIR && inode->i_size0) {
				printf("\tDIRECTORY BIGGER THAN 2^16: I = %d\n",
					inum);
				set(CLEAR);
			}
			if((lc[inum]=inode->i_nlink)<=0) {
				printf("\tALLOCATED INODE WITH ZERO LINK COUNT I = %d\n",
					inum);
				set(CLEAR);
			}
			badblk = dupblk = 0;
			a = 1;
			size = 0;
			if(NOTSPL)for(ALLAP)
			if( a >= 0 && (a = pass1(*ap)) == 1 && LARGE)
				a = iblock(*ap,&pass1);
			if(badblk && badblk != MAXBAD)
				printf("\t%6l BAD BLOCKS\tI = %d\n",
				badblk, inum);
			if( get() != CLEAR ) sizechk();
		} else if(inode->i_mode) {
			printf("\tPARTIALLY ALLOCATED INODE I = %d\n", inum);
			for(ap=inode;ap<&inode[1];*ap++ = 0);
			imod++;
		}
	}

/*
 * *** Phase 1b ***
 */
	if(dc==dups) goto phase2;

	printf("Phase 1b - Rescan for more DUPS\n");

	a = 1;
	for(ALLINUM)
	if(get()) {
		ginode();
		if(NOTSPL)for(ALLAP)
		if( a >= 0 && (a = pass1b(*ap)) == 1 && LARGE)
			 a = iblock(*ap,&pass1b);
		if( a == -1) break;
	}
/*
 * *** Phase 2 ***
 */

phase2:
	printf("Phase 2 - Check Pathnames\n");

	inum = 1;
	lc[1]++;
	descend();
	if(dmod) getblk(0);
/*
 * *** Phase 3 ***
 */
	printf("Phase 3 - Check Reference Counts\n");

	for(ALLINUM)
	switch(get()) {
	case FSTATE:
		if(lc[inum]) adj();
		continue;
	case DSTATE:
	case CLEAR:
		clri();
	}
	if(imod) {
		inum=imax+NINODE;
		ginode();
	}
	free(lc);
	free(state);

/*
 * *** Phase 4 ***
 */
	if (sflag) {
		printf("Phase 4 - Check Free List (IGNORED)\n");
		goto salvage;
	}

	printf("Phase 4 - Check Free List\n");

	freebit = getcore( a = ((fmax >> 3) & 017777) + 1);
	cp = &freebit[a];
	bp = bitmap;
	for (ap = freebit; ap <= cp; ) *ap++ = *bp++;
	fixfree = dupfree = badfree = 0;
	while( blk = frechk()) {
		if (pass4(blk) == -1)
			break;
		n_free++;
	}
	free(freebit);
	if( badfree ) printf("\t%d BAD BLOCKS IN FREE LIST\n", badfree);
	if ( dupfree ) printf("\t%d DUPLICATE BLOCKS IN FREE LIST\n", dupfree);
	if( fixfree == 0 && (n_blks+n_free) != (fmax-fmin)) {
		printf("%d BLOCK(S) MISSING\n",fmax-fmin-n_blks-n_free);
		fixfree++;
	}
	/* +++pc - don't have s_tfree in the superblock
	if(fixfree == 0 && n_free != sblock.s_tfree) {
		printf("\tSUPER BLOCK COUNT OF FREE BLOCKS INCORRECT\tFIX? ");
		if(reply()) {
			bread(&sblock,1,512);
			sblock.s_tfree = n_free;
			bwrite(&sblock,1,512);
			sync();
		}
	} */
	/* +++pc - set our copy of sblock.s_tfree - sb_tfree */
	sb_tfree = n_free;

	if(fixfree && rflag == 0) {
		printf("\tSALVAGE FREE LIST? ");
		sflag = reply();
		if(sflag) {
			printf("\tENTER DEVICE TYPE? ( 3, 4, X:X ) ");
			tmparg = pathname ;	/* +++rx array, not its address */
			while( (*tmparg++ = getchar()) != '\n');
			*--tmparg = 0;
			stype(pathname);
		}
	}
/*
 * *** Phase 5 ***
 */

salvage:
	if(sflag==0 || rflag) goto statistic;

	printf("Phase 5 - Salvage Free List\n");

	makefree();
	/* +++ if we had s_tfree it would be zero here
        n_free = sblock.s_tfree;
	* we are computing the value
	*/
	n_free = sb_tfree;

/*
 * *** statistics ***
 */

statistic:
	printf("%d files\t%d blocks\t %d free\n",
	n_files,n_blks,n_free);
	free(bitmap);
	close(diskw);
	close(diskr);
	if(mod && mountdev) {
		printf("\n*****BOOT UNIX(NO SYNC!)*****\n");
		for(;;);
	}
	if(mod)printf("\n*****FILE SYSTEM WAS MODIFIED*****\n");
}
/*
 * Pass 1 check for bad or duplicated blocks
 *
 * Check each block number to make sure it is:
 *     1. within the range of the file system
 *     2. not allocated to another file
 * If it is not within range of the file system, mark the inode for
 * clearing and return with a zero value.  If it is already allocated
 * add it to the list of duplicate blocks.  If it is multiply duplicated
 * add it to the list of multiply duplicated blocks.  In either case return
 * a one value.  If the number of bad or duplicated blocks is in
 * excess of that which is allowed, query the operator for
 * confirmation before continueing.  If the operator replies "no"
 * terminate the program.
 * If it passes both tests, increment the number of blocks seen (size),
 * update the last block seen (lblock), and return with a value of one.
 *
 * blk	unsigned int containing the block number to check.
 *
 * return:	1 if the block number is within the file system or
 *		  it has already been seen.
 *		0 if the block number is bad.
 *		-1 excessive duplicate or bad blocks seen and operator
 *		   indicated to continue.
 */
pass1(blk)
/* +++ pc unsigned int	blk; */
int	blk;
{
	register int a,b,*ip;

	if(blk<fmin || blk>=fmax) {
		set(CLEAR);
		badblk++;
		if( badblk == MAXBAD ) {
			printf("\tEXCESSIVE BAD BLOCKS I = %d\tCONTINUE? ", inum);
			if(reply()) return(-1); else exit1(1);
		}
		return(0);
	}
	if(bitmap[a=((blk>>3)&017777)]&(b=(1<<(blk&07)))) {
		blkerr("DUP",blk);
		if(dupblk == MAXDUP) {
			printf("\tEXCESSIVE DUPLICATE BLOCKS\tCONTINUE? ");
			if(reply()) return(-1); else exit1(1);
		}
		if(el >= &dups[DUPTABLESIZE] && dupexcess == 0) {
			dupexcess++;
			printf("\tEXCESSIVE DUPS\tCONTINUE? ");
			if(reply()) return(-1); else exit1(1);
		}
		if(dupexcess == 0) {
			ip = dups;
			while(ip<dc)
			if(*ip++ == blk) {
				*el++ = blk;
				return(1);
			}
			*el++ = *dc;
			*dc++ = blk;
		}
	} else {
		bitmap[a] |= b;
		n_blks++;
	}
	size++;
	lblock = blk;
	return(1);
}
/*
 * Pass 1b rescan for more duplicate blocks
 *
 * Pass 1b will look at block number (blk) and see if it is among
 * the duplicate block list.  If it is it is added to the list of
 * multiply duplicate blocks and an error message is produced.
 *
 * blk	unsigned int containing the block number to check.
 *
 * return:	1 if the block number is in range of the filesystem
 *		0 if the block is out of range
 *		-1 found all the blocks we are looking for
 */
pass1b(blk)
/* +++pc unsigned int	blk; */
int blk;
{
	register int *ip;

	if(blk<fmin || blk>=fmax) return(0);
	ip = dups;
	while(ip<dc)
	if(*ip++ == blk) {
		blkerr("DUP",blk);
		*--ip = *--dc;
		*dc = blk;
		if(dc == dups) return(-1);
		break;
	}
	return(1);
}
/*
 * pass2 descend down the file structure checking for bad files
 *
 * Append to the current pathname the directory entry.  Try to descend
 * down it if it is a subdirectory.  If this file is associated with a
 * bad or duplicated block, or if it points to an unallocated
 * inode; the operator is prompted for confirmation whether to remove
 * this directory entry.  If the operator concurs the entry is removed.
 *
 * blk	unsigned int containing the block number of the directory we
 *	are checking
 *
 * return:	always 1
 */
pass2(blk)
/* +++pc unsigned int	blk; */
int blk;
{
	register struct DE *dp;
	register char *c;

	dp = buf;
	do {
		getblk(blk);
		if(inum = dp->dnum) {
			c = &dp->dname[0];
			while((*pp = *c++) && pp++ && c<&dp->dname[14]);
			*pp = 0;
			if(descend()) {
				getblk(blk);
				dmod = 1;
				dp->dnum = 0;
			}
			pp = name;
		}
	} while(++dp<&buf[256]);
	return(1);
}
/*
 * pass3 removing blocks from the bitmap
 *
 * If the block number (blk) is in range it is removed from the block
 * bitmap and possibly from the duplicated block list.
 * The count of the number of blocks seen is also decremented.
 *
 * blk	unsigned int containing the block number that is being removed

 *
 * return:	0 if the block number is not in range
 *		1 if the block number was in range and was removed
 */
pass3(blk)
/* +++pc unsigned int	blk; */
int blk;
{
	register int a,b,*ip;

	if(blk<fmin || blk>=fmax) return(0);
	if(bitmap[a=((blk>>3)&017777)]&(b=1<<(blk&07))) {
		ip = dups;
		while(ip<el)
		if(*ip++ == blk) {
			*--ip = *--el;
			goto ret;
		}
		bitmap[a] &= ~ b;
		n_blks--;
	}
ret:
	return(1);
}
/*
 * Pass 4 check for bad or duplicated blocks in the free list
 *
 * Check each block number to make sure it is:
 *	1. within the range of the file system
 *	2. not allocated to another file
 * If it is not within the range of the filesystem increment the bad block
 * count (badfree) and indicate an error in the free list (fixfree).
 * If the block is already allocated increment the duplicate block count
 * (dupfree), indicate an error in the free list (fixfree) and add it to
 * the list of duplicated blocks.  If the number of duplicated blocks or bad
 * blocks exceed that which is allowed, issue an error message and query the
 * operator to see if he wishes to proceed.  If the operator does not wish to
 * proceed return with a negative one value to stop checking the free list.
 *
 * blk	unsigned int containing the block number to check.
 *
 * return:	1 to continue checking the free list
 *		-1 to stop checking the free list due to too many duplicate
 *		  blocks, or too many bad blocks.
 */
pass4(blk)
/* +++pc unsigned int	blk; */
int blk;
{
	register int a,b,*ip;

	if(blk<fmin || blk>=fmax) {
		fixfree++;
		badfree++;
		if( badfree > MAXBAD ) {
			printf("\tEXCESSIVE BAD BLOCKS IN FREE LIST\tCONTINUE? ");
			if(reply()) return(-1); else exit1(1);
		}
		return(1);
	}
	if(freebit[a=((blk>>3)&017777)]&(b=(1<<(blk&07)))) {
		dupfree++;
		fixfree++;
		if( el >= &dups[DUPTABLESIZE] && dupexcess == 0) {
			dupexcess++;
			printf("\tEXCESSIVE DUPS IN FREE LIST\tCONTINUE? ");
			if(reply()) return(-1); else exit1(1);
		}
		*el++ = blk;
	} else freebit[a] |= b;
	return(1);
}
/*
 * Get an inode in core
 *
 * If the inode (inum) is in core return with a pointer for the
 * structure inode.  If the set of inodes in core have been
 * modified since they were read in write them back out to the
 * filesystem.  Read in the new set of inodes returning a pointer
 * the the inode (inum) requested.
 *
 * return: A pointer to the structure inode
 */
struct inode *ginode()	/* +++rx spell out the return type */
{
	static struct inode ibuf[16];
	static int cib;
	register int ib;

	if((ib=2+(inum-1)/16)!=cib) {
		if(imod && (rflag==0)) {
			bwrite(ibuf, cib, 512);
			imod = 0;
			mod = 1;
		}
		bread(ibuf, cib=ib, 512);
	}
	return(inode = &ibuf[(inum-1)%16]);
}
/*
 * indirect block processing routine
 *
 * Reads in a block (blk) and processes each entry in that block by function
 * (func).  This function is typically invoked for the files that are
 * large files.
 *
 * blk	block number to read all the indirect blocks from
 * func	function used to process the blocks read from (blk)
 *
 * return:	value returned by function (func)
 */
iblock(blk,func)
/* +++pc unsigned int	blk; */
int blk;
int	(*func)();
{
	register int *ap;
	int a;

	ap = buf;
	do {
		getblk(blk);
		if(*ap)
			if( ( a = (*func)(*ap) ) < 0) break;
	} while(++ap<&buf[256]);
	return(a);
}
/*
 * block errors
 *
 * Print out the block number (blk), the type of block error (s),
 * and the inode number (inum).  Mark the inode containing the bad
 * block as being unallocated.
 *
 * s	char pointer pointing to the type of error ( BAD, or DUP )
 * blk	unsigned int containing the number of the offending block
 *
 * return: nothing returned
 */
blkerr(s,blk)
/* +++pc unsigned int	blk; */
int blk;
char	*s;
{
 	printf("\t%6l %s I = %d\n",blk,s,inum);
	set(CLEAR);
}
/*
 * possibly clear a bad inode
 *
 * Prompt the operator for confirmation to clear a bad inode.  This inode
 * may be considered bad for several reasons:
 *	1. unreferenced
 *	2. bad or duplicate block
 * The inode may be a directory or it may be a file.  If the operator confirms
 * removal of the inode all blocks that it uses are removed from the
 * block bitmap and possibly from the duplicate block list.
 *
 * return:	nothing
 */
clri()
{
	register int *ap;

	ginode();
	printf("\t%s %s",
	((inode->i_nlink==0)||(get()!=CLEAR))? "UNREFERENCED":"BAD/DUP",
	DIR? "DIRECTORY":"FILE");
	pinode();
	printf("SIZE = %s\tCLEAR? ", locv(inode->i_size0, inode->i_size1));
	if(reply()) {
		n_files--;
		if(NOTSPL)for(ALLAP)
		if(pass3(*ap) && LARGE)
			iblock(*ap,&pass3);
		for(ap=inode;ap<&inode[1];*ap++ = 0);
		imod = 1;
	}
}
/*
 * adjust the link count
 *
 * Something is wrong with the link count for a file.  Under operator
 * confirmation the link count is fixed, if the link count indicated
 * an inode is no longer allocated then it is cleared.
 *
 * return:	nothing
 */
adj()
{
	ginode();
	if(inode->i_nlink==lc[inum])
		clri();
	else {
		printf("\tLINK COUNT %s",
		DIR? "DIRECTORY":"FILE");
		pinode();
		printf("SIZE = %s\tADJUST? ", locv(inode->i_size0, inode->i_size1));
		if(reply()) {
			inode->i_nlink -= lc[inum];
			imod = 1;
		}
	}
}
/*
 * descend down through all the directories and subdirectories
 *
 * Descend down from the root directory into all the subdirectories.
 * Each inode's state is checked.  If it is a directory, each file
 * is check to see whether it is a subdirectory.  If it is then that
 * directory is processed.  If it is a file or a
 * directory the link count for that inode is decremented.  A check
 * is made for directory pointers to unallocated inodes or inodes
 * that contain bad or duplicate blocks.  With operator concurrence
 * these directory entries are removed.
 *
 * return:	0 if directory entry is OK or operator does not confirm
 *		  removal of this directory entry
 *		1 if directory entry should be removed
 */
descend()
{
	register int *ip,*ap;
	int a[8];
	extern int pass2();
	char *lname;

	if(inum>imax)
		return(direrr("I OUT OF RANGE"));
again:
	switch(get()) {
	case DSTATE:
		set(FSTATE);
		lc[inum]--;
		ip = &ginode()->i_addr[0];
		for(ap=a;ap<&a[8];) *ap++ = *ip++;
		*pp++ = '/';
		lname = name;
		name = pp;
		if(LARGE) {
			for(ap=a;ap<&a[8];ap++)
			if(*ap) iblock(*ap,&pass2);
		} else {
			for(ap=a;ap<&a[8];ap++)
			if(*ap) pass2(*ap);
		}
		name = lname;
		*--pp = 0;
		return(0);
	case FSTATE:
		lc[inum]--;
		return(0);
	case 0:
		return(direrr("UNALLOCATED"));
	case CLEAR:
		if(direrr("DUP/BAD")) return(1);
		ginode();
		set(DIR ? DSTATE : FSTATE );
		goto again;
	}
}
/*
 * something is wrong with a directory entry
 *
 * Print an error message about a directory entry, printing
 * the error message (s), inode number (inum), the pathname thus far
 * (pathname), the owner, the mode, and the date of last modification.
 * Direrr then prompts the user for permission to remove
 * this directory entry.
 *
 * s	char pointer pointing to the error message
 *
 * return:	0 if we should not remove this directory entry
 *		1 if we should remove this directory entry
 */
direrr(s)
char	*s;
{
	printf("\t%s", s);
	pinode();
	printf("%s\tREMOVE? ", pathname);
	return(reply());
}
/*
 * find the reply to a question
 *
 * If the yes flag (yflag) is on print the word "yes" and return
 * with a value of 1.  If the no flag (nflag) is on print the word
 * "no" and return a value of 0.  If neither of the flags are set
 * wait for the user to respond with a character.  If the
 * character is a "y" return with a 1, otherwise return
 * with a 0.
 *
 * return:	1 for a yes response
 *		0 for a no response
 */
reply()
{
	register char	c;

	if(nflag) {
		printf(" no\n");
		return(0);
	}
	if(yflag) {
		printf(" yes\n");
		return(1);
	}
	c = getchar();
	while(c != '\n' && getchar() != '\n');
	printf("\n");
	if(c == 'y')
		return(1);
	else
		return(0);
}
/*
 * input one character from standard input
 *
 * Read one character from standard input (0) and return that character.
 *
 * return: A character that was input.
 */
getchar()
{
	char c;

	if(read(0,&c,1) != 1) return(0);
	else return(c);
}
/*
 * +++rx buffered input for the checklist file
 *
 * This libc has no stdio.  fopen and getc on the struct buf declared at the
 * top -- V6's stdio buffer, fd plus a 512 byte block -- are the whole of what
 * fsck asks of it.
 */
fopen(name, bp)
char	*name;
struct	buf *bp;
{
	if((bp->fildes = open(name, 0)) < 0)
		return(-1);
	bp->nleft = 0;
	bp->nextp = bp->buff;
	return(0);
}

getc(bp)
struct	buf *bp;
{
	if(bp->nleft <= 0) {
		if((bp->nleft = read(bp->fildes, bp->buff, 512)) <= 0)
			return(EOF);
		bp->nextp = bp->buff;
	}
	bp->nleft--;
	return(*bp->nextp++ & 0377);
}
/*
 * read, write, and seek error message routine
 *
 * Prints an error message reflecting the type of error (s), the
 * filesystem being looked at (dev), and the block number the error
 * occured on (b).
 *
 * s	char pointer to the type of error (read, write, seek)
 * b	unsigned int containing the block number of the offense
 *
 * return: if the -n option is on or the operator responds 'no'
 *         then this routine does not return
 *         otherwise no return value.
 */
rwerr(s,b)
char	*s;
/* +++pc unsigned int	b; */
int b;
{
	printf("\n\tCAN NOT %s: %s\tBLOCK %d\tCONTINUE? ",s,dev,b);
	if(reply() == 0) exit1(1);
}
/*
 * get a block into core
 *
 * If the block we want is in core just return.  If the block in core
 * has been modified since we read it in (dmod is true) write it
 * back out to the filesystem.  Read in the block we want and
 * leave it in (buf).
 *
 * blk	unsigned int containing the block number to get
 *
 * return:	nothing
 */
getblk(blk)
/* +++pc unsigned int	blk; */
int blk;
{
	/* +++pc static unsigned int cb; */
	static int cb;

	if(blk==cb) return;
	if(dmod && (rflag==0)) {
		bwrite(buf, cb, 512);
		dmod = 0;
	}
	bread(buf, cb=blk, 512);
}
/*
 * set the inode state
 *
 * Set the current inode (inum) state in the inode
 * state table (state) to the value requested (s).
 *
 * s	new state for inode (inum)
 *
 * return: nothing
 */
set(s)
{
	register char *sp;
	register mi;

	mi = inum;
	sp = &state[mi>>2];
	mi &= 03;
	*sp &= ~m[mi];
	*sp |= s<< (mi*LSTATE);
}

/*
 * get inode state
 *
 * Get the current inode (inum) state from the inode state table (state)
 * and return that value.
 *
 * return:	integer containing the state of the current inode.
 */
get()
{
	register mi;
	register char *sp;

	mi = inum;
	sp = &state[mi>>2];
	mi &= 03;
	return((*sp >> (mi*LSTATE)) & CLEAR);
}

/*
 * read a block
 *
 * Read the first (count) bytes of block (blk) into buffer (buf).
 *
 * buf	char pointer pointing to the area we will read the data into
 * blk	unsigned int containing the block number to be read
 * count int specifying the number of bytes to read
 *
 * return:	nothing
 */
/* +++pc unsigned int	blk; */
/* +++pc C is complaining that buf is redeclared, so rename buf to bufp */
bread(bufp, blk, count)
void	*bufp;		/* +++rx callers pass inode, super block and int buffers */
int 	blk;
int	count;
{
	if (seek(diskr, blk, 3)<0)
		rwerr("SEEK", blk);
	if (read(diskr, bufp, count) != count)
		rwerr("READ", blk);
}

/*
 * write a block
 *
 * Write the first (count) bytes of buffer (buf) into block (blk) of disk
 * filesystem (diskw).
 *
 * buf	char pointer pointing to the buffer to be written
 * blk	unsigned int containing the block number to be written to
 * count int containing the number of bytes in buf to be written
 *
 * return:	nothing
 */
/* +++pc unsigned int	blk; */
/* +++pc C is complaining that buf is redeclared, so rename buf to bufp */
bwrite(bufp, blk, count)
void	*bufp;		/* +++rx callers pass inode, super block and int buffers */
int	 blk;
int	count;
{
	if (seek(diskw, blk, 3) < 0)
		rwerr("SEEK", blk);
	if (write(diskw, bufp, count) != count)
		rwerr("WRITE", blk);
	mod = 1;
}

/*
 * get core for bitmap usage
 *
 * Get (n) bytes of core from the system for use as temporary bitmap
 * buffer area.  If space is available a char pointer to the area gotten
 * is returned, if not an error message is issued and program
 * execution is terminated.
 *
 * n	unsigned int containing the number of bytes to get
 *
 * return:	if space is available then a char pointer to the area
 *		gotten is returned.  If not an error message is issued
 *		and program execution is terminated.
 */
/* +++pc use char * here for arithmetic
 * register unsigned n; */
/* +++rx a pointer is not an integer to this compiler: n is the byte count */
char *getcore(n)
uint n;
{
	register char *p, *p1;

 	if( n&077 ) n = (n&0177700) + 64;
	p = p1 = alloc(n);
	if (p == 0) {
		printf("\tCAN NOT GET ENOUGH CORE\n");
		exit1(1);
	}
	do {
		*p++ = 0;
	} while (--n);
	return(p1);
}

/*
 * +++rx V6's core allocator is alloc/free; here it is malloc/free (ulib.c),
 * and free is already the same call fsck makes.
 */
char *alloc(n)
uint n;
{
	return(malloc(n));
}

/*
 * +++rx long output conversion (V6 libc locv)
 *
 * Render the 32-bit value (hi:lo) as decimal text and return a pointer to
 * it; fsck prints file sizes, which are the inode's 24-bit i_size0:i_size1
 * pair, with %s.  This printf has no long conversion, so the division is
 * done a word at a time with libc's ldiv, as ed does it.
 */
char *locv(hi, lo)
int hi, lo;
{
	static char lbuf[12];
	register char *p;
	uint h, l, q, rem;

	h = hi & 0377;
	l = lo;
	p = &lbuf[11];
	*p = 0;
	do {
		q = ldiv(0, h, 10, &rem);
		l = ldiv(rem, l, 10, &rem);
		h = q;
		*--p = rem + '0';
	} while (h || l);
	return(p);
}

/*
 * +++rx owner name (V6 libc getpw)
 *
 * getpw copied the /etc/passwd line for a uid into the caller's buffer;
 * this system has no password file, so hand back the uid itself.  fsck
 * only wants the field up to the first colon.
 */
getpw(uid, buf)
int uid;
char *buf;
{
	register char *p;

	p = locv(0, uid & 0377);
	while (*p)
		*buf++ = *p++;
	*buf++ = ':';
	*buf = 0;
	return(0);
}
/*
 * type of salvage
 *
 * Scan the array of characters (argv) to find out what type
 * of device we are trying to do a salvage for.
 * 4 ... rp04
 * 3 ... rp03
 * Blocks-per-cylinder:Blocks-to-skip (for everything else)
 * return the number of blocks per cylinder in (cylsiz) and
 * the number of blocks to skip in (stepsize).
 *
 * return:	nothing
 */
stype(argv)
char	*argv;
{
	if (argv[1] == 0) {
		if (*argv == '3') {
			cylsiz = 200;
			stepsize = 5;
			return;
		}
		if (*argv == '4') {
			cylsiz = 418;
			stepsize = 9;
			return;
		}
	}
	if ((cylsiz = number(argv)) <= 0)
		goto error;
	while(*argv != ':')
		if (*argv++ == 0) goto error;
	++argv;
	if ((stepsize = number(argv)) <= 0) goto error;
	if (stepsize >= cylsiz) goto error;
	return;
error:
	cylsiz = stepsize = 1;
	return;
}
/*
 * check the free list
 *
 * Take block numbers out of the superblock and return them to the caller.
 * When the free list in the superblock is exhausted bring in the next
 * 100 free blocks from the indirect block pointer.  If the free list
 * contains a zero block, or the number of entries in the indirect
 * free block is greater than 100 or less than 0 return a zero.
 *
 * return:	0 bad free list
 *		 otherwise return the block number
 */
frechk()
{
	register b, i;

	i = --sblock.s_nfree;
	if (i<0 || i>=100) {
		printf("\tBAD FREEBLOCK COUNT\n");
		fixfree++;
		return(0);
	}
	b = sblock.s_free[i];
	if (b == 0)
		return(0);
	if (sblock.s_nfree <= 0) {
		bread(buf, b, 512);
		sblock.s_nfree = buf[0];
		for(i=0; i<100; i++)
			sblock.s_free[i] = buf[i+1];
	}
	return(b);
}
/*
 * build a free list
 *
 * Add block number (blk) to the superblock. If the superblock is full
 * write the freelist into an indirect block (blk) and start a new superblock
 * freelist.
 *
 * return:	nothing
 */
bldfree(blk)
/* +++pc unsigned int	blk; */
/* +++rx int, as everywhere else a block number is passed */
int blk;
{
	register i;

	if (sblock.s_nfree >= 100) {
		buf[0] = sblock.s_nfree;
		for(i=0; i<100; i++)
			buf[i+1] = sblock.s_free[i];
		sblock.s_nfree = 0;
		bwrite(buf, blk, 512);
	}
	sblock.s_free[sblock.s_nfree++] = blk;
	/* +++pc: don't have this
	   but we need the count
	if (blk) sblock.s_tfree++;  */

	if (blk) sb_tfree++;

}
/*
 * ASCII decimal to binary number conversion
 *
 * Convert a string of ASCII decimal characters into a binary number.
 * Stopping criteria is the first non-numeric character found.  The
 * converted integer is returned.
 *
 * as	char pointer to the string of bytes to be converted
 *
 * return:	the binary value of the string of characters
 */
number(as)
char *as;
{
	register n, c;
	register char *s;

	s = as;
	n = 0;
	while ((c = *s++) >= '0' && c <= '9') {
		n = n*10+c-'0';
	}
	return(n);
}

/*
 * make a new free list
 *
 * Using the bitmap (bitmap) previously created, create a new free block
 * list.  This list is to be spaced by skipping every
 * (stepsize) blocks with a cylinder size (cylsiz).  It
 * also updates all the other statistics necessary.
 *
 * return:	nothing
 */
makefree()
{
	/* +++rx block numbers are ints here, not the char * V6 used for
	 * unsigned arithmetic */
	int	k,first,last;
	register i,j;
	int	cyl;
	int	x,y,z;

	sblock.s_nfree = 0;
	sblock.s_ninode = 0;
	sblock.s_flock = 0;
	sblock.s_ilock = 0;
	sblock.s_fmod = 0;
	/* +++pc: don't have these
        sblock.s_tfree = 0;
	sblock.s_tinode = n_files;
	reset out value
	*/
	sb_tfree = 0;

	bldfree(0);
	last = sblock.s_fsize-1;
	first = sblock.s_isize+2;
	cyl = 0;
	while (((cyl += cylsiz) <= last) && (cyl>=cylsiz));
	do {
		cyl -= cylsiz;
		i = cylsiz - stepsize;
		x = 0;
		y = -1;
		while(1) {
			k = i + cyl;
			if (k<=last && k>=first)
			{
				j = k;
				if ((bitmap[(j>>3)&017777] & (1<<(j&07)))==0)
					bldfree(j);
			}
			if ((i -= stepsize) < 0) i += cylsiz;
			x++;
			if (i == cylsiz - stepsize)
				if (y == -1) {
					z = x;
					y = (cylsiz / z) - 1;
				}
			if (z == x) {
				x = 0;
				if (y-- == 0) break;
				i--;
			}
		}
	}
	while (cyl != 0);
	bwrite(&sblock, 1, 512);
}
/*
 * check a file size
 *
 * See if the number of blocks we saw is the same as the number of
 * blocks the system thinks there are.  If the two don't agree issue
 * an error message and mark the inode for clearing.  If they are the same
 * and the inode is a directory make sure the size is a multiple of 16.
 * If not also issue an error message.  Finally if everything is ok
 * make sure that there are no allocated directory entries beyond
 * the size of a directory.
 *
 * return:	nothing
 */
sizechk()
{
	int
		tsize,	/* number of blocks in file */
		a,	/* temporary */
		dsize	/* what directory size should be */
	;

	register	struct	DE	*dp;
	/* pc+++ added to simplify code below */
	char *cp;
	int dist;

	tsize = ((inode->i_size1>>9)&0177) | (inode->i_size0<<7);
	if(a = (inode->i_size1 & 0777)) tsize++;
	if(tsize>8)
		tsize += (tsize+255)/256;
	if(size != tsize) {
		printf("\tSYSTEM THINKS SIZE IS %d BLOCKS, ACTUAL SIZE IS %d BLOCKS\tI = %d\n",
			tsize, size, inum);
		set(CLEAR);
		return;
	}
	if(DIR && a) {
		if(a&017) {
			printf("\tDIRECTORY SIZE NOT 16 BYTE ALLIGNED\tI = %d\n",
				inum);
			set(CLEAR);
			return;
		}
		dsize = 0;
		getblk(lblock);
		for(dp = &buf[(a>>1)&0377]; dp < &buf[256]; dp++)
			/* +++pc The compiler hates this saying Expression syntax
			   perhaps it doesn't do casts
			if(dp->dnum)
				dsize = (inode->i_size1&0177000) +
					(char *) dp - (char *) &buf[0] + 16; */
			if(dp->dnum) {
				cp = dp;
				dist = cp - &buf[0] + 16;
				dsize = (inode->i_size1&0177000) + dist;
				}
		if(dsize) {
			printf("\tDIRECTORY SIZE INCORRECT\tI = %d\tBLOCK = %d\tFIX? ",
				inum, lblock);
			if(reply()) {
				inode->i_size1 = dsize;
				imod++;
			} else set(CLEAR);
		}
	}
}
/*
 * print information for an inode
 *
 * Print the inode number (inum), the owner, the mode, and
 * the last modification date.
 *
 * return:	nothing
 */
pinode()
{
	register char *ip;
	char
		uidbuf[200],	/* user id buffer */
		*timebuf	/* pointer to last modify time */
	;

	ginode();
	getpw(inode->i_uid, uidbuf);
	for(ip = uidbuf ; *ip++ != ':' ;);
	*--ip = 0;
	timebuf = ctime(inode->i_mtime);
	printf(" I = %d OWNER = %s MODE = %o\n", inum, uidbuf, inode->i_mode);
	if(inode->i_mtime[0] < year)
		printf("\t\tMTIME = %-7.7s %-4.4s ", timebuf+4, timebuf+20);
	else
		printf("\t\tMTIME = %-12.12s ", timebuf+4);
}

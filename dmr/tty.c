/*
 * general TTY subroutines
 */
#include "os.h"

void sgtty(int *v);
int canon(struct tty *atp);
void ttyoutput(int ac, struct tty *tp);
int ttrstrt(int atp);

/*
 * Input mapping table-- if an entry is non-zero, when the
 * corresponding character is typed preceded by "\" the escape
 * sequence is replaced by the table value.  Mostly used for
 * upper-case only terminals.
 */
char    maptab[] =
{
    000,000,000,000,004,000,000,000,
    000,000,000,000,000,000,000,000,
    000,000,000,000,000,000,000,000,
    000,000,000,000,000,000,000,000,
    000,'|',000,'#',000,000,000,'`',
    '{','}',000,000,000,000,000,000,
    000,000,000,000,000,000,000,000,
    000,000,000,000,000,000,000,000,
    '@',000,000,000,000,000,000,000,
    000,000,000,000,000,000,000,000,
    000,000,000,000,000,000,000,000,
    000,000,000,000,000,000,'~',000,
    000,'A','B','C','D','E','F','G',
    'H','I','J','K','L','M','N','O',
    'P','Q','R','S','T','U','V','W',
    'X','Y','Z',000,000,000,000,000,
};

char partab[] = {
    0001,0201,0201,0001,0201,0001,0001,0201,
    0202,0004,0003,0205,0005,0206,0201,0001,
    0201,0001,0001,0201,0001,0201,0201,0001,
    0001,0201,0201,0001,0201,0001,0001,0201,
    0200,0000,0000,0200,0000,0200,0200,0000,
    0000,0200,0200,0000,0200,0000,0000,0200,
    0000,0200,0200,0000,0200,0000,0000,0200,
    0200,0000,0000,0200,0000,0200,0200,0000,
    0200,0000,0000,0200,0000,0200,0200,0000,
    0000,0200,0200,0000,0200,0000,0000,0200,
    0000,0200,0200,0000,0200,0000,0000,0200,
    0200,0000,0000,0200,0000,0200,0200,0000,
    0000,0200,0200,0000,0200,0000,0000,0200,
    0200,0000,0000,0200,0000,0200,0200,0000,
    0200,0000,0000,0200,0000,0200,0200,0000,
    0000,0200,0200,0000,0200,0000,0000,0201
};

/*
 * The actual structure of a clist block manipulated by
 * getc and putc (mch.s)
 */
struct cblock {
    struct cblock *c_next;
    char info[6];
};

/* The character lists-- space for 6*NCLIST characters */
struct cblock cfree[NCLIST];
/* List head for unused character blocks. */
struct cblock *cfreelist;

/*
 * structure of device registers for KL, DL, and DC
 * interfaces-- more particularly, those for which the
 * SSTART bit is off and can be treated by general routines
 * (that is, not DH).
 */
struct klreg {
    int ttrcsr;
    int ttrbuf;
    int tttcsr;
    int tttbuf;
};

/*
 * The routine implementing the gtty system call.
 * Just call lower level routine and pass back values.
 */
void gtty(void)
{
    int v[3];
    int up, *vp;

    vp = v;
    sgtty(vp);
    if (u.u_error)
        return;
    up = u.u_arg[0];
    suword(up, *vp++);
    suword(up+2, *vp++);
    suword(up+4, *vp++);
}

/*
 * The routine implementing the stty system call.
 * Read in values and call lower level.
 */
void stty(void)
{
    int up;

    up = u.u_arg[0];
    u.u_arg[0] = fuword(up);
    u.u_arg[1] = fuword(up+2);
    u.u_arg[2] = fuword(up+4);
    sgtty(0);
}

/*
 * Stuff common to stty and gtty.
 * Check legality and switch out to individual
 * device routine.
 * v  is 0 for stty; the parameters are taken from u.u_arg[].
 * c  is non-zero for gtty and is the place in which the device
 * routines place their information.
 */
void sgtty(int *v)
{
    register struct file *fp;
    register struct inode *ip;

    if ((fp = getf(u.u_ar0[R0])) == NULL)
        return;
    ip = fp->f_inode;
    if ((ip->i_mode&IFMT) != IFCHR) {
        u.u_error = ENOTTY;
        return;
    }
    (*cdevsw[major(ip->i_addr[0])].d_sgtty)(ip->i_addr[0], v);
}

/*
 * Wait for output to drain, then flush input waiting.
 */
void wflushtty(struct tty *atp)
{
    register struct tty *tp;

    tp = atp;
    spl5();
    while (tp->t_outq.c_cc) {
        tp->t_state |= ASLEEP;
        sleep(&tp->t_outq, TTOPRI);
    }
    flushtty(tp);
    spl0();
}

/*
 * Initialize clist by freeing all character blocks, then count
 * number of character devices. (Once-only routine)
 */
void cinit(void)
{
    register int ccp;
    register struct cblock *cp;
    register struct cdevsw *cdp;

    ccp = cfree;
    for (cp=(ccp+07)&~07; cp <= &cfree[NCLIST-1]; cp++) {
        cp->c_next = cfreelist;
        cfreelist = cp;
    }
    ccp = 0;
    for(cdp = cdevsw; cdp->d_open; cdp++)
        ccp++;
    nchrdev = ccp;
}

/*
 * flush all TTY queues
 */
void flushtty(struct tty *atp)
{
    register struct tty *tp;
    register int sps;

    tp = atp;
    while (getc(&tp->t_canq) >= 0);
    while (getc(&tp->t_outq) >= 0);
    wakeup(&tp->t_rawq);
    wakeup(&tp->t_outq);
    sps = getps();
    spl5();
    while (getc(&tp->t_rawq) >= 0);
    tp->t_delct = 0;
    setps(sps);
}

/*
 * transfer raw input list to canonical list,
 * doing erase-kill processing and handling escapes.
 * It waits until a full line has been typed in cooked mode,
 * or until any character has been typed in raw mode.
 */
int canon(struct tty *atp)
{
    register char *bp;
    char *bp1;
    register struct tty *tp;
    register int c;

    tp = atp;
    spl5();
    while (tp->t_delct==0) {
        if ((tp->t_state&CARR_ON)==0)
            return(0);
        sleep(&tp->t_rawq, TTIPRI);
    }
    spl0();
loop:
    bp = &canonb[2];
    while ((c=getc(&tp->t_rawq)) >= 0) {
        if (c==0377) {
            tp->t_delct--;
            break;
        }
        if ((tp->t_flags&RAW)==0) {
            if (bp[-1]!='\\') {
                if (c==tp->t_erase) {
                    if (bp > &canonb[2])
                        bp--;
                    continue;
                }
                if (c==tp->t_kill)
                    goto loop;
                if (c==CEOT)
                    continue;
            } else
            if (maptab[c] && (maptab[c]==c || (tp->t_flags&LCASE))) {
                if (bp[-2] != '\\')
                    c = maptab[c];
                bp--;
            }
        }
        *bp++ = c;
        if (bp>=canonb+CANBSIZ)
            break;
    }
    bp1 = bp;
    bp = &canonb[2];
    c = &tp->t_canq;
    while (bp<bp1)
        putc(*bp++, (struct clist *)c);
    return(1);
}

/*
 * Place a character on raw TTY input queue, putting in delimiters
 * and waking up top half as needed.
 * Also echo if required.
 * The arguments are the character and the appropriate
 * tty structure.
 */
void ttyinput(int ac, struct tty *atp)
{
    register int t_flags, c;
    register struct tty *tp;

    tp = atp;
    c = ac;
    t_flags = tp->t_flags;
    if ((c &= 0177) == '\r' && t_flags&CRMOD)
        c = '\n';
    if ((t_flags&RAW)==0 && (c==CQUIT || c==CINTR)) {
        signal(tp, c==CINTR? SIGINT:SIGQIT);
        flushtty(tp);
        return;
    }
    if (tp->t_rawq.c_cc>=TTYHOG) {
        flushtty(tp);
        return;
    }
    if (t_flags&LCASE && c>='A' && c<='Z')
        c += 'a'-'A';
    putc(c, &tp->t_rawq);
    if (t_flags&RAW || c=='\n' || c==004) {
        wakeup(&tp->t_rawq);
        if (putc(0377, &tp->t_rawq)==0)
            tp->t_delct++;
    }
    if (t_flags&ECHO) {
        ttyoutput(c, tp);
        ttstart(tp);
    }
}

/*
 * put character on TTY output queue, adding delays,
 * expanding tabs, and handling the CR/NL bit.
 * It is called both from the top half for output, and from
 * interrupt level for echoing.
 * The arguments are the character and the tty structure.
 */
void ttyoutput(int ac, struct tty *tp)
{
    register int c;
    register struct tty *rtp;
    register char *colp;
    int ctype;

    rtp = tp;
    c = ac&0177;
    /*
     * Ignore EOT in normal mode to avoid hanging up
     * certain terminals.
     */
    if (c==004 && (rtp->t_flags&RAW)==0)
        return;
    /*
     * Turn tabs to spaces as required
     */
    if (c=='\t' && rtp->t_flags&XTABS) {
        do
            ttyoutput(' ', rtp);
        while (rtp->t_col&07);
        return;
    }
    /*
     * for upper-case-only terminals,
     * generate escapes.
     */
    if (rtp->t_flags&LCASE) {
        colp = "({)}!|^~'`";
        while(*colp++)
            if(c == *colp++) {
                ttyoutput('\\', rtp);
                c = colp[-2];
                break;
            }
        if ('a'<=c && c<='z')
            c += 'A' - 'a';
    }
    /*
     * turn <nl> to <cr><lf> if desired.
     */
    if (c=='\n' && rtp->t_flags&CRMOD)
        ttyoutput('\r', rtp);
    if (putc(c, &rtp->t_outq))
        return;
    /*
     * Calculate delays.
     * The numbers here represent clock ticks
     * and are not necessarily optimal for all terminals.
     * The delays are indicated by characters above 0200,
     * thus (unfortunately) restricting the transmission
     * path to 7 bits.
     */
    colp = &rtp->t_col;
    ctype = partab[c];
    c = 0;
    switch (ctype&077) {

    /* ordinary */
    case 0:
        (*colp)++;

    /* non-printing */
    case 1:
        break;

    /* backspace */
    case 2:
        if (*colp)
            (*colp)--;
        break;

    /* newline */
    case 3:
        ctype = (rtp->t_flags >> 8) & 03;
        if(ctype == 1) { /* tty 37 */
            if (*colp)
                c = max((*colp>>4) + 3, 6);
        } else
        if(ctype == 2) { /* vt05 */
            c = 6;
        }
        *colp = 0;
        break;

    /* tab */
    case 4:
        ctype = (rtp->t_flags >> 10) & 03;
        if(ctype == 1) { /* tty 37 */
            c = 1 - (*colp | ~07);
            if(c < 5)
                c = 0;
        }
        *colp |= 07;
        (*colp)++;
        break;

    /* vertical motion */
    case 5:
        if(rtp->t_flags & VTDELAY) /* tty 37 */
            c = 0177;
        break;

    /* carriage return */
    case 6:
        ctype = (rtp->t_flags >> 12) & 03;
        if(ctype == 1) { /* tn 300 */
            c = 5;
        } else
        if(ctype == 2) { /* ti 700 */
            c = 10;
        }
        *colp = 0;
    }
    if(c)
        putc(c|0200, &rtp->t_outq);
}

/*
 * Restart typewriter output following a delay
 * timeout.
 * The name of the routine is passed to the timeout
 * subroutine and it is called during a clock interrupt.
 */
int ttrstrt(int atp)
{
    register struct tty *tp;

    tp = atp;
    tp->t_state &= ~TIMEOUT;
    ttstart(tp);

    return 0;
}

/*
 * Start output on the typewriter. It is used from the top half
 * after some characters have been put on the output queue,
 * from the interrupt routine to transmit the next
 * character, and after a timeout has finished.
 * If the SSTART bit is off for the tty the work is done here,
 * using the protocol of the single-line interfaces (KL, DL, DC);
 * otherwise the address word of the tty structure is
 * taken to be the name of the device-dependent startup routine.
 */
void ttstart(struct tty *atp)
{
    register struct klreg *addr;
    int c, s;
    register struct tty *tp;

    tp = atp;
    addr = tp->t_addr;
    /*
     * Block the UART transmit interrupt across the DONE test, the getc and the
     * THR write.  ttstart runs both from the top half (ttwrite's final call, at
     * spl0) and from the tx interrupt; without this a tx interrupt landing
     * between the DONE check and uart_putc double-pulls t_outq and overwrites
     * the THR (FIFO off), losing a character -- the intermittent console drop,
     * easy to hit under the VMM where the emulated UART finishes a byte at once.
     */
    s = getps();
    spl5();
    if (tp->t_state&SSTART)
        goto out;                       /* (*addr.func)(tp); */
    if ((addr->tttcsr&DONE)==0 || tp->t_state&TIMEOUT)
        goto out;
    if ((c=getc(&tp->t_outq)) >= 0) {
        if (c<=0177) {
          #ifdef KL_BACKEND_UART
            addr->tttbuf = c | (partab[c]&0200);
            addr->tttcsr &= ~DONE;
            uart_putc(c);
          #else
            do bios_putc(c);
            while ((c=getc(&tp->t_outq)) >= 0);
          #endif
        } else {
            timeout(ttrstrt, (int)tp, c&0177);
            tp->t_state |= TIMEOUT;
        }
    }
out:
    setps(s);
}

/*
 * Called from device's read routine after it has
 * calculated the tty-structure given as argument.
 * The pc is backed up for the duration of this call.
 * In case of a caught interrupt, an RTI will re-execute.
 */
void ttread(struct tty *atp)
{
    register struct tty *tp;

    tp = atp;
    if ((tp->t_state&CARR_ON)==0)
        return;
    if (tp->t_canq.c_cc || canon(tp))
        while (tp->t_canq.c_cc && passc(getc(&tp->t_canq))>=0);
}

/*
 * Called from the device's write routine after it has
 * calculated the tty-structure given as argument.
 */
void ttwrite(struct tty *atp)
{
    register struct tty *tp;
    register int c;

    tp = atp;
    if ((tp->t_state&CARR_ON)==0)
        return;
    while ((c=cpass())>=0) {
        spl5();
        while (tp->t_outq.c_cc > TTHIWAT) {
            ttstart(tp);
            tp->t_state |= ASLEEP;
            sleep(&tp->t_outq, TTOPRI);
        }
        spl0();
        ttyoutput(c, tp);
    }
    ttstart(tp);
}

/*
 * Common code for gtty and stty functions on typewriters.
 * If v is non-zero then gtty is being done and information is
 * passed back therein;
 * if it is zero stty is being done and the input information is in the
 * u_arg array.
 */
int ttystty(struct tty *tp, int *av)
{
    register int *v;
    register char *bv;

    if(v = av) {
        *v++ = tp->t_speeds;
        bv = (char *)v;
        bv[0] = tp->t_erase;
        bv[1] = tp->t_kill;
        v[1] = tp->t_flags;
        return(1);
    }
    wflushtty(tp);
    v = u.u_arg;
    tp->t_speeds = *v++;
    bv = (char *)v;
    tp->t_erase = bv[0];
    tp->t_kill = bv[1];
    tp->t_flags = v[1];
    return(0);
}

int getc(struct clist *pList)
{
    int ret = -1;
    struct cblock *pBlock = NULL;
    int s;

    s = getps();
    spl5();
    if(pList->c_cf == NULL)
    {
        pList->c_cl = NULL;
        setps(s);
        return ret;
    }

    ret = *((unsigned char *)pList->c_cf);
    pList->c_cf++;
    if(--pList->c_cc == 0)
    {
        pBlock = (struct cblock *)((pList->c_cf-1) & ~0x7);
        pList->c_cf = pList->c_cl = NULL;
    }
    else if(pList->c_cf % sizeof(struct cblock) == 0)
    {
        pBlock = (struct cblock *)((pList->c_cf-1) & ~0x7);
        pList->c_cf = pBlock->c_next->info;
    }
    if(pBlock != NULL)
    {
        pBlock->c_next = cfreelist;
        cfreelist = pBlock;
    }
    setps(s);
    return ret;
}

int putc(char c, struct clist *pList)
{
    struct cblock *pBlock = NULL;
    char *p;
    int s;

    s = getps();
    spl5();
    if(pList->c_cl == NULL)
    {
        pBlock = cfreelist;
        if(pBlock == NULL)
        {
            goto error;
        }
        cfreelist = cfreelist->c_next;
        pBlock->c_next = NULL;
        pList->c_cl = pBlock->info;
        pList->c_cf = pBlock->info;
    }
    else if(pList->c_cl%8 == 0)
    {
        pBlock = cfreelist;
        if(pBlock == NULL)
        {
            goto error;
        }
        cfreelist = cfreelist->c_next;
        ((struct cblock *)(pList->c_cl-8))->c_next = pBlock;
        pBlock->c_next = NULL;
        pList->c_cl = pBlock->info;
    }

    *((char *)pList->c_cl) = c;
    pList->c_cl++;
    pList->c_cc++;
    setps(s);
    return 0;
error:
    printk("ERROR cfreelist is NULL");
    setps(s);
    return -1;
}

/* What the files of wifi/lx/src know of one another. */
#ifndef _LX_H_
#define _LX_H_

int	lx_base_init(void);		/* lx_base.c: the thread that counts the ticks and runs the timers */
int	lx_pool_init(void);		/* lx_base.c: the memory the radio reads and writes; 0, or an errno */
long	lx_stub(const char *name);	/* lx_base.c: what a function that is not there yet does */
void	lx_pause_ms(unsigned ms);	/* lx_base.c: sleep */

#endif

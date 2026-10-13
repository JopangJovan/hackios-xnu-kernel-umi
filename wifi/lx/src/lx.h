/* What the files of wifi/lx/src know of one another. */
#ifndef _LX_H_
#define _LX_H_

int	lx_base_init(void);		/* lx_base.c: the thread that counts the ticks and runs the timers */
int	lx_pool_init(void);		/* lx_base.c: the memory the radio reads and writes; 0, or an errno */
long	lx_stub(const char *name);	/* lx_base.c: what a function that is not there yet does */
void	lx_pause_ms(unsigned ms);	/* lx_base.c: sleep */
void	lx_mark(const char *what);	/* lx_base.c: a point reached, said and waited at if the kernel was asked to */
int	lx_station_scan(void);		/* lx_80211.c: an interface in station mode, a scan, what was heard; 0, or an errno */
int	lx_station_join(void);		/* lx_80211.c: join the network the kernel was given; 0 when frames can pass */
void	lx_station_say(void);		/* lx_80211.c: what has passed since */
void	lx_station_leave(void);		/* lx_80211.c: leave it, and the interface down */
int	lx_station_send(const unsigned char *frame, unsigned long length);	/* lx_80211.c: an Ethernet frame for the network */

#endif

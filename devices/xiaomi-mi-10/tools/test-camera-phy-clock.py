from pathlib import Path
import tempfile,subprocess
src=(Path(__file__).resolve().parents[1]/'overlays/xnu/files/bsd/dev/umi_camera_phy.h').read_text()
src=src.split('/* PHY TABLES */')[0]
pre=r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
typedef uintptr_t vm_offset_t;
static struct {vm_offset_t cc;bool front;} uc={.cc=1};
static bool isp_resetting;
static uint32_t regs[0x10000/4], original[0x10000/4];
static unsigned writes, fail_reg; static bool lock_fail;
static uint32_t ur(vm_offset_t p,unsigned o){assert(p==1&&o<0x10000);return regs[o/4];}
static void uw(vm_offset_t p,unsigned o,uint32_t v){assert(p==1&&o<0x10000);writes++; if(o==fail_reg){regs[o/4]=1;return;}regs[o/4]=v;
 if(o==0x38&&v==1&&!lock_fail)regs[0]|=1u<<31;
 if(o>0x1000&&(o==0x7058||o==0xc0f8||o==0xa068||o==0x6060||o==0x6080||o==0xc000||o==0xc01c))regs[o/4]&=~1u;
}
static bool upoll(vm_offset_t p,unsigned o,uint32_t m,uint32_t v,unsigned us){(void)us;return (ur(p,o)&m)==v;}
static void hakios_tl(const char *f,...){(void)f;}
'''
post=r'''
static void reset(void){memset(regs,0,sizeof(regs));regs[4/4]=0x25;regs[0x40/4]=0x1111;regs[0xc/4]=0x2200;memcpy(original,regs,sizeof(regs));writes=0;fail_reg=~0u;lock_fail=false;isp_resetting=true;}
static void clean(void){uphyclocks_down(); /* lock is read-only */ regs[0]&=~(1u<<31);assert(!memcmp(regs,original,sizeof(regs)));}
int main(void){reset();isp_resetting=false;assert(uphyclocks_up()==EBUSY);assert(!writes);reset();regs[0]=1;assert(uphyclocks_up()==EBUSY);assert(!writes);
 reset();assert(uphyclocks_up()==0);assert(regs[0xc004/4]==0x301);assert(regs[0x6064/4]==0x203);assert(regs[4/4]==0x3e);assert(regs[0x40/4]==0x8000);clean();
 reset();uc.front=true;assert(uphyclocks_up()==0);assert(regs[0x6084/4]==0x203&&!regs[0x6064/4]);clean();uc.front=false;
 reset();lock_fail=true;assert(uphyclocks_up()==ETIMEDOUT);clean();
 unsigned cmds[]={0x7058,0xc0f8,0xa068,0x6060,0xc000,0xc01c};for(unsigned i=0;i<6;i++){reset();fail_reg=cmds[i];assert(uphyclocks_up()!=0);fail_reg=~0u;clean();}
 puts("PASS: camera high-rate clocks, occupied PLL refusal, lock/RCG failures and full rollback");}
'''
with tempfile.TemporaryDirectory() as t:
 p=Path(t);(p/'test.c').write_text(pre+src+post);subprocess.run(['cc','-std=c11','-Wall','-Wextra',str(p/'test.c'),'-o',str(p/'test')],check=True);subprocess.run([str(p/'test')],check=True)

from pathlib import Path
import tempfile,subprocess
src=(Path(__file__).resolve().parents[1]/'overlays/xnu/files/bsd/dev/umi_camera_phy.h').read_text()
pre=r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
typedef uintptr_t vm_offset_t;
static struct {vm_offset_t cc;bool front;} uc={.cc=1}; static bool isp_resetting;
static uint32_t regs[0x10000/4], phy[0x4000/4], saved[0x4000/4];static unsigned writes;static bool no_map;static vm_offset_t mapped_phy;
static uint32_t ur(vm_offset_t p,unsigned o){assert(p==1||p==2||p==0x2002);return p==1?regs[o/4]:phy[((p==0x2002?0x2000:0)+o)/4];}
static void uw(vm_offset_t p,unsigned o,uint32_t v){writes++;if(p==1)regs[o/4]=v;else {assert((p==2||p==0x2002)&&o<0x2000);phy[((p==0x2002?0x2000:0)+o)/4]=v;}}
static bool upoll(vm_offset_t p,unsigned o,uint32_t m,uint32_t v,unsigned us){(void)us;return (ur(p,o)&m)==v;}
static vm_offset_t umap(vm_offset_t p,unsigned n){assert(p==0xac70000u&&n==0x4000);mapped_phy=p;return no_map?0:2;}
static void hakios_tl(const char *f,...){(void)f;}
static void ums(unsigned n){(void)n;}
'''
post=r'''
int main(void){no_map=true;assert(uphy_on()==ENOMEM);assert(!writes);no_map=false;phy[0x814/4]=0x2a;assert(uphy_on()==EBUSY);assert(!writes);phy[0x814/4]=0;
 memcpy(saved,phy,sizeof(phy));assert(uphy_on()==0);assert(phy[0x814/4]==0x2a);assert(phy[0x81c/4]==0x52);assert(phy[0x10c/4]==19&&phy[0x30c/4]==19&&phy[0x50c/4]==19);
 uphy_down();assert(!memcmp(phy,saved,sizeof(phy)));unsigned old=writes;uphy_down();assert(writes==old);uc.front=true;assert(uphy_on()==0);assert(mapped_phy==0xac70000&&up_phy==0x2002);assert(phy[0x2814/4]==0xd5&&phy[0x281c/4]==2);
 unsigned settle[]={0x8,0x708,0x208,0x408,0x608};for(unsigned i=0;i<5;i++)assert(phy[(0x2000+settle[i])/4]==19);
 uphy_down();assert(!memcmp(phy,saved,sizeof(phy)));
 puts("PASS: UMI C-PHY three-trio setup, settle19, refusal and rollback");}
'''
with tempfile.TemporaryDirectory() as t:
 p=Path(t);(p/'test.c').write_text(pre+src+post);subprocess.run(['cc','-std=c11',str(p/'test.c'),'-o',str(p/'test')],check=True);subprocess.run([str(p/'test')],check=True)

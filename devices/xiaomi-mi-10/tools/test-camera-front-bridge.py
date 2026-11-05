#!/usr/bin/env python3
"""Exercise actual cdev header/read/write/close code for different camera sizes."""
from pathlib import Path
import re,subprocess,tempfile
r=Path(__file__).resolve().parents[3]
s=(r/'devices/xiaomi-mi-10/overlays/xnu/files/bsd/dev/umi_camera_ios.h').read_text()
pure=s.split('/* DEVICE METHODS */')[0]
methods='\n'.join(re.search(r'static int\n'+name+r'\(.*?\n}',s,re.S).group() for name in ['umi_cam_close','umi_cam_read','umi_cam_write'])
abi=(r/'xnu/files/bsd/sys/hakios.camera.h').read_text()
pre=r"""
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#define __unused __attribute__((unused))
#define Z_WAITOK 0
#define lck_mtx_lock(p) ((void)(p), (change_sensor_on_lock ? (umi_cam_sensor=change_sensor_on_lock,change_sensor_on_lock=0) : 0))
#define lck_mtx_unlock(p) ((void)(p))
typedef int dev_t;typedef long user_ssize_t;typedef char *caddr_t;struct proc {int dummy;};
static uint32_t umi_cam_sensor=1,umi_cam_focus=330,umi_cam_frame_focus=330,umi_cam_lines=1600,umi_cam_gain=128,umi_cam_frame_lines=2373;
static unsigned captures,releases,change_sensor_on_lock;static bool active;
static int umi_cam_lock;static uint64_t umi_cam_sequence;static uint8_t *umi_cam_preview,*raw;
struct uio {long left,pos;uint8_t *data;bool write;};
static long uio_resid(struct uio *u){return u->left;}
static int uiomove(char *p,int n,struct uio *u){assert(n>=0&&n<=u->left);if(u->write)memcpy(p,u->data+u->pos,n);else memcpy(u->data+u->pos,p,n);u->left-=n;u->pos+=n;return 0;}
static void *kalloc_data(unsigned n,int f){(void)f;assert(n>=2120640);return malloc(n);}
static uint64_t mach_absolute_time(void){return 42;}
static int umi_camera_ios_capture(void){captures++;active=true;umi_cam_frame_focus=umi_cam_sensor==2?0xffffffffu:330;return 0;}
static void umi_camera_ios_release(void){releases++;active=false;}
static const uint8_t *umi_camera_sensor_data(void){return raw;}
"""
post=r"""
static int command(unsigned op,unsigned a){struct hakios_camera_command c={.command=op,.a=a};struct uio u={sizeof(c),0,(uint8_t *)&c,true};return umi_cam_write(0,&u,0);}
int main(void){raw=calloc(1,9000000);uint8_t *out=malloc(2120640+sizeof(struct hakios_camera_frame)+16);struct hakios_camera_frame *h=(void *)out;
 active=true;assert(command(HAKIOS_CAMERA_SENSOR,2)==0&&umi_cam_sensor==2&&!active&&releases==1);
 assert(umi_cam_frame_lines==2064);assert(command(HAKIOS_CAMERA_FOCUS,330)==ENOTSUP);
 assert(command(HAKIOS_CAMERA_SENSOR,3)==ENODEV&&umi_cam_sensor==2&&releases==1);
 struct uio u={sizeof(*h),0,out,false};assert(!umi_cam_read(0,&u,0));
 assert(h->sensor==2&&h->width==1296&&h->height==970&&h->stride==1620&&h->focus==HAKIOS_CAMERA_NO_FOCUS&&h->line_ns==16080&&h->frame_lines_min==2064);
 unsigned before=captures;u=(struct uio){sizeof(*h)+1571400-1,0,out,false};assert(umi_cam_read(0,&u,0)==EMSGSIZE&&captures==before&&!u.pos);
 memset(out,0xa5,2120640+sizeof(*h)+16);u=(struct uio){sizeof(*h)+1571400,0,out,false};assert(!umi_cam_read(0,&u,0)&&!u.left);
 for(unsigned i=sizeof(*h)+1571400;i<sizeof(*h)+1571400+16;i++)assert(out[i]==0xa5);
 /* A stale larger rear buffer must see a short read rather than a fabricated padded frame. */
 u=(struct uio){sizeof(*h)+2120640,0,out,false};assert(!umi_cam_read(0,&u,0)&&u.left==2120640-1571400);
 assert(command(HAKIOS_CAMERA_SENSOR,1)==0&&umi_cam_sensor==1&&umi_cam_frame_lines==2373);
 before=captures;u=(struct uio){sizeof(*h)+1571400,0,out,false};assert(umi_cam_read(0,&u,0)==EMSGSIZE&&captures==before);
 u=(struct uio){sizeof(*h)+2120640,0,out,false};assert(!umi_cam_read(0,&u,0)&&h->sensor==1&&h->width==1504&&h->focus==330);
 assert(!umi_cam_close(0,0,0,0)&&!active);
 unsigned old_focus=umi_cam_focus;change_sensor_on_lock=2;assert(command(HAKIOS_CAMERA_FOCUS,400)==ENOTSUP&&umi_cam_focus==old_focus);
 free(raw);free(out);free(umi_cam_preview);
 puts("PASS: front/rear switch, dynamic geometry, fixed front focus, stale-buffer bounds and close cleanup");return 0;
}
"""
with tempfile.TemporaryDirectory() as t:
 p=Path(t);(p/'test.c').write_text(pre+abi+pure+methods+post)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)

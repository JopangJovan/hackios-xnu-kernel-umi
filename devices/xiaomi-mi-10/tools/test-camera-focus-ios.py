#!/usr/bin/env python3
from pathlib import Path
import re,subprocess,tempfile
r=Path(__file__).resolve().parents[3]
s=(r/'devices/xiaomi-mi-10/overlays/xnu/files/bsd/dev/umi_camera_ios.h').read_text()
header=s.split('/* DEVICE METHODS */')[0]
write=re.search(r'static int\numi_cam_write\(.*?\n}',s,re.S).group()
abi=(r/'xnu/files/bsd/sys/hakios.camera.h').read_text()
pre=r"""
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#define __unused __attribute__((unused))
#define ENOTSUP EOPNOTSUPP
#define UID_ROOT 0
#define GID_WHEEL 0
#define mach_absolute_time() 0
#define lck_mtx_lock(p) ((void)(p))
#define lck_mtx_unlock(p) ((void)(p))
typedef int dev_t;
typedef long user_ssize_t;
typedef char *caddr_t;
struct uio {long size;void *data;};
static long uio_resid(struct uio *u){return u->size;}
static int uiomove(char *p,int n,struct uio *u){memcpy(p,u->data,n);return 0;}
static int umi_cam_lock;
static uint32_t umi_cam_lines,umi_cam_gain,umi_cam_frame_lines;
static uint32_t umi_cam_sensor=1;
static uint32_t umi_cam_focus=330,umi_cam_frame_focus;
static void umi_camera_ios_release(void) {}
"""
post=r"""
int main(void){(void)umi_camera_reduce_raw10;
 struct hakios_camera_frame h;
 umi_cam_frame_focus=330;umi_camera_header(&h,1,0,1600,128,2373);assert(h.focus==330);
 umi_cam_frame_focus=HAKIOS_CAMERA_NO_FOCUS;umi_camera_header(&h,2,0,1600,128,2373);assert(h.focus==HAKIOS_CAMERA_NO_FOCUS);
 struct hakios_camera_command c={.command=HAKIOS_CAMERA_FOCUS,.a=700};struct uio u={sizeof(c),&c};
 assert(umi_cam_write(0,&u,0)==0&&umi_cam_focus==700);
 c.a=1024;assert(umi_cam_write(0,&u,0)==EINVAL&&umi_cam_focus==700);
 c.a=0;assert(umi_cam_write(0,&u,0)==0&&umi_cam_focus==0);
 c.a=1023;assert(umi_cam_write(0,&u,0)==0&&umi_cam_focus==1023);
 c.bytes=4;assert(umi_cam_write(0,&u,0)==ENOTSUP);
 return 0;
}
"""
with tempfile.TemporaryDirectory() as t:
 p=Path(t);(p/'test.c').write_text(pre+abi+header+write+post)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS: focus ABI bounds, requested position and confirmed-frame metadata')

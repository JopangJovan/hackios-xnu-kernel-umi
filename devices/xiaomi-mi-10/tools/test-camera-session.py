#!/usr/bin/env python3
from pathlib import Path
import subprocess,tempfile
src=(Path(__file__).resolve().parents[1]/'overlays/xnu/files/bsd/dev/umi_camera_session.h').read_text()
pre=r"""
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <errno.h>
#define HAKIOS_CAMERA_NO_FOCUS 0xffffffffu
static struct {bool active,front;} uc;
static bool umi_cam_session,umi_cam_leaving;
static unsigned starts,stops,captures,focuses,resets,front_captures;
static int fail_start,fail_capture,fail_focus,fail_reset;
static uint32_t umi_cam_sensor=1;
static uint32_t umi_cam_focus=330,umi_cam_frame_focus;
static unsigned isp_ife,isp_csid;
static int usensor_stream(bool on){(void)on;return 0;}
static void hakios_tl(const char *f,...){(void)f;}
static void ucleanup(void){if(uc.active)stops++;uc.active=umi_cam_session=false;}
static int uprobe(unsigned stage){assert(stage==8&&umi_cam_session);starts++;if(fail_start)return EIO;uc.active=true;return 0;}
int uisp_capture_reset(void){resets++;return fail_reset?EIO:0;}
static int ufocus_apply(uint32_t target){assert(target==umi_cam_focus);focuses++;return fail_focus?EIO:0;}
static int umi_camera_sensor_frame(unsigned a,unsigned b,int(*stream)(bool)){(void)a;(void)b;(void)stream;captures++;assert(resets==captures);return fail_capture?EIO:0;}
int umi_camera_front_frame(unsigned a,unsigned b,int(*stream)(bool)){front_captures++;return umi_camera_sensor_frame(a,b,stream);}
"""
post=r"""
static void reset(void){uc.active=uc.front=umi_cam_session=umi_cam_leaving=false;umi_cam_sensor=1;starts=stops=captures=focuses=resets=front_captures=0;fail_start=fail_capture=fail_focus=fail_reset=0;}
int main(void){
 reset();assert(umi_camera_ios_capture()==0&&uc.active&&umi_cam_session&&starts==1&&!stops);
 assert(umi_camera_ios_capture()==0&&starts==1&&captures==1&&focuses==1&&!stops&&umi_cam_frame_focus==330);
 assert(umi_camera_ios_capture()==0&&starts==1&&captures==2&&!stops);
 umi_camera_ios_release();assert(!uc.active&&!umi_cam_session&&stops==1);umi_camera_ios_release();assert(stops==1);
 assert(umi_camera_ios_capture()==0&&starts==2);umi_camera_ios_release();
 reset();fail_start=1;assert(umi_camera_ios_capture()==EIO&&!uc.active&&!umi_cam_session);
 reset();assert(umi_camera_ios_capture()==0);fail_capture=1;assert(umi_camera_ios_capture()==EIO&&!uc.active&&!umi_cam_session&&stops==1);
 reset();assert(umi_camera_ios_capture()==0);fail_focus=1;assert(umi_camera_ios_capture()==0&&umi_cam_frame_focus==HAKIOS_CAMERA_NO_FOCUS);umi_camera_ios_release();
 reset();assert(umi_camera_ios_capture()==0);fail_reset=1;assert(umi_camera_ios_capture()==EIO&&!uc.active&&stops==1&&!captures);
 reset();umi_cam_sensor=2;assert(umi_camera_ios_capture()==0&&uc.front);
 assert(umi_camera_ios_capture()==0&&front_captures==1&&!focuses&&umi_cam_frame_focus==HAKIOS_CAMERA_NO_FOCUS);
 umi_camera_ios_release();assert(!uc.front&&!uc.active&&umi_cam_sensor==2);
 assert(umi_camera_ios_capture()==0&&uc.front);fail_capture=1;assert(umi_camera_ios_capture()==EIO&&!uc.front&&!uc.active);
 reset();umi_cam_leaving=true;assert(umi_camera_ios_capture()==ENXIO&&!starts);
 reset();uc.active=true;assert(umi_camera_ios_capture()==EBUSY&&!starts&&!stops);
 puts("PASS: camera session reuse, close/reopen, capture failure cleanup and shutdown refusal");return 0;
}
"""
with tempfile.TemporaryDirectory() as t:
 p=Path(t);(p/'test.c').write_text(pre+src+post)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)

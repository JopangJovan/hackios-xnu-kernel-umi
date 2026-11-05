#!/usr/bin/env python3
"""Compile the actual board motor backend against bounded I2C fault models."""
from pathlib import Path
import subprocess,tempfile
src=(Path(__file__).resolve().parents[1]/'overlays/xnu/files/bsd/dev/umi_camera_focus.h').read_text()
pre=r"""
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#include <errno.h>
#define HAKIOS_CAMERA_NO_FOCUS 0xffffffffu
static unsigned ops,fail_at,writes,active_steps,waited;
static uint16_t dac;
static uint8_t mode;
static bool corrupt,position_written;
static void hakios_tl(const char *f,...) {(void)f;}
static void ums(unsigned n) {assert(n<=20);waited+=n;}
static bool ufocus_bus_read(uint8_t reg,uint8_t *data,unsigned n) {
 if(++ops==fail_at)return false;
 if(reg==2){assert(n==1);*data=mode;}
 else if(n==1){assert(reg<=1);uint16_t v=(dac<<6)|(corrupt?1:0);*data=reg?v&255:v>>8;}
 else {assert(reg==0&&n==2);uint16_t v=mode==0x20?0xff:(dac<<6)|(corrupt?1:0);data[0]=v>>8;data[1]=v;}
 return true;
}
static bool ufocus_bus_write(uint8_t reg,uint16_t value,unsigned n) {
 if(++ops==fail_at)return false;
 writes++;
 if(reg==2){assert(n==1&&(value==0||value==0x40));mode=value;}
 else {assert(reg==0&&n==2&&!(value&63));if(mode!=0)return false;unsigned next=value>>6;
  assert(next>=150&&next<=750);
  if(position_written){unsigned delta=next>dac?next-dac:dac-next;assert(delta<=24);active_steps++;}
  else {assert(next==150);position_written=true;}
  dac=next;
 }
 return true;
}
"""
post=r"""
static void reset(void){ops=fail_at=writes=active_steps=waited=0;dac=0;mode=0x40;corrupt=position_written=false;ufocus_owned=false;ufocus_position=HAKIOS_CAMERA_NO_FOCUS;}
int main(void){
 reset();assert(ufocus_up(330)==0);assert(mode==0&&dac==343);assert(ufocus_position==343&&active_steps>0);
 unsigned count=ops;
 unsigned before=writes;assert(ufocus_apply(330)==0&&writes==before&&dac==343);
 assert(ufocus_apply(378)==0&&dac==371&&mode==0);
 assert(ufocus_apply(1024)==EINVAL&&dac==371);
 ufocus_down(false);assert(mode==0x40&&dac==150&&!ufocus_owned);
 unsigned old=ops;ufocus_down(false);assert(ops==old);
 reset();mode=0x20;assert(ufocus_up(330)==0&&dac==343);ufocus_down(false);
 reset();assert(ufocus_up(0)==0&&dac==150);ufocus_down(false);
 reset();assert(ufocus_up(1023)==0&&dac==750);ufocus_down(true);assert(mode==0x40&&!ufocus_owned);
 reset();assert(ufocus_up(1024)==EINVAL&&!ops);
 reset();mode=0x23;assert(ufocus_up(330)!=0&&!writes&&!ufocus_owned);
 reset();corrupt=true;assert(ufocus_up(330)!=0&&!ufocus_owned&&mode==0x40);
 for(unsigned n=1;n<=count;n++){reset();fail_at=n;assert(ufocus_up(330)!=0);assert(!ufocus_owned);ufocus_down(false);}
 reset();assert(ufocus_up(330)==0);fail_at=ops+1;ufocus_down(false);assert(mode==0x40&&!ufocus_owned);
 puts("PASS: AK7377 packing, conservative bounds, incremental movement, readback, NACKs and park/standby");
 return 0;
}
"""
with tempfile.TemporaryDirectory() as t:
 p=Path(t);(p/'test.c').write_text(pre+src+post)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)

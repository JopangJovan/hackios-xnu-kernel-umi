#!/usr/bin/env python3
"""Exercise the production Sahara service against a software MHI endpoint."""
from pathlib import Path
import subprocess
import tempfile

device = Path(__file__).resolve().parents[1]
bsd = device / 'overlays/xnu/files/bsd'
source = (bsd / 'dev/umi_mhi.c').read_text()
source = source.replace('#include <libkern/libkern.h>', '#include <string.h>\n#include <stdio.h>')
source = source.replace('#include <sys/systm.h>', '')
model = r'''
#include <assert.h>
static struct umi_mhi h;
static uint32_t channels[128*11], events[3*11], er[3][1024];
static uint32_t tx[512], rx[512], payload[32768], txbuf[8192];
static unsigned produced, sends, sent_data, image_length;
static bool back_to_back, final_image;
static const unsigned char image[65576] = {0x22, 1, 2, 3};
static void sync_noop(void *p, const struct umi_mhi_dma *d) { (void)p;(void)d; }
static void delay_noop(void *p, unsigned us) { (void)p;(void)us; }
static uint32_t read_noop(void *p, uint32_t o) { (void)p;(void)o; return 0x201; }
static int alloc_model(void *p,size_t n,struct umi_mhi_dma *d) {
 (void)p; assert(n <= sizeof(txbuf));
 *d=(struct umi_mhi_dma){txbuf,0x20090000,sizeof(txbuf)}; return 0;
}
static void event_model(uint64_t ptr, uint32_t length, unsigned channel) {
 uint32_t *e=er[1]+(produced++ % 256)*4;
 words_set_address(e,0,ptr);e[2]=(2u<<24)|length;e[3]=(channel<<24)|(0x22u<<16);
 words_set_address(events+11,7,h.event_ring[1].address+(produced%256)*16);
}
static void write_model(void *p,uint32_t o,uint32_t v) {
 (void)p;(void)v;
 if(o != 0x300+2*8) return;
 unsigned slot=(unsigned)((h.sahara_tx_pending-h.sahara_tx_ring.address)/16);
 uint32_t *t=tx+slot*4;
 assert(t[3]==((2u<<16)|(1u<<9)));
 assert(words_address(t,0)==h.sahara_tx_buffer.address);
 sends++;
 int done=sent_data==image_length;
 if(!done) {
  unsigned chunk=image_length-sent_data;
  if(chunk>32768) chunk=32768;
  assert(t[2]==chunk);assert(!memcmp(txbuf,image+sent_data,chunk));sent_data+=chunk;
 } else { assert(t[2]==8);assert(txbuf[0]==5 && txbuf[1]==8); }
 event_model(h.sahara_tx_pending,t[2],2);
 if(sent_data<image_length) return;
 memset(payload,0,48);
 if(!done) {payload[0]=4;payload[1]=16;payload[2]=34;}
 else {payload[0]=6;payload[1]=12;payload[2]=final_image;}
 event_model(h.sahara_rx_ring.address,payload[1],3);
 if(done && back_to_back) {
  uint32_t *hello=payload+8192;
  memset(hello,0,48);hello[0]=1;hello[1]=48;hello[2]=2;hello[3]=1;
  words_set_address(rx+4,0,h.sahara_rx_buffers.address+32768);
  event_model(h.sahara_rx_ring.address+16,48,3);
 }
}
static struct umi_mhi_io io={.context=&h,.dma_start=0x20000000,.dma_end=0x20200000,
 .profile=UMI_MHI_PROFILE_SDX55,.allocate=alloc_model,.sync_for_device=sync_noop,
 .sync_for_cpu=sync_noop,.delay_us=delay_noop,.read32=read_noop,.write32=write_model};
static void reset_model(void) {
 memset(&h,0,sizeof(h));memset(events,0,sizeof(events));memset(er,0,sizeof(er));
 produced=sends=sent_data=0;image_length=40;back_to_back=false;h.io=&io;h.channel_db=0x300;h.event_db=0x700;
 h.channels=(struct umi_mhi_dma){channels,0x20001000,sizeof(channels)};
 h.events=(struct umi_mhi_dma){events,0x20010000,sizeof(events)};
 for(unsigned i=0;i<3;i++) {
  uint64_t a=0x20020000+i*0x1000;
  h.event_ring[i]=(struct umi_mhi_dma){er[i],a,i==1?4096:512};
  h.event_consumer[i]=a;h.event_wp[i]=a+h.event_ring[i].size-16;
  words_set_address(events+i*11,7,a);
 }
 h.sahara_tx_ring=(struct umi_mhi_dma){tx,0x20040000,sizeof(tx)};
 h.sahara_rx_ring=(struct umi_mhi_dma){rx,0x20050000,sizeof(rx)};
 h.sahara_rx_buffers=(struct umi_mhi_dma){payload,0x20060000,sizeof(payload)};
 words_set_address(rx,0,h.sahara_rx_buffers.address);
 h.sahara_tx_wp=h.sahara_tx_ring.address;h.sahara_tx_pending=UINT64_MAX;
 h.sahara_rx_wp=h.sahara_rx_ring.address+64;
 h.sahara_started=h.sahara_received=true;
 h.sahara_rx_bytes=20;h.sahara_rx_words[0]=3;h.sahara_rx_words[1]=20;
 h.sahara_rx_words[2]=34;h.sahara_rx_words[4]=40;
}
static void test_efs_sync(void) {
 static uint8_t shadow[2u<<20], page[4096];
 uint32_t hello[12]={1,48,2,1,48,2}, reply[12]={0};
 uint32_t announce[4]={9,16,0x400000,52}, table[13]={1,0x800000,sizeof(shadow)};
 uint32_t reset[2]={8,8};
 unsigned n=0;
 reset_model();
 h.efs_shadow=shadow;
 memcpy((uint8_t *)table+32,"m9kefs1",8);
 assert(!efs_reply(&h,hello,sizeof(hello),reply,&n) && n==48 && reply[0]==2 && reply[5]==2);
 assert(!efs_reply(&h,announce,sizeof(announce),reply,&n) && reply[0]==10 && reply[2]==announce[2] && reply[3]==52);
 assert(!efs_reply(&h,table,sizeof(table),reply,&n) && reply[2]==table[1]);
 for (unsigned pos=0;pos<sizeof(shadow);pos+=sizeof(page)) {
  assert(n==16 && reply[0]==10 && reply[2]==table[1]+pos && reply[3]==sizeof(page));
  memset(page,(uint8_t)(pos/sizeof(page)+1),sizeof(page));
  assert(!efs_reply(&h,page,sizeof(page),reply,&n) && !memcmp(shadow+pos,page,sizeof(page)));
 }
 assert(n==8 && reply[0]==7);
 /* The RESET response consumes the outstanding receive descriptor. A second
  * memory-debug session must be able to deliver its HELLO on the next slot. */
 static uint32_t ring[EFS_SLOTS*4];
 h.efs_rx_ring=(struct umi_mhi_dma){ring,0x20071000,sizeof(ring)};
 h.efs_rx_buffer=(struct umi_mhi_dma){page,0x20072000,sizeof(page)};
 h.efs_rx_slot=EFS_SLOTS-1;
 memcpy(page,reset,sizeof(reset));
 assert(!efs_receive(&h,h.efs_rx_ring.address+h.efs_rx_slot*16,sizeof(reset)));
 assert(h.efs_rx_slot==0 && h.efs_tx_count==0 && ring[2]==EFS_CHUNK);
 assert(!efs_reply(&h,hello,sizeof(hello),reply,&n) && n==48);
 assert(!efs_reply(&h,announce,sizeof(announce),reply,&n));
 table[1] += 0x400000;
 memcpy((uint8_t *)table+32,"m9kefs2",8);
 assert(!efs_reply(&h,table,sizeof(table),reply,&n) && reply[2]==table[1]);
 for (unsigned pos=0;pos<sizeof(shadow);pos+=sizeof(page)) {
  assert(n==16 && reply[2]==table[1]+pos);
  memset(page,(uint8_t)(pos/sizeof(page)+2),sizeof(page));
  assert(!efs_reply(&h,page,sizeof(page),reply,&n) && !memcmp(shadow+pos,page,sizeof(page)));
 }
 assert(n==8 && reply[0]==7);
 assert(!efs_reply(&h,reset,sizeof(reset),reply,&n) && !n);
 assert(!efs_reply(&h,hello,sizeof(hello),reply,&n));
 assert(!efs_reply(&h,announce,sizeof(announce),reply,&n));
 ((uint8_t *)table)[32]='X';
 assert(efs_reply(&h,table,sizeof(table),reply,&n)!=0 && n==0);
}
int main(void) {
 test_efs_sync();
 reset_model();assert(umi_mhi_sahara_serve_image(&h,34,image,40,10)==0);assert(sends==2);
 assert(!h.sahara_complete);
 reset_model();final_image=true;
 assert(umi_mhi_sahara_serve_image(&h,34,image,40,10)==0 && h.sahara_complete);
 final_image=false;
 reset_model();back_to_back=true;
 assert(umi_mhi_sahara_serve_image(&h,34,image,40,10)==0);
 assert(h.sahara_received && h.sahara_rx_words[0]==1 && h.sahara_rx_bytes==48);
 reset_model();h.sahara_tx_wp=h.sahara_tx_ring.address+127*16;
 assert(umi_mhi_sahara_serve_image(&h,34,image,40,10)==0);assert(sends==2);
 reset_model();image_length=sizeof(image);h.sahara_rx_words[4]=image_length;
 assert(umi_mhi_sahara_serve_image(&h,34,image,sizeof(image),10)==0);assert(sends==4);
 reset_model();h.sahara_rx_words[3]=39;h.sahara_rx_words[4]=2;
 assert(umi_mhi_sahara_serve_image(&h,34,image,40,10)==EINVAL);assert(!sends);
 reset_model();h.sahara_rx_words[2]=35;
 assert(umi_mhi_sahara_serve_image(&h,34,image,40,10)==ENOENT);assert(!sends);
 reset_model();h.sahara_rx_words[4]=0;
 assert(umi_mhi_sahara_serve_image(&h,34,image,40,10)==EINVAL);assert(!sends);
 reset_model();h.sahara_rx_words[0]=0x12;h.sahara_rx_bytes=32;
 h.sahara_rx_words[2]=34;h.sahara_rx_words[3]=0;h.sahara_rx_words[4]=0;
 h.sahara_rx_words[5]=1;h.sahara_rx_words[6]=40;
 assert(umi_mhi_sahara_serve_image(&h,34,image,40,10)==EINVAL);assert(!sends);
 reset_model();h.poisoned=true;
 assert(umi_mhi_sahara_serve_image(&h,34,image,40,10)==EIO);assert(!sends);
 puts("PASS: Sahara image service, DONE handshake, bounds and wrong-ID rejection");
}
'''
with tempfile.TemporaryDirectory(prefix='umi-sahara-test-') as directory:
    test = Path(directory) / 'test.c'
    test.write_text(source + model)
    binary = Path(directory) / 'test'
    subprocess.run(['cc', '-std=gnu11', '-O1', '-fsanitize=address,undefined',
                    '-I', str(bsd), str(test), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

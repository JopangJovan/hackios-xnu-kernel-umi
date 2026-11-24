from pathlib import Path
import subprocess,tempfile
r=Path(__file__).resolve().parents[3];src=(r/'devices/xiaomi-mi-10/overlays/xnu/files/bsd/dev/umi_camera_ios.h').read_text().split('/* DEVICE METHODS */')[0]
abi=(r/'xnu/files/bsd/sys/hakios.camera.h').read_text()
with tempfile.TemporaryDirectory() as t:
 p=Path(t);code='#include <stdint.h>\n#include <stdlib.h>\n#include <string.h>\n#include <assert.h>\n'+abi+'\nstatic uint32_t umi_cam_sensor=1,umi_cam_frame_focus=HAKIOS_CAMERA_NO_FOCUS;\n'+src+'''\nint main(void){struct hakios_camera_frame f;umi_camera_header(&f,7,1234,800,128,2373);assert(!memcmp(f.magic,"RWFRAME1",8));assert(f.header_bytes==sizeof(f));assert(f.width==1504&&f.height==1128&&f.stride==1880&&f.format==1&&f.sensor==1&&f.bayer==1);assert(f.sequence==7&&f.time==1234&&f.exposure_lines==800&&f.gain==128&&f.frame_lines==2373);assert(f.focus==HAKIOS_CAMERA_NO_FOCUS);
uint8_t *raw=malloc(8482560),*small=malloc(2120656);memset(small,0xa5,2120656);
for(unsigned y=0;y<2256;y++)for(unsigned x=0;x<3008;x+=4){unsigned low=0;for(unsigned k=0;k<4;k++){unsigned xx=x+k;unsigned v=xx>=1504?100:((y&1)?((xx&1)?100:500):((xx&1)?500:900));raw[y*3760+x/4*5+k]=v>>2;low|=(v&3)<<(2*k);}raw[y*3760+x/4*5+4]=low;}
umi_camera_reduce_raw10(raw,small);
assert((small[0]*4+(small[4]&3))==900);assert((small[1]*4+((small[4]>>2)&3))==500);assert((small[1880]*4+(small[1884]&3))==500);assert((small[1881]*4+((small[1884]>>2)&3))==100);
assert((small[940]*4+(small[944]&3))==100);for(unsigned i=2120640;i<2120656;i++)assert(small[i]==0xa5);free(raw);free(small);return 0;}''';(p/'test.c').write_text(code);subprocess.run(['cc',str(p/'test.c'),'-o',str(p/'test')],check=True);subprocess.run([str(p/'test')],check=True)
print('PASS: UMI frame protocol dimensions, identity, focus metadata, Bayer phase and output guards')

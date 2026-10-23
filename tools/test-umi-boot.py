#!/usr/bin/env python3
"""Host-only boot packaging checks. Dummy firmware never leaves the temp directory."""
import gzip
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
DTB = ROOT.parent / 'umi.dtb'


class UmiBoot(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='umi-boot-test-')
        self.addCleanup(self.tmp.cleanup)
        self.work = Path(self.tmp.name)
        self.fd = self.work / 'dummy.fd'
        self.fd.write_bytes(b'\xa5' * 0x1000)
        self.payload = self.work / 'payload.bin'
        subprocess.run(['python3', str(ROOT / 'xnuloader/mkpayload.py'),
                        '-o', str(self.payload), '--usb'], check=True, capture_output=True)
        self.env = dict(os.environ, UEFI_TARGET='umi', UEFI_FD=str(self.fd),
                        FD_BASE='0x9fc00000', FD_SIZE='0x1000',
                        PAYLOAD_BASE='0xf0000000', DTB=str(DTB))
        self.out = self.work / 'boot.img'

    def pack(self):
        return subprocess.run(['python3', str(ROOT / 'mkxnuimg.py'),
                               '-p', str(self.payload), '-o', str(self.out)],
                              env=self.env, capture_output=True, text=True)

    def test_missing_firmware_never_uses_redwood(self):
        self.env['UEFI_FD'] = str(self.work / 'missing.fd')
        result = self.pack()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('no redwood fallback', result.stderr)
        self.assertFalse(self.out.exists())

    def test_missing_memory_map_rejected(self):
        self.env.pop('FD_BASE')
        result = self.pack()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('set FD_BASE', result.stderr)

    def test_overlap_rejected(self):
        self.env['PAYLOAD_BASE'] = self.env['FD_BASE']
        self.assertIn('overlaps', self.pack().stderr)

    def test_fd_size_rejected(self):
        self.env['FD_SIZE'] = '0x2000'
        self.assertIn('differs from FD_SIZE', self.pack().stderr)

    def test_redwood_dtb_rejected(self):
        self.env['DTB'] = str(ROOT / 'prebuilt/uefi/redwood.dtb')
        self.assertIn('not the supplied Xiaomi', self.pack().stderr)

    def test_packed_shim_firmware_payload_and_dtb(self):
        result = self.pack()
        self.assertEqual(result.returncode, 0, result.stderr)
        boot = self.out.read_bytes()
        self.assertEqual(boot[:8], b'ANDROID!')
        kernel_size = struct.unpack_from('<I', boot, 8)[0]
        page_size = struct.unpack_from('<I', boot, 36)[0]
        kernel = boot[page_size:page_size + kernel_size]
        dtb = DTB.read_bytes()
        self.assertTrue(kernel.endswith(dtb))
        image = gzip.decompress(kernel[:-len(dtb)])
        self.assertEqual(image[56:60], b'ARM\x64')
        self.assertEqual(struct.unpack_from('<QQ', image, 8), (0x9fc00000, 0x1000))
        fd_at = image.index(self.fd.read_bytes())
        self.assertEqual(fd_at % 16, 0)
        pay_at = fd_at + self.fd.stat().st_size
        payload = self.payload.read_bytes()
        self.assertEqual(image[pay_at:pay_at + len(payload)], payload)

    def test_actual_loader_argument_gate(self):
        source = (ROOT / 'xnuloader/loader.c').read_text()
        def function(name):
            start = source.index('static int\n' + name + '(')
            end = source.index('\n}\n', start) + 3
            return source[start:end]
        harness = r'''#include <stdint.h>
#include <string.h>
#include <assert.h>
#define str_len strlen
static int print(const char *fmt, ...) { (void)fmt; return 0; }
struct platform { int unused; };
static const struct platform platform_umi = {0}, other = {0};
static const struct platform *PLAT = &platform_umi;
struct payload_info { uint64_t forced_base; const char *cmdline; };
'''
        harness += function('starts_with') + function('umi_bootargs_ok')
        harness += r'''
int main(void) {
    struct payload_info p = {0, "-v cpus=1 loader_return_after=300 epoch=1 sb_delay=8"};
    assert(umi_bootargs_ok(&p));
    p.cmdline = ""; assert(umi_bootargs_ok(&p));
    p.cmdline = "bootcpu=0 cpus=8 umi_touch=1 umifts_scan=1"; assert(umi_bootargs_ok(&p));
    p.cmdline = "adsp=1 adsp_umi=1 sound=1"; assert(umi_bootargs_ok(&p));
    p.cmdline = "adsp_umi=1 sound=2 adsp=1"; assert(umi_bootargs_ok(&p));
    p.cmdline = "umi_amp_probe=5"; assert(umi_bootargs_ok(&p));
    const char *audio_bad[] = {"adsp=1 sound=1", "adsp_umi=1 sound=1", "umi_amp_probe=6",
        "adsp=1 adsp_umi=1 sound=3", "adsp=1 adsp_umi=1 sound=4",
        "adsp=1 adsp_umi=1 sound=12", "adsp=1 adsp_umi=1 adsp_step=11 adsp_return=1"};
    for (unsigned i = 0; i < sizeof(audio_bad)/sizeof(audio_bad[0]); ++i) {
        p.cmdline = audio_bad[i]; assert(!umi_bootargs_ok(&p));
    }
    const char *bad[] = {"gpu=1", "-v gpu=1", "wifi_step=1", "ufs=2",
        "ufs_write=1", "touchscreen=1", "cpufreq=1", "adsp=1", "-vfoo", "cam=5"};
    for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i) {
        p.cmdline = bad[i]; assert(!umi_bootargs_ok(&p));
    }
    p.cmdline = "-v"; p.forced_base = 0xc0000000;
    assert(!umi_bootargs_ok(&p));
    PLAT = &other; assert(umi_bootargs_ok(&p));
    return 0;
}
'''
        cfile, binary = self.work / 'args.c', self.work / 'args'
        cfile.write_text(harness)
        subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(cfile), '-o', str(binary)],
                       check=True, capture_output=True)
        subprocess.run([str(binary)], check=True, capture_output=True)

    def test_cs35l41_memory_windows(self):
        source = (ROOT / 'xnu/files/bsd/dev/redwood_speaker.c').read_text()
        start = source.index('static bool\numi_cs35l41_region(')
        end = source.index('\n}\n', start) + 3
        harness = '#include <stdbool.h>\n#include <stdint.h>\n#include <assert.h>\n'
        harness += source[start:end]
        harness += r'''
int main(void) {
    uint32_t reg;
    assert(umi_cs35l41_region(0x10, 0, 18960, false, &reg) && reg == 0x03800000);
    assert(umi_cs35l41_region(0x12, 0, 6132, false, &reg) && reg == 0x02c00000);
    assert(!umi_cs35l41_region(0xf0, 0x2014, 4, false, &reg));
    assert(!umi_cs35l41_region(0x10, 0xffffffff, 20, false, &reg));
    assert(!umi_cs35l41_region(0x11, 0, 4, false, &reg));
    assert(!umi_cs35l41_region(0x12, 0, 6144, false, &reg));
    assert(!umi_cs35l41_region(6, 0x1ff4, 8, true, &reg));
    assert(!umi_cs35l41_region(6, 1, 4, true, &reg));
    return 0;
}
'''
        cfile, binary = self.work / 'cirrus.c', self.work / 'cirrus'
        cfile.write_text(harness)
        subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(cfile), '-o', str(binary)],
                       check=True, capture_output=True)
        subprocess.run([str(binary)], check=True, capture_output=True)

    def test_build_failure_is_failure(self):
        self.env['UEFI_FD'] = str(self.work / 'missing.fd')
        self.env['SKIP'] = 'kernel loader images'
        result = subprocess.run(['bash', 'common/build.sh', 'umi'], cwd=ROOT,
                                env=self.env, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn('boot it with:', result.stdout)


if __name__ == '__main__':
    unittest.main()

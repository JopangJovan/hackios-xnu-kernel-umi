#!/usr/bin/env python3
"""Emit and compare the full production Apple DT for each loader platform on the host."""
from pathlib import Path
import struct
import subprocess
import tempfile

device = Path(__file__).resolve().parents[1]
s = (device / "loader/loader.c").read_text()
writer = s[s.index("struct dt_writer {"):s.index("/* Seconds since 1970")]
writer += s[s.index("static void\ndt_prop_padded"):s.index("/* Platform description")]
platforms = s[s.index("struct platform {"):s.index("/*\n * The devices the loader knows")]
tree = s[s.index("const struct platform *plat = PLAT;"):s.index("/* a tree that did not fit")]
guard = 'if (plat == &platform_umi) {\n\t\tdt_begin(&w, "baseband");'
assert tree.count(guard) == 1
baseline = tree.replace(guard, 'if (0) {\n\t\tdt_begin(&w, "baseband");')
# Stable inputs in place of MMIO, firmware clock and PSCI. Run the actual
# production writer, including unchanged siblings, ranges, CPUs and all parents.
prefix = r"""
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "identity.h"
#define str_len strlen
#define str_eq(a,b) (!strcmp((a),(b)))
#define READ_SYSREG(x) 24000000ULL
#define print(...) ((void)0)
#define PANIC_LOG_BASE 0x12340000ULL
#define PANIC_LOG_SIZE 4096
#define NVRAM_BANK_SIZE 0x4000
#define firmware_epoch() 11
#define psci_call(...) 1
#define PSCI_NOT_SUPPORTED (-1)
#define build_nvram_image(p) ((void)(p))
"""
declarations = r"""
int main(int argc, char **argv) {
    const struct platform *PLAT = !strcmp(argv[1], "umi") ? &platform_umi :
        !strcmp(argv[1], "qemu") ? &platform_qemu_virt : &platform_redwood;
    uint8_t bytes[65536];
    uintptr_t dt_pa = (uintptr_t)bytes, dt_max = sizeof(bytes);
    uint64_t kernel_mpidr=0, this_mpidr=0x700, phys_base=1, win_size=2;
    uint64_t rd_size=16, rd1_size=32, rd_pa=100, rd1_pa=200;
    int camera_requested=argc>2, torch_requested=argc>2, more_cores=1;
"""
end = """
    if (w.overflow || w.depth != 0) return 1;
    return fwrite(bytes, 1, w.cur - bytes, stdout) != (size_t)(w.cur - bytes);
}
"""

def decode(blob):
    nodes = {}
    def walk(offset, parent):
        props, children = struct.unpack_from("<II", blob, offset)
        offset += 8
        fields = {}
        for _ in range(props):
            name, length = struct.unpack_from("<32sI", blob, offset)
            name = name.split(b"\0")[0].decode()
            assert name not in fields
            offset += 36
            fields[name] = blob[offset:offset + length]
            offset += (length + 3) & ~3
        name = fields["name"].rstrip(b"\0").decode()
        path = parent + "/" + name
        assert path not in nodes
        nodes[path] = fields
        for _ in range(children):
            offset = walk(offset, path)
        return offset
    assert walk(0, "") == len(blob)
    return nodes

with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    # Synthetic identity only; the test does not require any files from an IPSW.
    (tmp / "identity.h").write_text("struct identity_prop {const char *name; unsigned offset, length;};\n" +
        "".join(f'static const struct identity_prop identity_{key}[]={{{{"fixture",0,2}}}};\n'
                f'static const unsigned char identity_{key}_data[]="x";\n'
                for key in ("root", "product", "haptics", "camera")))
    for label, body in (("previous", baseline), ("current", tree)):
        src = prefix + writer + platforms + declarations + body.replace(
            "memset((void *)PANIC_LOG_BASE, 0, PANIC_LOG_SIZE);", "/* host has no physical panic area */") + end
        (tmp / "emit.c").write_text(src)
        subprocess.run(["cc", "-std=c11", "-O2", "-Werror", str(tmp / "emit.c"), "-o", str(tmp / label)], check=True)
    for platform in ("umi", "qemu", "redwood"):
        for camera in ([], ["camera"]):
            old, new = [decode(subprocess.check_output([str(tmp / program), platform, *camera]))
                        for program in ("previous", "current")]
            if platform == "umi":
                node = new.pop("/device-tree/arm-io/baseband")
                assert node == {"name": b"baseband\0", "compatible": b"qcom,ext-sdx55m\0",
                                "model": b"Qualcomm SDX55\0", "vendor-id": struct.pack("<I", 0x17cb),
                                "device-id": struct.pack("<I", 0x0306)}
            assert old == new, platform
print("PASS: only UMI gains one Qualcomm baseband node; all other Apple DT nodes and properties match")

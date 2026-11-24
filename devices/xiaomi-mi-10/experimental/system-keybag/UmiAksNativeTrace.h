/* Temporary native keybag ABI tracing. Never dereference/log secrets or pointers. */
#ifndef UMI_AKS_NATIVE_TRACE_H
#define UMI_AKS_NATIVE_TRACE_H
#include <libkern/OSAtomic.h>
static void umi_native_aks_trace(uint32_t sel, const IOExternalMethodArguments *a) {
    int enabled=0;
    if (!PE_parse_boot_argn("umi_aks_lab",&enabled,sizeof(enabled)) || enabled!=1) return;
    if (!(sel==2||sel==3||sel==4||sel==5||sel==6||sel==7||sel==0xc||sel==0xd||
          sel==0xe||sel==0xf||sel==0x11||sel==0x17||sel==0x1f||sel==0x23||sel==0x69)) return;
    char name[32]={0};proc_selfname(name,sizeof(name));
    if (strcmp(name,"keybagd") && strcmp(name,"lockdownd") && strcmp(name,"profiled")) return;
    static volatile SInt32 seen[128];
    if (OSIncrementAtomic(&seen[sel])>=6) return;
    const uint64_t *v=a->scalarInput;
    IOLog("UMI AKS native: caller=%s selector=0x%x scalars=%u/%u structures=%u/%u\n",
          name,sel,a->scalarInputCount,a->scalarOutputCount,a->structureInputSize,a->structureOutputSize);
    if (!v) return;
    if (sel==2 && a->scalarInputCount==6) {
        IOLog("UMI AKS native: create type=%llu mode=%lld secret_empty=%d extension_empty=%d\n",
              v[0],(int64_t)v[1],v[3]==0,v[5]==0);
    } else if (sel==0xf && a->scalarInputCount==11) {
        IOLog("UMI AKS native: change handle=%lld flags=%llu mask=%llu old_empty=%d new_empty=%d extensions_empty=%d\n",
              (int64_t)v[0],v[1],v[2],v[4]==0,v[6]==0,!(v[7]||v[8]||v[9]||v[10]));
    } else if (sel==5 && a->scalarInputCount==4) {
        IOLog("UMI AKS native: install handle=%lld kind=%lld secret_empty=%d\n",(int64_t)v[0],(int64_t)v[1],v[3]==0);
    } else if (a->scalarInputCount && sel!=2 && sel!=6) {
        IOLog("UMI AKS native: handle=%lld\n",(int64_t)v[0]);
    }
}
#endif

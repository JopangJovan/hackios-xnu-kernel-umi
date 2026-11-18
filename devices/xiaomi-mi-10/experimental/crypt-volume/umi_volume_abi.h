#ifndef UMI_VOLUME_ABI_H
#define UMI_VOLUME_ABI_H
#include <stdint.h>
#include <sys/ioccom.h>
#define UV_BYTES (16u*1024u*1024u)
#define UV_SECTOR 4096u
#define UV_RECORD (UV_SECTOR+32u)
#define UV_HEADER 128u
#define UV_IMAGE (UV_HEADER+(UV_BYTES/UV_SECTOR)*UV_RECORD)
/* Test-only: root caller supplies a synthetic master unwrapped by lab AKS. */
struct uv_attach { uint32_t mode; uint32_t reserved; uint8_t master[32]; uint8_t uuid[16]; };
#define UV_ATTACH _IOW('U',120,struct uv_attach)
#define UV_FINISH _IO('U',121)
#define UV_CLEAR _IO('U',122)
#define UV_JOURNAL 8192u
#define UV_LIVE_BYTES (UV_IMAGE+UV_JOURNAL)
struct uv_live_attach { uint8_t master[32],uuid[16]; char path[192]; };
struct uv_live_stats { uint64_t writes,replayed,discarded; };
#define UV_LIVE_ATTACH _IOW('U',123,struct uv_live_attach)
#define UV_LIVE_STATS _IOR('U',124,struct uv_live_stats)
#define UV_LIVE_INJECT _IO('U',125)
#endif

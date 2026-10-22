/* UMI RAM-only XNUP handoff. No filesystem or partition log access. */
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include "UmiXnu.h"
#define UMI_PAYLOAD_BASE 0xF0000000ULL
#define UMI_PAYLOAD_SIZE 0x04000000ULL
VOID
UmiStartPayloadLoader (VOID)
{
  CONST UINT32 *Header = (CONST UINT32 *)(UINTN)UMI_PAYLOAD_BASE;
  CONST UINT8 *Cursor;
  CONST UINT8 *End;
  EFI_HANDLE Image;
  EFI_STATUS Status;
  Print (L"HackiOS UMI UEFI: looking for RAM loader\r\n");
  if (Header[0] != 0x50554e58 || Header[1] > UMI_PAYLOAD_SIZE - 8) {
    Print (L"No XNUP payload at 0xF0000000\r\n");
    return;
  }
  Cursor = (CONST UINT8 *)(Header + 2);
  End = Cursor + Header[1];
  while ((UINTN)(End - Cursor) >= 24) {
    UINT32 Size = *(CONST UINT32 *)(Cursor + 16);
    UINT64 Aligned = ((UINT64)Size + 15) & ~15ULL;
    if (Cursor[0] == 0) return;
    if (Aligned > (UINT64)(End - Cursor - 24)) return;
    if (CompareMem (Cursor, "loader.efi\0", 11) == 0) {
      Status = gBS->LoadImage (FALSE, gImageHandle, NULL, (VOID *)(Cursor + 24), Size, &Image);
      Print (L"UMI LoadImage: %r\r\n", Status);
      if (!EFI_ERROR (Status)) {
        Status = gBS->StartImage (Image, NULL, NULL);
        Print (L"UMI loader returned: %r\r\n", Status);
      }
      return;
    }
    Cursor += 24 + (UINTN)Aligned;
  }
}

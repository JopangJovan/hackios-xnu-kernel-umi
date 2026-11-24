/* UMI RAM-only XNUP handoff. No filesystem or partition log access. */
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include "UmiXnu.h"
#define UMI_PAYLOAD_BASE 0xF0000000ULL
#define UMI_PAYLOAD_SIZE 0x04000000ULL

STATIC
VOID
UmiPayloadDiagnostic (IN CONST CHAR16 *Stage, IN EFI_STATUS Status)
{
  Print (L"HackiOS UMI UEFI handoff: %s (%r)\r\n", Stage, Status);
  Print (L"This message remains for 15 seconds before normal boot.\r\n");
  gBS->Stall (15000000);
}

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
    UmiPayloadDiagnostic (L"no XNUP payload", EFI_NOT_FOUND);
    return;
  }
  Cursor = (CONST UINT8 *)(Header + 2);
  End = Cursor + Header[1];
  while ((UINTN)(End - Cursor) >= 24) {
    UINT32 Size = *(CONST UINT32 *)(Cursor + 16);
    UINT64 Aligned = ((UINT64)Size + 15) & ~15ULL;
    if (Cursor[0] == 0) {
      UmiPayloadDiagnostic (L"no loader.efi entry", EFI_NOT_FOUND);
      return;
    }
    if (Aligned > (UINT64)(End - Cursor - 24)) {
      UmiPayloadDiagnostic (L"malformed XNUP entry", EFI_COMPROMISED_DATA);
      return;
    }
    if (CompareMem (Cursor, "loader.efi\0", 11) == 0) {
      Status = gBS->LoadImage (FALSE, gImageHandle, NULL, (VOID *)(Cursor + 24), Size, &Image);
      Print (L"UMI LoadImage: %r\r\n", Status);
      if (!EFI_ERROR (Status)) {
        Status = gBS->StartImage (Image, NULL, NULL);
        Print (L"UMI loader returned: %r\r\n", Status);
        UmiPayloadDiagnostic (L"loader returned", Status);
      } else {
        UmiPayloadDiagnostic (L"LoadImage failed", Status);
      }
      return;
    }
    Cursor += 24 + (UINTN)Aligned;
  }
  UmiPayloadDiagnostic (L"no loader.efi entry", EFI_NOT_FOUND);
}

/** @file
  Understanding which kexts need which for the picker options editor.

  OpenCore adds kexts in the order of Kernel > Add, and a kext can only be linked with the ones
  which are added before it (or are in the kernel cache already). The kexts a kext needs are
  listed in the OSBundleLibraries key of its Info.plist.

  Copyright (C) 2026, OpenCore contributors. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-3-Clause
**/

#include "OpenCoreEditorInternal.h"

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/OcStringLib.h>

/**
  Identifiers which the system provides.
**/
#define ED_KEXT_APPLE_PREFIX  "com.apple."

STATIC
BOOLEAN
EdKextHasLibrary (
  IN  CONST ED_KEXT_INFO  *Info,
  IN  CONST CHAR8         *Identifier
  )
{
  UINT32  Index;

  for (Index = 0; Index < Info->LibraryCount; ++Index) {
    if (AsciiStrCmp (Info->Libraries[Index], Identifier) == 0) {
      return TRUE;
    }
  }

  return FALSE;
}

/**
  Remember the libraries of one of the OSBundleLibraries dictionaries.
**/
STATIC
EFI_STATUS
EdKextAddLibraries (
  IN OUT ED_KEXT_INFO  *Info,
  IN     XML_NODE      *Dict
  )
{
  UINT32       Index;
  UINT32       Length;
  CONST CHAR8  *Key;
  CHAR8        *Copy;

  if (EditPlistNodeKind (Dict) != EditPlistKindDict) {
    return EFI_SUCCESS;
  }

  for (Index = 0; Index < EditPlistChildCount (Dict); ++Index) {
    Key = EditPlistChildKey (Dict, Index);
    if (  (Key == NULL)
       || (Key[0] == '\0')
       || (AsciiStrnCmp (Key, ED_KEXT_APPLE_PREFIX, L_STR_LEN (ED_KEXT_APPLE_PREFIX)) == 0)
       || EdKextHasLibrary (Info, Key)
       || (Info->LibraryCount >= ED_KEXT_MAX_LIBRARIES))
    {
      continue;
    }

    Length = (UINT32)AsciiStrLen (Key);
    Copy   = AllocatePool ((UINTN)Length + 1);
    if (Copy == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }

    CopyMem (Copy, Key, (UINTN)Length + 1);
    Info->Libraries[Info->LibraryCount++] = Copy;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EdKextParseInfo (
  IN  CONST VOID    *Data,
  IN  UINT32        Size,
  OUT ED_KEXT_INFO  *Info
  )
{
  EFI_STATUS   Status;
  EDIT_PLIST   Plist;
  XML_NODE     *Node;
  CONST CHAR8  *Content;
  UINTN        Length;

  ZeroMem (Info, sizeof (*Info));

  Status = EditPlistLoad (&Plist, Data, Size);
  if (EFI_ERROR (Status)) {
    return EFI_INVALID_PARAMETER;
  }

  Node = EditPlistFindChild (Plist.Root, "CFBundleIdentifier");
  if ((Node != NULL) && (EditPlistNodeKind (Node) == EditPlistKindString)) {
    Content = EditPlistGetContent (&Plist, Node);
    Length  = AsciiStrLen (Content);
    if (Length > 0) {
      Info->Identifier = AllocatePool (Length + 1);
      if (Info->Identifier == NULL) {
        EditPlistFree (&Plist);
        return EFI_OUT_OF_RESOURCES;
      }

      CopyMem (Info->Identifier, Content, Length + 1);
    }
  }

  Info->Libraries = AllocateZeroPool (ED_KEXT_MAX_LIBRARIES * sizeof (*Info->Libraries));
  if (Info->Libraries == NULL) {
    EdKextFreeInfo (Info);
    EditPlistFree (&Plist);
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // OpenCore looks at both on 64-bit.
  //
  Status = EdKextAddLibraries (Info, EditPlistFindChild (Plist.Root, "OSBundleLibraries"));
  if (!EFI_ERROR (Status)) {
    Status = EdKextAddLibraries (Info, EditPlistFindChild (Plist.Root, "OSBundleLibraries_x86_64"));
  }

  EditPlistFree (&Plist);

  if (EFI_ERROR (Status)) {
    EdKextFreeInfo (Info);
    return Status;
  }

  Info->HasInfo = TRUE;
  return EFI_SUCCESS;
}

VOID
EdKextFreeInfo (
  IN OUT ED_KEXT_INFO  *Info
  )
{
  UINT32  Index;

  if (Info->Identifier != NULL) {
    FreePool (Info->Identifier);
  }

  if (Info->Libraries != NULL) {
    for (Index = 0; Index < Info->LibraryCount; ++Index) {
      FreePool (Info->Libraries[Index]);
    }

    FreePool (Info->Libraries);
  }

  ZeroMem (Info, sizeof (*Info));
}

STATIC
BOOLEAN
EdKextIsProvided (
  IN  CONST CHAR8  *Identifier,
  IN  CONST CHAR8  **Provided,
  IN  UINT32       ProvidedCount
  )
{
  UINT32  Index;

  for (Index = 0; (Provided != NULL) && (Index < ProvidedCount); ++Index) {
    if ((Provided[Index] != NULL) && (AsciiStrCmp (Provided[Index], Identifier) == 0)) {
      return TRUE;
    }
  }

  return FALSE;
}

UINT32
EdKextFindProblems (
  IN  CONST ED_KEXT_INFO  *Kexts,
  IN  CONST BOOLEAN       *Enabled,
  IN  UINT32              Count,
  IN  CONST UINT32        *Order,
  IN  CONST CHAR8         **Provided OPTIONAL,
  IN  UINT32              ProvidedCount,
  IN OUT ED_KEXT_PROBLEM  *Problems
  )
{
  UINT32  *Position;
  UINT32  Index;
  UINT32  Kext;
  UINT32  Library;
  UINT32  Other;
  UINT32  Above;
  UINT32  Below;
  UINT32  Off;
  UINT32  Total;

  Total = 0;

  for (Index = 0; Index < Count; ++Index) {
    ZeroMem (&Problems[Index], sizeof (Problems[Index]));
    Problems[Index].Provider = MAX_UINT32;
  }

  Position = AllocatePool (((UINTN)Count + 1) * sizeof (*Position));
  if (Position == NULL) {
    return 0;
  }

  for (Index = 0; Index < Count; ++Index) {
    Position[Order[Index]] = Index;
  }

  for (Kext = 0; Kext < Count; ++Kext) {
    if (!Enabled[Kext]) {
      continue;
    }

    if (!Kexts[Kext].HasInfo || Kexts[Kext].FilesMissing) {
      Problems[Kext].Kind = EdKextNoFiles;
      ++Total;
      continue;
    }

    for (Library = 0; Library < Kexts[Kext].LibraryCount; ++Library) {
      if (EdKextIsProvided (Kexts[Kext].Libraries[Library], Provided, ProvidedCount)) {
        continue;
      }

      //
      // It is enough to have one kext with the identifier in front.
      //
      Above = MAX_UINT32;
      Below = MAX_UINT32;
      Off   = MAX_UINT32;
      for (Other = 0; Other < Count; ++Other) {
        //
        // A kext which OpenCore does not add provides nothing.
        //
        if (  (Other == Kext)
           || (Kexts[Other].Identifier == NULL)
           || Kexts[Other].FilesMissing
           || (AsciiStrCmp (Kexts[Other].Identifier, Kexts[Kext].Libraries[Library]) != 0))
        {
          continue;
        }

        if (!Enabled[Other]) {
          if (Off == MAX_UINT32) {
            Off = Other;
          }
        } else if (Position[Other] < Position[Kext]) {
          Above = Other;
          break;
        } else if (Below == MAX_UINT32) {
          Below = Other;
        }
      }

      if (Above != MAX_UINT32) {
        continue;
      }

      if (Problems[Kext].Kind == EdKextOk) {
        Problems[Kext].Library = Library;
        if (Below != MAX_UINT32) {
          Problems[Kext].Kind     = EdKextOrder;
          Problems[Kext].Provider = Below;
        } else if (Off != MAX_UINT32) {
          Problems[Kext].Kind     = EdKextDisabled;
          Problems[Kext].Provider = Off;
        } else {
          Problems[Kext].Kind = EdKextMissing;
        }
      }

      ++Problems[Kext].LibraryProblems;
    }

    if (Problems[Kext].Kind != EdKextOk) {
      ++Total;
    }
  }

  FreePool (Position);
  return Total;
}

/**
  Where the sorting is at for one kext.
**/
typedef struct {
  UINT32    Kext;
  UINT32    Library;
  UINT32    Scan;
} ED_KEXT_FRAME;

UINT32
EdKextSort (
  IN     CONST ED_KEXT_INFO  *Kexts,
  IN     CONST BOOLEAN       *Enabled,
  IN     UINT32              Count,
  IN OUT UINT32              *Order
  )
{
  UINT8          *State;
  UINT32         *Result;
  ED_KEXT_FRAME  *Stack;
  ED_KEXT_FRAME  *Frame;
  UINT32         Depth;
  UINT32         Done;
  UINT32         Index;
  UINT32         Start;
  UINT32         Other;
  UINT32         Changed;
  BOOLEAN        Descended;

  if (Count < 2) {
    return 0;
  }

  State  = AllocateZeroPool ((UINTN)Count);
  Result = AllocatePool ((UINTN)Count * sizeof (*Result));
  Stack  = AllocatePool ((UINTN)Count * sizeof (*Stack));
  if ((State == NULL) || (Result == NULL) || (Stack == NULL)) {
    if (State != NULL) {
      FreePool (State);
    }

    if (Result != NULL) {
      FreePool (Result);
    }

    if (Stack != NULL) {
      FreePool (Stack);
    }

    return 0;
  }

  Done = 0;

  //
  // Depth first, as the kexts are in the list. Every kext is put out after all it needs,
  // and each is entered once, so a cycle of kexts needing each other ends at some point.
  //
  for (Start = 0; Start < Count; ++Start) {
    if (State[Order[Start]] != 0) {
      continue;
    }

    Depth               = 1;
    Stack[0].Kext       = Order[Start];
    Stack[0].Library    = 0;
    Stack[0].Scan       = 0;
    State[Order[Start]] = 1;

    while (Depth > 0) {
      Frame     = &Stack[Depth - 1];
      Descended = FALSE;

      //
      // A kext that is off needs nothing.
      //
      while (!Descended && Enabled[Frame->Kext] && (Frame->Library < Kexts[Frame->Kext].LibraryCount)) {
        while (!Descended && (Frame->Scan < Count)) {
          Other = Order[Frame->Scan++];
          if (  (Other != Frame->Kext)
             && (State[Other] == 0)
             && Enabled[Other]
             && (Kexts[Other].Identifier != NULL)
             && (AsciiStrCmp (Kexts[Other].Identifier, Kexts[Frame->Kext].Libraries[Frame->Library]) == 0))
          {
            State[Other]         = 1;
            Stack[Depth].Kext    = Other;
            Stack[Depth].Library = 0;
            Stack[Depth].Scan    = 0;
            ++Depth;
            Descended = TRUE;
          }
        }

        if (!Descended) {
          ++Frame->Library;
          Frame->Scan = 0;
        }
      }

      if (!Descended) {
        State[Frame->Kext] = 2;
        Result[Done++]     = Frame->Kext;
        --Depth;
      }
    }
  }

  Changed = 0;
  for (Index = 0; Index < Count; ++Index) {
    if (Order[Index] != Result[Index]) {
      ++Changed;
    }

    Order[Index] = Result[Index];
  }

  FreePool (State);
  FreePool (Result);
  FreePool (Stack);

  return Changed;
}

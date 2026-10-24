/** @file
  OpenCore driver.

Copyright (c) 2019, vit9696. All rights reserved.<BR>
This program and the accompanying materials
are licensed and made available under the terms and conditions of the BSD License
which accompanies this distribution.  The full text of the license may be found at
http://opensource.org/licenses/bsd-license.php

THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

#include <Library/OcMainLib.h>

#include <Guid/AppleVariable.h>
#include <Guid/OcVariable.h>

#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/OcVariableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

STATIC
VOID
OcReportVersion (
  IN OC_GLOBAL_CONFIG  *Config
  )
{
  CONST CHAR8  *Version;

  Version = OcMiscGetVersionString ();

  DEBUG ((DEBUG_INFO, "OC: Current version is %a\n", Version));

  if ((Config->Misc.Security.ExposeSensitiveData & OCS_EXPOSE_VERSION_VAR) != 0) {
    OcSetSystemVariable (
      OC_VERSION_VARIABLE_NAME,
      OPEN_CORE_NVRAM_ATTR,
      AsciiStrLen (Version),
      (VOID *)Version,
      NULL
      );
  } else {
    OcSetSystemVariable (
      OC_VERSION_VARIABLE_NAME,
      OPEN_CORE_NVRAM_ATTR,
      L_STR_LEN ("UNK-000-0000-00-00"),
      "UNK-000-0000-00-00",
      NULL
      );
  }
}

STATIC
VOID
OcDeleteNvram (
  IN OC_GLOBAL_CONFIG  *Config
  )
{
  EFI_STATUS   Status;
  UINT32       DeleteGuidIndex;
  UINT32       AddGuidIndex;
  UINT32       DeleteVariableIndex;
  UINT32       AddVariableIndex;
  CONST CHAR8  *AsciiVariableName;
  CHAR16       *UnicodeVariableName;
  GUID         VariableGuid;
  OC_ASSOC     *VariableMap;
  VOID         *CurrentValue;
  UINTN        CurrentValueSize;
  BOOLEAN      SameContents;

  for (DeleteGuidIndex = 0; DeleteGuidIndex < Config->Nvram.Delete.Count; ++DeleteGuidIndex) {
    Status = OcProcessVariableGuid (
               OC_BLOB_GET (Config->Nvram.Delete.Keys[DeleteGuidIndex]),
               &VariableGuid,
               NULL,
               NULL
               );

    if (EFI_ERROR (Status)) {
      continue;
    }

    //
    // When variable is set and non-volatile variable setting is used,
    // we do not want a variable to be constantly removed and added every reboot,
    // as it will negatively impact flash memory. In case the variable is already set
    // and has the same value we do not delete it.
    //
    for (AddGuidIndex = 0; AddGuidIndex < Config->Nvram.Add.Count; ++AddGuidIndex) {
      if (AsciiStrCmp (
            OC_BLOB_GET (Config->Nvram.Delete.Keys[DeleteGuidIndex]),
            OC_BLOB_GET (Config->Nvram.Add.Keys[AddGuidIndex])
            ) == 0)
      {
        break;
      }
    }

    for (DeleteVariableIndex = 0; DeleteVariableIndex < Config->Nvram.Delete.Values[DeleteGuidIndex]->Count; ++DeleteVariableIndex) {
      AsciiVariableName = OC_BLOB_GET (Config->Nvram.Delete.Values[DeleteGuidIndex]->Values[DeleteVariableIndex]);

      //
      // '#' is filtered in all keys, but for values we need to do it ourselves.
      //
      if (AsciiVariableName[0] == '#') {
        DEBUG ((DEBUG_INFO, "OC: Variable skip deleting %a\n", AsciiVariableName));
        continue;
      }

      UnicodeVariableName = AsciiStrCopyToUnicode (AsciiVariableName, 0);

      if (UnicodeVariableName == NULL) {
        DEBUG ((DEBUG_WARN, "OC: Failed to convert NVRAM variable name %a\n", AsciiVariableName));
        continue;
      }

      if (AddGuidIndex != Config->Nvram.Add.Count) {
        VariableMap = NULL;
        for (AddVariableIndex = 0; AddVariableIndex < Config->Nvram.Add.Values[AddGuidIndex]->Count; ++AddVariableIndex) {
          if (AsciiStrCmp (AsciiVariableName, OC_BLOB_GET (Config->Nvram.Add.Values[AddGuidIndex]->Keys[AddVariableIndex])) == 0) {
            VariableMap = Config->Nvram.Add.Values[AddGuidIndex];
            break;
          }
        }

        if (VariableMap != NULL) {
          Status = GetVariable2 (UnicodeVariableName, &VariableGuid, &CurrentValue, &CurrentValueSize);

          if (!EFI_ERROR (Status)) {
            SameContents = CurrentValueSize == VariableMap->Values[AddVariableIndex]->Size
                           && CompareMem (OC_BLOB_GET (VariableMap->Values[AddVariableIndex]), CurrentValue, CurrentValueSize) == 0;
            FreePool (CurrentValue);
          } else if ((Status == EFI_NOT_FOUND) && (VariableMap->Values[AddVariableIndex]->Size == 0)) {
            SameContents = TRUE;
          } else {
            SameContents = FALSE;
          }

          if (SameContents) {
            DEBUG ((DEBUG_INFO, "OC: Not deleting NVRAM %g:%a, matches add\n", &VariableGuid, AsciiVariableName));
            FreePool (UnicodeVariableName);
            continue;
          }
        }
      }

      Status = gRT->SetVariable (UnicodeVariableName, &VariableGuid, 0, 0, 0);
      DEBUG ((
        EFI_ERROR (Status) && Status != EFI_NOT_FOUND ? DEBUG_WARN : DEBUG_INFO,
        "OC: Deleting NVRAM %g:%a - %r\n",
        &VariableGuid,
        AsciiVariableName,
        Status
        ));

      FreePool (UnicodeVariableName);
    }
  }
}

STATIC
VOID
OcAddNvram (
  IN OC_GLOBAL_CONFIG  *Config
  )
{
  EFI_STATUS  Status;
  UINT32      GuidIndex;
  UINT32      VariableIndex;
  GUID        VariableGuid;
  OC_ASSOC    *VariableMap;

  for (GuidIndex = 0; GuidIndex < Config->Nvram.Add.Count; ++GuidIndex) {
    Status = OcProcessVariableGuid (
               OC_BLOB_GET (Config->Nvram.Add.Keys[GuidIndex]),
               &VariableGuid,
               NULL,
               NULL
               );

    if (EFI_ERROR (Status)) {
      continue;
    }

    VariableMap = Config->Nvram.Add.Values[GuidIndex];

    for (VariableIndex = 0; VariableIndex < VariableMap->Count; ++VariableIndex) {
      OcSetNvramVariable (
        OC_BLOB_GET (VariableMap->Keys[VariableIndex]),
        &VariableGuid,
        Config->Nvram.WriteFlash ? OPEN_CORE_NVRAM_NV_ATTR : OPEN_CORE_NVRAM_ATTR,
        VariableMap->Values[VariableIndex]->Size,
        OC_BLOB_GET (VariableMap->Values[VariableIndex]),
        NULL,
        FALSE
        );
    }
  }
}

//
// Header of the OC_BOOT_ARGS_BACKUP_VARIABLE_NAME variable, followed by the original
// boot-args data and the data they were replaced with.
//
#define OC_BOOT_ARGS_BACKUP_MAGIC  SIGNATURE_32 ('O', 'C', 'B', 'A')

#define OC_BOOT_ARGS_MAX_SIZE  1024U

#pragma pack(push, 1)
typedef struct {
  UINT32    Magic;
  UINT32    OldAttributes;
  UINT32    OldSize;
  UINT32    NewSize;
} OC_BOOT_ARGS_BACKUP;
#pragma pack(pop)

STATIC
EFI_STATUS
OcReadBootArgsBackup (
  OUT OC_BOOT_ARGS_BACKUP  **Backup
  )
{
  EFI_STATUS           Status;
  OC_BOOT_ARGS_BACKUP  *Data;
  UINTN                Size;
  UINT32               Total;

  *Backup = NULL;

  Data   = NULL;
  Status = GetVariable3 (OC_BOOT_ARGS_BACKUP_VARIABLE_NAME, &gOcVendorVariableGuid, (VOID **)&Data, &Size, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (  (Size < sizeof (*Data))
     || (Data->Magic != OC_BOOT_ARGS_BACKUP_MAGIC)
     || (Data->OldSize == 0)
     || (Data->OldSize > OC_BOOT_ARGS_MAX_SIZE)
     || (Data->NewSize > OC_BOOT_ARGS_MAX_SIZE)
     || BaseOverflowTriAddU32 (sizeof (*Data), Data->OldSize, Data->NewSize, &Total)
     || (Total != Size))
  {
    FreePool (Data);
    return EFI_COMPROMISED_DATA;
  }

  *Backup = Data;
  return EFI_SUCCESS;
}

BOOLEAN
OcIsBootArgsReplaced (
  VOID
  )
{
  EFI_STATUS           Status;
  OC_BOOT_ARGS_BACKUP  *Backup;

  Status = OcReadBootArgsBackup (&Backup);
  if (EFI_ERROR (Status)) {
    return FALSE;
  }

  FreePool (Backup);
  return TRUE;
}

VOID
OcRestoreBootArgs (
  VOID
  )
{
  EFI_STATUS           Status;
  OC_BOOT_ARGS_BACKUP  *Backup;
  UINT8                *Original;
  UINT8                *Replacement;
  VOID                 *Current;
  UINTN                CurrentSize;
  UINT32               CurrentAttributes;
  BOOLEAN              Restore;

  Status = OcReadBootArgsBackup (&Backup);
  if (Status == EFI_NOT_FOUND) {
    return;
  }

  if (Status == EFI_COMPROMISED_DATA) {
    //
    // Not something which we can make sense of, do not keep it around.
    //
    DEBUG ((DEBUG_WARN, "OC: Dropping invalid boot-args backup\n"));
    gRT->SetVariable (OC_BOOT_ARGS_BACKUP_VARIABLE_NAME, &gOcVendorVariableGuid, 0, 0, NULL);
    return;
  }

  if (EFI_ERROR (Status)) {
    //
    // The backup cannot be read at the moment, which does not mean it is gone, try again next time.
    //
    DEBUG ((DEBUG_WARN, "OC: Cannot read boot-args backup - %r\n", Status));
    return;
  }

  Original    = (UINT8 *)(Backup + 1);
  Replacement = Original + Backup->OldSize;
  Restore     = FALSE;

  Current = NULL;
  Status  = GetVariable3 (L"boot-args", &gAppleBootVariableGuid, &Current, &CurrentSize, &CurrentAttributes);
  if (Status == EFI_NOT_FOUND) {
    //
    // The replacement was not kept in flash, which is what is expected after a reset.
    //
    Restore = TRUE;
  } else if (!EFI_ERROR (Status)) {
    if (  ((CurrentAttributes & EFI_VARIABLE_NON_VOLATILE) == 0)
       && (CurrentSize == Backup->NewSize)
       && (CompareMem (Current, Replacement, CurrentSize) == 0))
    {
      //
      // The replacement is still there (restoring on request), remove it.
      //
      Status  = gRT->SetVariable (L"boot-args", &gAppleBootVariableGuid, 0, 0, NULL);
      Restore = !EFI_ERROR (Status);
    } else {
      DEBUG ((DEBUG_INFO, "OC: Not restoring boot-args as they were changed meanwhile\n"));
    }

    FreePool (Current);
  }

  if (Restore) {
    Status = gRT->SetVariable (
                    L"boot-args",
                    &gAppleBootVariableGuid,
                    Backup->OldAttributes,
                    Backup->OldSize,
                    Original
                    );
    DEBUG ((EFI_ERROR (Status) ? DEBUG_WARN : DEBUG_INFO, "OC: Restoring boot-args - %r\n", Status));
    if (EFI_ERROR (Status)) {
      //
      // Keep the backup, so that the next start tries again.
      //
      FreePool (Backup);
      return;
    }
  }

  gRT->SetVariable (OC_BOOT_ARGS_BACKUP_VARIABLE_NAME, &gOcVendorVariableGuid, 0, 0, NULL);
  FreePool (Backup);
}

EFI_STATUS
OcSetBootArgs (
  IN CONST CHAR8  *Args,
  IN BOOLEAN      Persistent
  )
{
  EFI_STATUS           Status;
  UINTN                Length;
  VOID                 *Original;
  UINTN                OriginalSize;
  UINT32               OriginalAttributes;
  BOOLEAN              HasOriginal;
  BOOLEAN              HasBackup;
  OC_BOOT_ARGS_BACKUP  *Backup;

  Length = AsciiStrLen (Args);
  if (Length >= OC_BOOT_ARGS_MAX_SIZE) {
    return EFI_INVALID_PARAMETER;
  }

  Original = NULL;
  Status   = GetVariable3 (L"boot-args", &gAppleBootVariableGuid, &Original, &OriginalSize, &OriginalAttributes);
  if (Status == EFI_NOT_FOUND) {
    HasOriginal = FALSE;
  } else if (EFI_ERROR (Status)) {
    return Status;
  } else {
    HasOriginal = TRUE;
    if (OriginalSize > OC_BOOT_ARGS_MAX_SIZE) {
      FreePool (Original);
      return EFI_COMPROMISED_DATA;
    }
  }

  if (Persistent) {
    //
    // The new value is what is wanted from now on, thus there is nothing to put back.
    //
    gRT->SetVariable (OC_BOOT_ARGS_BACKUP_VARIABLE_NAME, &gOcVendorVariableGuid, 0, 0, NULL);

    if (HasOriginal && (OriginalAttributes != OPEN_CORE_NVRAM_NV_ATTR)) {
      //
      // The attributes of a variable cannot be changed in place.
      //
      Status = gRT->SetVariable (L"boot-args", &gAppleBootVariableGuid, 0, 0, NULL);
      if (EFI_ERROR (Status)) {
        FreePool (Original);
        return Status;
      }
    }

    if (HasOriginal) {
      FreePool (Original);
    }

    if (Length == 0) {
      Status = gRT->SetVariable (L"boot-args", &gAppleBootVariableGuid, 0, 0, NULL);
      return Status == EFI_NOT_FOUND ? EFI_SUCCESS : Status;
    }

    return gRT->SetVariable (L"boot-args", &gAppleBootVariableGuid, OPEN_CORE_NVRAM_NV_ATTR, Length, (VOID *)Args);
  }

  //
  // A replacement for a single boot is not kept in flash and goes away with a reset.
  // The variable which is in flash will not, so it has to be remembered before it is removed.
  //
  HasBackup = FALSE;
  if (HasOriginal && ((OriginalAttributes & EFI_VARIABLE_NON_VOLATILE) != 0)) {
    Backup = AllocatePool (sizeof (*Backup) + OriginalSize + Length);
    if (Backup == NULL) {
      FreePool (Original);
      return EFI_OUT_OF_RESOURCES;
    }

    Backup->Magic         = OC_BOOT_ARGS_BACKUP_MAGIC;
    Backup->OldAttributes = OriginalAttributes;
    Backup->OldSize       = (UINT32)OriginalSize;
    Backup->NewSize       = (UINT32)Length;
    CopyMem (Backup + 1, Original, OriginalSize);
    CopyMem ((UINT8 *)(Backup + 1) + OriginalSize, Args, Length);

    Status = gRT->SetVariable (
                    OC_BOOT_ARGS_BACKUP_VARIABLE_NAME,
                    &gOcVendorVariableGuid,
                    OPEN_CORE_NVRAM_NV_ATTR,
                    sizeof (*Backup) + OriginalSize + Length,
                    Backup
                    );
    FreePool (Backup);
    if (EFI_ERROR (Status)) {
      FreePool (Original);
      return Status;
    }

    HasBackup = TRUE;
  }

  if (HasOriginal) {
    Status = gRT->SetVariable (L"boot-args", &gAppleBootVariableGuid, 0, 0, NULL);
    if (EFI_ERROR (Status)) {
      if (HasBackup) {
        gRT->SetVariable (OC_BOOT_ARGS_BACKUP_VARIABLE_NAME, &gOcVendorVariableGuid, 0, 0, NULL);
      }

      FreePool (Original);
      return Status;
    }
  }

  Status = EFI_SUCCESS;
  if (Length > 0) {
    Status = gRT->SetVariable (L"boot-args", &gAppleBootVariableGuid, OPEN_CORE_NVRAM_ATTR, Length, (VOID *)Args);
  }

  if (EFI_ERROR (Status) && HasOriginal) {
    //
    // Do not leave the system without the arguments it had.
    //
    gRT->SetVariable (L"boot-args", &gAppleBootVariableGuid, OriginalAttributes, OriginalSize, Original);
    if (HasBackup) {
      gRT->SetVariable (OC_BOOT_ARGS_BACKUP_VARIABLE_NAME, &gOcVendorVariableGuid, 0, 0, NULL);
    }
  }

  if (HasOriginal) {
    FreePool (Original);
  }

  return Status;
}

VOID
OcLoadNvramSupport (
  IN OC_STORAGE_CONTEXT  *Storage,
  IN OC_GLOBAL_CONFIG    *Config
  )
{
  //
  // If the picker options editor replaced boot-args for the previous boot, put them back first,
  // so that everything below deals with the same state as if the editor was never used.
  //
  OcRestoreBootArgs ();

  OcLoadLegacyNvram (
    Storage,
    &Config->Nvram.Legacy,
    Config->Nvram.LegacyOverwrite,
    Config->Uefi.Quirks.RequestBootVarRouting
    );

  OcDeleteNvram (Config);

  OcAddNvram (Config);

  OcReportVersion (Config);
}

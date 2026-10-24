/** @file
  Options editor of the OpenCore picker: boot arguments and config.plist.

  It is started from the picker with F2 and works in text mode, taking
  the console over for as long as it runs.

  Copyright (C) 2026, OpenCore contributors. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-3-Clause
**/

#include <Library/OcMainLib.h>

#include <Guid/AppleVariable.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/OcFileLib.h>
#include <Library/OcStringLib.h>
#include <Library/PrintLib.h>
#include <Library/ResetSystemLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include "OpenCoreEditorInternal.h"
#include "OpenCoreEditorUi.h"

/**
  Longest boot-args which can be edited.
**/
#define ED_BOOT_ARGS_LENGTH  512U

/**
  GUID under which macOS keeps boot-args, as it is written in the configuration.
**/
#define ED_APPLE_BOOT_GUID_TEXT  "7C436110-AB2A-4BBB-A880-FE41995C9F82"

#define ED_BOOT_ARGS_KEY  "boot-args"

/**
  Deepest nesting of the configuration which can be browsed.
**/
#define ED_MAX_DEPTH  ((UINTN)XML_PARSER_NEST_LEVEL + 2U)

/**
  Widest data which can be edited as hexadecimal text.
**/
#define ED_MAX_DATA_SIZE  256U

/**
  State of the editor.
**/
typedef struct {
  ED_UI                 Ui;
  OC_PICKER_CONTEXT     *Picker;
  OC_STORAGE_CONTEXT    *Storage;
  OC_BOOT_ENTRY         *Entry;
  //
  // Message to show the next time the main menu is drawn.
  //
  CHAR8                 Status[128];
  BOOLEAN               StatusIsError;
} ED_STATE;

/**
  Current boot-args variable.
**/
typedef struct {
  BOOLEAN    Exists;
  //
  // Kept in flash and not going away with the next reset.
  //
  BOOLEAN    InFlash;
  //
  // Nothing but printable ASCII, so it is safe to edit.
  //
  BOOLEAN    Editable;
  CHAR8      Text[ED_BOOT_ARGS_LENGTH + 1];
} ED_BOOT_ARGS;

/**
  A level of the configuration browser.
**/
typedef struct {
  XML_NODE    *Node;
  CHAR8       Name[64];
  ED_LIST     List;
} ED_LEVEL;

typedef struct {
  ED_STATE      *State;
  EDIT_PLIST    Plist;
  UINT32        Baseline;
  ED_LEVEL      Levels[ED_MAX_DEPTH];
  UINTN         Depth;
  CHAR8         Status[128];
  BOOLEAN       StatusIsError;
} ED_BROWSER;

STATIC
VOID
EdSetStatus (
  OUT CHAR8        *Status,
  IN  UINTN        Size,
  OUT BOOLEAN      *IsError,
  IN  BOOLEAN      Error,
  IN  CONST CHAR8  *Text
  )
{
  AsciiSPrint (Status, Size, "%a", Text);
  *IsError = Error;
}

STATIC
VOID
EdUnicodeToAscii (
  IN  CONST CHAR16  *Text,
  OUT CHAR8         *Buffer,
  IN  UINTN         Size
  )
{
  UINTN  Index;

  for (Index = 0; (Index + 1 < Size) && (Text[Index] != L'\0'); ++Index) {
    Buffer[Index] = ((Text[Index] >= 0x20) && (Text[Index] <= 0x7E)) ? (CHAR8)Text[Index] : '?';
  }

  Buffer[Index] = '\0';
}

/**
  Get the name of the configuration which is in use, for the screen.
**/
STATIC
VOID
EdConfigNameAscii (
  OUT CHAR8  *Buffer,
  IN  UINTN  Size
  )
{
  EdUnicodeToAscii (OcMiscGetConfigName (), Buffer, Size);
}

STATIC
BOOLEAN
EdSameConfigName (
  IN  CONST CHAR16  *First,
  IN  CONST CHAR16  *Second
  )
{
  while (CharToUpper (*First) == CharToUpper (*Second)) {
    if (*First == L'\0') {
      return TRUE;
    }

    ++First;
    ++Second;
  }

  return FALSE;
}

STATIC
BOOLEAN
EdIsConfigEditable (
  IN  CONST ED_STATE  *State,
  OUT CONST CHAR8     **Reason  OPTIONAL
  )
{
  if (Reason != NULL) {
    *Reason = NULL;
  }

  if ((State->Storage == NULL) || (State->Storage->FileSystem == NULL) || (State->Storage->StorageRoot == NULL)) {
    if (Reason != NULL) {
      *Reason = "The configuration file system is not available";
    }

    return FALSE;
  }

  if (State->Storage->HasVault) {
    //
    // Any change would not match the vault, and OpenCore refuses to start then.
    //
    if (Reason != NULL) {
      *Reason = "The vault is used, config.plist is protected by it";
    }

    return FALSE;
  }

  return TRUE;
}

STATIC
VOID
EdReadBootArgs (
  OUT ED_BOOT_ARGS  *Args
  )
{
  EFI_STATUS  Status;
  UINT8       *Data;
  UINTN       Size;
  UINT32      Attributes;
  UINTN       Index;

  ZeroMem (Args, sizeof (*Args));
  Args->Editable = TRUE;

  Data   = NULL;
  Status = GetVariable3 (L"boot-args", &gAppleBootVariableGuid, (VOID **)&Data, &Size, &Attributes);
  if (EFI_ERROR (Status)) {
    return;
  }

  Args->Exists  = TRUE;
  Args->InFlash = (Attributes & EFI_VARIABLE_NON_VOLATILE) != 0;

  //
  // OpenCore writes the arguments with a terminator, macOS usually does not.
  //
  while ((Size > 0) && (Data[Size - 1] == '\0')) {
    --Size;
  }

  if (Size > ED_BOOT_ARGS_LENGTH) {
    Args->Editable = FALSE;
    Size           = ED_BOOT_ARGS_LENGTH;
  }

  for (Index = 0; Index < Size; ++Index) {
    if ((Data[Index] >= 0x20) && (Data[Index] <= 0x7E)) {
      Args->Text[Index] = (CHAR8)Data[Index];
    } else {
      Args->Text[Index] = '?';
      Args->Editable    = FALSE;
    }
  }

  FreePool (Data);
}

/**
  Check that a file has the expected contents.
**/
STATIC
BOOLEAN
EdVerifyFile (
  IN  EFI_FILE_PROTOCOL  *Root,
  IN  CONST CHAR16       *Path,
  IN  CONST VOID         *Data,
  IN  UINT32             Size
  )
{
  VOID     *Read;
  UINT32   ReadSize;
  BOOLEAN  Same;

  Read = OcReadFileFromDirectory (Root, Path, &ReadSize, 0);
  if (Read == NULL) {
    return FALSE;
  }

  Same = (ReadSize == Size) && (CompareMem (Read, Data, Size) == 0);
  FreePool (Read);

  return Same;
}

/**
  Replace config.plist, with the previous file saved next to it.

  @param[in]  Storage  OpenCore storage.
  @param[in]  Old      Contents of the file being replaced.
  @param[in]  OldSize  Size of Old.
  @param[in]  New      New contents.
  @param[in]  NewSize  Size of New.

  @retval EFI_SUCCESS  The file has been replaced and read back.
  @retval other        The file may still be as it was, or has been put back.
**/
STATIC
EFI_STATUS
EdWriteConfig (
  IN  OC_STORAGE_CONTEXT  *Storage,
  IN  CONST VOID          *Old,
  IN  UINT32              OldSize,
  IN  CONST VOID          *New,
  IN  UINT32              NewSize
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *Root;
  CHAR16             ConfigPath[OC_STORAGE_SAFE_PATH_MAX + OC_CONFIG_MAX_NAME + 16];
  CHAR16             BackupPath[OC_STORAGE_SAFE_PATH_MAX + OC_CONFIG_MAX_NAME + 16];

  if (StrLen (Storage->StorageRoot) > OC_STORAGE_SAFE_PATH_MAX) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // The configuration in use is the one that is changed.
  //
  UnicodeSPrint (ConfigPath, sizeof (ConfigPath), L"%s\\%s", Storage->StorageRoot, OcMiscGetConfigName ());
  UnicodeSPrint (BackupPath, sizeof (BackupPath), L"%s\\%s.bak", Storage->StorageRoot, OcMiscGetConfigName ());

  //
  // The storage itself is opened for reading only.
  //
  Status = Storage->FileSystem->OpenVolume (Storage->FileSystem, &Root);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // First the backup, the configuration is not touched if it cannot be made.
  //
  OcDeleteFile (Root, BackupPath);
  Status = OcSetFileData (Root, BackupPath, Old, OldSize);
  if (!EFI_ERROR (Status) && !EdVerifyFile (Root, BackupPath, Old, OldSize)) {
    Status = EFI_VOLUME_CORRUPTED;
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "OCE: Failed to write backup - %r\n", Status));
    OcDeleteFile (Root, BackupPath);
    Root->Close (Root);
    return Status;
  }

  OcDeleteFile (Root, ConfigPath);
  Status = OcSetFileData (Root, ConfigPath, New, NewSize);
  if (!EFI_ERROR (Status) && !EdVerifyFile (Root, ConfigPath, New, NewSize)) {
    Status = EFI_VOLUME_CORRUPTED;
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "OCE: Failed to write configuration - %r, putting the previous one back\n", Status));
    OcDeleteFile (Root, ConfigPath);
    OcSetFileData (Root, ConfigPath, Old, OldSize);
  }

  Root->Close (Root);
  return Status;
}

/**
  Check changes to the configuration, and write them.

  @param[in]  State     Editor state.
  @param[in]  Plist     Edited configuration.
  @param[in]  Baseline  Number of problems which the configuration had before editing.
  @param[out] Reason    Text to show on failure.
**/
STATIC
EFI_STATUS
EdCommitConfig (
  IN  ED_STATE    *State,
  IN  EDIT_PLIST  *Plist,
  IN  UINT32      Baseline,
  OUT CONST CHAR8 **Reason
  )
{
  EFI_STATUS  Status;
  CHAR8       *Text;
  UINT32      Size;
  UINT32      Errors;

  *Reason = NULL;

  Status = EditPlistRender (Plist, &Text, &Size);
  if (EFI_ERROR (Status)) {
    *Reason = "Not enough memory to prepare config.plist";
    return Status;
  }

  //
  // The result has to load, and not to be any worse than what it was.
  // OpenCore does not start with a configuration it cannot load.
  //
  Status = EditPlistCountErrors (Text, Size, &Errors);
  if (EFI_ERROR (Status) || (Errors > Baseline)) {
    DEBUG ((DEBUG_WARN, "OCE: Rejecting configuration with %u problems vs %u - %r\n", Errors, Baseline, Status));
    FreePool (Text);
    *Reason = "The changed config.plist would not load properly, nothing was written";
    return EFI_COMPROMISED_DATA;
  }

  Status = EdWriteConfig (State->Storage, Plist->Source, Plist->SourceSize, Text, Size);
  FreePool (Text);

  if (EFI_ERROR (Status)) {
    *Reason = "Cannot write config.plist (the file system may be read-only)";
  }

  return Status;
}

/**
  Load config.plist for editing.

  @param[in]   State     Editor state.
  @param[out]  Plist     Loaded document.
  @param[out]  Baseline  Number of problems which the configuration has.
  @param[out]  Reason    Text to show on failure.
**/
STATIC
EFI_STATUS
EdLoadConfig (
  IN  ED_STATE    *State,
  OUT EDIT_PLIST  *Plist,
  OUT UINT32      *Baseline,
  OUT CONST CHAR8 **Reason
  )
{
  EFI_STATUS  Status;
  VOID        *Data;
  UINT32      Size;

  if (!EdIsConfigEditable (State, Reason)) {
    return EFI_ACCESS_DENIED;
  }

  *Reason = NULL;

  Data = OcStorageReadFileUnicode (State->Storage, OcMiscGetConfigName (), &Size);
  if (Data == NULL) {
    *Reason = "Cannot read config.plist";
    return EFI_NOT_FOUND;
  }

  Status = EditPlistLoad (Plist, Data, Size);
  if (EFI_ERROR (Status)) {
    FreePool (Data);
    *Reason = "Cannot parse config.plist";
    return Status;
  }

  Status = EditPlistCountErrors (Data, Size, Baseline);
  FreePool (Data);
  if (EFI_ERROR (Status)) {
    EditPlistFree (Plist);
    *Reason = "OpenCore cannot load this config.plist";
    return Status;
  }

  return EFI_SUCCESS;
}

/**
  Find a dictionary entry ignoring the case of the key.
**/
STATIC
XML_NODE *
EdFindChildNoCase (
  IN  XML_NODE     *Dict,
  IN  CONST CHAR8  *Key
  )
{
  UINT32  Index;

  for (Index = 0; Index < EditPlistChildCount (Dict); ++Index) {
    if (AsciiStriCmp (EditPlistChildKey (Dict, Index), Key) == 0) {
      return EditPlistChildValue (Dict, Index);
    }
  }

  return NULL;
}

/**
  Store boot arguments in config.plist as the value of the NVRAM boot-args variable.
**/
STATIC
EFI_STATUS
EdSaveBootArgsToConfig (
  IN  ED_STATE     *State,
  IN  CONST CHAR8  *Args,
  OUT CONST CHAR8  **Reason
  )
{
  EFI_STATUS  Status;
  EDIT_PLIST  Plist;
  UINT32      Baseline;
  XML_NODE    *Add;
  XML_NODE    *Guid;
  XML_NODE    *Value;

  Status = EdLoadConfig (State, &Plist, &Baseline, Reason);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Add = EditPlistFindChild (EditPlistFindChild (Plist.Root, "NVRAM"), "Add");
  if (EditPlistNodeKind (Add) != EditPlistKindDict) {
    EditPlistFree (&Plist);
    *Reason = "NVRAM > Add is not found in config.plist";
    return EFI_NOT_FOUND;
  }

  Guid = EdFindChildNoCase (Add, ED_APPLE_BOOT_GUID_TEXT);
  if (Guid == NULL) {
    Status = EditPlistAddDictWithString (&Plist, Add, ED_APPLE_BOOT_GUID_TEXT, ED_BOOT_ARGS_KEY, Args);
  } else if (EditPlistNodeKind (Guid) != EditPlistKindDict) {
    Status = EFI_UNSUPPORTED;
  } else {
    Value = EditPlistFindChild (Guid, ED_BOOT_ARGS_KEY);
    if (Value != NULL) {
      Status = EditPlistSetString (&Plist, Value, Args);
    } else {
      Status = EditPlistAddString (&Plist, Guid, ED_BOOT_ARGS_KEY, Args);
    }
  }

  if (EFI_ERROR (Status)) {
    EditPlistFree (&Plist);
    *Reason = Status == EFI_INVALID_PARAMETER
              ? "The arguments contain characters which cannot be saved to config.plist"
              : "Cannot change NVRAM > Add > boot-args in config.plist";
    return Status;
  }

  if (EditPlistPatchCount (&Plist) == 0) {
    //
    // Already what is wanted.
    //
    EditPlistFree (&Plist);
    return EFI_SUCCESS;
  }

  Status = EdCommitConfig (State, &Plist, Baseline, Reason);
  EditPlistFree (&Plist);

  return Status;
}

STATIC
BOOLEAN
EdBootArgsFilter (
  IN CHAR8  Character
  )
{
  return Character != '<' && Character != '>';
}

/**
  Let the user choose how boot arguments are applied, and apply them.

  @param[in,out]  State  Editor state.
  @param[in]      Args   Boot arguments to apply.

  @retval TRUE   The arguments are applied, the editor of arguments is done.
  @retval FALSE  Esc was pressed, the arguments are still to be edited.
**/
STATIC
BOOLEAN
EdBootArgsApply (
  IN OUT ED_STATE   *State,
  IN     CONST CHAR8 *Args
  )
{
  ED_UI        *Ui;
  CHAR8        Line[ED_BOOT_ARGS_LENGTH + 80];
  CHAR8        Subtitle[160];
  CHAR8        Name[128];
  CONST CHAR8  *Items[3];
  BOOLEAN      Enabled[3];
  UINTN        Selected;
  UINTN        Row;
  UINTN        Offset;
  UINTN        Width;
  UINTN        Length;
  INTN         Choice;
  EFI_STATUS   Status;
  CONST CHAR8  *Reason;

  Ui = &State->Ui;

  Items[0]   = "Boot once      (NVRAM is put back the next time OpenCore starts)";
  Items[1]   = "Keep in NVRAM  (stays until changed, like the nvram tool)";
  Items[2]   = "Keep in NVRAM and save to config.plist (NVRAM > Add)";
  Enabled[0] = TRUE;
  Enabled[1] = TRUE;
  Enabled[2] = EdIsConfigEditable (State, NULL);
  Selected   = 0;

  //
  // This is a screen of its own, which the caller has to draw again when it gets back.
  //
  EdUiClear (Ui);

  Subtitle[0] = '\0';
  if (State->Entry != NULL) {
    EdUnicodeToAscii (State->Entry->Name, Name, sizeof (Name));
    AsciiSPrint (Subtitle, sizeof (Subtitle), "Highlighted entry: %a", Name);
  }

  EdUiTitle (Ui, "Apply boot arguments", Subtitle);
  EdUiPut (Ui, 0, 3, Ui->AttrNormal, "Boot arguments to apply:", 0);

  //
  // The arguments can be longer than a row.
  //
  Width  = Ui->Columns - 1;
  Length = AsciiStrLen (Args);
  if (Length == 0) {
    EdUiPut (Ui, 0, 4, Ui->AttrChanged, "(none, boot-args are removed)", 0);
  } else {
    for (Row = 0, Offset = 0; (Row < 4) && (Offset < Length); ++Row, Offset += Width) {
      AsciiStrnCpyS (Line, sizeof (Line), &Args[Offset], MIN (Width, Length - Offset));
      EdUiPut (Ui, 0, 4 + Row, Ui->AttrChanged, Line, 0);
    }
  }

  while (TRUE) {
    EdUiHint (Ui, "Enter: apply   Esc: back");

    Choice = EdUiMenu (Ui, 9, Items, Enabled, ARRAY_SIZE (Items), &Selected);
    if (Choice < 0) {
      return FALSE;
    }

    Reason = NULL;
    Status = EFI_SUCCESS;

    if (Choice == 2) {
      //
      // Configuration first as it is more likely to fail, and nothing has been changed yet.
      //
      Status = EdSaveBootArgsToConfig (State, Args, &Reason);
    }

    if (!EFI_ERROR (Status)) {
      Status = OcSetBootArgs (Args, Choice != 0);
      if (EFI_ERROR (Status)) {
        Reason = "Cannot change the NVRAM boot-args variable";
      }
    }

    if (EFI_ERROR (Status)) {
      AsciiSPrint (Line, sizeof (Line), "%a (%r)", Reason != NULL ? Reason : "Failed", Status);
      EdUiNotify (Ui, Line, TRUE);
      continue;
    }

    EdSetStatus (
      State->Status,
      sizeof (State->Status),
      &State->StatusIsError,
      FALSE,
      Choice == 0
      ? "Boot arguments are set for this boot only"
      : (Choice == 1 ? "Boot arguments are kept in NVRAM" : "Boot arguments are kept in NVRAM and saved to config.plist")
      );
    return TRUE;
  }
}

STATIC
VOID
EdBootArgsScreen (
  IN OUT ED_STATE  *State
  )
{
  ED_UI         *Ui;
  ED_BOOT_ARGS  Current;
  CHAR8         Buffer[ED_BOOT_ARGS_LENGTH + 1];
  CHAR8         Line[ED_BOOT_ARGS_LENGTH + 80];
  UINTN         FieldRows;

  Ui = &State->Ui;

  EdReadBootArgs (&Current);
  if (!Current.Editable) {
    EdUiTitle (Ui, "Boot arguments", NULL);
    EdUiNotify (Ui, "The current boot-args hold bytes which cannot be edited here", TRUE);
    return;
  }

  AsciiStrCpyS (Buffer, sizeof (Buffer), Current.Text);

  while (TRUE) {
    EdUiClear (Ui);
    EdUiTitle (Ui, "Boot arguments", "Used by macOS entries");

    AsciiSPrint (
      Line,
      sizeof (Line),
      "NVRAM now: %a%a",
      Current.Exists ? Current.Text : "(not set)",
      Current.Exists ? (Current.InFlash ? "" : "   [this boot only]") : ""
      );
    EdUiPut (Ui, 0, 3, Ui->AttrNormal, Line, 0);

    if (State->Picker->AppleBootArgs[0] != '\0') {
      AsciiSPrint (Line, sizeof (Line), "Added by hotkeys (kept as they are): %a", State->Picker->AppleBootArgs);
      EdUiPut (Ui, 0, 4, Ui->AttrNormal, Line, 0);
    }

    if ((State->Entry != NULL) && ((State->Entry->Type & OC_BOOT_APPLE_ANY) == 0)) {
      EdUiPut (Ui, 0, 5, Ui->AttrChanged, "The highlighted entry is not macOS, boot-args matter only for macOS", 0);
    }

    EdUiPut (Ui, 0, 7, Ui->AttrNormal, "New boot arguments:", 0);
    EdUiHint (Ui, "Enter: continue   Esc: clear, then cancel   Arrows, Home, End, Del: edit");

    FieldRows = Ui->Rows - 8 - 4;
    if (FieldRows > 8) {
      FieldRows = 8;
    }

    if (EdUiEditLine (Ui, 8, FieldRows, Buffer, sizeof (Buffer), EdBootArgsFilter) == EdLineCancelled) {
      return;
    }

    if (EdBootArgsApply (State, Buffer)) {
      return;
    }
  }
}

//
// Boot flags: the usual boot arguments which can be switched on and off.
//

typedef struct {
  CONST CHAR8    *Flag;
  CONST CHAR8    *Description;
} ED_BOOT_FLAG;

STATIC CONST ED_BOOT_FLAG  mBootFlags[] = {
  { "-v",                       "Verbose boot"                          },
  { "-s",                       "Single user mode"                      },
  { "-x",                       "Safe mode"                             },
  { "-f",                       "Ignore the kernel cache"               },
  { "-no_compat_check",         "Skip the macOS compatibility check"    },
  { "keepsyms=1",               "Keep symbols for panic reports"        },
  { "debug=0x100",              "Do not restart after a kernel panic"   },
  { "cpus=1",                   "Use one processor core only"           },
  { "nv_disable=1",             "Disable NVIDIA graphics acceleration"  },
  { "amfi_get_out_of_my_way=1", "Disable AMFI (needed by root patches)" }
};

typedef enum {
  EdFlagOff,
  EdFlagOn,
  EdFlagOther
} ED_FLAG_STATE;

/**
  Check if a token of boot arguments is about the same setting as a flag:
  the very same token for switches like -v, or any value for flags like debug=0x100.
**/
STATIC
BOOLEAN
EdTokenIsSetting (
  IN  CONST CHAR8  *Token,
  IN  UINTN        Length,
  IN  CONST CHAR8  *Flag
  )
{
  CONST CHAR8  *Equals;
  UINTN        KeyLength;

  Equals = AsciiStrStr (Flag, "=");
  if (Equals == NULL) {
    return Length == AsciiStrLen (Flag) && AsciiStrnCmp (Token, Flag, Length) == 0;
  }

  KeyLength = (UINTN)(Equals - Flag) + 1;
  return Length >= KeyLength && AsciiStrnCmp (Token, Flag, KeyLength) == 0;
}

STATIC
ED_FLAG_STATE
EdGetFlagState (
  IN  CONST CHAR8  *Args,
  IN  CONST CHAR8  *Flag
  )
{
  ED_FLAG_STATE  State;
  UINTN          Index;
  UINTN          Length;

  State = EdFlagOff;
  Index = 0;
  while (Args[Index] != '\0') {
    if (Args[Index] == ' ') {
      ++Index;
      continue;
    }

    for (Length = 0; (Args[Index + Length] != '\0') && (Args[Index + Length] != ' '); ++Length) {
    }

    if (EdTokenIsSetting (&Args[Index], Length, Flag)) {
      if ((Length == AsciiStrLen (Flag)) && (AsciiStrnCmp (&Args[Index], Flag, Length) == 0)) {
        return EdFlagOn;
      }

      State = EdFlagOther;
    }

    Index += Length;
  }

  return State;
}

/**
  Remove all tokens which are about the setting of the flag.
**/
STATIC
VOID
EdRemoveFlag (
  IN OUT CHAR8        *Args,
  IN     CONST CHAR8  *Flag
  )
{
  UINTN  Index;
  UINTN  Length;
  UINTN  End;
  UINTN  Total;

  Index = 0;
  while (Args[Index] != '\0') {
    if (Args[Index] == ' ') {
      ++Index;
      continue;
    }

    for (Length = 0; (Args[Index + Length] != '\0') && (Args[Index + Length] != ' '); ++Length) {
    }

    if (!EdTokenIsSetting (&Args[Index], Length, Flag)) {
      Index += Length;
      continue;
    }

    //
    // Remove the token with the spaces after it.
    //
    End = Index + Length;
    while (Args[End] == ' ') {
      ++End;
    }

    Total = AsciiStrLen (Args);
    CopyMem (&Args[Index], &Args[End], Total - End + 1);
  }

  //
  // No dangling space is left if the token was the last one.
  //
  Total = AsciiStrLen (Args);
  while ((Total > 0) && (Args[Total - 1] == ' ')) {
    Args[--Total] = '\0';
  }
}

/**
  Switch a flag on or off.

  @retval FALSE  The flag does not fit and arguments are left as they were.
**/
STATIC
BOOLEAN
EdToggleFlag (
  IN OUT CHAR8        *Args,
  IN     UINTN        Size,
  IN     CONST CHAR8  *Flag
  )
{
  CHAR8  Saved[ED_BOOT_ARGS_LENGTH + 1];
  UINTN  Length;

  AsciiStrCpyS (Saved, sizeof (Saved), Args);

  if (EdGetFlagState (Args, Flag) == EdFlagOn) {
    EdRemoveFlag (Args, Flag);
    return TRUE;
  }

  EdRemoveFlag (Args, Flag);

  Length = AsciiStrLen (Args);
  if (Length + (Length > 0 ? 1 : 0) + AsciiStrLen (Flag) >= Size) {
    AsciiStrCpyS (Args, Size, Saved);
    return FALSE;
  }

  if (Length > 0) {
    Args[Length++] = ' ';
  }

  AsciiStrCpyS (&Args[Length], Size - Length, Flag);
  return TRUE;
}

typedef struct {
  CHAR8    Original[ED_BOOT_ARGS_LENGTH + 1];
  CHAR8    Args[ED_BOOT_ARGS_LENGTH + 1];
} ED_FLAGS;

STATIC
VOID
EdFlagItem (
  IN  VOID     *Context,
  IN  UINTN    Index,
  OUT CHAR8    *Text,
  IN  UINTN    Size,
  OUT BOOLEAN  *Changed,
  OUT BOOLEAN  *Dim,
  OUT BOOLEAN  *Error
  )
{
  ED_FLAGS       *Flags;
  ED_FLAG_STATE  State;
  CHAR8          Flag[32];
  UINTN          Length;

  Flags = Context;
  State = EdGetFlagState (Flags->Args, mBootFlags[Index].Flag);

  *Changed = State != EdGetFlagState (Flags->Original, mBootFlags[Index].Flag);
  *Dim     = FALSE;
  *Error   = FALSE;

  //
  // [~] is for a flag which is set with another value than the one it is switched on with.
  //
  AsciiSPrint (Flag, sizeof (Flag), "%a", mBootFlags[Index].Flag);
  for (Length = AsciiStrLen (Flag); Length < 26; ++Length) {
    Flag[Length] = ' ';
  }

  Flag[26] = '\0';

  AsciiSPrint (
    Text,
    Size,
    "%a [%a] %a %a",
    *Changed ? "*" : " ",
    State == EdFlagOn ? "x" : (State == EdFlagOther ? "~" : " "),
    Flag,
    mBootFlags[Index].Description
    );
}

STATIC
VOID
EdBootFlagsScreen (
  IN OUT ED_STATE  *State
  )
{
  ED_UI               *Ui;
  ED_BOOT_ARGS        Current;
  ED_FLAGS            *Flags;
  ED_LIST             List;
  OC_PICKER_KEY_INFO  *Key;
  UINTN               Visible;
  CHAR8               Line[ED_BOOT_ARGS_LENGTH + 80];
  BOOLEAN             Redraw;

  Ui = &State->Ui;

  EdReadBootArgs (&Current);
  if (!Current.Editable) {
    EdUiTitle (Ui, "Boot flags", NULL);
    EdUiNotify (Ui, "The current boot-args hold bytes which cannot be edited here", TRUE);
    return;
  }

  Flags = AllocateZeroPool (sizeof (*Flags));
  if (Flags == NULL) {
    EdUiNotify (Ui, "Not enough memory", TRUE);
    return;
  }

  AsciiStrCpyS (Flags->Original, sizeof (Flags->Original), Current.Text);
  AsciiStrCpyS (Flags->Args, sizeof (Flags->Args), Current.Text);

  List.Count    = ARRAY_SIZE (mBootFlags);
  List.Selected = 0;
  List.Top      = 0;
  Visible       = EdUiContentRows (Ui);
  Redraw        = TRUE;

  while (TRUE) {
    if (Redraw) {
      EdUiClear (Ui);
      Redraw = FALSE;
    }

    EdUiTitle (Ui, "Boot flags", "Used by macOS entries");
    EdListDraw (Ui, 3, Visible, &List, EdFlagItem, Flags);

    AsciiSPrint (Line, sizeof (Line), "Boot arguments: %a", Flags->Args[0] != '\0' ? Flags->Args : "(none)");
    EdUiStatus (Ui, Ui->AttrChanged, Line);
    EdUiHint (Ui, "Space: on/off   Enter: apply   Esc: back");

    Key = EdUiWaitKey (Ui, FALSE);

    switch (Key->OcKeyCode) {
      case OC_INPUT_UP:
      case OC_INPUT_DOWN:
      case OC_INPUT_TOP:
      case OC_INPUT_BOTTOM:
        EdListMove (&List, Visible, Key->OcKeyCode);
        break;
      case OC_INPUT_MORE:
      case OC_INPUT_RIGHT:
        if (!EdToggleFlag (Flags->Args, sizeof (Flags->Args), mBootFlags[List.Selected].Flag)) {
          EdUiNotify (Ui, "The boot arguments would become too long", TRUE);
        }

        break;
      case OC_INPUT_CONTINUE:
        Redraw = TRUE;
        if (EdBootArgsApply (State, Flags->Args)) {
          FreePool (Flags);
          return;
        }

        break;
      case OC_INPUT_ABORTED:
      case OC_INPUT_LEFT:
        FreePool (Flags);
        return;
      default:
        break;
    }
  }
}

//
// Description of values.
//

STATIC
XML_NODE *
EdFindEnabled (
  IN  XML_NODE  *Dict
  )
{
  XML_NODE  *Enabled;

  Enabled = EditPlistFindChild (Dict, "Enabled");
  if ((Enabled != NULL) && (EditPlistNodeKind (Enabled) == EditPlistKindBoolean)) {
    return Enabled;
  }

  return NULL;
}

/**
  Describe an array element: a dictionary is named after its most telling entry.
**/
STATIC
VOID
EdArrayLabel (
  IN  CONST EDIT_PLIST  *Plist,
  IN  XML_NODE          *Element,
  IN  UINT32            Index,
  OUT CHAR8             *Buffer,
  IN  UINTN             Size
  )
{
  STATIC CONST CHAR8  *Names[] = { "BundlePath", "Path", "Name", "Identifier", "Comment", "Arguments" };

  UINTN       Item;
  XML_NODE    *Entry;
  XML_NODE    *Enabled;
  CONST CHAR8 *Title;

  Title   = "";
  Enabled = NULL;

  if (EditPlistNodeKind (Element) == EditPlistKindDict) {
    Enabled = EdFindEnabled (Element);

    for (Item = 0; Item < ARRAY_SIZE (Names); ++Item) {
      Entry = EditPlistFindChild (Element, Names[Item]);
      if ((Entry != NULL) && (EditPlistNodeKind (Entry) == EditPlistKindString)) {
        Title = EditPlistGetContent (Plist, Entry);
        if (Title[0] != '\0') {
          break;
        }
      }
    }
  }

  AsciiSPrint (
    Buffer,
    Size,
    "[%u] %a%a",
    Index,
    Enabled != NULL ? (EditPlistGetBoolean (Plist, Enabled) ? "[x] " : "[ ] ") : "",
    Title
    );
}

STATIC
VOID
EdDescribeValue (
  IN  CONST EDIT_PLIST  *Plist,
  IN  XML_NODE          *Node,
  OUT CHAR8             *Buffer,
  IN  UINTN             Size
  )
{
  CHAR8        Number[32];
  CONST CHAR8  *Content;
  UINT8        *Data;
  UINTN        DataSize;
  UINTN        Index;
  UINTN        Length;
  CHAR8        Hex[8 * 3 + 1];

  switch (EditPlistNodeKind (Node)) {
    case EditPlistKindDict:
      AsciiSPrint (Buffer, Size, "> {%u}", EditPlistChildCount (Node));
      break;
    case EditPlistKindArray:
      AsciiSPrint (Buffer, Size, "> [%u]", EditPlistChildCount (Node));
      break;
    case EditPlistKindBoolean:
      AsciiSPrint (Buffer, Size, "%a", EditPlistGetBoolean (Plist, Node) ? "true" : "false");
      break;
    case EditPlistKindInteger:
      EditPlistGetInteger (Plist, Node, Number, sizeof (Number));
      AsciiSPrint (Buffer, Size, "%a", Number);
      break;
    case EditPlistKindString:
      Content = EditPlistGetContent (Plist, Node);
      Length  = AsciiStrLen (Content);
      if (Length > 90) {
        AsciiSPrint (Buffer, Size, "\"%.90a...\"", Content);
      } else {
        AsciiSPrint (Buffer, Size, "\"%a\"", Content);
      }

      break;
    case EditPlistKindData:
      if (EFI_ERROR (EditPlistGetData (Plist, Node, &Data, &DataSize))) {
        AsciiSPrint (Buffer, Size, "<invalid data>");
        break;
      }

      Hex[0] = '\0';
      for (Index = 0; (Index < DataSize) && (Index < 8); ++Index) {
        AsciiSPrint (&Hex[Index * 3], sizeof (Hex) - Index * 3, "%02X ", Data[Index]);
      }

      AsciiSPrint (Buffer, Size, "<%u bytes> %a%a", (UINT32)DataSize, Hex, DataSize > 8 ? "..." : "");
      if (Data != NULL) {
        FreePool (Data);
      }

      break;
    default:
      AsciiSPrint (Buffer, Size, "<%a>", XmlNodeName (Node));
      break;
  }
}

/**
  Check if anything in the node has been changed.
**/
STATIC
BOOLEAN
EdIsChanged (
  IN  CONST EDIT_PLIST  *Plist,
  IN  XML_NODE          *Node
  )
{
  UINT32  Index;
  UINT32  Start;
  UINT32  End;

  if (EditPlistIsPatched (Plist, Node)) {
    return TRUE;
  }

  if (!XmlNodeSourceRange (Node, &Start, &End)) {
    return FALSE;
  }

  for (Index = 0; Index < Plist->PatchCount; ++Index) {
    if ((Plist->Patches[Index].Start >= Start) && (Plist->Patches[Index].Start < End)) {
      return TRUE;
    }
  }

  return FALSE;
}

/**
  Settings which change how OpenCore verifies things or starts are not to be edited here:
  mistakes in them stop OpenCore from starting. Switches can be changed.
**/
STATIC
BOOLEAN
EdIsProtected (
  IN  CONST ED_BROWSER  *Browser,
  IN  XML_NODE          *Node
  )
{
  return Browser->Depth == 2
         && AsciiStrCmp (Browser->Levels[1].Name, "Misc") == 0
         && AsciiStrCmp (Browser->Levels[2].Name, "Security") == 0
         && EditPlistNodeKind (Node) != EditPlistKindBoolean;
}

STATIC
VOID
EdBrowserItem (
  IN  VOID     *Context,
  IN  UINTN    Index,
  OUT CHAR8    *Text,
  IN  UINTN    Size,
  OUT BOOLEAN  *Changed,
  OUT BOOLEAN  *Dim,
  OUT BOOLEAN  *Error
  )
{
  ED_BROWSER   *Browser;
  XML_NODE     *Node;
  XML_NODE     *Child;
  CONST CHAR8  *Key;
  CHAR8        Label[160];
  CHAR8        Value[200];
  UINTN        Length;

  Browser = Context;
  Node    = Browser->Levels[Browser->Depth].Node;
  Child   = EditPlistChildValue (Node, (UINT32)Index);
  Key     = EditPlistChildKey (Node, (UINT32)Index);

  *Changed = EdIsChanged (&Browser->Plist, Child);
  *Dim     = EdIsProtected (Browser, Child);
  *Error   = FALSE;

  if (Key != NULL) {
    AsciiSPrint (Label, sizeof (Label), "%a", Key);
  } else {
    EdArrayLabel (&Browser->Plist, Child, (UINT32)Index, Label, sizeof (Label));
  }

  //
  // Values start at the same column unless the label is very long.
  //
  Length = AsciiStrLen (Label);
  if (Length > 34) {
    Label[34] = '\0';
    Length    = 34;
  }

  while (Length < 34) {
    Label[Length++] = ' ';
  }

  Label[Length] = '\0';

  if ((Key == NULL) && (EditPlistNodeKind (Child) == EditPlistKindDict)) {
    Value[0] = '\0';
    if (EditPlistNodeKind (Child) == EditPlistKindDict) {
      AsciiSPrint (Value, sizeof (Value), "> {%u}", EditPlistChildCount (Child));
    }
  } else {
    EdDescribeValue (&Browser->Plist, Child, Value, sizeof (Value));
  }

  AsciiSPrint (Text, Size, "%a %a %a", *Changed ? "*" : " ", Label, Value);
}

//
// Editing of values.
//

/**
  Check changes made to a value, and take them back if they spoil the configuration.
**/
STATIC
BOOLEAN
EdCheckChange (
  IN OUT ED_BROWSER  *Browser,
  IN     XML_NODE    *Node
  )
{
  EFI_STATUS  Status;
  CHAR8       *Text;
  UINT32      Size;
  UINT32      Errors;

  Status = EditPlistRender (&Browser->Plist, &Text, &Size);
  if (!EFI_ERROR (Status)) {
    Status = EditPlistCountErrors (Text, Size, &Errors);
    FreePool (Text);
  }

  if (EFI_ERROR (Status) || (Errors > Browser->Baseline)) {
    EditPlistRevert (&Browser->Plist, Node);
    EdUiNotify (&Browser->State->Ui, "OpenCore does not accept this value, it was not changed", TRUE);
    return FALSE;
  }

  return TRUE;
}

STATIC
BOOLEAN
EdIntegerFilter (
  IN CHAR8  Character
  )
{
  return (Character >= '0' && Character <= '9')
         || (Character >= 'a' && Character <= 'f')
         || (Character >= 'A' && Character <= 'F')
         || Character == 'x'
         || Character == 'X';
}

STATIC
BOOLEAN
EdHexFilter (
  IN CHAR8  Character
  )
{
  return (Character >= '0' && Character <= '9')
         || (Character >= 'a' && Character <= 'f')
         || (Character >= 'A' && Character <= 'F')
         || Character == ' ';
}

STATIC
BOOLEAN
EdValueFilter (
  IN CHAR8  Character
  )
{
  return Character != '<' && Character != '>';
}

/**
  Parse a decimal number or a hexadecimal number which starts with 0x.
**/
STATIC
BOOLEAN
EdParseInteger (
  IN  CONST CHAR8  *Text,
  OUT UINT64       *Value
  )
{
  UINT64  Result;
  UINT64  Digit;
  UINT64  Base;
  UINTN   Index;

  Base  = 10;
  Index = 0;
  if ((Text[0] == '0') && ((Text[1] == 'x') || (Text[1] == 'X'))) {
    Base  = 16;
    Index = 2;
  }

  if (Text[Index] == '\0') {
    return FALSE;
  }

  Result = 0;
  for ( ; Text[Index] != '\0'; ++Index) {
    if ((Text[Index] >= '0') && (Text[Index] <= '9')) {
      Digit = (UINT64)(Text[Index] - '0');
    } else if ((Base == 16) && (Text[Index] >= 'a') && (Text[Index] <= 'f')) {
      Digit = (UINT64)(Text[Index] - 'a') + 10;
    } else if ((Base == 16) && (Text[Index] >= 'A') && (Text[Index] <= 'F')) {
      Digit = (UINT64)(Text[Index] - 'A') + 10;
    } else {
      return FALSE;
    }

    if (Result > (MAX_UINT64 - Digit) / Base) {
      return FALSE;
    }

    Result = Result * Base + Digit;
  }

  *Value = Result;
  return TRUE;
}

/**
  Show the heading of an editing screen.
**/
STATIC
VOID
EdValueScreen (
  IN OUT ED_BROWSER   *Browser,
  IN     CONST CHAR8  *Kind,
  IN     CONST CHAR8  *Name,
  IN     CONST CHAR8  *Current,
  IN     CONST CHAR8  *Hint
  )
{
  ED_UI  *Ui;
  CHAR8  Line[256];

  Ui = &Browser->State->Ui;

  EdUiClear (Ui);
  AsciiSPrint (Line, sizeof (Line), "Edit %a", Kind);
  EdUiTitle (Ui, Line, Name);
  AsciiSPrint (Line, sizeof (Line), "Now: %a", Current);
  EdUiPut (Ui, 0, 3, Ui->AttrNormal, Line, 0);
  EdUiPut (Ui, 0, 5, Ui->AttrNormal, "New value:", 0);
  EdUiHint (Ui, Hint);
}

STATIC
VOID
EdEditInteger (
  IN OUT ED_BROWSER   *Browser,
  IN     XML_NODE     *Node,
  IN     CONST CHAR8  *Name
  )
{
  ED_UI       *Ui;
  CHAR8       Buffer[32];
  CHAR8       Current[32];
  UINT64      Value;

  Ui = &Browser->State->Ui;

  EditPlistGetInteger (&Browser->Plist, Node, Current, sizeof (Current));
  AsciiStrCpyS (Buffer, sizeof (Buffer), Current);

  while (TRUE) {
    EdValueScreen (Browser, "integer", Name, Current, "Decimal number or 0x hexadecimal.   Enter: accept   Esc: clear, then cancel");
    if (EdUiEditLine (Ui, 6, 1, Buffer, sizeof (Buffer), EdIntegerFilter) == EdLineCancelled) {
      return;
    }

    if (EdParseInteger (Buffer, &Value)) {
      break;
    }

    EdUiNotify (Ui, "Not a number", TRUE);
  }

  if (!EFI_ERROR (EditPlistSetInteger (&Browser->Plist, Node, Value))) {
    EdCheckChange (Browser, Node);
  }
}

STATIC
VOID
EdEditString (
  IN OUT ED_BROWSER   *Browser,
  IN     XML_NODE     *Node,
  IN     CONST CHAR8  *Name
  )
{
  ED_UI        *Ui;
  CHAR8        Buffer[EDIT_PLIST_MAX_VALUE_LENGTH + 1];
  CONST CHAR8  *Current;
  UINTN        Rows;

  Ui      = &Browser->State->Ui;
  Current = EditPlistGetContent (&Browser->Plist, Node);

  if (!EditPlistIsPlainString (Current)) {
    EdUiNotify (Ui, "This string has characters which cannot be edited here", TRUE);
    return;
  }

  AsciiStrCpyS (Buffer, sizeof (Buffer), Current);

  Rows = Ui->Rows - 6 - 4;
  while (TRUE) {
    EdValueScreen (Browser, "string", Name, Current, "Enter: accept   Esc: clear, then cancel   Arrows, Home, End, Del: edit");
    if (EdUiEditLine (Ui, 6, Rows, Buffer, sizeof (Buffer), EdValueFilter) == EdLineCancelled) {
      return;
    }

    if (EditPlistIsPlainString (Buffer)) {
      break;
    }

    EdUiNotify (Ui, "Only printable characters are accepted, & has to be part of &amp; &lt; &gt; &quot; or &apos;", TRUE);
  }

  if (!EFI_ERROR (EditPlistSetString (&Browser->Plist, Node, Buffer))) {
    EdCheckChange (Browser, Node);
  }
}

STATIC
VOID
EdEditData (
  IN OUT ED_BROWSER   *Browser,
  IN     XML_NODE     *Node,
  IN     CONST CHAR8  *Name
  )
{
  ED_UI       *Ui;
  EFI_STATUS  Status;
  UINT8       *Data;
  UINTN       Size;
  CHAR8       Buffer[ED_MAX_DATA_SIZE * 2 + 1];
  CHAR8       Current[ED_MAX_DATA_SIZE * 2 + 1];
  UINT8       Bytes[ED_MAX_DATA_SIZE];
  UINTN       Index;
  UINTN       Digits;
  UINTN       Count;
  UINT8       Nibble;

  Ui = &Browser->State->Ui;

  Status = EditPlistGetData (&Browser->Plist, Node, &Data, &Size);
  if (EFI_ERROR (Status)) {
    EdUiNotify (Ui, "This data is not valid base64", TRUE);
    return;
  }

  if (Size > ED_MAX_DATA_SIZE) {
    if (Data != NULL) {
      FreePool (Data);
    }

    EdUiNotify (Ui, "This data is too large to be edited here", TRUE);
    return;
  }

  for (Index = 0; Index < Size; ++Index) {
    AsciiSPrint (&Current[Index * 2], sizeof (Current) - Index * 2, "%02X", Data[Index]);
  }

  Current[Size * 2] = '\0';
  if (Data != NULL) {
    FreePool (Data);
  }

  AsciiStrCpyS (Buffer, sizeof (Buffer), Current);

  while (TRUE) {
    EdValueScreen (Browser, "data", Name, Current[0] != '\0' ? Current : "(empty)", "Hexadecimal bytes, e.g. E7030000.   Enter: accept   Esc: clear, then cancel");
    if (EdUiEditLine (Ui, 6, Ui->Rows - 6 - 4, Buffer, sizeof (Buffer), EdHexFilter) == EdLineCancelled) {
      return;
    }

    Digits = 0;
    Count  = 0;
    for (Index = 0; Buffer[Index] != '\0'; ++Index) {
      if (Buffer[Index] == ' ') {
        continue;
      }

      if ((Buffer[Index] >= '0') && (Buffer[Index] <= '9')) {
        Nibble = (UINT8)(Buffer[Index] - '0');
      } else if ((Buffer[Index] >= 'a') && (Buffer[Index] <= 'f')) {
        Nibble = (UINT8)(Buffer[Index] - 'a' + 10);
      } else {
        Nibble = (UINT8)(Buffer[Index] - 'A' + 10);
      }

      if ((Digits & 1) == 0) {
        Bytes[Count] = (UINT8)(Nibble << 4);
      } else {
        Bytes[Count++] |= Nibble;
      }

      ++Digits;
    }

    if ((Digits & 1) == 0) {
      break;
    }

    EdUiNotify (Ui, "An even number of hexadecimal digits is needed", TRUE);
  }

  if (!EFI_ERROR (EditPlistSetData (&Browser->Plist, Node, Bytes, Count))) {
    EdCheckChange (Browser, Node);
  }
}

STATIC
VOID
EdToggle (
  IN OUT ED_BROWSER  *Browser,
  IN     XML_NODE    *Node
  )
{
  EditPlistSetBoolean (&Browser->Plist, Node, !EditPlistGetBoolean (&Browser->Plist, Node));
}

//
// The browser.
//

STATIC
VOID
EdBrowserInitLevel (
  IN OUT ED_BROWSER   *Browser,
  IN     UINTN        Depth,
  IN     XML_NODE     *Node,
  IN     CONST CHAR8  *Name
  )
{
  ED_LEVEL  *Level;

  Level = &Browser->Levels[Depth];

  Level->Node = Node;
  AsciiSPrint (Level->Name, sizeof (Level->Name), "%a", Name);
  Level->List.Count    = EditPlistChildCount (Node);
  Level->List.Selected = 0;
  Level->List.Top      = 0;
}

STATIC
VOID
EdBrowserPath (
  IN  CONST ED_BROWSER  *Browser,
  OUT CHAR8             *Path,
  IN  UINTN             Size
  )
{
  UINTN  Level;
  UINTN  Length;

  EdConfigNameAscii (Path, Size);

  for (Level = 1; Level <= Browser->Depth; ++Level) {
    Length = AsciiStrLen (Path);
    AsciiSPrint (&Path[Length], Size - Length, " > %a", Browser->Levels[Level].Name);
  }
}

STATIC
VOID
EdBrowserDraw (
  IN OUT ED_BROWSER  *Browser
  )
{
  ED_UI     *Ui;
  ED_LEVEL  *Level;
  CHAR8     Path[256];
  CHAR8     Title[OC_CONFIG_MAX_NAME + 16];
  CHAR8     Line[288];
  UINTN     Visible;

  Ui      = &Browser->State->Ui;
  Level   = &Browser->Levels[Browser->Depth];
  Visible = EdUiContentRows (Ui);

  EdBrowserPath (Browser, Path, sizeof (Path));
  EdConfigNameAscii (Title, sizeof (Title) - L_STR_LEN ("Edit "));
  AsciiSPrint (Line, sizeof (Line), "Edit %a", Title);
  AsciiStrCpyS (Title, sizeof (Title), Line);

  if (EditPlistPatchCount (&Browser->Plist) > 0) {
    AsciiSPrint (Line, sizeof (Line), "%a   (%u unsaved changes)", Path, EditPlistPatchCount (&Browser->Plist));
    EdUiTitle (Ui, Title, Line);
  } else {
    EdUiTitle (Ui, Title, Path);
  }

  EdListDraw (Ui, 3, Visible, &Level->List, EdBrowserItem, Browser);

  if (Level->List.Count == 0) {
    EdUiPut (Ui, 0, 3, Ui->AttrHint, "  (empty)", 0);
  }

  if (Browser->Status[0] != '\0') {
    EdUiStatus (Ui, Browser->StatusIsError ? Ui->AttrError : Ui->AttrChanged, Browser->Status);
  } else {
    AsciiSPrint (Line, sizeof (Line), "%u of %u", (UINT32)(Level->List.Count == 0 ? 0 : Level->List.Selected + 1), (UINT32)Level->List.Count);
    EdUiStatus (Ui, Ui->AttrHint, Line);
  }

  EdUiHint (Ui, "Enter: open/edit  Space: on/off  Left/Esc: back  F10: save");
}

/**
  Get what is selected.
**/
STATIC
XML_NODE *
EdBrowserSelected (
  IN  CONST ED_BROWSER  *Browser,
  OUT CONST CHAR8       **Key  OPTIONAL
  )
{
  CONST ED_LEVEL  *Level;

  Level = &Browser->Levels[Browser->Depth];
  if (Level->List.Count == 0) {
    return NULL;
  }

  if (Key != NULL) {
    *Key = EditPlistChildKey (Level->Node, (UINT32)Level->List.Selected);
  }

  return EditPlistChildValue (Level->Node, (UINT32)Level->List.Selected);
}

STATIC
VOID
EdBrowserSetStatus (
  OUT ED_BROWSER   *Browser,
  IN  BOOLEAN      IsError,
  IN  CONST CHAR8  *Text
  )
{
  EdSetStatus (Browser->Status, sizeof (Browser->Status), &Browser->StatusIsError, IsError, Text);
}

STATIC
VOID
EdBrowserActivate (
  IN OUT ED_BROWSER  *Browser
  )
{
  XML_NODE     *Child;
  CONST CHAR8  *Key;
  CHAR8        Name[160];
  ED_LEVEL     *Level;

  Child = EdBrowserSelected (Browser, &Key);
  if (Child == NULL) {
    return;
  }

  Level = &Browser->Levels[Browser->Depth];

  if (Key != NULL) {
    AsciiSPrint (Name, sizeof (Name), "%a", Key);
  } else {
    EdArrayLabel (&Browser->Plist, Child, (UINT32)Level->List.Selected, Name, sizeof (Name));
  }

  switch (EditPlistNodeKind (Child)) {
    case EditPlistKindDict:
    case EditPlistKindArray:
      if (Browser->Depth + 1 >= ED_MAX_DEPTH) {
        EdBrowserSetStatus (Browser, TRUE, "Nested too deeply");
        return;
      }

      ++Browser->Depth;
      //
      // Elements of arrays are named by their position here.
      //
      if (Key == NULL) {
        AsciiSPrint (Name, sizeof (Name), "[%u]", (UINT32)Level->List.Selected);
      }

      EdBrowserInitLevel (Browser, Browser->Depth, Child, Name);
      break;
    case EditPlistKindBoolean:
      EdToggle (Browser, Child);
      break;
    case EditPlistKindInteger:
    case EditPlistKindString:
    case EditPlistKindData:
      if (EdIsProtected (Browser, Child)) {
        EdBrowserSetStatus (Browser, TRUE, "Protected: a mistake here can stop OpenCore from starting, use a file editor");
        return;
      }

      if (EditPlistNodeKind (Child) == EditPlistKindInteger) {
        EdEditInteger (Browser, Child, Name);
      } else if (EditPlistNodeKind (Child) == EditPlistKindString) {
        EdEditString (Browser, Child, Name);
      } else {
        EdEditData (Browser, Child, Name);
      }

      break;
    default:
      EdBrowserSetStatus (Browser, TRUE, "This kind of value cannot be edited here");
      break;
  }
}

/**
  Space: switch the value on or off, or the Enabled entry of the selected dictionary.
**/
STATIC
VOID
EdBrowserQuickToggle (
  IN OUT ED_BROWSER  *Browser
  )
{
  XML_NODE  *Child;

  Child = EdBrowserSelected (Browser, NULL);
  if (Child == NULL) {
    return;
  }

  if (EditPlistNodeKind (Child) == EditPlistKindDict) {
    Child = EdFindEnabled (Child);
  }

  if ((Child == NULL) || (EditPlistNodeKind (Child) != EditPlistKindBoolean)) {
    EdBrowserSetStatus (Browser, FALSE, "Nothing to switch here");
    return;
  }

  EdToggle (Browser, Child);
}

/**
  Save the changes.

  @retval TRUE  Changes have been written.
**/
STATIC
BOOLEAN
EdBrowserSave (
  IN OUT ED_BROWSER  *Browser
  )
{
  EFI_STATUS   Status;
  CONST CHAR8  *Reason;
  CHAR8        Line[200];

  if (EditPlistPatchCount (&Browser->Plist) == 0) {
    EdBrowserSetStatus (Browser, FALSE, "No changes to save");
    return FALSE;
  }

  Status = EdCommitConfig (Browser->State, &Browser->Plist, Browser->Baseline, &Reason);
  if (EFI_ERROR (Status)) {
    AsciiSPrint (Line, sizeof (Line), "%a (%r)", Reason != NULL ? Reason : "Failed", Status);
    EdUiNotify (&Browser->State->Ui, Line, TRUE);
    return FALSE;
  }

  return TRUE;
}

STATIC
VOID
EdRestart (
  IN OUT ED_STATE  *State
  )
{
  EdUiPut (&State->Ui, 0, State->Ui.Rows - 3, State->Ui.AttrNormal, "Restarting...", 0);
  ResetWarm ();
}

/**
  Ask what to do with changes which have not been saved.

  @retval 0   Save and restart.
  @retval 1   Save.
  @retval 2   Discard the changes.
  @retval -1  Keep editing.
**/
STATIC
INTN
EdAskUnsaved (
  IN OUT ED_UI  *Ui
  )
{
  CONST CHAR8  *Items[3];
  UINTN        Selected;

  EdUiPut (Ui, 0, Ui->Rows - 3, Ui->AttrChanged, "There are unsaved changes:", 0);
  EdUiHint (Ui, "Enter: choose   Esc: keep editing");
  Items[0] = "Save and restart";
  Items[1] = "Save";
  Items[2] = "Discard the changes";
  Selected = 0;

  return EdUiMenu (Ui, 4, Items, NULL, ARRAY_SIZE (Items), &Selected);
}

STATIC
VOID
EdConfigEditor (
  IN OUT ED_STATE  *State
  )
{
  ED_UI              *Ui;
  ED_BROWSER         *Browser;
  EFI_STATUS         Status;
  CONST CHAR8        *Reason;
  OC_PICKER_KEY_INFO *Key;
  UINTN              Visible;
  INTN               Choice;
  BOOLEAN            Saved;
  BOOLEAN            Redraw;

  Ui = &State->Ui;

  Browser = AllocateZeroPool (sizeof (*Browser));
  if (Browser == NULL) {
    EdUiNotify (Ui, "Not enough memory", TRUE);
    return;
  }

  Browser->State = State;

  Status = EdLoadConfig (State, &Browser->Plist, &Browser->Baseline, &Reason);
  if (EFI_ERROR (Status)) {
    EdUiTitle (Ui, "Edit config", NULL);
    EdUiNotify (Ui, Reason != NULL ? Reason : "Cannot load the config", TRUE);
    FreePool (Browser);
    return;
  }

  EdBrowserInitLevel (Browser, 0, Browser->Plist.Root, "");

  Visible = EdUiContentRows (Ui);
  Redraw  = TRUE;

  while (TRUE) {
    //
    // Clearing the screen is slow on some systems, thus it is only done when something else was shown.
    //
    if (Redraw) {
      EdUiClear (Ui);
      Redraw = FALSE;
    }

    EdBrowserDraw (Browser);

    Key = EdUiWaitKey (Ui, FALSE);

    //
    // A message is shown until the next key.
    //
    Browser->Status[0] = '\0';

    switch (Key->OcKeyCode) {
      case OC_INPUT_UP:
      case OC_INPUT_DOWN:
      case OC_INPUT_TOP:
      case OC_INPUT_BOTTOM:
        EdListMove (&Browser->Levels[Browser->Depth].List, Visible, Key->OcKeyCode);
        break;
      case OC_INPUT_RIGHT:
      case OC_INPUT_CONTINUE:
        EdBrowserActivate (Browser);
        Redraw = TRUE;
        break;
      case OC_INPUT_MORE:
        EdBrowserQuickToggle (Browser);
        break;
      case OC_INPUT_FUNCTIONAL (10):
        Redraw = TRUE;
        if (EdBrowserSave (Browser)) {
          EdSetStatus (State->Status, sizeof (State->Status), &State->StatusIsError, FALSE, "The config is saved, restart to apply the changes");
          if (EdUiConfirm (Ui, "Saved. Restart now to apply the changes?")) {
            EdRestart (State);
          }

          goto Done;
        }

        break;
      case OC_INPUT_LEFT:
      case OC_INPUT_ABORTED:
        Redraw = TRUE;
        if (Browser->Depth > 0) {
          --Browser->Depth;
          break;
        }

        if (EditPlistPatchCount (&Browser->Plist) == 0) {
          goto Done;
        }

        Choice = EdAskUnsaved (Ui);
        if (Choice == 2) {
          goto Done;
        }

        if (Choice >= 0) {
          Saved = EdBrowserSave (Browser);
          if (Saved) {
            EdSetStatus (State->Status, sizeof (State->Status), &State->StatusIsError, FALSE, "The config is saved, restart to apply the changes");
            if (Choice == 0) {
              EdRestart (State);
            }

            goto Done;
          }
        }

        break;
      default:
        break;
    }
  }

Done:
  EditPlistFree (&Browser->Plist);
  FreePool (Browser);
}

//
// Kexts: switching them on and off and putting them in the order they are loaded in.
//

/**
  Largest Info.plist of a kext that is looked into.
**/
#define ED_KEXT_MAX_INFO_SIZE  (1024U * 1024U)

/**
  Most kexts the screen works with. It looks at how the kexts fit together on every change.
**/
#define ED_KEXT_MAX_KEXTS  1024U

/**
  A kext of Kernel > Add as the kexts screen has it.
**/
typedef struct {
  XML_NODE        *Dict;
  //
  // Enabled as it is in the file, NULL if there is no boolean.
  //
  XML_NODE        *Enabled;
  BOOLEAN         WasEnabled;
  //
  // Enabled was not in the file and has been added.
  //
  BOOLEAN         EnabledAdded;
  //
  // Enabled is in the file, but it is not a boolean.
  //
  BOOLEAN         EnabledBroken;
  //
  // The kext has been moved by the user and is not where it was.
  //
  BOOLEAN         Moved;
  CHAR8           *Name;
  CHAR8           *Comment;
  //
  // Why OpenCore does not add the kext, if its files are not in order.
  //
  CONST CHAR8     *FileProblem;
} ED_KEXT_ROW;

typedef struct {
  ED_STATE           *State;
  EDIT_PLIST         Plist;
  UINT32             Baseline;
  XML_NODE           *Array;
  UINT32             Count;
  //
  // By the index of the kext in the file.
  //
  ED_KEXT_ROW        *Rows;
  ED_KEXT_INFO       *Infos;
  BOOLEAN            *Enabled;
  ED_KEXT_PROBLEM    *Problems;
  //
  // For every position in the load order, the index of the kext in the file.
  //
  UINT32             *Order;
  UINT32             ProblemKexts;
  //
  // Identifiers of the kexts which Kernel > Force provides.
  //
  CONST CHAR8        **Provided;
  UINT32             ProvidedCount;
  ED_LIST            List;
  CHAR8              Status[200];
  BOOLEAN            StatusIsError;
} ED_KEXTS;

STATIC
CONST CHAR8 *
EdDictString (
  IN  CONST EDIT_PLIST  *Plist,
  IN  XML_NODE          *Dict,
  IN  CONST CHAR8       *Key
  )
{
  XML_NODE  *Node;

  Node = EditPlistFindChild (Dict, Key);
  if ((Node != NULL) && (EditPlistNodeKind (Node) == EditPlistKindString)) {
    return EditPlistGetContent (Plist, Node);
  }

  return "";
}

STATIC
CHAR8 *
EdCopyString (
  IN  CONST CHAR8  *Text
  )
{
  CHAR8  *Copy;
  UINTN  Size;

  Size = AsciiStrLen (Text) + 1;
  Copy = AllocatePool (Size);
  if (Copy != NULL) {
    CopyMem (Copy, Text, Size);
  }

  return Copy;
}

/**
  Get the path of a file of a kext, the way OpenCore forms it.
**/
STATIC
BOOLEAN
EdKextFilePath (
  OUT CHAR16       *Path,
  IN  UINTN        Size,
  IN  CONST CHAR8  *BundlePath,
  IN  CONST CHAR8  *File
  )
{
  if (EFI_ERROR (OcUnicodeSafeSPrint (Path, Size * sizeof (CHAR16), OPEN_CORE_KEXT_PATH L"%a\\%a", BundlePath, File))) {
    return FALSE;
  }

  UnicodeUefiSlashes (Path);
  return TRUE;
}

/**
  Look into the files of a kext: what OpenCore needs, and what the kext needs.
**/
STATIC
VOID
EdKextReadFiles (
  IN  ED_KEXTS      *Kexts,
  IN  ED_KEXT_ROW   *Row,
  OUT ED_KEXT_INFO  *Info
  )
{
  CONST CHAR8  *BundlePath;
  CONST CHAR8  *PlistPath;
  CONST CHAR8  *ExecutablePath;
  CHAR16       Path[OC_STORAGE_SAFE_PATH_MAX];
  VOID         *Data;
  UINT32       Size;

  ZeroMem (Info, sizeof (*Info));

  if (Row->Dict == NULL) {
    Row->FileProblem   = "this is not a dictionary";
    Info->FilesMissing = TRUE;
    return;
  }

  BundlePath     = EdDictString (&Kexts->Plist, Row->Dict, "BundlePath");
  PlistPath      = EdDictString (&Kexts->Plist, Row->Dict, "PlistPath");
  ExecutablePath = EdDictString (&Kexts->Plist, Row->Dict, "ExecutablePath");

  //
  // Kexts for 32-bit systems are not looked at.
  //
  if (AsciiStrCmp (EdDictString (&Kexts->Plist, Row->Dict, "Arch"), "i386") == 0) {
    Info->HasInfo = TRUE;
    return;
  }

  if ((BundlePath[0] == '\0') || (PlistPath[0] == '\0')) {
    Row->FileProblem   = "BundlePath or PlistPath is empty";
    Info->FilesMissing = TRUE;
    return;
  }

  Data = NULL;
  Size = 0;
  if (EdKextFilePath (Path, ARRAY_SIZE (Path), BundlePath, PlistPath)) {
    Data = OcStorageReadFileUnicode (Kexts->State->Storage, Path, &Size);
  }

  if ((Data == NULL) || (Size > ED_KEXT_MAX_INFO_SIZE) || EFI_ERROR (EdKextParseInfo (Data, Size, Info))) {
    Row->FileProblem   = "Info.plist is missing or broken";
    Info->FilesMissing = TRUE;
  }

  if (Data != NULL) {
    FreePool (Data);
  }

  if (Info->FilesMissing) {
    return;
  }

  if (  (ExecutablePath[0] != '\0')
     && (!EdKextFilePath (Path, ARRAY_SIZE (Path), BundlePath, ExecutablePath)
         || !OcStorageExistsFileUnicode (Kexts->State->Storage, Path)))
  {
    Row->FileProblem   = "the executable is missing";
    Info->FilesMissing = TRUE;
  }
}

STATIC
BOOLEAN
EdKextIsEnabled (
  IN  CONST ED_KEXTS  *Kexts,
  IN  UINT32          Kext
  )
{
  CONST ED_KEXT_ROW  *Row;

  Row = &Kexts->Rows[Kext];
  if (Row->Enabled != NULL) {
    return EditPlistGetBoolean (&Kexts->Plist, Row->Enabled);
  }

  return Row->EnabledAdded;
}

/**
  Find out the order and the state of the kexts, and what is wrong with them.
**/
STATIC
VOID
EdKextsRefresh (
  IN OUT ED_KEXTS  *Kexts
  )
{
  UINT32  Index;

  for (Index = 0; Index < Kexts->Count; ++Index) {
    Kexts->Order[Index]   = EditPlistArrayOriginalIndex (&Kexts->Plist, Kexts->Array, Index);
    Kexts->Enabled[Index] = EdKextIsEnabled (Kexts, Index);
  }

  //
  // A kext that is back in its place is not moved any more, and nothing is when all are back.
  //
  for (Index = 0; Index < Kexts->Count; ++Index) {
    if (Kexts->Order[Index] == Index) {
      Kexts->Rows[Index].Moved = FALSE;
    }
  }

  Kexts->ProblemKexts = EdKextFindProblems (
                          Kexts->Infos,
                          Kexts->Enabled,
                          Kexts->Count,
                          Kexts->Order,
                          Kexts->Provided,
                          Kexts->ProvidedCount,
                          Kexts->Problems
                          );
}

STATIC
VOID
EdKextsFree (
  IN OUT ED_KEXTS  *Kexts
  )
{
  UINT32  Index;

  if (Kexts->Rows != NULL) {
    for (Index = 0; Index < Kexts->Count; ++Index) {
      if (Kexts->Rows[Index].Name != NULL) {
        FreePool (Kexts->Rows[Index].Name);
      }

      if (Kexts->Rows[Index].Comment != NULL) {
        FreePool (Kexts->Rows[Index].Comment);
      }
    }

    FreePool (Kexts->Rows);
  }

  if (Kexts->Infos != NULL) {
    for (Index = 0; Index < Kexts->Count; ++Index) {
      EdKextFreeInfo (&Kexts->Infos[Index]);
    }

    FreePool (Kexts->Infos);
  }

  if (Kexts->Enabled != NULL) {
    FreePool (Kexts->Enabled);
  }

  if (Kexts->Problems != NULL) {
    FreePool (Kexts->Problems);
  }

  if (Kexts->Order != NULL) {
    FreePool (Kexts->Order);
  }

  if (Kexts->Provided != NULL) {
    FreePool ((VOID *)Kexts->Provided);
  }

  EditPlistFree (&Kexts->Plist);
  FreePool (Kexts);
}

/**
  Read the kexts of the configuration in use.

  @param[in]   State   Editor state.
  @param[out]  Result  What was read.
  @param[out]  Reason  Text to show on failure.
**/
STATIC
EFI_STATUS
EdKextsLoad (
  IN  ED_STATE    *State,
  OUT ED_KEXTS    **Result,
  OUT CONST CHAR8 **Reason
  )
{
  EFI_STATUS   Status;
  ED_KEXTS     *Kexts;
  XML_NODE     *Force;
  XML_NODE     *Entry;
  XML_NODE     *Enabled;
  CONST CHAR8  *Name;
  UINT32       Index;

  *Result = NULL;

  Kexts = AllocateZeroPool (sizeof (*Kexts));
  if (Kexts == NULL) {
    *Reason = "Not enough memory";
    return EFI_OUT_OF_RESOURCES;
  }

  Kexts->State = State;

  Status = EdLoadConfig (State, &Kexts->Plist, &Kexts->Baseline, Reason);
  if (EFI_ERROR (Status)) {
    FreePool (Kexts);
    return Status;
  }

  Kexts->Array = EditPlistFindChild (EditPlistFindChild (Kexts->Plist.Root, "Kernel"), "Add");
  if (EditPlistNodeKind (Kexts->Array) != EditPlistKindArray) {
    *Reason = "Kernel > Add is not found in the config";
    EdKextsFree (Kexts);
    return EFI_NOT_FOUND;
  }

  Kexts->Count = EditPlistChildCount (Kexts->Array);
  if (Kexts->Count > ED_KEXT_MAX_KEXTS) {
    *Reason = "Too many kexts to work with";
    EdKextsFree (Kexts);
    return EFI_UNSUPPORTED;
  }

  Kexts->Rows     = AllocateZeroPool (((UINTN)Kexts->Count + 1) * sizeof (*Kexts->Rows));
  Kexts->Infos    = AllocateZeroPool (((UINTN)Kexts->Count + 1) * sizeof (*Kexts->Infos));
  Kexts->Enabled  = AllocateZeroPool (((UINTN)Kexts->Count + 1) * sizeof (*Kexts->Enabled));
  Kexts->Problems = AllocateZeroPool (((UINTN)Kexts->Count + 1) * sizeof (*Kexts->Problems));
  Kexts->Order    = AllocateZeroPool (((UINTN)Kexts->Count + 1) * sizeof (*Kexts->Order));
  if (  (Kexts->Rows == NULL) || (Kexts->Infos == NULL) || (Kexts->Enabled == NULL)
     || (Kexts->Problems == NULL) || (Kexts->Order == NULL))
  {
    *Reason = "Not enough memory";
    EdKextsFree (Kexts);
    return EFI_OUT_OF_RESOURCES;
  }

  for (Index = 0; Index < Kexts->Count; ++Index) {
    Entry = EditPlistChildValue (Kexts->Array, Index);

    if (EditPlistNodeKind (Entry) == EditPlistKindDict) {
      Kexts->Rows[Index].Dict = Entry;
      Enabled                 = EditPlistFindChild (Entry, "Enabled");
      if (Enabled != NULL) {
        if (EditPlistNodeKind (Enabled) == EditPlistKindBoolean) {
          Kexts->Rows[Index].Enabled    = Enabled;
          Kexts->Rows[Index].WasEnabled = EditPlistGetBoolean (&Kexts->Plist, Enabled);
        } else {
          Kexts->Rows[Index].EnabledBroken = TRUE;
        }
      }

      Name = EdDictString (&Kexts->Plist, Entry, "BundlePath");
      Kexts->Rows[Index].Name    = EdCopyString (Name[0] != '\0' ? Name : "(no BundlePath)");
      Kexts->Rows[Index].Comment = EdCopyString (EdDictString (&Kexts->Plist, Entry, "Comment"));
    } else {
      Kexts->Rows[Index].Name    = EdCopyString ("(not a dictionary)");
      Kexts->Rows[Index].Comment = EdCopyString ("");
    }

    if ((Kexts->Rows[Index].Name == NULL) || (Kexts->Rows[Index].Comment == NULL)) {
      *Reason = "Not enough memory";
      EdKextsFree (Kexts);
      return EFI_OUT_OF_RESOURCES;
    }

    EdKextReadFiles (Kexts, &Kexts->Rows[Index], &Kexts->Infos[Index]);
  }

  //
  // Kernel > Force: these kexts are read from the system, and are there before the others.
  //
  Force = EditPlistFindChild (EditPlistFindChild (Kexts->Plist.Root, "Kernel"), "Force");
  if (EditPlistNodeKind (Force) == EditPlistKindArray) {
    Kexts->Provided = AllocateZeroPool (((UINTN)EditPlistChildCount (Force) + 1) * sizeof (*Kexts->Provided));
    for (Index = 0; (Kexts->Provided != NULL) && (Index < EditPlistChildCount (Force)); ++Index) {
      Entry   = EditPlistChildValue (Force, Index);
      Enabled = EditPlistFindChild (Entry, "Enabled");
      Name    = EdDictString (&Kexts->Plist, Entry, "Identifier");
      if (  (Enabled != NULL)
         && (EditPlistNodeKind (Enabled) == EditPlistKindBoolean)
         && EditPlistGetBoolean (&Kexts->Plist, Enabled)
         && (Name[0] != '\0'))
      {
        Kexts->Provided[Kexts->ProvidedCount++] = Name;
      }
    }
  }

  for (Index = 0; Index < Kexts->Count; ++Index) {
    Kexts->Order[Index] = Index;
  }

  EdKextsRefresh (Kexts);

  *Result = Kexts;
  return EFI_SUCCESS;
}

STATIC
VOID
EdKextsSetStatus (
  OUT ED_KEXTS     *Kexts,
  IN  BOOLEAN      IsError,
  IN  CONST CHAR8  *Text
  )
{
  EdSetStatus (Kexts->Status, sizeof (Kexts->Status), &Kexts->StatusIsError, IsError, Text);
}

/**
  Get how many kexts are not as they are in the file.
**/
STATIC
UINT32
EdKextsChanges (
  IN  CONST ED_KEXTS  *Kexts
  )
{
  UINT32  Index;
  UINT32  Changes;

  Changes = 0;
  for (Index = 0; Index < Kexts->Count; ++Index) {
    if (Kexts->Rows[Index].Moved || (Kexts->Enabled[Index] != Kexts->Rows[Index].WasEnabled)) {
      ++Changes;
    }
  }

  return Changes;
}

STATIC
VOID
EdKextItem (
  IN  VOID     *Context,
  IN  UINTN    Index,
  OUT CHAR8    *Text,
  IN  UINTN    Size,
  OUT BOOLEAN  *Changed,
  OUT BOOLEAN  *Dim,
  OUT BOOLEAN  *Error
  )
{
  ED_KEXTS     *Kexts;
  UINT32       Kext;
  ED_KEXT_ROW  *Row;
  BOOLEAN      On;

  Kexts = Context;
  Kext  = Kexts->Order[Index];
  Row   = &Kexts->Rows[Kext];
  On    = Kexts->Enabled[Kext];

  *Error   = Kexts->Problems[Kext].Kind != EdKextOk;
  *Changed = Row->Moved || (On != Row->WasEnabled);
  *Dim     = !On;

  AsciiSPrint (
    Text,
    Size,
    Kexts->Count >= 100 ? "%4u [%c]%c %a%a%a" : "%2u [%c]%c %a%a%a",
    (UINT32)(Index + 1),
    On ? 'x' : ' ',
    *Error ? '!' : (*Changed ? '*' : ' '),
    Row->Name,
    Row->Comment[0] != '\0' ? "   " : "",
    Row->Comment
    );
}

/**
  Say what is the matter with a kext, or what it is.
**/
STATIC
VOID
EdKextDescribe (
  IN  CONST ED_KEXTS  *Kexts,
  IN  UINT32          Kext,
  OUT CHAR8           *Text,
  IN  UINTN           Size,
  OUT BOOLEAN         *IsProblem
  )
{
  CONST ED_KEXT_PROBLEM  *Problem;
  CONST ED_KEXT_INFO     *Info;
  CHAR8                  More[32];

  Problem    = &Kexts->Problems[Kext];
  Info       = &Kexts->Infos[Kext];
  *IsProblem = Problem->Kind != EdKextOk;
  More[0]    = '\0';

  if (Problem->LibraryProblems > 1) {
    AsciiSPrint (More, sizeof (More), "  (+%u more)", Problem->LibraryProblems - 1);
  }

  switch (Problem->Kind) {
    case EdKextNoFiles:
      AsciiSPrint (Text, Size, "OpenCore will not add this kext, %a", Kexts->Rows[Kext].FileProblem != NULL ? Kexts->Rows[Kext].FileProblem : "its files are missing");
      break;
    //
    // The long part is last, as it is cut when it does not fit.
    //
    case EdKextOrder:
      AsciiSPrint (Text, Size, "Loaded too early, F5 fixes it%a: needs %a", More, Info->Libraries[Problem->Library]);
      break;
    case EdKextDisabled:
      AsciiSPrint (Text, Size, "Needs a kext which is off%a: %a (%a)", More, Info->Libraries[Problem->Library], Kexts->Rows[Problem->Provider].Name);
      break;
    case EdKextMissing:
      AsciiSPrint (Text, Size, "Needs a kext which is not in the list%a: %a", More, Info->Libraries[Problem->Library]);
      break;
    default:
      if (Info->Identifier != NULL) {
        AsciiSPrint (Text, Size, "%a", Info->Identifier);
      } else {
        Text[0] = '\0';
      }

      break;
  }
}

STATIC
VOID
EdKextsDraw (
  IN OUT ED_KEXTS  *Kexts
  )
{
  ED_UI    *Ui;
  UINT32   Index;
  UINT32   On;
  UINT32   Changes;
  CHAR8    Name[OC_CONFIG_MAX_NAME + 1];
  CHAR8    Subtitle[200];
  CHAR8    Line[256];
  BOOLEAN  IsProblem;

  Ui = &Kexts->State->Ui;

  On = 0;
  for (Index = 0; Index < Kexts->Count; ++Index) {
    if (Kexts->Enabled[Index]) {
      ++On;
    }
  }

  EdConfigNameAscii (Name, sizeof (Name));
  AsciiSPrint (Subtitle, sizeof (Subtitle), "%a: %u of %u on", Name, On, Kexts->Count);

  if (Kexts->ProblemKexts > 0) {
    AsciiSPrint (Line, sizeof (Line), ", %u with problems", Kexts->ProblemKexts);
    AsciiStrCatS (Subtitle, sizeof (Subtitle), Line);
  }

  Changes = EdKextsChanges (Kexts);
  if (Changes > 0) {
    AsciiSPrint (Line, sizeof (Line), ", %u unsaved changes", Changes);
    AsciiStrCatS (Subtitle, sizeof (Subtitle), Line);
  }

  EdUiTitle (Ui, "Kexts", Subtitle);
  EdUiPut (Ui, 0, 2, Ui->AttrHint, "Kexts are loaded from the top to the bottom, each needs the ones above it", 0);

  EdListDraw (Ui, 3, EdUiContentRows (Ui), &Kexts->List, EdKextItem, Kexts);

  if (Kexts->Count == 0) {
    EdUiPut (Ui, 0, 3, Ui->AttrHint, "  (there are no kexts in Kernel > Add)", 0);
  }

  if (Kexts->Status[0] != '\0') {
    EdUiStatus (Ui, Kexts->StatusIsError ? Ui->AttrError : Ui->AttrChanged, Kexts->Status);
  } else if (Kexts->Count > 0) {
    EdKextDescribe (Kexts, Kexts->Order[Kexts->List.Selected], Line, sizeof (Line), &IsProblem);
    EdUiStatus (Ui, IsProblem ? Ui->AttrError : Ui->AttrHint, Line);
  } else {
    EdUiStatus (Ui, Ui->AttrHint, "");
  }

  EdUiHint (Ui, "Space: on/off   +/-: move down/up   F5: fix the order   F10: save   Esc: back");
}

/**
  Make sure that the selected item is in sight.
**/
STATIC
VOID
EdKextsReveal (
  IN OUT ED_KEXTS  *Kexts,
  IN     UINTN     Visible
  )
{
  if (Kexts->List.Selected < Kexts->List.Top) {
    Kexts->List.Top = Kexts->List.Selected;
  } else if (Kexts->List.Selected >= Kexts->List.Top + Visible) {
    Kexts->List.Top = Kexts->List.Selected - Visible + 1;
  }
}

STATIC
VOID
EdKextsToggle (
  IN OUT ED_KEXTS  *Kexts
  )
{
  EFI_STATUS   Status;
  UINT32       Kext;
  ED_KEXT_ROW  *Row;
  BOOLEAN      Now;
  BOOLEAN      Value;

  if (Kexts->Count == 0) {
    return;
  }

  Kext = Kexts->Order[Kexts->List.Selected];
  Row  = &Kexts->Rows[Kext];
  Now  = Kexts->Enabled[Kext];

  if (Row->Dict == NULL) {
    EdKextsSetStatus (Kexts, TRUE, "This entry is not a dictionary, it can be seen in Edit config.plist");
    return;
  }

  if (Row->EnabledBroken) {
    EdKextsSetStatus (Kexts, TRUE, "Enabled is not a boolean here, it can be fixed in Edit config.plist");
    return;
  }

  if (Row->Enabled != NULL) {
    Value  = !Now;
    Status = EditPlistSetBoolean (&Kexts->Plist, Row->Enabled, Value);
    if (!EFI_ERROR (Status) && (Value == Row->WasEnabled)) {
      //
      // Back to what the file says.
      //
      EditPlistRevert (&Kexts->Plist, Row->Enabled);
    }
  } else if (Row->EnabledAdded) {
    EditPlistRevertAdded (&Kexts->Plist, Row->Dict);
    Row->EnabledAdded = FALSE;
    Status            = EFI_SUCCESS;
  } else {
    //
    // The file says nothing, which means off.
    //
    Status = EditPlistAddBoolean (&Kexts->Plist, Row->Dict, "Enabled", TRUE);
    if (!EFI_ERROR (Status)) {
      Row->EnabledAdded = TRUE;
    }
  }

  if (EFI_ERROR (Status)) {
    EdKextsSetStatus (Kexts, TRUE, "This kext cannot be switched here, it can be done in Edit config.plist");
    return;
  }

  EdKextsRefresh (Kexts);
}

STATIC
VOID
EdKextsMove (
  IN OUT ED_KEXTS  *Kexts,
  IN     BOOLEAN   Down,
  IN     UINTN     Visible
  )
{
  EFI_STATUS  Status;
  UINTN       Target;

  if (Kexts->Count == 0) {
    return;
  }

  if (Down && (Kexts->List.Selected + 1 >= Kexts->Count)) {
    EdKextsSetStatus (Kexts, FALSE, "This one is the last already");
    return;
  }

  if (!Down && (Kexts->List.Selected == 0)) {
    EdKextsSetStatus (Kexts, FALSE, "This one is the first already");
    return;
  }

  Target = Down ? Kexts->List.Selected + 1 : Kexts->List.Selected - 1;
  Status = EditPlistArrayMove (&Kexts->Plist, Kexts->Array, (UINT32)Kexts->List.Selected, (UINT32)Target);
  if (EFI_ERROR (Status)) {
    EdKextsSetStatus (Kexts, TRUE, "The kexts cannot be moved in this config, it is not laid out for that");
    return;
  }

  Kexts->Rows[Kexts->Order[Kexts->List.Selected]].Moved = TRUE;
  Kexts->List.Selected                                  = Target;
  EdKextsReveal (Kexts, Visible);
  EdKextsRefresh (Kexts);
}

/**
  Tell what is still wrong with the kexts after their order is as good as it gets.

  @param[in]   Kexts  Kexts.
  @param[in]   Prefix  Start of the message.
  @param[out]  Text    Message.
  @param[in]   Size    Size of Text.
**/
STATIC
VOID
EdKextsLeftovers (
  IN  CONST ED_KEXTS  *Kexts,
  IN  CONST CHAR8     *Prefix,
  OUT CHAR8           *Text,
  IN  UINTN           Size
  )
{
  UINT32  Index;
  UINT32  Files;
  UINT32  Off;
  UINT32  Order;

  Files = 0;
  Off   = 0;
  Order = 0;

  for (Index = 0; Index < Kexts->Count; ++Index) {
    switch (Kexts->Problems[Index].Kind) {
      case EdKextNoFiles:
        ++Files;
        break;
      case EdKextOrder:
        ++Order;
        break;
      case EdKextDisabled:
      case EdKextMissing:
        ++Off;
        break;
      default:
        break;
    }
  }

  if (Order > 0) {
    //
    // Kexts which wait for each other.
    //
    AsciiSPrint (Text, Size, "Some kexts need each other, their order cannot be fixed");
  } else if ((Files > 0) && (Off > 0)) {
    AsciiSPrint (Text, Size, "%a Marked kexts lack files or need kexts that are off/missing", Prefix);
  } else if (Files > 0) {
    AsciiSPrint (Text, Size, "%a Marked kexts lack files which OpenCore needs", Prefix);
  } else {
    AsciiSPrint (Text, Size, "%a Marked kexts need kexts that are off or missing", Prefix);
  }
}

/**
  Put the kexts after the ones they need.
**/
STATIC
VOID
EdKextsFix (
  IN OUT ED_KEXTS  *Kexts,
  IN     UINTN     Visible
  )
{
  EFI_STATUS  Status;
  UINT32      *Order;
  UINT32      Selected;
  UINT32      Index;
  UINT32      Changed;
  CHAR8       Line[128];

  if (Kexts->Count == 0) {
    return;
  }

  Order = AllocatePool (((UINTN)Kexts->Count + 1) * sizeof (*Order));
  if (Order == NULL) {
    EdKextsSetStatus (Kexts, TRUE, "Not enough memory");
    return;
  }

  CopyMem (Order, Kexts->Order, (UINTN)Kexts->Count * sizeof (*Order));
  Changed = EdKextSort (Kexts->Infos, Kexts->Enabled, Kexts->Count, Order);

  if (Changed == 0) {
    if (Kexts->ProblemKexts == 0) {
      EdKextsSetStatus (Kexts, FALSE, "The order is right already");
    } else {
      EdKextsLeftovers (Kexts, "The order is right.", Line, sizeof (Line));
      EdKextsSetStatus (Kexts, TRUE, Line);
    }

    FreePool (Order);
    return;
  }

  Selected = Kexts->Order[Kexts->List.Selected];
  Status   = EditPlistArraySetOrder (&Kexts->Plist, Kexts->Array, Order, Kexts->Count);
  if (EFI_ERROR (Status)) {
    EdKextsSetStatus (Kexts, TRUE, "The kexts cannot be moved in this config, it is not laid out for that");
    FreePool (Order);
    return;
  }

  //
  // The ones which got another place are shown, and the selection stays with its kext.
  //
  for (Index = 0; Index < Kexts->Count; ++Index) {
    if (Kexts->Order[Index] != Order[Index]) {
      Kexts->Rows[Order[Index]].Moved = TRUE;
    }

    if (Order[Index] == Selected) {
      Kexts->List.Selected = Index;
    }
  }

  FreePool (Order);
  EdKextsReveal (Kexts, Visible);
  EdKextsRefresh (Kexts);

  if (Kexts->ProblemKexts == 0) {
    EdKextsSetStatus (Kexts, FALSE, "The order is fixed: every kext comes after the ones it needs");
  } else {
    EdKextsLeftovers (Kexts, "The order is fixed.", Line, sizeof (Line));
    EdKextsSetStatus (Kexts, TRUE, Line);
  }
}

/**
  Check that the text to be saved has the kexts in the order, and in the state, which are on the screen.
**/
STATIC
BOOLEAN
EdKextsVerify (
  IN  CONST ED_KEXTS  *Kexts
  )
{
  EFI_STATUS  Status;
  CHAR8       *Text;
  UINT32      Size;
  EDIT_PLIST  Again;
  XML_NODE    *Array;
  XML_NODE    *Entry;
  XML_NODE    *Enabled;
  UINT32      Position;
  UINT32      Kext;
  BOOLEAN     Right;
  BOOLEAN     On;

  Status = EditPlistRender (&Kexts->Plist, &Text, &Size);
  if (EFI_ERROR (Status)) {
    return FALSE;
  }

  Status = EditPlistLoad (&Again, Text, Size);
  FreePool (Text);
  if (EFI_ERROR (Status)) {
    return FALSE;
  }

  Right = FALSE;
  Array = EditPlistFindChild (EditPlistFindChild (Again.Root, "Kernel"), "Add");
  if ((EditPlistNodeKind (Array) == EditPlistKindArray) && (EditPlistChildCount (Array) == Kexts->Count)) {
    Right = TRUE;
    for (Position = 0; Right && (Position < Kexts->Count); ++Position) {
      Kext  = Kexts->Order[Position];
      Entry = EditPlistChildValue (Array, Position);

      if (Kexts->Rows[Kext].Dict == NULL) {
        Right = EditPlistNodeKind (Entry) != EditPlistKindDict;
        continue;
      }

      Enabled = EditPlistFindChild (Entry, "Enabled");
      On      = (Enabled != NULL) && (EditPlistNodeKind (Enabled) == EditPlistKindBoolean) && EditPlistGetBoolean (&Again, Enabled);
      Right   = EditPlistNodeKind (Entry) == EditPlistKindDict
                && AsciiStrCmp (EdDictString (&Again, Entry, "BundlePath"), EdDictString (&Kexts->Plist, Kexts->Rows[Kext].Dict, "BundlePath")) == 0
                && (Kexts->Rows[Kext].EnabledBroken || (On == Kexts->Enabled[Kext]));
    }
  }

  EditPlistFree (&Again);
  return Right;
}

STATIC
BOOLEAN
EdKextsSave (
  IN OUT ED_KEXTS  *Kexts
  )
{
  EFI_STATUS   Status;
  CONST CHAR8  *Reason;
  CHAR8        Line[200];

  if (EditPlistPatchCount (&Kexts->Plist) == 0) {
    EdKextsSetStatus (Kexts, FALSE, "No changes to save");
    return FALSE;
  }

  //
  // Nothing is written which is not what the user sees.
  //
  if (!EdKextsVerify (Kexts)) {
    EdUiNotify (&Kexts->State->Ui, "Internal error: the changed config does not match the list, nothing was written", TRUE);
    return FALSE;
  }

  Status = EdCommitConfig (Kexts->State, &Kexts->Plist, Kexts->Baseline, &Reason);
  if (EFI_ERROR (Status)) {
    AsciiSPrint (Line, sizeof (Line), "%a (%r)", Reason != NULL ? Reason : "Failed", Status);
    EdUiNotify (&Kexts->State->Ui, Line, TRUE);
    return FALSE;
  }

  return TRUE;
}

STATIC
VOID
EdKextsEditor (
  IN OUT ED_STATE  *State
  )
{
  ED_UI               *Ui;
  ED_KEXTS            *Kexts;
  EFI_STATUS          Status;
  CONST CHAR8         *Reason;
  OC_PICKER_KEY_INFO  *Key;
  UINTN               Visible;
  INTN                Choice;
  BOOLEAN             Redraw;

  Ui = &State->Ui;

  EdUiClear (Ui);
  EdUiTitle (Ui, "Kexts", NULL);
  EdUiStatus (Ui, Ui->AttrHint, "Reading the kexts...");

  Status = EdKextsLoad (State, &Kexts, &Reason);
  if (EFI_ERROR (Status)) {
    EdUiNotify (Ui, Reason != NULL ? Reason : "Cannot read the kexts", TRUE);
    return;
  }

  Visible           = EdUiContentRows (Ui);
  Redraw            = TRUE;
  Kexts->List.Count = Kexts->Count;

  while (TRUE) {
    if (Redraw) {
      EdUiClear (Ui);
      Redraw = FALSE;
    }

    EdKextsDraw (Kexts);

    Key = EdUiWaitKeyWithPlusMinus (Ui);

    //
    // A message is shown until the next key.
    //
    Kexts->Status[0] = '\0';

    switch (Key->OcKeyCode) {
      case OC_INPUT_UP:
      case OC_INPUT_DOWN:
      case OC_INPUT_TOP:
      case OC_INPUT_BOTTOM:
        EdListMove (&Kexts->List, Visible, Key->OcKeyCode);
        break;
      case OC_INPUT_MORE:
      case OC_INPUT_CONTINUE:
        EdKextsToggle (Kexts);
        break;
      case OC_INPUT_PLUS:
        EdKextsMove (Kexts, TRUE, Visible);
        break;
      case OC_INPUT_MINUS:
        EdKextsMove (Kexts, FALSE, Visible);
        break;
      case OC_INPUT_FUNCTIONAL (5):
        EdKextsFix (Kexts, Visible);
        break;
      case OC_INPUT_FUNCTIONAL (10):
        Redraw = TRUE;
        if (EdKextsSave (Kexts)) {
          EdSetStatus (State->Status, sizeof (State->Status), &State->StatusIsError, FALSE, "The config is saved, restart to apply the changes");
          if (EdUiConfirm (Ui, "Saved. Restart now to apply the changes?")) {
            EdRestart (State);
          }

          goto Done;
        }

        break;
      case OC_INPUT_LEFT:
      case OC_INPUT_ABORTED:
        Redraw = TRUE;
        if (EditPlistPatchCount (&Kexts->Plist) == 0) {
          goto Done;
        }

        Choice = EdAskUnsaved (Ui);
        if (Choice == 2) {
          goto Done;
        }

        if ((Choice >= 0) && EdKextsSave (Kexts)) {
          EdSetStatus (State->Status, sizeof (State->Status), &State->StatusIsError, FALSE, "The config is saved, restart to apply the changes");
          if (Choice == 0) {
            EdRestart (State);
          }

          goto Done;
        }

        break;
      default:
        break;
    }
  }

Done:
  EdKextsFree (Kexts);
}

//
// Choosing the configuration file.
//

/**
  Short note on where the configuration in use comes from, for the main menu.
**/
STATIC
CONST CHAR8 *
EdConfigSourceNote (
  IN  OC_CONFIG_SOURCE  Source
  )
{
  switch (Source) {
    case OcConfigSourceOnce:
      return "  (this start only)";
    case OcConfigSourceKept:
      return "  (kept)";
    case OcConfigSourceFallback:
      return "  (the chosen one failed)";
    default:
      return "";
  }
}

/**
  Where the configuration in use comes from, for the list of configurations.
**/
STATIC
CONST CHAR8 *
EdConfigSourceDescription (
  IN  OC_CONFIG_SOURCE  Source
  )
{
  switch (Source) {
    case OcConfigSourceOnce:
      return "chosen for this start only";
    case OcConfigSourceKept:
      return "chosen to stay until another one is chosen";
    default:
      return "the default";
  }
}

STATIC
VOID
EdConfigItem (
  IN  VOID     *Context,
  IN  UINTN    Index,
  OUT CHAR8    *Text,
  IN  UINTN    Size,
  OUT BOOLEAN  *Changed,
  OUT BOOLEAN  *Dim,
  OUT BOOLEAN  *Error
  )
{
  OC_CONFIG_FILE  *Files;
  CHAR8           Name[OC_CONFIG_MAX_NAME + 1];
  BOOLEAN         InUse;
  BOOLEAN         IsDefault;

  Files = Context;
  EdUnicodeToAscii (Files[Index].Name, Name, sizeof (Name));

  InUse     = EdSameConfigName (Files[Index].Name, OcMiscGetConfigName ());
  IsDefault = EdSameConfigName (Files[Index].Name, OPEN_CORE_CONFIG_PATH);

  *Changed = FALSE;
  *Dim     = FALSE;
  *Error   = FALSE;
  AsciiSPrint (
    Text,
    Size,
    "%a%a",
    Name,
    InUse ? (IsDefault ? "   (in use, default)" : "   (in use)") : (IsDefault ? "   (default)" : "")
    );
}

/**
  Look into a configuration before starting OpenCore with it.

  @param[in]   State         Editor state.
  @param[in]   Name          Name of the configuration file.
  @param[out]  AllowsEditor  Whether the configuration enables this editor.
  @param[out]  Reason        Text to show on failure.
**/
STATIC
EFI_STATUS
EdCheckConfig (
  IN  ED_STATE      *State,
  IN  CONST CHAR16  *Name,
  OUT BOOLEAN       *AllowsEditor,
  OUT CONST CHAR8   **Reason
  )
{
  EFI_STATUS  Status;
  VOID        *Data;
  UINT32      Size;
  UINT32      Errors;
  EDIT_PLIST  Plist;
  XML_NODE    *Node;

  *AllowsEditor = FALSE;
  *Reason       = NULL;

  Data = OcStorageReadFileUnicode (State->Storage, Name, &Size);
  if (Data == NULL) {
    *Reason = "cannot read the file";
    return EFI_NOT_FOUND;
  }

  Status = EditPlistLoad (&Plist, Data, Size);
  if (EFI_ERROR (Status)) {
    FreePool (Data);
    *Reason = "this is not a valid plist with a dictionary";
    return Status;
  }

  Status = EditPlistCountErrors (Data, Size, &Errors);
  FreePool (Data);
  if (EFI_ERROR (Status)) {
    EditPlistFree (&Plist);
    *Reason = "OpenCore cannot load it";
    return Status;
  }

  Node = EditPlistFindChild (EditPlistFindChild (EditPlistFindChild (Plist.Root, "Misc"), "Security"), "AllowPickerEdit");

  *AllowsEditor = (Node != NULL) && (EditPlistNodeKind (Node) == EditPlistKindBoolean) && EditPlistGetBoolean (&Plist, Node);

  EditPlistFree (&Plist);
  return EFI_SUCCESS;
}

/**
  Ask how a configuration is to be used, then restart OpenCore with it.
**/
STATIC
VOID
EdConfigSwitch (
  IN OUT ED_STATE      *State,
  IN     CONST CHAR16  *Name
  )
{
  ED_UI        *Ui;
  CHAR8        AsciiName[OC_CONFIG_MAX_NAME + 1];
  CHAR8        Line[256];
  CHAR8        Once[128];
  CONST CHAR8  *Items[2];
  BOOLEAN      Enabled[2];
  UINTN        Count;
  UINTN        Selected;
  INTN         Choice;
  BOOLEAN      AllowsEditor;
  BOOLEAN      IsDefault;
  EFI_STATUS   Status;
  CONST CHAR8  *Reason;

  Ui = &State->Ui;

  if (EdSameConfigName (Name, OcMiscGetConfigName ())) {
    EdUiNotify (Ui, "This config is in use already", FALSE);
    return;
  }

  EdUnicodeToAscii (Name, AsciiName, sizeof (AsciiName));

  //
  // Do not restart into something which cannot work.
  //
  Status = EdCheckConfig (State, Name, &AllowsEditor, &Reason);
  if (EFI_ERROR (Status)) {
    AsciiSPrint (Line, sizeof (Line), "%a: %a", AsciiName, Reason);
    EdUiNotify (Ui, Line, TRUE);
    return;
  }

  IsDefault = EdSameConfigName (Name, OPEN_CORE_CONFIG_PATH);

  EdUiClear (Ui);
  AsciiSPrint (Line, sizeof (Line), "Start OpenCore with %a", AsciiName);
  EdUiTitle (Ui, Line, "OpenCore restarts to apply a different config");

  if (IsDefault) {
    Items[0]   = "Restart with the default config (and forget the choice made before)";
    Enabled[0] = TRUE;
    Count      = 1;
  } else {
    AsciiSPrint (Once, sizeof (Once), "This time only  (the next start uses %s again)", OPEN_CORE_CONFIG_PATH);
    Items[0]   = Once;
    Items[1]   = "Keep it        (until another config is chosen)";
    Enabled[0] = TRUE;
    Enabled[1] = AllowsEditor;
    Count      = 2;

    if (!AllowsEditor) {
      EdUiPut (Ui, 0, 3, Ui->AttrError, "AllowPickerEdit is off in this config, so this editor would not be there to", 0);
      EdUiPut (Ui, 0, 4, Ui->AttrError, "switch back. It can be used for one start only.", 0);
    }
  }

  Selected = 0;
  EdUiHint (Ui, "Enter: restart now   Esc: back");

  Choice = EdUiMenu (Ui, 6, Items, Enabled, Count, &Selected);
  if (Choice < 0) {
    return;
  }

  Status = OcMiscSetConfigChoice (State->Storage, Name, !IsDefault && (Choice == 1));
  if (EFI_ERROR (Status)) {
    AsciiSPrint (Line, sizeof (Line), "Cannot save the choice (%r), the file system may be read-only", Status);
    EdUiNotify (Ui, Line, TRUE);
    return;
  }

  EdRestart (State);

  //
  // Still here means that the system cannot restart by itself.
  //
  EdSetStatus (
    State->Status,
    sizeof (State->Status),
    &State->StatusIsError,
    FALSE,
    "The choice is saved, restart the computer to apply it"
    );
}

STATIC
VOID
EdConfigChooser (
  IN OUT ED_STATE  *State
  )
{
  ED_UI               *Ui;
  OC_CONFIG_FILE      *Files;
  ED_LIST             List;
  OC_PICKER_KEY_INFO  *Key;
  OC_CONFIG_SOURCE    Source;
  CONST CHAR16        *Failed;
  CHAR8               Name[OC_CONFIG_MAX_NAME + 1];
  CHAR8               Failure[OC_CONFIG_MAX_NAME + 80];
  CHAR8               Subtitle[160];
  UINTN               Count;
  UINTN               Index;
  UINTN               Visible;
  BOOLEAN             Redraw;

  Ui = &State->Ui;

  if ((State->Storage == NULL) || State->Storage->HasVault) {
    EdUiTitle (Ui, "Config files", NULL);
    EdUiNotify (Ui, "The vault is used, OpenCore can only start with the default config", TRUE);
    return;
  }

  Files = AllocateZeroPool (OC_CONFIG_MAX_FILES * sizeof (*Files));
  if (Files == NULL) {
    EdUiNotify (Ui, "Not enough memory", TRUE);
    return;
  }

  Count = OcMiscListConfigs (State->Storage, Files, OC_CONFIG_MAX_FILES);
  if (Count == 0) {
    FreePool (Files);
    EdUiTitle (Ui, "Config files", NULL);
    EdUiNotify (Ui, "No config files were found", TRUE);
    return;
  }

  List.Count    = Count;
  List.Selected = 0;
  List.Top      = 0;
  for (Index = 0; Index < Count; ++Index) {
    if (EdSameConfigName (Files[Index].Name, OcMiscGetConfigName ())) {
      List.Selected = Index;
    }
  }

  Visible = EdUiContentRows (Ui);
  Redraw  = TRUE;

  if (List.Selected >= Visible) {
    List.Top = List.Selected - Visible + 1;
  }

  while (TRUE) {
    if (Redraw) {
      EdUiClear (Ui);
      Redraw = FALSE;
    }

    Source = OcMiscGetConfigSource (&Failed);
    EdConfigNameAscii (Name, sizeof (Name));
    AsciiSPrint (
      Subtitle,
      sizeof (Subtitle),
      "In use: %a, %a",
      Name,
      EdConfigSourceDescription (Source)
      );
    EdUiTitle (Ui, "Config files", Subtitle);
    EdListDraw (Ui, 3, Visible, &List, EdConfigItem, Files);

    if (Source == OcConfigSourceFallback) {
      EdUnicodeToAscii (Failed, Name, sizeof (Name));
      AsciiSPrint (Failure, sizeof (Failure), "The chosen %a could not be used, the default config is in use", Name);
      EdUiStatus (Ui, Ui->AttrError, Failure);
    } else if (Count == 1) {
      EdUiStatus (Ui, Ui->AttrHint, "Other configs are files named config*.plist next to this one");
    } else {
      EdUiStatus (Ui, Ui->AttrHint, "");
    }

    EdUiHint (Ui, "Enter: start OpenCore with it   Esc: back");

    Key = EdUiWaitKey (Ui, FALSE);

    switch (Key->OcKeyCode) {
      case OC_INPUT_UP:
      case OC_INPUT_DOWN:
      case OC_INPUT_TOP:
      case OC_INPUT_BOTTOM:
        EdListMove (&List, Visible, Key->OcKeyCode);
        break;
      case OC_INPUT_CONTINUE:
      case OC_INPUT_RIGHT:
        EdConfigSwitch (State, Files[List.Selected].Name);
        Redraw = TRUE;
        break;
      case OC_INPUT_ABORTED:
      case OC_INPUT_LEFT:
        FreePool (Files);
        return;
      default:
        break;
    }
  }
}

//
// Main menu.
//

STATIC
VOID
EdMainMenu (
  IN OUT ED_STATE  *State
  )
{
  ED_UI             *Ui;
  ED_BOOT_ARGS      Args;
  CHAR8             Subtitle[160];
  CHAR8             Name[128];
  CHAR8             Texts[7][200];
  CONST CHAR8       *Items[7];
  BOOLEAN           Enabled[7];
  UINTN             Count;
  UINTN             Selected;
  INTN              Choice;
  CONST CHAR8       *Reason;
  BOOLEAN           Replaced;

  Ui       = &State->Ui;
  Selected = 0;

  while (TRUE) {
    EdReadBootArgs (&Args);
    Replaced = OcIsBootArgsReplaced ();

    EdUiClear (Ui);

    Subtitle[0] = '\0';
    if (State->Entry != NULL) {
      EdUnicodeToAscii (State->Entry->Name, Name, sizeof (Name));
      AsciiSPrint (Subtitle, sizeof (Subtitle), "Highlighted entry: %a", Name);
    }

    EdUiTitle (Ui, "OpenCore Options", Subtitle);

    Count = 0;

    AsciiSPrint (Texts[Count], sizeof (Texts[Count]), "Boot arguments:  %a", Args.Exists ? Args.Text : "(not set)");
    Items[Count]   = Texts[Count];
    Enabled[Count] = TRUE;
    ++Count;

    Items[Count]   = "Boot flags       (-v, -s, -x, ...)";
    Enabled[Count] = TRUE;
    ++Count;

    EdConfigNameAscii (Name, sizeof (Name));
    if (EdIsConfigEditable (State, &Reason)) {
      AsciiSPrint (Texts[Count], sizeof (Texts[Count]), "Edit %a", Name);
    } else {
      AsciiSPrint (Texts[Count], sizeof (Texts[Count]), "Edit %a  (%a)", Name, Reason);
    }

    Items[Count]   = Texts[Count];
    Enabled[Count] = TRUE;
    ++Count;

    if (EdIsConfigEditable (State, &Reason)) {
      AsciiSPrint (Texts[Count], sizeof (Texts[Count]), "Kexts            (on/off and load order)");
    } else {
      AsciiSPrint (Texts[Count], sizeof (Texts[Count]), "Kexts  (%a)", Reason);
    }

    Items[Count]   = Texts[Count];
    Enabled[Count] = TRUE;
    ++Count;

    AsciiSPrint (
      Texts[Count],
      sizeof (Texts[Count]),
      "Config file:  %a%a",
      Name,
      EdConfigSourceNote (OcMiscGetConfigSource (NULL))
      );
    Items[Count]   = Texts[Count];
    Enabled[Count] = TRUE;
    ++Count;

    Items[Count]   = "Restart";
    Enabled[Count] = TRUE;
    ++Count;

    if (Replaced) {
      Items[Count]   = "Put the original boot arguments back now";
      Enabled[Count] = TRUE;
      ++Count;
    }

    if (State->Status[0] != '\0') {
      EdUiStatus (Ui, State->StatusIsError ? Ui->AttrError : Ui->AttrChanged, State->Status);
      State->Status[0] = '\0';
    }

    EdUiHint (Ui, "Up/Down, Enter: choose   Esc: back to the boot menu");

    Choice = EdUiMenu (Ui, 4, Items, Enabled, Count, &Selected);

    switch (Choice) {
      case 0:
        EdBootArgsScreen (State);
        break;
      case 1:
        EdBootFlagsScreen (State);
        break;
      case 2:
        if (EdIsConfigEditable (State, &Reason)) {
          EdConfigEditor (State);
        } else {
          EdUiNotify (Ui, Reason, TRUE);
        }

        break;
      case 3:
        if (EdIsConfigEditable (State, &Reason)) {
          EdKextsEditor (State);
        } else {
          EdUiNotify (Ui, Reason, TRUE);
        }

        break;
      case 4:
        EdConfigChooser (State);
        break;
      case 5:
        if (EdUiConfirm (Ui, "Restart the computer?")) {
          EdRestart (State);
        }

        break;
      case 6:
        OcRestoreBootArgs ();
        EdSetStatus (State->Status, sizeof (State->Status), &State->StatusIsError, FALSE, "The original boot arguments are back");
        break;
      default:
        return;
    }
  }
}

EFI_STATUS
EFIAPI
OcMiscShowEditor (
  IN OUT OC_PICKER_CONTEXT  *Context,
  IN     OC_BOOT_ENTRY      *BootEntry OPTIONAL
  )
{
  EFI_STATUS  Status;
  ED_STATE    *State;

  //
  // Changing boot arguments and configuration is as sensitive as doing it with
  // hotkeys, so it needs the password if one is set.
  //
  Status = OcRequestPrivilege (Context, OcPrivilegeAuthorized);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "OCE: Privilege request - %r\n", Status));
    return Status;
  }

  State = AllocateZeroPool (sizeof (*State));
  if (State == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  State->Picker  = Context;
  State->Storage = Context->StorageContext;
  State->Entry   = BootEntry;

  Status = EdUiInit (&State->Ui, Context);
  if (EFI_ERROR (Status)) {
    FreePool (State);
    return Status;
  }

  EdMainMenu (State);

  EdUiDone (&State->Ui);
  FreePool (State);

  return EFI_SUCCESS;
}

/** @file
  Choosing the configuration file which OpenCore starts with.

  Most of the configuration is applied before the picker is shown, thus a configuration
  chosen in the picker cannot take effect right away. It is remembered in a small file
  of the OpenCore directory instead, and the next start of OpenCore uses the chosen file.

  Copyright (C) 2026, OpenCore contributors. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-3-Clause
**/

#include <Library/OcMainLib.h>

#include <Guid/FileInfo.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/OcFileLib.h>
#include <Library/OcStringLib.h>
#include <Library/PrintLib.h>

STATIC CHAR16            mConfigName[OC_CONFIG_MAX_NAME + 1] = OPEN_CORE_CONFIG_PATH;
STATIC CHAR16            mFailedName[OC_CONFIG_MAX_NAME + 1];
STATIC OC_CONFIG_SOURCE  mConfigSource = OcConfigSourceDefault;

BOOLEAN
OcMiscIsConfigName (
  IN  CONST CHAR16  *Name
  )
{
  STATIC CONST CHAR16  Prefix[] = L"config";
  STATIC CONST CHAR16  Suffix[] = L".plist";

  UINTN   Length;
  UINTN   Index;
  CHAR16  Character;

  Length = StrLen (Name);
  if ((Length < L_STR_LEN (Prefix) + L_STR_LEN (Suffix)) || (Length > OC_CONFIG_MAX_NAME)) {
    return FALSE;
  }

  //
  // The case does not matter to the file system.
  //
  for (Index = 0; Index < L_STR_LEN (Prefix); ++Index) {
    if (CharToUpper (Name[Index]) != CharToUpper (Prefix[Index])) {
      return FALSE;
    }
  }

  for (Index = 0; Index < L_STR_LEN (Suffix); ++Index) {
    if (CharToUpper (Name[Length - L_STR_LEN (Suffix) + Index]) != CharToUpper (Suffix[Index])) {
      return FALSE;
    }
  }

  //
  // Only characters which are safe on every file system and cannot make up a path.
  //
  for (Index = 0; Index < Length; ++Index) {
    Character = Name[Index];
    if (  !((Character >= L'0') && (Character <= L'9'))
       && !((Character >= L'a') && (Character <= L'z'))
       && !((Character >= L'A') && (Character <= L'Z'))
       && (Character != L'.')
       && (Character != L'_')
       && (Character != L'-')
       && (Character != L'+')
       && (Character != L'(')
       && (Character != L')')
       && (Character != L' '))
    {
      return FALSE;
    }
  }

  //
  // There is no reason for repeated dots.
  //
  for (Index = 1; Index < Length; ++Index) {
    if ((Name[Index - 1] == L'.') && (Name[Index] == L'.')) {
      return FALSE;
    }
  }

  return TRUE;
}

STATIC
BOOLEAN
ConfigIsDefaultName (
  IN  CONST CHAR16  *Name
  )
{
  UINTN  Index;

  for (Index = 0; (Name[Index] != L'\0') || (OPEN_CORE_CONFIG_PATH[Index] != L'\0'); ++Index) {
    if (CharToUpper (Name[Index]) != CharToUpper (OPEN_CORE_CONFIG_PATH[Index])) {
      return FALSE;
    }
  }

  return TRUE;
}

STATIC
INTN
ConfigCompareNames (
  IN  CONST CHAR16  *First,
  IN  CONST CHAR16  *Second
  )
{
  UINTN   Index;
  CHAR16  A;
  CHAR16  B;

  //
  // The default configuration is always the first.
  //
  if (ConfigIsDefaultName (First)) {
    return ConfigIsDefaultName (Second) ? 0 : -1;
  }

  if (ConfigIsDefaultName (Second)) {
    return 1;
  }

  for (Index = 0; ; ++Index) {
    A = CharToUpper (First[Index]);
    B = CharToUpper (Second[Index]);
    if (A != B) {
      return A < B ? -1 : 1;
    }

    if (A == L'\0') {
      return 0;
    }
  }
}

/**
  Get the root of the file system and the full path of a file in the OpenCore directory.
**/
STATIC
EFI_STATUS
ConfigOpenRoot (
  IN  OC_STORAGE_CONTEXT  *Storage,
  IN  CONST CHAR16        *Name,
  OUT EFI_FILE_PROTOCOL   **Root,
  OUT CHAR16              *Path,
  IN  UINTN               PathSize
  )
{
  EFI_STATUS  Status;

  if ((Storage->FileSystem == NULL) || (Storage->StorageRoot == NULL)) {
    return EFI_UNSUPPORTED;
  }

  if (StrLen (Storage->StorageRoot) + StrLen (Name) + 2 > PathSize) {
    return EFI_BAD_BUFFER_SIZE;
  }

  Status = Storage->FileSystem->OpenVolume (Storage->FileSystem, Root);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  UnicodeSPrint (Path, PathSize * sizeof (CHAR16), L"%s\\%s", Storage->StorageRoot, Name);
  return EFI_SUCCESS;
}

STATIC
VOID
ConfigDeleteChoice (
  IN  OC_STORAGE_CONTEXT  *Storage
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *Root;
  CHAR16             Path[OC_STORAGE_SAFE_PATH_MAX + 32];

  Status = ConfigOpenRoot (Storage, OC_CONFIG_CHOICE_PATH, &Root, Path, ARRAY_SIZE (Path));
  if (EFI_ERROR (Status)) {
    return;
  }

  OcDeleteFile (Root, Path);
  Root->Close (Root);
}

/**
  Check if the text starts with the word "keep", whatever the case.
**/
STATIC
BOOLEAN
ConfigIsKeepWord (
  IN  CONST CHAR8  *Text,
  IN  UINT32       Size
  )
{
  STATIC CONST CHAR8  Word[] = "keep";

  UINTN  Index;

  if (Size < L_STR_LEN (Word)) {
    return FALSE;
  }

  for (Index = 0; Index < L_STR_LEN (Word); ++Index) {
    if (AsciiCharToUpper (Text[Index]) != AsciiCharToUpper (Word[Index])) {
      return FALSE;
    }
  }

  return Size == L_STR_LEN (Word) || Text[Index] == ' ' || Text[Index] == '\t' || Text[Index] == '\r' || Text[Index] == '\n' || Text[Index] == '\0';
}

/**
  Read the choice of the configuration, without removing it.

  @param[in]   Storage     OpenCore storage.
  @param[out]  Name        Name of the chosen configuration file.
  @param[out]  Persistent  Whether the choice stays.

  @retval EFI_SUCCESS            There is a choice with a name which is acceptable.
  @retval EFI_NOT_FOUND          There is no choice.
  @retval EFI_INVALID_PARAMETER  There is a choice, but it makes no sense.
**/
STATIC
EFI_STATUS
ConfigReadChoice (
  IN  OC_STORAGE_CONTEXT  *Storage,
  OUT CHAR16              *Name,
  OUT BOOLEAN             *Persistent
  )
{
  CHAR8    *Data;
  UINT32   Size;
  UINTN    Index;
  UINTN    Length;
  BOOLEAN  Valid;

  *Persistent = FALSE;

  Data = OcStorageReadFileUnicode (Storage, OC_CONFIG_CHOICE_PATH, &Size);
  if (Data == NULL) {
    return EFI_NOT_FOUND;
  }

  //
  // The first line is the name.
  //
  Index = 0;
  while ((Index < Size) && ((Data[Index] == ' ') || (Data[Index] == '\t') || (Data[Index] == '\r') || (Data[Index] == '\n'))) {
    ++Index;
  }

  Valid  = TRUE;
  Length = 0;
  while ((Index < Size) && (Data[Index] != '\r') && (Data[Index] != '\n') && (Data[Index] != '\0')) {
    if (Length < OC_CONFIG_MAX_NAME) {
      Name[Length] = (CHAR16)(UINT8)Data[Index];
    } else {
      Valid = FALSE;
    }

    ++Length;
    ++Index;
  }

  if (Length > OC_CONFIG_MAX_NAME) {
    Valid  = FALSE;
    Length = 0;
  }

  while ((Length > 0) && ((Name[Length - 1] == L' ') || (Name[Length - 1] == L'\t'))) {
    --Length;
  }

  Name[Length] = L'\0';
  Valid        = Valid && OcMiscIsConfigName (Name);

  //
  // The rest may tell to keep the choice.
  //
  if (Valid) {
    while (Index < Size) {
      while ((Index < Size) && ((Data[Index] == ' ') || (Data[Index] == '\t') || (Data[Index] == '\r') || (Data[Index] == '\n'))) {
        ++Index;
      }

      if (ConfigIsKeepWord (&Data[Index], Size - Index)) {
        *Persistent = TRUE;
        break;
      }

      while ((Index < Size) && (Data[Index] != '\r') && (Data[Index] != '\n')) {
        ++Index;
      }
    }
  }

  FreePool (Data);
  return Valid ? EFI_SUCCESS : EFI_INVALID_PARAMETER;
}

STATIC
EFI_STATUS
ConfigLoadFile (
  IN  OC_STORAGE_CONTEXT  *Storage,
  IN  CONST CHAR16        *Name,
  OUT OC_GLOBAL_CONFIG    *Config
  )
{
  EFI_STATUS  Status;
  VOID        *Data;
  UINT32      Size;

  Data = OcStorageReadFileUnicode (Storage, Name, &Size);
  if (Data == NULL) {
    DEBUG ((DEBUG_ERROR, "OC: Failed to load configuration %s!\n", Name));
    return EFI_NOT_FOUND;
  }

  DEBUG ((DEBUG_INFO, "OC: Loaded configuration %s of %u bytes\n", Name, Size));

  Status = OcConfigurationInit (Config, Data, Size, NULL);
  FreePool (Data);

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "OC: Failed to parse configuration %s!\n", Name));
  }

  return Status;
}

EFI_STATUS
OcMiscLoadConfig (
  IN  OC_STORAGE_CONTEXT  *Storage,
  OUT OC_GLOBAL_CONFIG    *Config
  )
{
  EFI_STATUS  Status;
  CHAR16      Chosen[OC_CONFIG_MAX_NAME + 1];
  BOOLEAN     Persistent;

  mConfigSource  = OcConfigSourceDefault;
  mFailedName[0] = L'\0';
  StrCpyS (mConfigName, ARRAY_SIZE (mConfigName), OPEN_CORE_CONFIG_PATH);

  //
  // The vault only knows the default configuration, and choosing another one would
  // make OpenCore refuse to start.
  //
  Status = Storage->HasVault ? EFI_NOT_FOUND : ConfigReadChoice (Storage, Chosen, &Persistent);

  if (Status == EFI_INVALID_PARAMETER) {
    //
    // Nothing can be made of it, and it is of no use later.
    //
    DEBUG ((DEBUG_WARN, "OC: Dropping the invalid choice of the configuration\n"));
    ConfigDeleteChoice (Storage);
  } else if (!EFI_ERROR (Status)) {
    if (!Persistent) {
      //
      // A choice for one start is used up before anything can go wrong with the chosen
      // configuration, so that the next start is back to normal whatever happens.
      //
      ConfigDeleteChoice (Storage);
    }

    if (!ConfigIsDefaultName (Chosen)) {
      Status = ConfigLoadFile (Storage, Chosen, Config);
      if (!EFI_ERROR (Status)) {
        StrCpyS (mConfigName, ARRAY_SIZE (mConfigName), Chosen);
        mConfigSource = Persistent ? OcConfigSourceKept : OcConfigSourceOnce;
        return EFI_SUCCESS;
      }

      DEBUG ((DEBUG_WARN, "OC: Cannot use the chosen configuration %s - %r, using the default one\n", Chosen, Status));
      StrCpyS (mFailedName, ARRAY_SIZE (mFailedName), Chosen);
      mConfigSource = OcConfigSourceFallback;
    }
  }

  return ConfigLoadFile (Storage, OPEN_CORE_CONFIG_PATH, Config);
}

CONST CHAR16 *
OcMiscGetConfigName (
  VOID
  )
{
  return mConfigName;
}

OC_CONFIG_SOURCE
OcMiscGetConfigSource (
  OUT CONST CHAR16  **FailedName  OPTIONAL
  )
{
  if (FailedName != NULL) {
    *FailedName = mFailedName;
  }

  return mConfigSource;
}

typedef struct {
  OC_CONFIG_FILE    *Files;
  UINTN             Count;
  UINTN             Max;
} CONFIG_LIST_CONTEXT;

STATIC
EFI_STATUS
ConfigListEntry (
  IN  EFI_FILE_HANDLE  Directory,
  IN  EFI_FILE_INFO    *FileInfo,
  IN  UINTN            FileInfoSize,
  IN  VOID             *Context  OPTIONAL
  )
{
  CONFIG_LIST_CONTEXT  *List;
  UINTN                Index;

  List = Context;

  if (  ((FileInfo->Attribute & EFI_FILE_DIRECTORY) != 0)
     || !OcMiscIsConfigName (FileInfo->FileName)
     || (List->Max == 0))
  {
    return EFI_NOT_FOUND;
  }

  if (List->Count < List->Max) {
    Index = List->Count++;
  } else if (ConfigIsDefaultName (FileInfo->FileName)) {
    //
    // The default configuration is the way back from any other one, so it is listed
    // even when there is no room for everything, in place of the last one found.
    //
    Index = List->Max - 1;
  } else {
    return EFI_NOT_FOUND;
  }

  StrCpyS (List->Files[Index].Name, OC_CONFIG_MAX_NAME + 1, FileInfo->FileName);

  return EFI_SUCCESS;
}

UINTN
OcMiscListConfigs (
  IN  OC_STORAGE_CONTEXT  *Storage,
  OUT OC_CONFIG_FILE      *Files,
  IN  UINTN               MaxFiles
  )
{
  EFI_STATUS           Status;
  EFI_FILE_PROTOCOL    *Root;
  EFI_FILE_PROTOCOL    *Directory;
  CONFIG_LIST_CONTEXT  List;
  UINTN                Index;
  UINTN                Other;
  OC_CONFIG_FILE       Temp;

  List.Files = Files;
  List.Count = 0;
  List.Max   = MaxFiles;

  if ((Storage->FileSystem == NULL) || (Storage->StorageRoot == NULL)) {
    return 0;
  }

  Status = Storage->FileSystem->OpenVolume (Storage->FileSystem, &Root);
  if (EFI_ERROR (Status)) {
    return 0;
  }

  Status = OcSafeFileOpen (Root, &Directory, Storage->StorageRoot, EFI_FILE_MODE_READ, 0);
  Root->Close (Root);
  if (EFI_ERROR (Status)) {
    return 0;
  }

  OcScanDirectory (Directory, ConfigListEntry, &List);
  Directory->Close (Directory);

  //
  // Order by name with the default configuration first, there are only a few of them.
  //
  for (Index = 1; Index < List.Count; ++Index) {
    for (Other = Index; Other > 0; --Other) {
      if (ConfigCompareNames (Files[Other - 1].Name, Files[Other].Name) <= 0) {
        break;
      }

      CopyMem (&Temp, &Files[Other - 1], sizeof (Temp));
      CopyMem (&Files[Other - 1], &Files[Other], sizeof (Temp));
      CopyMem (&Files[Other], &Temp, sizeof (Temp));
    }
  }

  return List.Count;
}

EFI_STATUS
OcMiscSetConfigChoice (
  IN  OC_STORAGE_CONTEXT  *Storage,
  IN  CONST CHAR16        *Name,
  IN  BOOLEAN             Persistent
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *Root;
  CHAR16             Path[OC_STORAGE_SAFE_PATH_MAX + 32];
  CHAR8              Ascii[OC_CONFIG_MAX_NAME + 1];
  CHAR8              Text[OC_CONFIG_MAX_NAME + 16];
  UINTN              Index;

  if (!OcMiscIsConfigName (Name) || Storage->HasVault) {
    return EFI_INVALID_PARAMETER;
  }

  for (Index = 0; Name[Index] != L'\0'; ++Index) {
    Ascii[Index] = (CHAR8)Name[Index];
  }

  Ascii[Index] = '\0';

  Status = ConfigOpenRoot (Storage, OC_CONFIG_CHOICE_PATH, &Root, Path, ARRAY_SIZE (Path));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // The old choice is removed first, which is also all that is needed to go back to the default.
  //
  Status = OcDeleteFile (Root, Path);
  if ((Status != EFI_SUCCESS) && (Status != EFI_NOT_FOUND)) {
    Root->Close (Root);
    return Status;
  }

  Status = EFI_SUCCESS;
  if (!ConfigIsDefaultName (Name)) {
    //
    // Line breaks come from the arguments, as the ones in the format would become CR LF.
    //
    AsciiSPrint (Text, sizeof (Text), "%a%a", Ascii, Persistent ? "\nkeep\n" : "\nonce\n");
    Status = OcSetFileData (Root, Path, Text, (UINT32)AsciiStrLen (Text));
  }

  Root->Close (Root);
  return Status;
}

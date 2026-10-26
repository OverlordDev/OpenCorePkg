/** @file
  Tests of the picker options editor, which run the real editor code on the host
  with a virtual console, scripted keys, a variable store, and a directory of files.

  Copyright (C) 2026, OpenCore contributors. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-3-Clause
**/

#include <Library/OcMainLib.h>

#include <Guid/AppleVariable.h>
#include <Guid/OcVariable.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/OcConsoleLib.h>
#include <Library/OcStringLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <UserBootServices.h>
#include <UserFile.h>
#include <UserGlobalVar.h>

#include <dirent.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../Library/OcMainLib/OpenCoreEditorInternal.h"

STATIC UINT32  mFailures;
STATIC UINT32  mChecks;

#define CHECK(Cond)  do { \
  ++mChecks; \
  if (!(Cond)) { \
    printf ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); \
    ++mFailures; \
  } \
} while (0)

//
// ---------------------------------------------------------------------------------
// Virtual console.
// ---------------------------------------------------------------------------------
//

#define SCREEN_COLUMNS  80
#define SCREEN_ROWS     25

STATIC CHAR16  mScreen[SCREEN_ROWS][SCREEN_COLUMNS + 1];
STATIC UINT8   mScreenAttr[SCREEN_ROWS][SCREEN_COLUMNS];
STATIC UINTN   mCursorColumn;
STATIC UINTN   mCursorRow;
STATIC UINTN   mAttribute;
STATIC UINT32  mLastColumnWrites;
STATIC UINT32  mLastRowWrites;
STATIC UINT32  mClearCount;
STATIC UINT32  mModeSetCount;
STATIC EFI_CONSOLE_CONTROL_SCREEN_MODE  mConsoleMode = EfiConsoleControlScreenGraphics;

STATIC EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  mFakeConOut;
STATIC EFI_SIMPLE_TEXT_OUTPUT_MODE     mFakeConOutMode;

STATIC
VOID
ClearVirtualScreen (
  VOID
  )
{
  UINTN  Row;
  UINTN  Col;

  for (Row = 0; Row < SCREEN_ROWS; ++Row) {
    for (Col = 0; Col < SCREEN_COLUMNS; ++Col) {
      mScreen[Row][Col]     = L' ';
      mScreenAttr[Row][Col] = (UINT8)mAttribute;
    }

    mScreen[Row][SCREEN_COLUMNS] = L'\0';
  }

  mCursorColumn = 0;
  mCursorRow    = 0;
}

STATIC
EFI_STATUS
EFIAPI
FakeReset (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
  IN BOOLEAN                          ExtendedVerification
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
FakeOutputString (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
  IN CHAR16                           *String
  )
{
  UINTN  Row;
  UINTN  Prefix;

  //
  // Debug messages of the libraries come here too, they are not what the editor shows.
  //
  if ((String[0] == L'O') && (String[1] == L'C')) {
    for (Prefix = 2; (Prefix < 8) && (String[Prefix] >= L'A') && (String[Prefix] <= L'Z'); ++Prefix) {
    }

    if ((String[Prefix] == L':') && (String[Prefix + 1] == L' ')) {
      return EFI_SUCCESS;
    }
  }

  for ( ; *String != L'\0'; ++String) {
    if (*String == L'\r') {
      mCursorColumn = 0;
    } else if (*String == L'\n') {
      ++mCursorRow;
    } else {
      if (mCursorRow < SCREEN_ROWS) {
        if (mCursorColumn == SCREEN_COLUMNS - 1) {
          ++mLastColumnWrites;
        }

        if (mCursorRow == SCREEN_ROWS - 1) {
          ++mLastRowWrites;
          if (getenv ("PICKER_TEST_DEBUG") != NULL) {
            printf ("DEBUG: write at last row col %u char %c\n", (unsigned)mCursorColumn, (char)*String);
          }
        }

        mScreen[mCursorRow][mCursorColumn]     = *String;
        mScreenAttr[mCursorRow][mCursorColumn] = (UINT8)mAttribute;
      }

      if (++mCursorColumn >= SCREEN_COLUMNS) {
        mCursorColumn = 0;
        ++mCursorRow;
      }
    }

    if (mCursorRow >= SCREEN_ROWS) {
      //
      // Scrolling, which the editor must never cause.
      //
      printf ("FAIL: console scrolled\n");
      ++mFailures;
      for (Row = 1; Row < SCREEN_ROWS; ++Row) {
        CopyMem (mScreen[Row - 1], mScreen[Row], sizeof (mScreen[0]));
      }

      mCursorRow = SCREEN_ROWS - 1;
    }
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
FakeTestString (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
  IN CHAR16                           *String
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
FakeQueryMode (
  IN  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
  IN  UINTN                            ModeNumber,
  OUT UINTN                            *Columns,
  OUT UINTN                            *Rows
  )
{
  *Columns = SCREEN_COLUMNS;
  *Rows    = SCREEN_ROWS;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
FakeSetMode (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
  IN UINTN                            ModeNumber
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
FakeSetAttribute (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
  IN UINTN                            Attribute
  )
{
  mAttribute              = Attribute;
  mFakeConOutMode.Attribute   = (INT32)Attribute;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
FakeClearScreen (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This
  )
{
  ++mClearCount;
  ClearVirtualScreen ();
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
FakeSetCursorPosition (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
  IN UINTN                            Column,
  IN UINTN                            Row
  )
{
  if ((Column >= SCREEN_COLUMNS) || (Row >= SCREEN_ROWS)) {
    return EFI_UNSUPPORTED;
  }

  mCursorColumn = Column;
  mCursorRow    = Row;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
FakeEnableCursor (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
  IN BOOLEAN                          Visible
  )
{
  mFakeConOutMode.CursorVisible = Visible;
  return EFI_SUCCESS;
}

STATIC
VOID
InitVirtualConsole (
  VOID
  )
{
  mFakeConOut.Reset             = FakeReset;
  mFakeConOut.OutputString      = FakeOutputString;
  mFakeConOut.TestString        = FakeTestString;
  mFakeConOut.QueryMode         = FakeQueryMode;
  mFakeConOut.SetMode           = FakeSetMode;
  mFakeConOut.SetAttribute      = FakeSetAttribute;
  mFakeConOut.ClearScreen       = FakeClearScreen;
  mFakeConOut.SetCursorPosition = FakeSetCursorPosition;
  mFakeConOut.EnableCursor      = FakeEnableCursor;
  mFakeConOut.Mode              = &mFakeConOutMode;

  mFakeConOutMode.MaxMode       = 1;
  mFakeConOutMode.Mode          = 0;
  mFakeConOutMode.Attribute     = EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK);
  mFakeConOutMode.CursorVisible = TRUE;

  gST->ConOut = &mFakeConOut;
  mAttribute  = (UINTN)mFakeConOutMode.Attribute;
  ClearVirtualScreen ();
}

STATIC
VOID
ScreenText (
  OUT CHAR8  *Buffer,
  IN  UINTN  Size
  )
{
  UINTN  Row;
  UINTN  Col;
  UINTN  Length;

  Length = 0;
  for (Row = 0; Row < SCREEN_ROWS; ++Row) {
    for (Col = 0; (Col < SCREEN_COLUMNS) && (Length + 2 < Size); ++Col) {
      Buffer[Length++] = (mScreen[Row][Col] >= 0x20 && mScreen[Row][Col] < 0x7F) ? (CHAR8)mScreen[Row][Col] : '?';
    }

    if (Length + 2 < Size) {
      Buffer[Length++] = '\n';
    }
  }

  Buffer[Length] = '\0';
}

STATIC
VOID
DumpScreen (
  VOID
  )
{
  CHAR8  Buffer[SCREEN_ROWS * (SCREEN_COLUMNS + 1) + 1];

  ScreenText (Buffer, sizeof (Buffer));
  printf ("----- screen -----\n%s------------------\n", Buffer);
}

//
// Screen text at the end of the last wait for a key, as the screen is cleared when the editor leaves.
//
STATIC CHAR8  mSnapshot[8 * 1024 * 1024];
STATIC CHAR8  mLastKeySnapshot[SCREEN_ROWS * (SCREEN_COLUMNS + 1) + 1];

STATIC
BOOLEAN
SeenOnScreen (
  IN CONST CHAR8  *Text
  )
{
  return strstr (mSnapshot, Text) != NULL;
}

//
// ---------------------------------------------------------------------------------
// Scripted keyboard.
// ---------------------------------------------------------------------------------
//

typedef enum {
  KeyUp = 1,
  KeyDown,
  KeyLeft,
  KeyRight,
  KeyHome,
  KeyEnd,
  KeyEnter,
  KeyEsc,
  KeySpace,
  KeyBackspace,
  KeyDelete,
  KeyF5,
  KeyF10,
  KeyPlus,
  KeyMinus,
  KeyChar
} SCRIPT_KEY;

typedef struct {
  SCRIPT_KEY    Key;
  CHAR8         Char;
} SCRIPT_ENTRY;

//
// Places of the items of the main menu of the editor, to be reached with Down from the first.
//
#define MENU_ARGS      0
#define MENU_FLAGS     1
#define MENU_CONFIG    2
#define MENU_KEXTS     3
#define MENU_LISTS     4
#define MENU_QUIRKS    5
#define MENU_CHECK     6
#define MENU_LOG       7
#define MENU_PANIC     8
#define MENU_COMPARE   9
#define MENU_UNDO      10
#define MENU_CHOOSER   11
#define MENU_RESTART   12
#define MENU_PUT_BACK  13

STATIC SCRIPT_ENTRY  mScript[4096];
STATIC UINTN         mScriptLength;
STATIC UINTN         mScriptPosition;
STATIC jmp_buf       mScriptEnd;
STATIC BOOLEAN       mShowScreens;
STATIC BOOLEAN       mScriptExhausted;
STATIC UINT32         mTypingWaits;
STATIC UINT32         mNavigationWaits;

STATIC
VOID
ScriptReset (
  VOID
  )
{
  mScriptLength    = 0;
  mScriptPosition  = 0;
  mScriptExhausted = FALSE;
  mTypingWaits     = 0;
  mNavigationWaits = 0;
}

STATIC
VOID
Press (
  IN SCRIPT_KEY  Key,
  IN UINTN       Times
  )
{
  while (Times-- > 0) {
    mScript[mScriptLength].Key  = Key;
    mScript[mScriptLength].Char = 0;
    ++mScriptLength;
  }
}

STATIC
VOID
Type (
  IN CONST CHAR8  *Text
  )
{
  while (*Text != '\0') {
    mScript[mScriptLength].Key  = KeyChar;
    mScript[mScriptLength].Char = *Text++;
    ++mScriptLength;
  }
}

STATIC
BOOLEAN
EFIAPI
FakeWaitForKeyInfo (
  IN OUT OC_PICKER_CONTEXT   *Context,
  IN     UINT64              EndTime,
  IN     OC_PICKER_KEY_MAP   KeyFilter,
  IN OUT OC_PICKER_KEY_INFO  *PickerKeyInfo
  )
{
  SCRIPT_ENTRY  Entry;
  BOOLEAN       Typing;

  //
  // Remember what is on the screen when the editor waits for a key.
  //
  ScreenText (mLastKeySnapshot, sizeof (mLastKeySnapshot));
  if (strlen (mSnapshot) + strlen (mLastKeySnapshot) + 1 < sizeof (mSnapshot)) {
    strcat (mSnapshot, mLastKeySnapshot);
  }

  if (mShowScreens) {
    printf ("=== waiting for a %s key (key %u of %u) ===\n%s", (KeyFilter & OC_PICKER_KEYS_TYPING) != 0 ? "typing" : "navigation", (unsigned)mScriptPosition, (unsigned)mScriptLength, mLastKeySnapshot);
  }

  if (mScriptPosition >= mScriptLength) {
    mScriptExhausted = TRUE;
    longjmp (mScriptEnd, 1);
  }

  Entry  = mScript[mScriptPosition++];
  Typing = (KeyFilter & OC_PICKER_KEYS_TYPING) != 0;

  if (Typing) {
    ++mTypingWaits;
  } else {
    ++mNavigationWaits;
  }

  //
  // These follow what the picker key handling reports.
  //
  PickerKeyInfo->OcKeyCode   = OC_INPUT_NO_ACTION;
  PickerKeyInfo->UnicodeChar = CHAR_NULL;

  switch (Entry.Key) {
    case KeyUp:
      if (!Typing) {
        PickerKeyInfo->OcKeyCode = OC_INPUT_UP;
      }

      break;
    case KeyDown:
      if (!Typing) {
        PickerKeyInfo->OcKeyCode = OC_INPUT_DOWN;
      }

      break;
    case KeyLeft:
      PickerKeyInfo->OcKeyCode = Typing ? OC_INPUT_TYPING_LEFT : OC_INPUT_LEFT;
      break;
    case KeyRight:
      PickerKeyInfo->OcKeyCode = Typing ? OC_INPUT_TYPING_RIGHT : OC_INPUT_RIGHT;
      break;
    case KeyHome:
      PickerKeyInfo->OcKeyCode = Typing ? OC_INPUT_TYPING_HOME : OC_INPUT_TOP;
      break;
    case KeyEnd:
      PickerKeyInfo->OcKeyCode = Typing ? OC_INPUT_TYPING_END : OC_INPUT_BOTTOM;
      break;
    case KeyEnter:
      PickerKeyInfo->OcKeyCode = Typing ? OC_INPUT_TYPING_CONFIRM : OC_INPUT_CONTINUE;
      if (Typing) {
        PickerKeyInfo->UnicodeChar = L'\r';
      }

      break;
    case KeyEsc:
      PickerKeyInfo->OcKeyCode = Typing ? OC_INPUT_TYPING_CLEAR_ALL : OC_INPUT_ABORTED;
      break;
    case KeySpace:
      if (Typing) {
        PickerKeyInfo->UnicodeChar = L' ';
      } else {
        PickerKeyInfo->OcKeyCode = OC_INPUT_MORE;
      }

      break;
    case KeyBackspace:
      if (Typing) {
        PickerKeyInfo->OcKeyCode   = OC_INPUT_TYPING_BACKSPACE;
        PickerKeyInfo->UnicodeChar = L'\b';
      }

      break;
    case KeyDelete:
      if (Typing) {
        PickerKeyInfo->OcKeyCode = OC_INPUT_TYPING_DELETE;
      }

      break;
    case KeyF5:
      if (!Typing) {
        PickerKeyInfo->OcKeyCode = OC_INPUT_FUNCTIONAL (5);
      }

      break;
    case KeyF10:
      if (!Typing) {
        PickerKeyInfo->OcKeyCode = OC_INPUT_FUNCTIONAL (10);
      }

      break;
    case KeyPlus:
      //
      // Plus and minus are reported only to those who ask for them, and are typed otherwise.
      //
      if (Typing) {
        PickerKeyInfo->UnicodeChar = L'+';
      } else if ((KeyFilter & OC_PICKER_KEYS_PLUS_MINUS) != 0) {
        PickerKeyInfo->OcKeyCode = OC_INPUT_PLUS;
      }

      break;
    case KeyMinus:
      if (Typing) {
        PickerKeyInfo->UnicodeChar = L'-';
      } else if ((KeyFilter & OC_PICKER_KEYS_PLUS_MINUS) != 0) {
        PickerKeyInfo->OcKeyCode = OC_INPUT_MINUS;
      }

      break;
    case KeyChar:
      if (Typing) {
        PickerKeyInfo->UnicodeChar = (CHAR16)Entry.Char;
      } else if ((Entry.Char >= '1') && (Entry.Char <= '9')) {
        PickerKeyInfo->OcKeyCode = Entry.Char - '1';
      } else if ((Entry.Char >= 'a') && (Entry.Char <= 'z')) {
        PickerKeyInfo->OcKeyCode = Entry.Char - 'a' + 9;
      }

      break;
  }

  return FALSE;
}

STATIC
VOID
EFIAPI
FakeFlushTypingBuffer (
  IN OUT OC_PICKER_CONTEXT  *Context
  )
{
}

//
// ---------------------------------------------------------------------------------
// Variables.
// ---------------------------------------------------------------------------------
//

#define MAX_VARIABLES  16

typedef struct {
  BOOLEAN    Used;
  CHAR16     Name[48];
  EFI_GUID   Guid;
  UINT32     Attributes;
  UINTN      Size;
  UINT8      Data[1200];
} FAKE_VARIABLE;

STATIC FAKE_VARIABLE  mVariables[MAX_VARIABLES];
STATIC BOOLEAN        mNvWriteProtected;
STATIC UINT32         mSetVariableCalls;

STATIC
FAKE_VARIABLE *
FindVariable (
  IN CONST CHAR16  *Name,
  IN CONST EFI_GUID *Guid
  )
{
  UINTN  Index;

  for (Index = 0; Index < MAX_VARIABLES; ++Index) {
    if (mVariables[Index].Used && (StrCmp (mVariables[Index].Name, Name) == 0) && CompareGuid (&mVariables[Index].Guid, Guid)) {
      return &mVariables[Index];
    }
  }

  return NULL;
}

STATIC
EFI_STATUS
EFIAPI
FakeGetVariable (
  IN     CHAR16    *VariableName,
  IN     EFI_GUID  *VendorGuid,
  OUT    UINT32    *Attributes OPTIONAL,
  IN OUT UINTN     *DataSize,
  OUT    VOID      *Data OPTIONAL
  )
{
  FAKE_VARIABLE  *Variable;

  Variable = FindVariable (VariableName, VendorGuid);
  if (Variable == NULL) {
    return EFI_NOT_FOUND;
  }

  if (Attributes != NULL) {
    *Attributes = Variable->Attributes;
  }

  if ((*DataSize < Variable->Size) || (Data == NULL)) {
    *DataSize = Variable->Size;
    return EFI_BUFFER_TOO_SMALL;
  }

  CopyMem (Data, Variable->Data, Variable->Size);
  *DataSize = Variable->Size;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
FakeSetVariable (
  IN CHAR16    *VariableName,
  IN EFI_GUID  *VendorGuid,
  IN UINT32    Attributes,
  IN UINTN     DataSize,
  IN VOID      *Data
  )
{
  FAKE_VARIABLE  *Variable;
  UINTN          Index;

  ++mSetVariableCalls;

  if (mNvWriteProtected && ((Attributes & EFI_VARIABLE_NON_VOLATILE) != 0)) {
    return EFI_SECURITY_VIOLATION;
  }

  Variable = FindVariable (VariableName, VendorGuid);

  //
  // As in the specification: no data, or no access, removes the variable.
  //
  if ((DataSize == 0) || ((Attributes & (EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS)) == 0)) {
    if (Variable == NULL) {
      return EFI_NOT_FOUND;
    }

    Variable->Used = FALSE;
    return EFI_SUCCESS;
  }

  if (Variable != NULL) {
    if (Variable->Attributes != Attributes) {
      return EFI_INVALID_PARAMETER;
    }
  } else {
    for (Index = 0; Index < MAX_VARIABLES; ++Index) {
      if (!mVariables[Index].Used) {
        break;
      }
    }

    if (Index == MAX_VARIABLES) {
      return EFI_OUT_OF_RESOURCES;
    }

    Variable = &mVariables[Index];
    ZeroMem (Variable, sizeof (*Variable));
    Variable->Used = TRUE;
    StrCpyS (Variable->Name, ARRAY_SIZE (Variable->Name), VariableName);
    CopyGuid (&Variable->Guid, VendorGuid);
  }

  if (DataSize > sizeof (Variable->Data)) {
    return EFI_OUT_OF_RESOURCES;
  }

  Variable->Attributes = Attributes;
  Variable->Size       = DataSize;
  CopyMem (Variable->Data, Data, DataSize);
  return EFI_SUCCESS;
}

STATIC
VOID
PutVariable (
  IN CONST CHAR16  *Name,
  IN EFI_GUID      *Guid,
  IN UINT32        Attributes,
  IN CONST VOID    *Data,
  IN UINTN         Size
  )
{
  EFI_STATUS  Status;

  Status = FakeSetVariable ((CHAR16 *)Name, Guid, Attributes, Size, (VOID *)Data);
  CHECK (!EFI_ERROR (Status));
}

STATIC
VOID
ResetPlatform (
  VOID
  )
{
  UINTN  Index;

  //
  // Variables which are not in flash are gone.
  //
  for (Index = 0; Index < MAX_VARIABLES; ++Index) {
    if (mVariables[Index].Used && ((mVariables[Index].Attributes & EFI_VARIABLE_NON_VOLATILE) == 0)) {
      mVariables[Index].Used = FALSE;
    }
  }
}

STATIC CONST UINT32  mNv       = EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS | EFI_VARIABLE_NON_VOLATILE;
STATIC CONST UINT32  mVolatile = EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS;

STATIC
BOOLEAN
BootArgsAre (
  IN CONST CHAR8  *Expected,
  IN UINTN        ExpectedSize,
  IN UINT32       ExpectedAttributes
  )
{
  FAKE_VARIABLE  *Variable;

  Variable = FindVariable (L"boot-args", &gAppleBootVariableGuid);
  return Variable != NULL
         && Variable->Size == ExpectedSize
         && CompareMem (Variable->Data, Expected, ExpectedSize) == 0
         && Variable->Attributes == ExpectedAttributes;
}

STATIC
BOOLEAN
NoBootArgs (
  VOID
  )
{
  return FindVariable (L"boot-args", &gAppleBootVariableGuid) == NULL;
}

STATIC
BOOLEAN
HasBackup (
  VOID
  )
{
  return FindVariable (OC_BOOT_ARGS_BACKUP_VARIABLE_NAME, &gOcVendorVariableGuid) != NULL;
}

//
// ---------------------------------------------------------------------------------
// Files: the storage directory of the test is a directory of the host.
// ---------------------------------------------------------------------------------
//

STATIC CHAR8  mDirectory[256];
STATIC BOOLEAN mFileWriteProtected;

STATIC
VOID
HostPath (
  IN  CONST CHAR16  *Name,
  OUT CHAR8         *Path,
  IN  UINTN         Size
  )
{
  STATIC CONST CHAR16  Root[] = L"\\EFI\\OC\\";

  UINTN  Length;
  UINTN  Index;

  //
  // The directory of OpenCore is the directory of the test, whether the name is given
  // from the root of the volume or relative to it.
  //
  for (Index = 0; Root[Index] != L'\0'; ++Index) {
    if (Name[Index] != Root[Index]) {
      break;
    }
  }

  if (Root[Index] == L'\0') {
    Name += Index;
  } else if (Name[0] == L'\\') {
    ++Name;
  }

  Length = AsciiSPrint (Path, Size, "%a/", mDirectory);
  while (*Name != L'\0' && Length + 1 < Size) {
    Path[Length++] = *Name == L'\\' ? '/' : (CHAR8)*Name;
    ++Name;
  }

  Path[Length] = '\0';
}

EFI_STATUS
TestDeleteFile (
  IN EFI_FILE_PROTOCOL  *Directory,
  IN CONST CHAR16       *FileName
  )
{
  CHAR8  Path[512];

  HostPath (FileName, Path, sizeof (Path));
  if (mFileWriteProtected) {
    return EFI_WRITE_PROTECTED;
  }

  return unlink (Path) == 0 ? EFI_SUCCESS : EFI_NOT_FOUND;
}

EFI_STATUS
TestSetFileData (
  IN EFI_FILE_PROTOCOL  *WritableFs OPTIONAL,
  IN CONST CHAR16       *FileName,
  IN CONST VOID         *Buffer,
  IN UINT32             Size
  )
{
  CHAR8  Path[512];
  FILE   *File;

  HostPath (FileName, Path, sizeof (Path));
  if (mFileWriteProtected) {
    return EFI_WRITE_PROTECTED;
  }

  File = fopen (Path, "wb");
  if (File == NULL) {
    return EFI_DEVICE_ERROR;
  }

  fwrite (Buffer, 1, Size, File);
  fclose (File);
  return EFI_SUCCESS;
}

STATIC
VOID *
ReadHostFile (
  IN  CONST CHAR8  *Path,
  OUT UINT32       *Size
  )
{
  FILE  *File;
  long  Length;
  UINT8 *Buffer;

  File = fopen (Path, "rb");
  if (File == NULL) {
    return NULL;
  }

  fseek (File, 0, SEEK_END);
  Length = ftell (File);
  fseek (File, 0, SEEK_SET);

  Buffer = AllocatePool ((UINTN)Length + 2);
  if (fread (Buffer, 1, (size_t)Length, File) != (size_t)Length) {
    FreePool (Buffer);
    fclose (File);
    return NULL;
  }

  fclose (File);
  Buffer[Length]     = 0;
  Buffer[Length + 1] = 0;
  *Size              = (UINT32)Length;
  return Buffer;
}

VOID *
TestReadFileFromDirectory (
  IN CONST EFI_FILE_PROTOCOL  *RootDirectory,
  IN CONST CHAR16             *FilePath,
  OUT UINT32                  *FileSize OPTIONAL,
  IN UINT32                   MaxFileSize OPTIONAL
  )
{
  CHAR8   Path[512];
  UINT32  Size;
  VOID    *Data;

  HostPath (FilePath, Path, sizeof (Path));
  Data = ReadHostFile (Path, &Size);
  if ((Data != NULL) && (FileSize != NULL)) {
    *FileSize = Size;
  }

  return Data;
}

VOID *
TestStorageReadFileUnicode (
  IN  OC_STORAGE_CONTEXT  *Context,
  IN  CONST CHAR16        *FilePath,
  OUT UINT32              *FileSize OPTIONAL
  )
{
  CHAR8   Path[512];
  UINT32  Size;
  VOID    *Data;

  HostPath (FilePath, Path, sizeof (Path));
  Data = ReadHostFile (Path, &Size);
  if ((Data != NULL) && (FileSize != NULL)) {
    *FileSize = Size;
  }

  return Data;
}

STATIC CONST CHAR8  *mFakeLog;
STATIC CONST CHAR8  *mFakePanic;

VOID *
TestReadPanic (
  OUT UINT32  *Size
  )
{
  VOID  *Copy;

  if (mFakePanic == NULL) {
    return NULL;
  }

  *Size = (UINT32)strlen (mFakePanic);
  Copy  = AllocatePool (*Size + 1);
  CopyMem (Copy, mFakePanic, *Size + 1);
  return Copy;
}

CONST CHAR8 *
TestGetLogBuffer (
  VOID
  )
{
  return mFakeLog;
}

BOOLEAN
TestStorageExistsFileUnicode (
  IN  OC_STORAGE_CONTEXT  *Context,
  IN  CONST CHAR16        *FilePath
  )
{
  CHAR8   Path[512];
  UINT32  Size;
  VOID    *Data;

  HostPath (FilePath, Path, sizeof (Path));
  Data = ReadHostFile (Path, &Size);
  if (Data != NULL) {
    FreePool (Data);
  }

  return Data != NULL;
}

STATIC
VOID *
ReadTestFile (
  IN  CONST CHAR8  *Name,
  OUT UINT32       *Size
  )
{
  CHAR8  Path[512];

  AsciiSPrint (Path, sizeof (Path), "%a/%a", mDirectory, Name);
  return ReadHostFile (Path, Size);
}

STATIC
VOID
WriteTestFile (
  IN CONST CHAR8  *Name,
  IN CONST VOID   *Data,
  IN UINT32       Size
  )
{
  CHAR8  Path[512];
  FILE   *File;
  CHAR8  *Slash;

  AsciiSPrint (Path, sizeof (Path), "%a/%a", mDirectory, Name);

  //
  // The directories of a name like Kexts/Lilu.kext/Contents/Info.plist.
  //
  for (Slash = Path + AsciiStrLen (mDirectory) + 1; *Slash != '\0'; ++Slash) {
    if (*Slash == '/') {
      *Slash = '\0';
      mkdir (Path, 0755);
      *Slash = '/';
    }
  }

  File = fopen (Path, "wb");
  fwrite (Data, 1, Size, File);
  fclose (File);
}

//
// ---------------------------------------------------------------------------------
// Stand-ins for the rest of OpenCore which the editor files refer to.
// ---------------------------------------------------------------------------------
//

STATIC jmp_buf  mResetJump;
STATIC UINT32   mResets;

VOID
EFIAPI
ResetWarm (
  VOID
  )
{
  ++mResets;
  longjmp (mResetJump, 1);
}

EFI_CONSOLE_CONTROL_SCREEN_MODE
OcConsoleControlSetMode (
  IN EFI_CONSOLE_CONTROL_SCREEN_MODE  Mode
  )
{
  EFI_CONSOLE_CONTROL_SCREEN_MODE  Old;

  Old          = mConsoleMode;
  mConsoleMode = Mode;
  ++mModeSetCount;
  return Old;
}

STATIC EFI_STATUS  mPrivilegeResult;
STATIC UINT32      mPrivilegeRequests;

EFI_STATUS
OcRequestPrivilege (
  IN OUT OC_PICKER_CONTEXT   *Context,
  IN     OC_PRIVILEGE_LEVEL  Level
  )
{
  ++mPrivilegeRequests;
  return mPrivilegeResult;
}

CONST CHAR8 *
OcMiscGetVersionString (
  VOID
  )
{
  return "TST-000-0000-00-00";
}

VOID
OcLoadLegacyNvram (
  IN OC_STORAGE_CONTEXT  *Storage,
  IN OC_NVRAM_LEGACY_MAP *Schema,
  IN BOOLEAN             Overwrite,
  IN BOOLEAN             BootVariableRouting
  )
{
}

VOID
OcSetNvramVariable (
  IN CONST CHAR8            *AsciiVariableName,
  IN EFI_GUID               *VariableGuid,
  IN UINT32                 Attributes,
  IN UINT32                 VariableSize,
  IN VOID                   *VariableData,
  IN OC_NVRAM_LEGACY_ENTRY  *SchemaEntry OPTIONAL,
  IN BOOLEAN                Overwrite
  )
{
}

EFI_STATUS
OcProcessVariableGuid (
  IN  CONST CHAR8  *AsciiVariableGuid,
  OUT GUID         *VariableGuid,
  OUT CHAR16       **VariableName,
  OUT UINTN        *VariableNameSize
  )
{
  return EFI_UNSUPPORTED;
}

//
// ---------------------------------------------------------------------------------
// Editor environment.
// ---------------------------------------------------------------------------------
//

STATIC UINT8                *mSample;
STATIC UINT32               mSampleSize;
STATIC OC_PICKER_CONTEXT    *mPicker;
STATIC OC_STORAGE_CONTEXT   mStorage;
STATIC OC_HOTKEY_CONTEXT    mHotKeys;
STATIC EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  mFileSystem;
STATIC EFI_FILE_PROTOCOL    mRootFile;
STATIC OC_BOOT_ENTRY        mEntry;
STATIC CHAR16               mEntryName[] = L"macOS Test";

STATIC
EFI_STATUS
EFIAPI
FakeRootClose (
  IN EFI_FILE_PROTOCOL  *This
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
FakeOpenVolume (
  IN  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *This,
  OUT EFI_FILE_PROTOCOL                **Root
  )
{
  *Root = &mRootFile;
  return EFI_SUCCESS;
}

STATIC
VOID
ClearVariables (
  VOID
  )
{
  ZeroMem (mVariables, sizeof (mVariables));
  mNvWriteProtected = FALSE;
}

STATIC
VOID
InitEnvironment (
  IN CONST CHAR8  *SamplePath
  )
{
  UINT32  Size;
  VOID    *Sample;
  CHAR8   Name[64];

  Sample = UserReadFile (SamplePath, &Size);
  if (Sample == NULL) {
    printf ("FAIL: cannot read %s\n", SamplePath);
    exit (2);
  }

  if (getenv ("PICKER_TEST_DIR") != NULL) {
    //
    // A copy of the OpenCore directory of someone to look at.
    //
    snprintf (mDirectory, sizeof (mDirectory), "%s", getenv ("PICKER_TEST_DIR"));
  } else {
    snprintf (mDirectory, sizeof (mDirectory), "/tmp/oc-picker-editor-test.%d", (int)getpid ());
    mkdir (mDirectory, 0755);

    AsciiSPrint (Name, sizeof (Name), "config.plist");
    WriteTestFile (Name, Sample, Size);
  }

  mSample     = Sample;
  mSampleSize = Size;

  gRT->GetVariable = FakeGetVariable;
  gRT->SetVariable = FakeSetVariable;

  InitVirtualConsole ();

  mFileSystem.OpenVolume = FakeOpenVolume;
  mRootFile.Close        = FakeRootClose;

  ZeroMem (&mStorage, sizeof (mStorage));
  mStorage.FileSystem  = &mFileSystem;
  mStorage.StorageRoot = L"\\EFI\\OC";

  mHotKeys.WaitForKeyInfo    = FakeWaitForKeyInfo;
  mHotKeys.FlushTypingBuffer = FakeFlushTypingBuffer;

  mPicker                 = AllocateZeroPool (sizeof (*mPicker));
  mPicker->HotKeyContext  = &mHotKeys;
  mPicker->StorageContext = &mStorage;
  mPicker->ShowEditor     = OcMiscShowEditor;

  mEntry.Name = mEntryName;
  mEntry.Type = OC_BOOT_APPLE_OS;
}

EFI_STATUS
TestSafeFileOpen (
  IN     CONST EFI_FILE_PROTOCOL  *Directory,
  OUT       EFI_FILE_PROTOCOL     **NewHandle,
  IN     CONST CHAR16             *FileName,
  IN     CONST UINT64             OpenMode,
  IN     CONST UINT64             Attributes
  )
{
  //
  // Any handle will do, the entries come from the directory of the test.
  //
  *NewHandle = &mRootFile;
  return EFI_SUCCESS;
}

EFI_STATUS
TestScanDirectory (
  IN      EFI_FILE_HANDLE             Directory,
  IN      OC_PROCESS_DIRECTORY_ENTRY  ProcessEntry,
  IN OUT  VOID                        *Context            OPTIONAL
  )
{
  struct dirent  **List;
  struct stat    Stat;
  CHAR8          Path[512];
  EFI_FILE_INFO  *Info;
  EFI_STATUS     Status;
  EFI_STATUS     Result;
  UINTN          Index;
  int            Total;
  int            Position;

  //
  // The order of the entries is the same on every host, the tests rely on it.
  //
  Total = scandir (mDirectory, &List, NULL, alphasort);
  if (Total < 0) {
    return EFI_DEVICE_ERROR;
  }

  Info   = AllocateZeroPool (SIZE_1KB);
  Status = EFI_NOT_FOUND;

  for (Position = 0; Position < Total; ++Position) {
    AsciiSPrint (Path, sizeof (Path), "%a/%a", mDirectory, List[Position]->d_name);
    if (stat (Path, &Stat) != 0) {
      continue;
    }

    ZeroMem (Info, SIZE_1KB);
    for (Index = 0; (List[Position]->d_name[Index] != '\0') && (Index < 200); ++Index) {
      Info->FileName[Index] = (CHAR16)(UINT8)List[Position]->d_name[Index];
    }

    Info->FileName[Index] = L'\0';
    Info->Size            = SIZE_OF_EFI_FILE_INFO + (Index + 1) * sizeof (CHAR16);
    Info->Attribute       = S_ISDIR (Stat.st_mode) ? EFI_FILE_DIRECTORY : 0;

    Result = ProcessEntry (Directory, Info, Info->Size, Context);
    if (Result == EFI_NOT_FOUND) {
      continue;
    }

    if (EFI_ERROR (Result)) {
      Status = Result;
      break;
    }

    Status = EFI_SUCCESS;
  }

  for (Position = 0; Position < Total; ++Position) {
    free (List[Position]);
  }

  free (List);
  FreePool (Info);
  return Status;
}

/**
  Run the editor with the keys of the script.

  @return TRUE if the editor ended by itself, and not for lack of keys.
**/
STATIC
BOOLEAN
RunEditor (
  OUT BOOLEAN  *Restarted  OPTIONAL
  )
{
  EFI_STATUS  Status;

  mSnapshot[0]        = '\0';
  mScriptPosition     = 0;
  mScriptExhausted    = FALSE;
  mResets             = 0;
  mLastColumnWrites   = 0;
  mLastRowWrites      = 0;
  mConsoleMode        = EfiConsoleControlScreenGraphics;
  mFakeConOutMode.CursorVisible = FALSE;
  mFakeConOutMode.Attribute     = EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK);
  mAttribute                = (UINTN)mFakeConOutMode.Attribute;

  if (Restarted != NULL) {
    *Restarted = FALSE;
  }

  if (setjmp (mResetJump) != 0) {
    if (Restarted != NULL) {
      *Restarted = TRUE;
    }

    return TRUE;
  }

  if (setjmp (mScriptEnd) != 0) {
    return FALSE;
  }

  Status = mPicker->ShowEditor (mPicker, &mEntry);
  CHECK (!EFI_ERROR (Status));

  //
  // Everything is given back.
  //
  CHECK (mConsoleMode == EfiConsoleControlScreenGraphics);
  CHECK (mFakeConOutMode.Attribute == EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK));
  CHECK (!mFakeConOutMode.CursorVisible);

  return TRUE;
}

#define RUN_EDITOR(Restarted)  do { \
  BOOLEAN  _Ended = RunEditor (Restarted); \
  if (!_Ended) { \
    printf ("FAIL %s:%d: the keys ran out, the editor did not end\n", __FILE__, __LINE__); \
    DumpScreen (); \
    ++mFailures; \
  } \
  CHECK (mLastColumnWrites == 0 && mLastRowWrites == 0); \
} while (0)

//
// ---------------------------------------------------------------------------------
// Variable tests.
// ---------------------------------------------------------------------------------
//

STATIC
VOID
TestBootArgsVariables (
  VOID
  )
{
  STATIC CONST CHAR8  Original[]  = "keepsyms=1 debug=0x100";
  STATIC CONST CHAR8  Terminated[] = "-v keepsyms=1";

  ClearVariables ();

  //
  // Nothing there: a replacement for one boot needs no backup, it is gone with the next reset.
  //
  CHECK (!EFI_ERROR (OcSetBootArgs ("-v", FALSE)));
  CHECK (BootArgsAre ("-v", 2, mVolatile));
  CHECK (!HasBackup ());
  CHECK (!OcIsBootArgsReplaced ());
  ResetPlatform ();
  OcRestoreBootArgs ();
  CHECK (NoBootArgs ());

  //
  // Kept in flash: the replacement for one boot is removed by a reset, the original comes back.
  //
  PutVariable (L"boot-args", &gAppleBootVariableGuid, mNv, Original, sizeof (Original) - 1);
  CHECK (!EFI_ERROR (OcSetBootArgs ("-x -v", FALSE)));
  CHECK (BootArgsAre ("-x -v", 5, mVolatile));
  CHECK (HasBackup ());
  CHECK (OcIsBootArgsReplaced ());
  ResetPlatform ();
  CHECK (NoBootArgs ());
  OcRestoreBootArgs ();
  CHECK (BootArgsAre (Original, sizeof (Original) - 1, mNv));
  CHECK (!HasBackup ());
  CHECK (!OcIsBootArgsReplaced ());

  //
  // The replacement is removed on request, without a reset.
  //
  CHECK (!EFI_ERROR (OcSetBootArgs ("-s", FALSE)));
  CHECK (BootArgsAre ("-s", 2, mVolatile));
  CHECK (HasBackup ());
  OcRestoreBootArgs ();
  CHECK (BootArgsAre (Original, sizeof (Original) - 1, mNv));
  CHECK (!HasBackup ());

  //
  // A second replacement in the same session keeps the original, not the first replacement.
  //
  CHECK (!EFI_ERROR (OcSetBootArgs ("-a", FALSE)));
  CHECK (!EFI_ERROR (OcSetBootArgs ("-b", FALSE)));
  CHECK (BootArgsAre ("-b", 2, mVolatile));
  ResetPlatform ();
  OcRestoreBootArgs ();
  CHECK (BootArgsAre (Original, sizeof (Original) - 1, mNv));
  CHECK (!HasBackup ());

  //
  // Removing the arguments for one boot.
  //
  CHECK (!EFI_ERROR (OcSetBootArgs ("", FALSE)));
  CHECK (NoBootArgs ());
  CHECK (HasBackup ());
  ResetPlatform ();
  OcRestoreBootArgs ();
  CHECK (BootArgsAre (Original, sizeof (Original) - 1, mNv));

  //
  // Changed meanwhile, e.g. by macOS: the change stays.
  //
  CHECK (!EFI_ERROR (OcSetBootArgs ("-x", FALSE)));
  ResetPlatform ();
  PutVariable (L"boot-args", &gAppleBootVariableGuid, mNv, "other", 5);
  OcRestoreBootArgs ();
  CHECK (BootArgsAre ("other", 5, mNv));
  CHECK (!HasBackup ());

  //
  // The original, as OpenCore writes it, with the terminator, is put back as it was.
  //
  ClearVariables ();
  PutVariable (L"boot-args", &gAppleBootVariableGuid, mNv, Terminated, sizeof (Terminated));
  CHECK (!EFI_ERROR (OcSetBootArgs ("-x", FALSE)));
  ResetPlatform ();
  OcRestoreBootArgs ();
  CHECK (BootArgsAre (Terminated, sizeof (Terminated), mNv));

  //
  // Read-only flash: nothing is touched when the original cannot be remembered.
  //
  mNvWriteProtected = TRUE;
  CHECK (EFI_ERROR (OcSetBootArgs ("-x", FALSE)));
  CHECK (BootArgsAre (Terminated, sizeof (Terminated), mNv));
  CHECK (!HasBackup ());
  CHECK (EFI_ERROR (OcSetBootArgs ("-x", TRUE)));
  CHECK (BootArgsAre (Terminated, sizeof (Terminated), mNv));
  mNvWriteProtected = FALSE;

  //
  // Kept for good.
  //
  CHECK (!EFI_ERROR (OcSetBootArgs ("one two", TRUE)));
  CHECK (BootArgsAre ("one two", 7, mNv));
  CHECK (!HasBackup ());
  ResetPlatform ();
  OcRestoreBootArgs ();
  CHECK (BootArgsAre ("one two", 7, mNv));

  //
  // Kept for good over a variable which is not in flash.
  //
  ClearVariables ();
  PutVariable (L"boot-args", &gAppleBootVariableGuid, mVolatile, "volatile", 8);
  CHECK (!EFI_ERROR (OcSetBootArgs ("persistent", TRUE)));
  CHECK (BootArgsAre ("persistent", 10, mNv));

  //
  // Replaced for a boot over a variable which is not in flash: no backup, nothing to put back.
  //
  ClearVariables ();
  PutVariable (L"boot-args", &gAppleBootVariableGuid, mVolatile, "volatile", 8);
  CHECK (!EFI_ERROR (OcSetBootArgs ("once", FALSE)));
  CHECK (BootArgsAre ("once", 4, mVolatile));
  CHECK (!HasBackup ());

  //
  // Removed for good.
  //
  CHECK (!EFI_ERROR (OcSetBootArgs ("", TRUE)));
  CHECK (NoBootArgs ());
  CHECK (!EFI_ERROR (OcSetBootArgs ("", TRUE)));
  CHECK (NoBootArgs ());

  //
  // Too long.
  //
  {
    CHAR8  Long[1100];

    SetMem (Long, sizeof (Long) - 1, 'a');
    Long[sizeof (Long) - 1] = '\0';
    CHECK (OcSetBootArgs (Long, TRUE) == EFI_INVALID_PARAMETER);
  }

  //
  // Damaged backup is dropped and boot-args are left alone.
  //
  ClearVariables ();
  PutVariable (OC_BOOT_ARGS_BACKUP_VARIABLE_NAME, &gOcVendorVariableGuid, mNv, "garbage", 7);
  PutVariable (L"boot-args", &gAppleBootVariableGuid, mNv, "keep", 4);
  OcRestoreBootArgs ();
  CHECK (!HasBackup ());
  CHECK (BootArgsAre ("keep", 4, mNv));
}

//
// ---------------------------------------------------------------------------------
// Editor tests.
// ---------------------------------------------------------------------------------
//

STATIC
VOID
TestEscapeOnly (
  VOID
  )
{
  ClearVariables ();
  ScriptReset ();
  Press (KeyEsc, 1);
  RUN_EDITOR (NULL);
  CHECK (SeenOnScreen ("OpenCore Options"));
  CHECK (SeenOnScreen ("Highlighted entry: macOS Test"));
  CHECK (SeenOnScreen ("Boot arguments:  (not set)"));
  CHECK (SeenOnScreen ("Edit config.plist"));
  CHECK (SeenOnScreen ("Restart"));
  CHECK (mPrivilegeRequests > 0);
  CHECK (mClearCount > 0);
}

STATIC
VOID
TestPrivilege (
  VOID
  )
{
  EFI_STATUS  Status;

  mPrivilegeResult   = EFI_ABORTED;
  mPrivilegeRequests = 0;
  mSnapshot[0]       = '\0';
  Status             = mPicker->ShowEditor (mPicker, &mEntry);
  CHECK (Status == EFI_ABORTED);
  CHECK (mPrivilegeRequests == 1);
  CHECK (!SeenOnScreen ("OpenCore Options"));
  mPrivilegeResult = EFI_SUCCESS;
}

STATIC
VOID
TestBootArgsEditor (
  VOID
  )
{
  ClearVariables ();
  PutVariable (L"boot-args", &gAppleBootVariableGuid, mNv, "keepsyms=1", 10);

  //
  // Edit, boot once.
  //
  ScriptReset ();
  Press (KeyEnter, 1);                 // Boot arguments
  Type (" debug=0x100");               // appended at the end of the existing text
  Press (KeyEnter, 1);                 // continue
  Press (KeyEnter, 1);                 // 1. Boot once
  Press (KeyEsc, 1);                   // leave
  RUN_EDITOR (NULL);
  CHECK (SeenOnScreen ("NVRAM now: keepsyms=1"));
  CHECK (SeenOnScreen ("keepsyms=1 debug=0x100"));
  CHECK (SeenOnScreen ("Boot once"));
  CHECK (SeenOnScreen ("Boot arguments are set for this boot only"));
  CHECK (BootArgsAre ("keepsyms=1 debug=0x100", 22, mVolatile));
  CHECK (HasBackup ());
  ResetPlatform ();
  OcRestoreBootArgs ();
  CHECK (BootArgsAre ("keepsyms=1", 10, mNv));

  //
  // Editing keys: Home, Delete, End, Left, Backspace, clearing.
  //
  ScriptReset ();
  Press (KeyEnter, 1);
  Press (KeyHome, 1);
  Press (KeyDelete, 4);                // keepsyms=1 -> "ms=1"... removes "keep"
  Press (KeyEnd, 1);
  Press (KeyBackspace, 1);             // removes the "1"
  Type ("2 -v");
  Press (KeyLeft, 2);
  Type ("X");                          // "ms=2 X-v"
  Press (KeyEnter, 1);
  Press (KeyDown, 1);                  // 2. Keep in NVRAM
  Press (KeyEnter, 1);
  Press (KeyEsc, 1);
  RUN_EDITOR (NULL);
  CHECK (BootArgsAre ("syms=2 X-v", 10, mNv));
  CHECK (!HasBackup ());

  //
  // Clearing: Esc empties the text, a further Esc cancels, nothing changes.
  //
  ScriptReset ();
  Press (KeyEnter, 1);
  Press (KeyEsc, 2);                   // clear, cancel
  Press (KeyEsc, 1);                   // leave
  RUN_EDITOR (NULL);
  CHECK (BootArgsAre ("syms=2 X-v", 10, mNv));

  //
  // Esc in the choice goes back to editing, no changes yet.
  //
  ScriptReset ();
  Press (KeyEnter, 1);
  Type ("Z");
  Press (KeyEnter, 1);
  Press (KeyEsc, 1);                   // back to editing
  Press (KeyBackspace, 1);             // remove Z again
  Press (KeyEnter, 1);
  Type ("2");                          // chooses item 2 directly
  Press (KeyEsc, 1);
  RUN_EDITOR (NULL);
  CHECK (BootArgsAre ("syms=2 X-v", 10, mNv));

  //
  // Removing the arguments.
  //
  ScriptReset ();
  Press (KeyEnter, 1);
  Press (KeyEsc, 1);                   // clear
  Press (KeyEnter, 1);                 // empty
  Press (KeyEnter, 1);                 // 1. Boot once
  Press (KeyEsc, 1);
  RUN_EDITOR (NULL);
  CHECK (NoBootArgs ());
  CHECK (HasBackup ());
  CHECK (SeenOnScreen ("none, boot-args are removed"));

  //
  // The replacement can be taken back from the menu.
  //
  ScriptReset ();
  Press (KeyDown, MENU_PUT_BACK);                  // "Put the original boot arguments back now"
  Press (KeyEnter, 1);
  Press (KeyEsc, 1);
  RUN_EDITOR (NULL);
  CHECK (SeenOnScreen ("Put the original boot arguments back now"));
  CHECK (BootArgsAre ("syms=2 X-v", 10, mNv));
  CHECK (!HasBackup ());
  CHECK (SeenOnScreen ("The original boot arguments are back"));

  //
  // Arguments with bytes which are not text are not edited.
  //
  ClearVariables ();
  PutVariable (L"boot-args", &gAppleBootVariableGuid, mNv, "a\x01" "b", 3);
  ScriptReset ();
  Press (KeyEnter, 1);
  Press (KeyEnter, 1);                 // dismiss the message
  Press (KeyEsc, 1);
  RUN_EDITOR (NULL);
  CHECK (SeenOnScreen ("cannot be edited here"));
  CHECK (BootArgsAre ("a\x01" "b", 3, mNv));

  //
  // Characters which cannot be typed: < and > are not accepted.
  //
  ClearVariables ();
  ScriptReset ();
  Press (KeyEnter, 1);
  Type ("a<b>c");
  Press (KeyEnter, 1);
  Press (KeyEnter, 1);                 // once
  Press (KeyEsc, 1);
  RUN_EDITOR (NULL);
  CHECK (BootArgsAre ("abc", 3, mVolatile));
}

/**
  Press keys to go down a path of the configuration which is shown by the editor.
  Dictionaries are entered by key, arrays by position written as #N.
**/
STATIC
VOID
NavigateTo (
  IN CONST CHAR8  **Path,
  IN UINTN        Count
  )
{
  EDIT_PLIST  Plist;
  XML_NODE    *Node;
  UINT32      Index;
  UINTN       Level;
  UINT32      Position;
  UINT32      Total;
  VOID        *Data;
  UINT32      Size;

  Data = ReadTestFile ("config.plist", &Size);
  CHECK (Data != NULL);
  CHECK (!EFI_ERROR (EditPlistLoad (&Plist, Data, Size)));
  FreePool (Data);

  Node = Plist.Root;
  for (Level = 0; Level < Count; ++Level) {
    Total    = EditPlistChildCount (Node);
    Position = MAX_UINT32;
    if (Path[Level][0] == '#') {
      Position = (UINT32)atoi (&Path[Level][1]);
    } else {
      for (Index = 0; Index < Total; ++Index) {
        if (AsciiStrCmp (EditPlistChildKey (Node, Index), Path[Level]) == 0) {
          Position = Index;
          break;
        }
      }
    }

    CHECK (Position < Total);
    if (Position >= Total) {
      break;
    }

    Press (KeyDown, Position);
    if (Level + 1 < Count) {
      Press (KeyEnter, 1);
    }

    Node = EditPlistChildValue (Node, Position);
  }

  EditPlistFree (&Plist);
}

STATIC
VOID
Enter (
  IN CONST CHAR8  **Path,
  IN UINTN        Count
  )
{
  //
  // Main menu: the third item, then the path, entering every level.
  //
  Press (KeyDown, 2);
  Press (KeyEnter, 1);
  NavigateTo (Path, Count);
}

/**
  Check that Changed is Original with the first Old after Key replaced with New and nothing else.
**/
STATIC
BOOLEAN
IsOriginalWithReplacement (
  IN CONST CHAR8  *Original,
  IN UINT32       OriginalSize,
  IN CONST CHAR8  *Changed,
  IN UINT32       ChangedSize,
  IN CONST CHAR8  *Key,
  IN CONST CHAR8  *Old,
  IN CONST CHAR8  *New
  )
{
  CHAR8   *Text;
  CHAR8   *Position;
  UINTN   Head;
  BOOLEAN Same;

  Text = AllocatePool ((UINTN)OriginalSize + 1);
  CopyMem (Text, Original, OriginalSize);
  Text[OriginalSize] = '\0';

  Position = strstr (Text, Key);
  if (Position == NULL) {
    FreePool (Text);
    return FALSE;
  }

  Position = strstr (Position, Old);
  if (Position == NULL) {
    FreePool (Text);
    return FALSE;
  }

  Head = (UINTN)(Position - Text);
  Same = ChangedSize == OriginalSize - strlen (Old) + strlen (New)
         && memcmp (Changed, Text, Head) == 0
         && memcmp (Changed + Head, New, strlen (New)) == 0
         && memcmp (Changed + Head + strlen (New), Text + Head + strlen (Old), OriginalSize - Head - strlen (Old)) == 0;

  FreePool (Text);
  return Same;
}

STATIC
VOID
TestConfigEditor (
  VOID
  )
{
  UINT32        OriginalSize;
  VOID          *Original;
  UINT32        NewSize;
  VOID          *New;
  UINT32        BackupSize;
  VOID          *Backup;
  BOOLEAN       Restarted;
  EDIT_PLIST    Plist;
  XML_NODE      *Node;
  CONST CHAR8   *Quirks[]   = { "Kernel", "Quirks", "AppleCpuPmCfgLock" };
  CONST CHAR8   *Timeout[]  = { "Misc", "Boot", "Timeout" };
  CONST CHAR8   *Vault[]    = { "Misc", "Security", "Vault" };
  CONST CHAR8   *Allow[]    = { "Misc", "Security", "AllowPickerEdit" };
  CONST CHAR8   *PickerMode[] = { "Misc", "Boot", "PickerMode" };
  CONST CHAR8   *Csr[]      = { "NVRAM", "Add", "7C436110-AB2A-4BBB-A880-FE41995C9F82", "csr-active-config" };
  CONST CHAR8   *Kext[]     = { "Kernel", "Add", "#0", "Enabled" };

  ClearVariables ();
  Original = ReadTestFile ("config.plist", &OriginalSize);
  CHECK (Original != NULL);

  //
  // Browse only.
  //
  ScriptReset ();
  Enter ((CONST CHAR8 *[]) { "ACPI", "Add" }, 2);
  Press (KeyEnter, 1);                 // open Add
  Press (KeyEsc, 8);                   // back to the root, leave the config editor, leave the options
  RUN_EDITOR (NULL);
  CHECK (SeenOnScreen ("Edit config.plist"));
  CHECK (SeenOnScreen ("config.plist > ACPI"));
  CHECK (SeenOnScreen ("Add"));
  CHECK (SeenOnScreen ("Enter: open/edit"));
  New = ReadTestFile ("config.plist", &NewSize);
  CHECK (NewSize == OriginalSize && memcmp (New, Original, NewSize) == 0);
  FreePool (New);

  //
  // Toggle a boolean with Enter and save with F10, restarting afterwards.
  //
  ScriptReset ();
  Enter (Quirks, ARRAY_SIZE (Quirks));
  Press (KeyEnter, 1);                 // toggles
  Press (KeyF10, 1);
  Press (KeyEnter, 1);                 // "Restart now?" - yes
  RUN_EDITOR (&Restarted);
  CHECK (Restarted);
  CHECK (mResets == 1);
  CHECK (SeenOnScreen ("config.plist > Kernel > Quirks"));
  CHECK (SeenOnScreen ("AppleCpuPmCfgLock"));
  CHECK (SeenOnScreen ("(1 unsaved changes)"));
  CHECK (SeenOnScreen ("Saved. Restart now"));

  New    = ReadTestFile ("config.plist", &NewSize);
  Backup = ReadTestFile ("config.plist.bak", &BackupSize);
  CHECK (Backup != NULL && BackupSize == OriginalSize && memcmp (Backup, Original, BackupSize) == 0);
  CHECK (New != NULL && NewSize == OriginalSize - 1);
  CHECK (!EFI_ERROR (EditPlistLoad (&Plist, New, NewSize)));
  Node = EditPlistFindChild (EditPlistFindChild (Plist.Root, "Kernel"), "Quirks");
  CHECK (EditPlistGetBoolean (&Plist, EditPlistFindChild (Node, "AppleCpuPmCfgLock")));
  EditPlistFree (&Plist);
  CHECK (IsOriginalWithReplacement (Original, OriginalSize, New, NewSize, "<key>AppleCpuPmCfgLock</key>", "<false/>", "<true/>"));
  FreePool (New);
  FreePool (Backup);

  //
  // The same key again, with the space bar, then leaving through the unsaved changes question.
  // Discarding changes keeps the file as it is.
  //
  New = ReadTestFile ("config.plist", &NewSize);
  ScriptReset ();
  Enter (Quirks, ARRAY_SIZE (Quirks));
  Press (KeySpace, 1);
  Press (KeyEsc, 3);                   // up from Quirks, up from Kernel, up from root -> question
  Press (KeyDown, 2);                  // Discard
  Press (KeyEnter, 1);
  Press (KeyEsc, 1);                   // leave the options
  RUN_EDITOR (&Restarted);
  CHECK (!Restarted);
  CHECK (SeenOnScreen ("There are unsaved changes"));
  CHECK (SeenOnScreen ("Save and restart"));
  CHECK (SeenOnScreen ("Discard the changes"));
  {
    UINT32  AgainSize;
    VOID    *Again;

    Again = ReadTestFile ("config.plist", &AgainSize);
    CHECK (AgainSize == NewSize && memcmp (Again, New, NewSize) == 0);
    FreePool (Again);
  }

  FreePool (New);

  //
  // Esc in the question keeps editing; Space on a kext switches its Enabled; save through the question.
  //
  New = ReadTestFile ("config.plist", &NewSize);
  ScriptReset ();
  Press (KeyDown, 2);
  Press (KeyEnter, 1);
  NavigateTo ((CONST CHAR8 *[]) { "Kernel", "Add" }, 2);   // enter Kernel, then Add is selected
  Press (KeyEnter, 1);                                      // open Add
  Press (KeySpace, 1);                                      // first kext: switch Enabled
  Press (KeyEsc, 3);                                        // question
  Press (KeyEsc, 1);                                        // keep editing
  Press (KeyF10, 1);                                        // save
  Press (KeyEsc, 1);                                        // do not restart
  Press (KeyEsc, 1);                                        // leave the options
  RUN_EDITOR (&Restarted);
  CHECK (!Restarted);
  CHECK (SeenOnScreen ("The config is saved, restart to apply the changes"));
  {
    UINT32  AgainSize;
    VOID    *Again;

    Again = ReadTestFile ("config.plist", &AgainSize);
    CHECK (AgainSize != NewSize);
    CHECK (!EFI_ERROR (EditPlistLoad (&Plist, Again, AgainSize)));
    Node = EditPlistChildValue (EditPlistFindChild (EditPlistFindChild (Plist.Root, "Kernel"), "Add"), 0);
    CHECK (EditPlistFindChild (Node, "Enabled") != NULL);
    EditPlistFree (&Plist);
    FreePool (Again);
  }

  FreePool (New);

  //
  // Numbers: Misc > Boot > Timeout.
  //
  ScriptReset ();
  Enter (Timeout, ARRAY_SIZE (Timeout));
  Press (KeyEnter, 1);                 // edit
  Press (KeyEsc, 1);                   // clear
  Type ("0x10");
  Press (KeyEnter, 1);                 // 16
  Press (KeyEsc, 3);                   // leave through the question
  Press (KeyEnter, 1);                 // Save and restart
  RUN_EDITOR (&Restarted);
  CHECK (Restarted);
  New = ReadTestFile ("config.plist", &NewSize);
  CHECK (!EFI_ERROR (EditPlistLoad (&Plist, New, NewSize)));
  Node = EditPlistFindChild (EditPlistFindChild (EditPlistFindChild (Plist.Root, "Misc"), "Boot"), "Timeout");
  {
    CHAR8  Number[32];

    EditPlistGetInteger (&Plist, Node, Number, sizeof (Number));
    CHECK (AsciiStrCmp (Number, "16") == 0);
  }

  EditPlistFree (&Plist);
  FreePool (New);

  //
  // Wrong input for a number is not accepted: the editor asks again.
  //
  ScriptReset ();
  Enter (Timeout, ARRAY_SIZE (Timeout));
  Press (KeyEnter, 1);
  Press (KeyEsc, 1);
  Type ("12ab");
  Press (KeyEnter, 1);                 // not a number
  Press (KeyEnter, 1);                 // dismiss
  Press (KeyEsc, 2);                   // clear, cancel
  Press (KeyEsc, 10);                  // leave everything
  RUN_EDITOR (&Restarted);
  CHECK (!Restarted);
  CHECK (SeenOnScreen ("Not a number"));

  //
  // Strings: Misc > Boot > PickerMode.
  //
  ScriptReset ();
  Enter (PickerMode, ARRAY_SIZE (PickerMode));
  Press (KeyEnter, 1);
  Press (KeyEsc, 1);
  Type ("External");
  Press (KeyEnter, 1);
  Press (KeyEsc, 3);
  Press (KeyDown, 1);                  // Save
  Press (KeyEnter, 1);
  Press (KeyEsc, 1);                   // leave the options
  RUN_EDITOR (&Restarted);
  CHECK (!Restarted);
  New = ReadTestFile ("config.plist", &NewSize);
  CHECK (!EFI_ERROR (EditPlistLoad (&Plist, New, NewSize)));
  Node = EditPlistFindChild (EditPlistFindChild (EditPlistFindChild (Plist.Root, "Misc"), "Boot"), "PickerMode");
  CHECK (AsciiStrCmp (EditPlistGetContent (&Plist, Node), "External") == 0);
  EditPlistFree (&Plist);
  FreePool (New);

  //
  // Data: NVRAM > Add > GUID > csr-active-config as hexadecimal bytes.
  //
  ScriptReset ();
  Enter (Csr, ARRAY_SIZE (Csr));
  Press (KeyEnter, 1);
  Press (KeyEsc, 1);
  Type ("E7 03 00 00");
  Press (KeyEnter, 1);
  Press (KeyEsc, 4);
  Press (KeyDown, 1);
  Press (KeyEnter, 1);
  Press (KeyEsc, 1);
  RUN_EDITOR (&Restarted);
  CHECK (!Restarted);
  CHECK (SeenOnScreen ("Hexadecimal bytes"));
  New = ReadTestFile ("config.plist", &NewSize);
  CHECK (!EFI_ERROR (EditPlistLoad (&Plist, New, NewSize)));
  Node = EditPlistFindChild (EditPlistFindChild (EditPlistFindChild (Plist.Root, "NVRAM"), "Add"), "7C436110-AB2A-4BBB-A880-FE41995C9F82");
  CHECK (Node != NULL);
  CHECK (AsciiStrCmp (EditPlistGetContent (&Plist, EditPlistFindChild (Node, "csr-active-config")), "5wMAAA==") == 0);
  EditPlistFree (&Plist);
  FreePool (New);

  //
  // Odd digit count is refused.
  //
  ScriptReset ();
  Enter (Csr, ARRAY_SIZE (Csr));
  Press (KeyEnter, 1);
  Press (KeyEsc, 1);
  Type ("E70");
  Press (KeyEnter, 1);
  Press (KeyEnter, 1);                 // dismiss
  Press (KeyEsc, 2);
  Press (KeyEsc, 10);
  RUN_EDITOR (&Restarted);
  CHECK (SeenOnScreen ("even number of hexadecimal digits"));

  //
  // Protected settings cannot be changed here, but switches can.
  //
  New = ReadTestFile ("config.plist", &NewSize);
  ScriptReset ();
  Enter (Vault, ARRAY_SIZE (Vault));
  Press (KeyEnter, 1);                 // refused
  Press (KeyEsc, 10);
  RUN_EDITOR (&Restarted);
  CHECK (SeenOnScreen ("Protected:"));
  {
    UINT32  AgainSize;
    VOID    *Again;

    Again = ReadTestFile ("config.plist", &AgainSize);
    CHECK (AgainSize == NewSize && memcmp (Again, New, NewSize) == 0);
    FreePool (Again);
  }

  FreePool (New);

  ScriptReset ();
  Enter (Allow, ARRAY_SIZE (Allow));
  Press (KeyEnter, 1);                 // a boolean in Security is fine
  Press (KeyEsc, 3);                   // up, up, question
  Press (KeyDown, 2);
  Press (KeyEnter, 1);                 // discard
  Press (KeyEsc, 1);
  RUN_EDITOR (&Restarted);
  CHECK (!SeenOnScreen ("Protected:"));
  CHECK (SeenOnScreen ("(1 unsaved changes)"));

  //
  // Nothing to save.
  //
  ScriptReset ();
  Press (KeyDown, 2);
  Press (KeyEnter, 1);
  Press (KeyF10, 1);
  Press (KeyEsc, 1);
  Press (KeyEsc, 1);
  RUN_EDITOR (&Restarted);
  CHECK (SeenOnScreen ("No changes to save"));

  FreePool (Original);

  (VOID)Kext;
}

STATIC
VOID
TestBootFlags (
  VOID
  )
{
  ClearVariables ();
  PutVariable (L"boot-args", &gAppleBootVariableGuid, mNv, "keepsyms=1 debug=0x144 -x", 25);

  //
  // Flags: -v (0) on, -x (2) off, debug=0x100 (6) replaces the other value, keepsyms=1 (5) off.
  //
  ScriptReset ();
  Press (KeyDown, 1);
  Press (KeyEnter, 1);                 // Boot flags
  Press (KeySpace, 1);                 // -v
  Press (KeyDown, 2);
  Press (KeySpace, 1);                 // -x off
  Press (KeyDown, 3);
  Press (KeySpace, 1);                 // keepsyms=1 off
  Press (KeyDown, 1);
  Press (KeySpace, 1);                 // debug=0x100 instead of debug=0x144
  Press (KeyEnter, 1);                 // continue
  Press (KeyEnter, 1);                 // boot once
  Press (KeyEsc, 1);
  RUN_EDITOR (NULL);
  CHECK (SeenOnScreen ("[x] -x"));
  CHECK (SeenOnScreen ("[x] keepsyms=1"));
  CHECK (SeenOnScreen ("[~] debug=0x100"));
  CHECK (SeenOnScreen ("Verbose boot"));
  CHECK (SeenOnScreen ("Boot arguments: keepsyms=1 debug=0x144 -x -v"));
  CHECK (BootArgsAre ("-v debug=0x100", 14, mVolatile));
  CHECK (HasBackup ());
  ResetPlatform ();
  OcRestoreBootArgs ();
  CHECK (BootArgsAre ("keepsyms=1 debug=0x144 -x", 25, mNv));

  //
  // Toggling twice is no change, the result is applied through the same choice as typed text.
  //
  ScriptReset ();
  Press (KeyDown, 1);
  Press (KeyEnter, 1);
  Press (KeySpace, 2);                 // -v on and off
  Press (KeyEnter, 1);
  Press (KeyDown, 1);                  // keep in NVRAM
  Press (KeyEnter, 1);
  Press (KeyEsc, 1);
  RUN_EDITOR (NULL);
  CHECK (BootArgsAre ("keepsyms=1 debug=0x144 -x", 25, mNv));
  CHECK (!HasBackup ());

  //
  // Esc leaves without changes; from the choice it goes back to the flags.
  //
  ScriptReset ();
  Press (KeyDown, 1);
  Press (KeyEnter, 1);
  Press (KeySpace, 1);
  Press (KeyEnter, 1);
  Press (KeyEsc, 1);                   // back to the flags
  Press (KeySpace, 1);                 // -v off again
  Press (KeyEsc, 1);                   // leave the flags
  Press (KeyEsc, 1);                   // leave the options
  RUN_EDITOR (NULL);
  CHECK (BootArgsAre ("keepsyms=1 debug=0x144 -x", 25, mNv));
  CHECK (SeenOnScreen ("Apply boot arguments"));

  //
  // No room for a flag.
  //
  {
    CHAR8  Long[512];

    SetMem (Long, 500, 'x');
    Long[500] = '\0';
    ClearVariables ();
    PutVariable (L"boot-args", &gAppleBootVariableGuid, mNv, Long, 500);

    ScriptReset ();
    Press (KeyDown, 1);
    Press (KeyEnter, 1);
    Press (KeyEnd, 1);                 // the last flag, which is too long to add
    Press (KeySpace, 1);
    Press (KeyEnter, 1);               // dismiss
    Press (KeyEsc, 2);
    RUN_EDITOR (NULL);
    CHECK (SeenOnScreen ("too long"));
    CHECK (BootArgsAre (Long, 500, mNv));
  }

  //
  // Flags are removed from anywhere in the arguments, nothing else is touched.
  //
  ClearVariables ();
  PutVariable (L"boot-args", &gAppleBootVariableGuid, mNv, "-v  a=1 -s -v b=2 -vv", 21);
  ScriptReset ();
  Press (KeyDown, 1);
  Press (KeyEnter, 1);
  Press (KeySpace, 1);                 // -v off (the same switch in two places, but -vv stays)
  Press (KeyEnter, 1);
  Press (KeyEnter, 1);
  Press (KeyEsc, 1);
  RUN_EDITOR (NULL);
  CHECK (BootArgsAre ("a=1 -s b=2 -vv", 14, mVolatile));
}

//
// ---------------------------------------------------------------------------------
// Choosing the configuration.
// ---------------------------------------------------------------------------------
//

STATIC OC_GLOBAL_CONFIG  *mTestConfig;

/**
  Load the configuration the way OpenCore does at start, return the timeout it sets.
**/
STATIC
UINT32
LoadConfigTimeout (
  OUT EFI_STATUS  *LoadStatus  OPTIONAL
  )
{
  EFI_STATUS  Status;
  UINT32      Timeout;

  if (mTestConfig == NULL) {
    mTestConfig = AllocatePool (sizeof (*mTestConfig));
  }

  Status  = OcMiscLoadConfig (&mStorage, mTestConfig);
  Timeout = 0;
  if (!EFI_ERROR (Status)) {
    Timeout = mTestConfig->Misc.Boot.Timeout;
    OcConfigurationFree (mTestConfig);
  }

  if (LoadStatus != NULL) {
    *LoadStatus = Status;
  }

  return Timeout;
}

/**
  Make a configuration out of the sample.
**/
STATIC
VOID
WriteConfigVariant (
  IN CONST CHAR8  *Name,
  IN UINT64       Timeout,
  IN BOOLEAN      AllowEdit
  )
{
  EDIT_PLIST  Plist;
  CHAR8       *Text;
  UINT32      Size;
  XML_NODE    *Boot;
  XML_NODE    *Security;

  CHECK (!EFI_ERROR (EditPlistLoad (&Plist, mSample, mSampleSize)));
  Boot     = EditPlistFindChild (EditPlistFindChild (Plist.Root, "Misc"), "Boot");
  Security = EditPlistFindChild (EditPlistFindChild (Plist.Root, "Misc"), "Security");
  CHECK (Boot != NULL && Security != NULL);
  CHECK (!EFI_ERROR (EditPlistSetInteger (&Plist, EditPlistFindChild (Boot, "Timeout"), Timeout)));
  CHECK (!EFI_ERROR (EditPlistSetBoolean (&Plist, EditPlistFindChild (Security, "AllowPickerEdit"), AllowEdit)));
  CHECK (!EFI_ERROR (EditPlistRender (&Plist, &Text, &Size)));
  WriteTestFile (Name, Text, Size);
  FreePool (Text);
  EditPlistFree (&Plist);
}

STATIC
VOID
RemoveTestFile (
  IN CONST CHAR8  *Name
  )
{
  CHAR8  Path[512];

  AsciiSPrint (Path, sizeof (Path), "%a/%a", mDirectory, Name);
  unlink (Path);
}

STATIC
BOOLEAN
HasMarker (
  VOID
  )
{
  UINT32  Size;
  VOID    *Data;

  Data = ReadTestFile ("config.choice", &Size);
  if (Data != NULL) {
    FreePool (Data);
  }

  return Data != NULL;
}

STATIC
BOOLEAN
MarkerIs (
  IN CONST CHAR8  *Text
  )
{
  UINT32   Size;
  VOID     *Data;
  BOOLEAN  Same;

  Data = ReadTestFile ("config.choice", &Size);
  if (Data == NULL) {
    printf ("  there is no config.choice\n");
    return FALSE;
  }

  Same = Size == AsciiStrLen (Text) && CompareMem (Data, Text, Size) == 0;
  if (!Same) {
    printf ("  config.choice is %u bytes: \"%.*s\"\n", (unsigned)Size, (int)Size, (CONST CHAR8 *)Data);
  }

  FreePool (Data);
  return Same;
}

STATIC
VOID
WriteMarker (
  IN CONST CHAR8  *Text
  )
{
  WriteTestFile ("config.choice", Text, (UINT32)AsciiStrLen (Text));
}

STATIC
BOOLEAN
ConfigIs (
  IN CONST CHAR16      *Name,
  IN OC_CONFIG_SOURCE  Source
  )
{
  return StrCmp (OcMiscGetConfigName (), Name) == 0 && OcMiscGetConfigSource (NULL) == Source;
}

/**
  Make a valid name of a configuration of the given length.
**/
STATIC
VOID
MakeConfigName (
  OUT CHAR8  *Name,
  IN  UINTN  Length
  )
{
  CopyMem (Name, "config", 6);
  SetMem (&Name[6], Length - 12, 'x');
  CopyMem (&Name[Length - 6], ".plist", 6);
  Name[Length] = '\0';
}

STATIC
VOID
TestConfigChoiceLibrary (
  VOID
  )
{
  EFI_STATUS      Status;
  UINT32          Timeout;
  CONST CHAR16    *Failed;
  OC_CONFIG_FILE  Files[OC_CONFIG_MAX_FILES];
  UINTN           Count;
  UINTN           Index;
  CHAR8           Name[OC_CONFIG_MAX_NAME + 16];
  CHAR8           Long[OC_CONFIG_MAX_NAME + 32];

  //
  // Nothing chosen: the default configuration.
  //
  RemoveTestFile ("config.choice");
  Timeout = LoadConfigTimeout (&Status);
  CHECK (!EFI_ERROR (Status) && Timeout == 5);
  CHECK (ConfigIs (L"config.plist", OcConfigSourceDefault));

  //
  // A choice for one start is used by the next start only.
  //
  WriteConfigVariant ("config2.plist", 77, TRUE);
  CHECK (!EFI_ERROR (OcMiscSetConfigChoice (&mStorage, L"config2.plist", FALSE)));
  CHECK (MarkerIs ("config2.plist\nonce\n"));
  Timeout = LoadConfigTimeout (&Status);
  CHECK (!EFI_ERROR (Status) && Timeout == 77);
  CHECK (ConfigIs (L"config2.plist", OcConfigSourceOnce));
  CHECK (!HasMarker ());
  Timeout = LoadConfigTimeout (&Status);
  CHECK (!EFI_ERROR (Status) && Timeout == 5);
  CHECK (ConfigIs (L"config.plist", OcConfigSourceDefault));

  //
  // A choice to stay is used by every start until it is taken back.
  //
  CHECK (!EFI_ERROR (OcMiscSetConfigChoice (&mStorage, L"config2.plist", TRUE)));
  CHECK (MarkerIs ("config2.plist\nkeep\n"));
  Timeout = LoadConfigTimeout (&Status);
  CHECK (Timeout == 77 && ConfigIs (L"config2.plist", OcConfigSourceKept) && HasMarker ());
  Timeout = LoadConfigTimeout (&Status);
  CHECK (Timeout == 77 && ConfigIs (L"config2.plist", OcConfigSourceKept) && HasMarker ());
  CHECK (!EFI_ERROR (OcMiscSetConfigChoice (&mStorage, L"config.plist", FALSE)));
  CHECK (!HasMarker ());
  Timeout = LoadConfigTimeout (&Status);
  CHECK (Timeout == 5 && ConfigIs (L"config.plist", OcConfigSourceDefault));

  //
  // Spaces and the case in the file are tolerated.
  //
  WriteMarker ("  config2.plist  \r\n  KEEP  \r\n");
  Timeout = LoadConfigTimeout (&Status);
  CHECK (Timeout == 77 && ConfigIs (L"config2.plist", OcConfigSourceKept) && HasMarker ());
  WriteMarker ("config2.plist\r\nkeeper\r\n");
  Timeout = LoadConfigTimeout (&Status);
  CHECK (Timeout == 77 && ConfigIs (L"config2.plist", OcConfigSourceOnce) && !HasMarker ());

  //
  // The chosen configuration is gone: the default one is used instead, a choice for
  // one start is used up, and a choice to stay is left for when the file is back.
  //
  WriteMarker ("config9.plist\nonce\n");
  Timeout = LoadConfigTimeout (&Status);
  CHECK (!EFI_ERROR (Status) && Timeout == 5);
  CHECK (ConfigIs (L"config.plist", OcConfigSourceFallback));
  CHECK (OcMiscGetConfigSource (&Failed) == OcConfigSourceFallback && StrCmp (Failed, L"config9.plist") == 0);
  CHECK (!HasMarker ());
  WriteMarker ("config9.plist\nkeep\n");
  Timeout = LoadConfigTimeout (&Status);
  CHECK (Timeout == 5 && ConfigIs (L"config.plist", OcConfigSourceFallback) && HasMarker ());
  RemoveTestFile ("config.choice");

  //
  // The chosen configuration cannot be parsed.
  //
  WriteTestFile ("config3.plist", "<plist><dict>", 13);
  WriteMarker ("config3.plist\nonce\n");
  Timeout = LoadConfigTimeout (&Status);
  CHECK (!EFI_ERROR (Status) && Timeout == 5);
  CHECK (ConfigIs (L"config.plist", OcConfigSourceFallback));
  CHECK (!HasMarker ());
  RemoveTestFile ("config3.plist");

  //
  // What does not look like the name of a configuration is not followed, and is removed.
  //
  MakeConfigName (Name, OC_CONFIG_MAX_NAME + 1);
  AsciiSPrint (Long, sizeof (Long), "%a\nonce\n", Name);
  {
    CONST CHAR8  *Bad[] = {
      "../config.plist\nonce\n",
      "config.txt\nkeep\n",
      "dir/config2.plist\nonce\n",
      "dir\\config2.plist\nonce\n",
      "config2.plist.bak\nonce\n",
      "config..plist\nonce\n",
      "\xC3\xA9" "config.plist\nonce\n",
      "config\xC3\xA9" ".plist\nonce\n",
      "",
      "   \n",
      "\n\n\n",
      "plist\nkeep\n",
      "keep\n",
      Long
    };

    for (Index = 0; Index < ARRAY_SIZE (Bad); ++Index) {
      WriteMarker (Bad[Index]);
      Timeout = LoadConfigTimeout (&Status);
      if (!(!EFI_ERROR (Status) && Timeout == 5 && ConfigIs (L"config.plist", OcConfigSourceDefault) && !HasMarker ())) {
        printf ("FAIL: unacceptable choice %u was not dropped\n", (unsigned)Index);
        ++mFailures;
      }

      ++mChecks;
    }
  }

  //
  // The longest acceptable name is followed, one character more is not.
  //
  {
    CHAR16  Wide[OC_CONFIG_MAX_NAME + 16];

    MakeConfigName (Name, OC_CONFIG_MAX_NAME);
    for (Index = 0; Index <= OC_CONFIG_MAX_NAME; ++Index) {
      Wide[Index] = (CHAR16)Name[Index];
    }

    CHECK (OcMiscIsConfigName (Wide));
    WriteTestFile (Name, mSample, mSampleSize);
    AsciiSPrint (Long, sizeof (Long), "%a\nkeep\n", Name);
    WriteMarker (Long);
    Timeout = LoadConfigTimeout (&Status);
    CHECK (!EFI_ERROR (Status) && Timeout == 5);
    CHECK (StrCmp (OcMiscGetConfigName (), Wide) == 0 && OcMiscGetConfigSource (NULL) == OcConfigSourceKept);
    RemoveTestFile ("config.choice");
    RemoveTestFile (Name);

    MakeConfigName (Name, OC_CONFIG_MAX_NAME + 1);
    for (Index = 0; Index <= OC_CONFIG_MAX_NAME + 1; ++Index) {
      Wide[Index] = (CHAR16)Name[Index];
    }

    CHECK (!OcMiscIsConfigName (Wide));
  }

  //
  // With a vault only the default configuration is known.
  //
  WriteMarker ("config2.plist\nkeep\n");
  mStorage.HasVault = TRUE;
  Timeout = LoadConfigTimeout (&Status);
  CHECK (Timeout == 5 && ConfigIs (L"config.plist", OcConfigSourceDefault));
  CHECK (HasMarker ());
  CHECK (OcMiscSetConfigChoice (&mStorage, L"config2.plist", FALSE) == EFI_INVALID_PARAMETER);
  mStorage.HasVault = FALSE;
  RemoveTestFile ("config.choice");

  //
  // Names which cannot be chosen.
  //
  CHECK (OcMiscSetConfigChoice (&mStorage, L"foo.plist", FALSE) == EFI_INVALID_PARAMETER);
  CHECK (OcMiscSetConfigChoice (&mStorage, L"..\\config.plist", FALSE) == EFI_INVALID_PARAMETER);
  CHECK (!HasMarker ());
  CHECK (OcMiscIsConfigName (L"config.plist"));
  CHECK (OcMiscIsConfigName (L"Config2.PLIST"));
  CHECK (OcMiscIsConfigName (L"config-gpu (new).plist"));
  CHECK (OcMiscIsConfigName (L"config_1.v2.plist"));
  CHECK (!OcMiscIsConfigName (L"config"));
  CHECK (!OcMiscIsConfigName (L".plist"));
  CHECK (!OcMiscIsConfigName (L"config.plis"));
  CHECK (!OcMiscIsConfigName (L"myconfig.plist"));
  CHECK (!OcMiscIsConfigName (L"config/2.plist"));
  CHECK (!OcMiscIsConfigName (L"config:2.plist"));
  CHECK (!OcMiscIsConfigName (L"config*.plist"));

  //
  // Listing: only configurations, default first, then ordered.
  //
  RemoveTestFile ("config2.plist");
  WriteConfigVariant ("config2.plist", 77, TRUE);
  WriteConfigVariant ("Config-B.PLIST", 11, TRUE);
  WriteConfigVariant ("config-ok (1).plist", 12, TRUE);
  WriteTestFile ("config.plist.bak", "x", 1);
  WriteTestFile ("._config.plist", "x", 1);
  WriteTestFile ("notes.txt", "x", 1);
  WriteTestFile ("vault.plist", "x", 1);
  {
    CHAR8  Path[512];

    AsciiSPrint (Path, sizeof (Path), "%a/configdir.plist", mDirectory);
    mkdir (Path, 0755);
  }

  Count = OcMiscListConfigs (&mStorage, Files, OC_CONFIG_MAX_FILES);
  CHECK (Count == 4);
  if (Count == 4) {
    CHECK (StrCmp (Files[0].Name, L"config.plist") == 0);
    CHECK (StrCmp (Files[1].Name, L"Config-B.PLIST") == 0);
    CHECK (StrCmp (Files[2].Name, L"config-ok (1).plist") == 0);
    CHECK (StrCmp (Files[3].Name, L"config2.plist") == 0);
  }

  //
  // The default configuration is listed even when there is no room for everything.
  //
  Count = OcMiscListConfigs (&mStorage, Files, 2);
  CHECK (Count == 2);
  CHECK (Count == 2 && StrCmp (Files[0].Name, L"config.plist") == 0);
  CHECK (Count == 2 && StrCmp (Files[1].Name, L"Config-B.PLIST") == 0);
  Count = OcMiscListConfigs (&mStorage, Files, 1);
  CHECK (Count == 1 && StrCmp (Files[0].Name, L"config.plist") == 0);
  CHECK (OcMiscListConfigs (&mStorage, Files, 0) == 0);

  {
    CHAR8  Path[512];

    AsciiSPrint (Path, sizeof (Path), "%a/configdir.plist", mDirectory);
    rmdir (Path);
  }

  RemoveTestFile ("config.plist.bak");
  RemoveTestFile ("._config.plist");
  RemoveTestFile ("notes.txt");
  RemoveTestFile ("vault.plist");
  RemoveTestFile ("Config-B.PLIST");
  RemoveTestFile ("config-ok (1).plist");

  Timeout = LoadConfigTimeout (&Status);
  CHECK (Timeout == 5 && ConfigIs (L"config.plist", OcConfigSourceDefault));
}

STATIC
VOID
TestConfigChooser (
  VOID
  )
{
  BOOLEAN  Restarted;
  UINT32   Size;
  VOID     *Data;
  VOID     *Before;
  UINT32   BeforeSize;
  UINT32   Timeout;

  RemoveTestFile ("config.choice");
  RemoveTestFile ("config2.plist");
  WriteConfigVariant ("config2.plist", 77, TRUE);

  //
  // The main menu tells which configuration is in use.
  //
  ScriptReset ();
  Press (KeyEsc, 1);
  RUN_EDITOR (NULL);
  CHECK (SeenOnScreen ("Config file:  config.plist"));
  CHECK (SeenOnScreen ("Edit config.plist"));

  //
  // Start with another one, this time only.
  //
  ScriptReset ();
  Press (KeyDown, MENU_CHOOSER);
  Press (KeyEnter, 1);                 // Config file
  Press (KeyDown, 1);                  // config2.plist
  Press (KeyEnter, 1);
  Press (KeyEnter, 1);                 // This time only
  RUN_EDITOR (&Restarted);
  CHECK (Restarted);
  CHECK (SeenOnScreen ("Config files"));
  CHECK (SeenOnScreen ("config.plist   (in use, default)"));
  CHECK (SeenOnScreen ("config2.plist"));
  CHECK (SeenOnScreen ("Start OpenCore with config2.plist"));
  CHECK (SeenOnScreen ("This time only  (the next start uses config.plist again)"));
  CHECK (SeenOnScreen ("Keep it"));
  CHECK (MarkerIs ("config2.plist\nonce\n"));
  RemoveTestFile ("config.choice");

  //
  // Keep it.
  //
  ScriptReset ();
  Press (KeyDown, MENU_CHOOSER);
  Press (KeyEnter, 1);
  Press (KeyDown, 1);
  Press (KeyEnter, 1);
  Press (KeyDown, 1);
  Press (KeyEnter, 1);                 // Keep it
  RUN_EDITOR (&Restarted);
  CHECK (Restarted);
  CHECK (MarkerIs ("config2.plist\nkeep\n"));
  RemoveTestFile ("config.choice");

  //
  // A configuration which does not enable the editor can be used for a single start only.
  //
  WriteConfigVariant ("config3.plist", 33, FALSE);
  ScriptReset ();
  Press (KeyDown, MENU_CHOOSER);
  Press (KeyEnter, 1);
  Press (KeyDown, 2);                  // config3.plist
  Press (KeyEnter, 1);
  Press (KeyDown, 1);
  Press (KeyEnter, 1);                 // keeping is not allowed, nothing happens
  Press (KeyUp, 1);
  Press (KeyEnter, 1);                 // this time only
  RUN_EDITOR (&Restarted);
  CHECK (Restarted);
  CHECK (SeenOnScreen ("AllowPickerEdit is off in this config"));
  CHECK (MarkerIs ("config3.plist\nonce\n"));
  RemoveTestFile ("config.choice");

  //
  // A configuration which cannot be loaded is not started with.
  //
  WriteTestFile ("config4.plist", "<plist><array/></plist>", 23);
  ScriptReset ();
  Press (KeyDown, MENU_CHOOSER);
  Press (KeyEnter, 1);
  Press (KeyDown, 3);                  // config4.plist
  Press (KeyEnter, 1);
  Press (KeyEnter, 1);                 // dismiss the message
  Press (KeyEsc, 5);
  RUN_EDITOR (&Restarted);
  CHECK (!Restarted);
  CHECK (SeenOnScreen ("config4.plist: this is not a valid plist"));
  CHECK (!HasMarker ());
  RemoveTestFile ("config4.plist");
  RemoveTestFile ("config3.plist");

  //
  // The configuration in use is already used.
  //
  ScriptReset ();
  Press (KeyDown, MENU_CHOOSER);
  Press (KeyEnter, 1);
  Press (KeyEnter, 1);                 // config.plist is selected
  Press (KeyEnter, 1);                 // dismiss
  Press (KeyEsc, 5);
  RUN_EDITOR (&Restarted);
  CHECK (!Restarted);
  CHECK (SeenOnScreen ("This config is in use already"));
  CHECK (!HasMarker ());

  //
  // With a vault there is nothing to choose.
  //
  mStorage.HasVault = TRUE;
  ScriptReset ();
  Press (KeyDown, MENU_CHOOSER);
  Press (KeyEnter, 1);
  Press (KeyEnter, 1);                 // dismiss
  Press (KeyEsc, 1);
  RUN_EDITOR (&Restarted);
  CHECK (SeenOnScreen ("The vault is used, OpenCore can only start with the default config"));
  mStorage.HasVault = FALSE;

  //
  // Only the default configuration: told how to get more.
  //
  RemoveTestFile ("config2.plist");
  ScriptReset ();
  Press (KeyDown, MENU_CHOOSER);
  Press (KeyEnter, 1);
  Press (KeyEsc, 2);
  RUN_EDITOR (NULL);
  CHECK (SeenOnScreen ("Other configs are files named config*.plist next to this one"));
  WriteConfigVariant ("config2.plist", 77, TRUE);

  //
  // Working with the configuration that was chosen: it is the one to be edited and saved.
  //
  Before = ReadTestFile ("config.plist", &BeforeSize);
  CHECK (!EFI_ERROR (OcMiscSetConfigChoice (&mStorage, L"config2.plist", TRUE)));
  Timeout = LoadConfigTimeout (NULL);
  CHECK (Timeout == 77 && ConfigIs (L"config2.plist", OcConfigSourceKept));

  ScriptReset ();
  Press (KeyEsc, 1);
  RUN_EDITOR (NULL);
  CHECK (SeenOnScreen ("Edit config2.plist"));
  CHECK (SeenOnScreen ("Config file:  config2.plist  (kept)"));

  {
    CONST CHAR8  *Quirks[] = { "Kernel", "Quirks", "AppleCpuPmCfgLock" };
    VOID         *Old2;
    UINT32       Old2Size;
    VOID         *New2;
    UINT32       New2Size;
    VOID         *Backup;
    UINT32       BackupSize;

    Old2 = ReadTestFile ("config2.plist", &Old2Size);
    ScriptReset ();
    Enter (Quirks, ARRAY_SIZE (Quirks));
    Press (KeyEnter, 1);
    Press (KeyF10, 1);
    Press (KeyEsc, 1);                 // do not restart
    Press (KeyEsc, 1);
    RUN_EDITOR (&Restarted);
    CHECK (!Restarted);
    CHECK (SeenOnScreen ("Edit config2.plist"));
    CHECK (SeenOnScreen ("config2.plist > Kernel > Quirks"));

    New2   = ReadTestFile ("config2.plist", &New2Size);
    Backup = ReadTestFile ("config2.plist.bak", &BackupSize);
    CHECK (New2 != NULL && New2Size == Old2Size - 1);
    CHECK (Backup != NULL && BackupSize == Old2Size && memcmp (Backup, Old2, Old2Size) == 0);
    Data = ReadTestFile ("config.plist", &Size);
    CHECK (Data != NULL && Size == BeforeSize && memcmp (Data, Before, Size) == 0);
    CHECK (ReadTestFile ("config.plist.bak", &Size) == NULL);
    FreePool (Data);
    FreePool (Old2);
    FreePool (New2);
    FreePool (Backup);
    RemoveTestFile ("config2.plist.bak");
  }

  //
  // Going back to the default configuration forgets the choice that stays.
  //
  ScriptReset ();
  Press (KeyDown, MENU_CHOOSER);
  Press (KeyEnter, 1);
  Press (KeyUp, 1);                    // config.plist
  Press (KeyEnter, 1);
  Press (KeyEnter, 1);                 // the only choice
  RUN_EDITOR (&Restarted);
  CHECK (Restarted);
  CHECK (SeenOnScreen ("config2.plist   (in use)"));
  CHECK (SeenOnScreen ("Restart with the default config"));
  CHECK (!HasMarker ());
  Timeout = LoadConfigTimeout (NULL);
  CHECK (Timeout == 5 && ConfigIs (L"config.plist", OcConfigSourceDefault));

  //
  // A choice that failed is told about.
  //
  WriteMarker ("config9.plist\nonce\n");
  Timeout = LoadConfigTimeout (NULL);
  CHECK (ConfigIs (L"config.plist", OcConfigSourceFallback));
  ScriptReset ();
  Press (KeyDown, MENU_CHOOSER);
  Press (KeyEnter, 1);
  Press (KeyEsc, 2);
  RUN_EDITOR (NULL);
  CHECK (SeenOnScreen ("Config file:  config.plist  (the chosen one failed)"));
  CHECK (SeenOnScreen ("The chosen config9.plist could not be used, the default config is in use"));

  //
  // A file system which cannot be written to.
  //
  Timeout = LoadConfigTimeout (NULL);
  mFileWriteProtected = TRUE;
  ScriptReset ();
  Press (KeyDown, MENU_CHOOSER);
  Press (KeyEnter, 1);
  Press (KeyDown, 1);
  Press (KeyEnter, 1);
  Press (KeyEnter, 1);                 // This time only: cannot be saved
  Press (KeyEnter, 1);                 // dismiss
  Press (KeyEsc, 5);
  RUN_EDITOR (&Restarted);
  mFileWriteProtected = FALSE;
  CHECK (!Restarted);
  CHECK (SeenOnScreen ("Cannot save the choice"));
  CHECK (!HasMarker ());

  FreePool (Before);
  RemoveTestFile ("config2.plist");
  RemoveTestFile ("config.choice");
  Timeout = LoadConfigTimeout (NULL);
  CHECK (Timeout == 5);
}

STATIC
VOID
TestConfigChooserMany (
  VOID
  )
{
  OC_CONFIG_FILE  *Files;
  CHAR8           Name[OC_CONFIG_MAX_NAME + 1];
  UINTN           Index;
  UINTN           Count;
  BOOLEAN         Restarted;

  RemoveTestFile ("config.choice");
  RemoveTestFile ("config2.plist");

  for (Index = 1; Index <= OC_CONFIG_MAX_FILES + 8; ++Index) {
    AsciiSPrint (Name, sizeof (Name), "config-%02u.plist", (UINT32)Index);
    WriteConfigVariant (Name, Index, TRUE);
  }

  //
  // Only so many are listed, and the default one is among them whatever the order of the files.
  //
  Files = AllocateZeroPool (OC_CONFIG_MAX_FILES * sizeof (*Files));
  Count = OcMiscListConfigs (&mStorage, Files, OC_CONFIG_MAX_FILES);
  CHECK (Count == OC_CONFIG_MAX_FILES);
  CHECK (StrCmp (Files[0].Name, L"config.plist") == 0);
  CHECK (StrCmp (Files[1].Name, L"config-01.plist") == 0);
  CHECK (StrCmp (Files[OC_CONFIG_MAX_FILES - 1].Name, L"config-31.plist") == 0);
  FreePool (Files);

  //
  // The list scrolls to the last one, which is chosen then.
  //
  ScriptReset ();
  Press (KeyDown, MENU_CHOOSER);
  Press (KeyEnter, 1);                 // Config file
  Press (KeyEnd, 1);
  Press (KeyEnter, 1);
  Press (KeyEnter, 1);                 // This time only
  RUN_EDITOR (&Restarted);
  CHECK (Restarted);
  CHECK (SeenOnScreen ("config-30.plist"));
  CHECK (SeenOnScreen ("Start OpenCore with config-31.plist"));
  CHECK (MarkerIs ("config-31.plist\nonce\n"));
  RemoveTestFile ("config.choice");

  //
  // And back to the top, where the default one, which is in use, is.
  //
  ScriptReset ();
  Press (KeyDown, MENU_CHOOSER);
  Press (KeyEnter, 1);                 // Config file
  Press (KeyEnd, 1);
  Press (KeyHome, 1);
  Press (KeyDown, 1);
  Press (KeyEnter, 1);                 // config-01.plist
  Press (KeyEsc, 1);
  Press (KeyUp, 1);
  Press (KeyEnter, 1);                 // config.plist is in use already
  Press (KeyEnter, 1);                 // dismiss
  Press (KeyEsc, 5);
  RUN_EDITOR (&Restarted);
  CHECK (!Restarted);
  CHECK (SeenOnScreen ("Start OpenCore with config-01.plist"));
  CHECK (SeenOnScreen ("This config is in use already"));
  CHECK (!HasMarker ());

  for (Index = 1; Index <= OC_CONFIG_MAX_FILES + 8; ++Index) {
    AsciiSPrint (Name, sizeof (Name), "config-%02u.plist", (UINT32)Index);
    RemoveTestFile (Name);
  }
}

STATIC
VOID
TestConfigLimits (
  VOID
  )
{
  UINT32   Size;
  VOID     *Original;
  VOID     *New;
  UINT32   NewSize;
  BOOLEAN  Restarted;

  Original = ReadTestFile ("config.plist", &Size);

  //
  // A vault makes the configuration read-only for the editor.
  //
  mStorage.HasVault = TRUE;
  ScriptReset ();
  Press (KeyDown, 2);
  Press (KeyEnter, 1);
  Press (KeyEnter, 1);                 // dismiss
  Press (KeyEsc, 1);
  RUN_EDITOR (&Restarted);
  CHECK (SeenOnScreen ("The vault is used"));
  mStorage.HasVault = FALSE;

  //
  // A read-only file system: the old file stays and the error is shown.
  //
  mFileWriteProtected = TRUE;
  ScriptReset ();
  Enter ((CONST CHAR8 *[]) { "Kernel", "Quirks", "AppleCpuPmCfgLock" }, 3);
  Press (KeyEnter, 1);                 // switch
  Press (KeyF10, 1);                   // cannot be saved
  Press (KeyEnter, 1);                 // dismiss
  Press (KeyEsc, 3);                   // question
  Press (KeyDown, 2);
  Press (KeyEnter, 1);                 // discard
  Press (KeyEsc, 1);
  RUN_EDITOR (&Restarted);
  CHECK (!Restarted);
  CHECK (SeenOnScreen ("Cannot write config.plist"));
  mFileWriteProtected = FALSE;
  New = ReadTestFile ("config.plist", &NewSize);
  CHECK (NewSize == Size && memcmp (New, Original, Size) == 0);
  FreePool (New);
  FreePool (Original);
}

STATIC
VOID
TestBootArgsToConfig (
  VOID
  )
{
  UINT32      Size;
  VOID        *Data;
  EDIT_PLIST  Plist;
  XML_NODE    *Node;
  UINT32      Baseline;
  UINT32      Errors;

  ClearVariables ();
  PutVariable (L"boot-args", &gAppleBootVariableGuid, mNv, "-v", 2);

  ScriptReset ();
  Press (KeyEnter, 1);
  Type (" keepsyms=1");
  Press (KeyEnter, 1);
  Press (KeyDown, 2);                  // 3. Keep in NVRAM and save to config.plist
  Press (KeyEnter, 1);
  Press (KeyEsc, 1);
  RUN_EDITOR (NULL);
  CHECK (BootArgsAre ("-v keepsyms=1", 13, mNv));
  CHECK (!HasBackup ());
  CHECK (SeenOnScreen ("saved to config.plist"));

  Data = ReadTestFile ("config.plist", &Size);
  CHECK (!EFI_ERROR (EditPlistLoad (&Plist, Data, Size)));
  Node = EditPlistFindChild (EditPlistFindChild (EditPlistFindChild (Plist.Root, "NVRAM"), "Add"), "7C436110-AB2A-4BBB-A880-FE41995C9F82");
  CHECK (Node != NULL);
  CHECK (AsciiStrCmp (EditPlistGetContent (&Plist, EditPlistFindChild (Node, "boot-args")), "-v keepsyms=1") == 0);
  CHECK (!EFI_ERROR (EditPlistCountErrors (Data, Size, &Errors)));
  EditPlistFree (&Plist);
  FreePool (Data);

  //
  // The configuration without that key gets it.
  //
  {
    STATIC CONST CHAR8  Minimal[] =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<plist version=\"1.0\">\n"
      "<dict>\n"
      "\t<key>NVRAM</key>\n"
      "\t<dict>\n"
      "\t\t<key>Add</key>\n"
      "\t\t<dict>\n"
      "\t\t\t<key>7c436110-ab2a-4bbb-a880-fe41995c9f82</key>\n"
      "\t\t\t<dict>\n"
      "\t\t\t\t<key>csr-active-config</key>\n"
      "\t\t\t\t<data>AAAAAA==</data>\n"
      "\t\t\t</dict>\n"
      "\t\t</dict>\n"
      "\t</dict>\n"
      "\t<key>Misc</key>\n"
      "\t<dict>\n"
      "\t\t<key>Security</key>\n"
      "\t\t<dict>\n"
      "\t\t\t<key>AllowPickerEdit</key>\n"
      "\t\t\t<true/>\n"
      "\t\t</dict>\n"
      "\t</dict>\n"
      "</dict>\n"
      "</plist>\n";

    WriteTestFile ("config.plist", Minimal, sizeof (Minimal) - 1);
    ClearVariables ();
    ScriptReset ();
    Press (KeyEnter, 1);
    Type ("-x");
    Press (KeyEnter, 1);
    Press (KeyDown, 2);
    Press (KeyEnter, 1);
    Press (KeyEsc, 1);
    RUN_EDITOR (NULL);

    Data = ReadTestFile ("config.plist", &Size);
    CHECK (!EFI_ERROR (EditPlistLoad (&Plist, Data, Size)));
    Node = EditPlistFindChild (EditPlistFindChild (EditPlistFindChild (Plist.Root, "NVRAM"), "Add"), "7c436110-ab2a-4bbb-a880-fe41995c9f82");
    CHECK (Node != NULL);
    CHECK (AsciiStrCmp (EditPlistGetContent (&Plist, EditPlistFindChild (Node, "boot-args")), "-x") == 0);
    CHECK (EditPlistFindChild (Node, "csr-active-config") != NULL);
    CHECK (strstr ((CONST CHAR8 *)Data, "\t\t\t\t<key>boot-args</key>\n\t\t\t\t<string>-x</string>\n\t\t\t</dict>") != NULL);
    Baseline = 0;
    CHECK (!EFI_ERROR (EditPlistCountErrors (Data, Size, &Baseline)));
    EditPlistFree (&Plist);
    FreePool (Data);
    CHECK (BootArgsAre ("-x", 2, mNv));
  }

  //
  // Without NVRAM > Add nothing is changed, not even NVRAM.
  //
  {
    STATIC CONST CHAR8  NoAdd[] =
      "<plist version=\"1.0\"><dict><key>Misc</key><dict/></dict></plist>";

    WriteTestFile ("config.plist", NoAdd, sizeof (NoAdd) - 1);
    ClearVariables ();
    PutVariable (L"boot-args", &gAppleBootVariableGuid, mNv, "-v", 2);
    ScriptReset ();
    Press (KeyEnter, 1);
    Type ("-x");
    Press (KeyEnter, 1);
    Press (KeyDown, 2);
    Press (KeyEnter, 1);
    Press (KeyEnter, 1);                 // dismiss the failure
    Press (KeyEsc, 8);                   // back to editing, clear, cancel, leave
    RUN_EDITOR (NULL);
    CHECK (SeenOnScreen ("NVRAM > Add is not found"));
    CHECK (BootArgsAre ("-v", 2, mNv));
  }
}

//
// ---------------------------------------------------------------------------------
// Kexts.
// ---------------------------------------------------------------------------------
//

typedef struct {
  CONST CHAR8    *Bundle;
  CONST CHAR8    *Executable;
  CONST CHAR8    *Id;
  CONST CHAR8    *Libs[3];
} KEXT_FILES;

#define LILU  "as.vit9696.Lilu"
#define PS2   "as.acidanthera.voodoo.driver.PS2Controller"
#define INPUT "me.kishorprins.VoodooInput"

//
// The kexts of Docs/Sample.plist, and what each of them needs.
//
STATIC CONST KEXT_FILES  mSampleKexts[] = {
  { "Lilu.kext",                         "Contents/MacOS/Lilu",                 LILU,                                  { NULL } },
  { "VirtualSMC.kext",                   "Contents/MacOS/VirtualSMC",           "as.vit9696.VirtualSMC",               { LILU, NULL } },
  { "WhateverGreen.kext",                "Contents/MacOS/WhateverGreen",        "as.vit9696.WhateverGreen",            { LILU, NULL } },
  { "AppleALC.kext",                     "Contents/MacOS/AppleALC",             "as.vit9696.AppleALC",                 { LILU, NULL } },
  { "IntelMausi.kext",                   "Contents/MacOS/IntelMausi",           "com.intel.driver.IntelMausi",         { NULL } },
  { "Legacy_USB3.kext",                  NULL,                                  "com.example.Legacy_USB3",             { NULL } },
  { "AppleMCEReporterDisabler.kext",     NULL,                                  "ru.joedm.AppleMCEReporterDisabler",   { NULL } },
  { "VoodooPS2Controller.kext",          "Contents/MacOS/VoodooPS2Controller",  PS2,                                   { NULL } },
  { "VoodooPS2Controller.kext/Contents/PlugIns/VoodooPS2Keyboard.kext", "Contents/MacOS/VoodooPS2Keyboard", "as.acidanthera.voodoo.driver.PS2Keyboard", { PS2, NULL } },
  { "VoodooPS2Controller.kext/Contents/PlugIns/VoodooPS2Mouse.kext",   "Contents/MacOS/VoodooPS2Mouse",    "as.acidanthera.voodoo.driver.PS2Mouse",    { PS2, NULL } },
  { "VoodooPS2Controller.kext/Contents/PlugIns/VoodooPS2Trackpad.kext", "Contents/MacOS/VoodooPS2Trackpad", "as.acidanthera.voodoo.driver.PS2Trackpad", { PS2, INPUT, NULL } },
  { "VoodooPS2Controller.kext/Contents/PlugIns/VoodooInput.kext",      "Contents/MacOS/VoodooInput",       INPUT,                                      { NULL } },
  { "AirportBrcmFixup.kext",             "Contents/MacOS/AirportBrcmFixup",     "as.vit9696.AirportBrcmFixup",         { LILU, NULL } },
  { "AirportBrcmFixup.kext/Contents/PlugIns/AirPortBrcm4360_Injector.kext", NULL, "as.vit9696.AirPortBrcm4360_Injector", { "as.vit9696.AirportBrcmFixup", NULL } },
  { "AirportBrcmFixup.kext/Contents/PlugIns/AirPortBrcmNIC_Injector.kext",  NULL, "as.vit9696.AirPortBrcmNIC_Injector",  { "as.vit9696.AirportBrcmFixup", NULL } },
  { "IOSkywalkFamily.kext",              "Contents/MacOS/IOSkywalkFamily",      "com.apple.iokit.IOSkywalkFamily",     { NULL } },
  { "IO80211FamilyLegacy.kext",          "Contents/MacOS/IO80211FamilyLegacy",  "com.apple.iokit.IO80211FamilyLegacy", { "com.apple.iokit.IOSkywalkFamily", NULL } },
  { "IO80211FamilyLegacy.kext/Contents/PlugIns/AirPortBrcmNIC.kext", "Contents/MacOS/AirPortBrcmNIC", "com.apple.driver.AirPortBrcmNIC", { "com.apple.iokit.IO80211FamilyLegacy", NULL } }
};

STATIC
VOID
WriteKextFiles (
  IN CONST KEXT_FILES  *Kext
  )
{
  CHAR8   Text[2048];
  CHAR8   Path[512];
  UINTN   Length;
  UINT32  Index;

  Length = AsciiSPrint (
             Text,
             sizeof (Text),
             "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
             "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
             "<plist version=\"1.0\">\n<dict>\n"
             "\t<key>CFBundleIdentifier</key>\n\t<string>%a</string>\n"
             "\t<key>OSBundleLibraries</key>\n\t<dict>\n"
             "\t\t<key>com.apple.kpi.libkern</key>\n\t\t<string>8.0.0</string>\n",
             Kext->Id
             );
  for (Index = 0; Index < ARRAY_SIZE (Kext->Libs) && Kext->Libs[Index] != NULL; ++Index) {
    Length += AsciiSPrint (&Text[Length], sizeof (Text) - Length, "\t\t<key>%a</key>\n\t\t<string>1.0</string>\n", Kext->Libs[Index]);
  }

  AsciiSPrint (&Text[Length], sizeof (Text) - Length, "\t</dict>\n</dict>\n</plist>\n");

  AsciiSPrint (Path, sizeof (Path), "Kexts/%a/Contents/Info.plist", Kext->Bundle);
  WriteTestFile (Path, Text, (UINT32)AsciiStrLen (Text));

  if (Kext->Executable != NULL) {
    AsciiSPrint (Path, sizeof (Path), "Kexts/%a/%a", Kext->Bundle, Kext->Executable);
    WriteTestFile (Path, "\xCF\xFA\xED\xFE", 4);
  }
}

STATIC
VOID
WriteSampleKexts (
  VOID
  )
{
  UINT32  Index;

  for (Index = 0; Index < ARRAY_SIZE (mSampleKexts); ++Index) {
    WriteKextFiles (&mSampleKexts[Index]);
  }
}

STATIC
VOID
RemoveKexts (
  VOID
  )
{
  CHAR8  Command[600];

  AsciiSPrint (Command, sizeof (Command), "rm -rf '%a/Kexts'", mDirectory);
  if (system (Command) != 0) {
    printf ("FAIL: cannot remove the kexts of the test\n");
    ++mFailures;
  }
}

STATIC
VOID
RemoveKextFile (
  IN CONST CHAR8  *Path
  )
{
  CHAR8  Name[512];

  AsciiSPrint (Name, sizeof (Name), "Kexts/%a", Path);
  RemoveTestFile (Name);
}

/**
  Tell the kexts of a configuration file in the order of the file: "Name+" for the ones which are on,
  "Name-" off and "Name?" when it does not say.
**/
STATIC
VOID
FileKexts (
  IN  CONST CHAR8  *File,
  OUT CHAR8        *Result,
  IN  UINTN        Size
  )
{
  UINT32      FileSize;
  VOID        *Data;
  EDIT_PLIST  Plist;
  XML_NODE    *Array;
  XML_NODE    *Enabled;
  UINT32      Index;

  Result[0] = '\0';
  Data      = ReadTestFile (File, &FileSize);
  if ((Data == NULL) || EFI_ERROR (EditPlistLoad (&Plist, Data, FileSize))) {
    AsciiStrCpyS (Result, Size, "cannot load");
    return;
  }

  Array = EditPlistFindChild (EditPlistFindChild (Plist.Root, "Kernel"), "Add");
  for (Index = 0; Index < EditPlistChildCount (Array); ++Index) {
    AsciiStrCatS (Result, Size, EditPlistGetContent (&Plist, EditPlistFindChild (EditPlistChildValue (Array, Index), "BundlePath")));
    Enabled = EditPlistFindChild (EditPlistChildValue (Array, Index), "Enabled");
    AsciiStrCatS (Result, Size, Enabled == NULL ? "? " : (EditPlistGetBoolean (&Plist, Enabled) ? "+ " : "- "));
  }

  EditPlistFree (&Plist);
  FreePool (Data);
}

STATIC
BOOLEAN
OnScreenNow (
  IN CONST CHAR8  *Text
  )
{
  CHAR8  Buffer[SCREEN_ROWS * (SCREEN_COLUMNS + 1) + 1];

  ScreenText (Buffer, sizeof (Buffer));
  return strstr (Buffer, Text) != NULL;
}

/**
  Run the editor with the keys of the script, which are meant to end with the editor still waiting
  for a key, so what it shows can be looked at.
**/
STATIC
VOID
RunEditorToLook (
  VOID
  )
{
  BOOLEAN  Ended;

  Ended = RunEditor (NULL);
  CHECK (!Ended);
  CHECK (mLastColumnWrites == 0 && mLastRowWrites == 0);

  if (getenv ("PICKER_TEST_DUMP") != NULL) {
    printf ("(line %u)\n", (unsigned)mChecks);
    DumpScreen ();
  }
}

#define EXPECTED_SAMPLE_KEXTS  \
  "Lilu.kext+ VirtualSMC.kext+ WhateverGreen.kext+ AppleALC.kext+ IntelMausi.kext- Legacy_USB3.kext- " \
  "AppleMCEReporterDisabler.kext- VoodooPS2Controller.kext- " \
  "VoodooPS2Controller.kext/Contents/PlugIns/VoodooPS2Keyboard.kext- " \
  "VoodooPS2Controller.kext/Contents/PlugIns/VoodooPS2Mouse.kext- " \
  "VoodooPS2Controller.kext/Contents/PlugIns/VoodooPS2Trackpad.kext- " \
  "VoodooPS2Controller.kext/Contents/PlugIns/VoodooInput.kext- " \
  "AirportBrcmFixup.kext- " \
  "AirportBrcmFixup.kext/Contents/PlugIns/AirPortBrcm4360_Injector.kext- " \
  "AirportBrcmFixup.kext/Contents/PlugIns/AirPortBrcmNIC_Injector.kext- " \
  "IOSkywalkFamily.kext- IO80211FamilyLegacy.kext- " \
  "IO80211FamilyLegacy.kext/Contents/PlugIns/AirPortBrcmNIC.kext- "

STATIC
VOID
TestKexts (
  IN CONST VOID  *Original,
  IN UINT32      OriginalSize
  )
{
  CHAR8    Kexts[2048];
  UINT32   Size;
  VOID     *Data;
  VOID     *Backup;
  UINT32   BackupSize;
  BOOLEAN  Restarted;

  RemoveTestFile ("config.choice");
  RemoveKexts ();
  WriteTestFile ("config.plist", Original, OriginalSize);
  RemoveTestFile ("config.plist.bak");
  WriteSampleKexts ();

  //
  // The sample as it is.
  //
  FileKexts ("config.plist", Kexts, sizeof (Kexts));
  CHECK (strcmp (Kexts, EXPECTED_SAMPLE_KEXTS) == 0);

  //
  // The main menu has them, and the list shows all of them in the order they are loaded in.
  //
  ScriptReset ();
  Press (KeyEsc, 1);
  RUN_EDITOR (NULL);
  CHECK (SeenOnScreen ("Kexts            (on/off and load order)"));

  ScriptReset ();
  Press (KeyDown, MENU_KEXTS);
  Press (KeyEnter, 1);
  RunEditorToLook ();
  CHECK (OnScreenNow (" Kexts"));
  CHECK (OnScreenNow ("config.plist: 4 of 18 on"));
  CHECK (OnScreenNow ("Kexts are loaded from the top to the bottom, each needs the ones above it"));
  CHECK (!OnScreenNow ("with problems"));
  CHECK (!OnScreenNow ("unsaved"));
  CHECK (OnScreenNow (">  1 [x]  Lilu.kext   Patch engine"));
  CHECK (OnScreenNow ("   2 [x]  VirtualSMC.kext   SMC emulator"));
  CHECK (OnScreenNow ("   5 [ ]  IntelMausi.kext   Intel Ethernet LAN"));
  CHECK (OnScreenNow ("  18 [ ]  IO80211FamilyLegacy.kext/Contents/PlugIns/AirPortBrcmNIC.kext") || OnScreenNow (" 18 [ ]  IO80211FamilyLegacy.kext/Contents/PlugIns/AirPortBrcmNIC.kext"));
  CHECK (OnScreenNow (LILU));
  CHECK (OnScreenNow ("Space: on/off   +/-: move down/up   F5: fix the order   F10: save   Esc: back"));

  //
  // Switching on, and thinking better of it.
  //
  ScriptReset ();
  Press (KeyDown, MENU_KEXTS);
  Press (KeyEnter, 1);
  Press (KeyDown, 4);
  Press (KeySpace, 1);                 // IntelMausi on
  RunEditorToLook ();
  CHECK (OnScreenNow ("5 of 18 on"));
  CHECK (OnScreenNow ("1 unsaved changes"));
  CHECK (OnScreenNow (">  5 [x]* IntelMausi.kext"));

  ScriptReset ();
  Press (KeyDown, MENU_KEXTS);
  Press (KeyEnter, 1);
  Press (KeyDown, 4);
  Press (KeySpace, 2);                 // on and off again
  Press (KeyEsc, 1);                   // there is nothing to save, so this leaves
  Press (KeyEsc, 1);
  RUN_EDITOR (NULL);
  CHECK (!SeenOnScreen ("There are unsaved changes"));

  ScriptReset ();
  Press (KeyDown, MENU_KEXTS);
  Press (KeyEnter, 1);
  Press (KeyDown, 4);
  Press (KeySpace, 1);
  Press (KeyEsc, 1);                   // asks
  Press (KeyDown, 2);
  Press (KeyEnter, 1);                 // discard
  Press (KeyEsc, 1);
  RUN_EDITOR (&Restarted);
  CHECK (!Restarted);
  CHECK (SeenOnScreen ("There are unsaved changes"));
  Data = ReadTestFile ("config.plist", &Size);
  CHECK (Data != NULL && Size == OriginalSize && memcmp (Data, Original, Size) == 0);
  CHECK (ReadTestFile ("config.plist.bak", &Size) == NULL);

  //
  // Esc from the question keeps editing.
  //
  ScriptReset ();
  Press (KeyDown, MENU_KEXTS);
  Press (KeyEnter, 1);
  Press (KeyDown, 4);
  Press (KeySpace, 1);
  Press (KeyEsc, 1);
  Press (KeyEsc, 1);
  RunEditorToLook ();
  CHECK (OnScreenNow ("1 unsaved changes"));

  //
  // Switching on, and saving. Only that one value of the file is different.
  //
  ScriptReset ();
  Press (KeyDown, MENU_KEXTS);
  Press (KeyEnter, 1);
  Press (KeyDown, 4);
  Press (KeySpace, 1);
  Press (KeyF10, 1);
  Press (KeyEsc, 1);                   // do not restart
  Press (KeyEsc, 1);
  RUN_EDITOR (&Restarted);
  CHECK (!Restarted);
  CHECK (SeenOnScreen ("Saved. Restart now to apply the changes?"));
  CHECK (SeenOnScreen ("The config is saved, restart to apply the changes"));
  FileKexts ("config.plist", Kexts, sizeof (Kexts));
  CHECK (strstr (Kexts, "AppleALC.kext+ IntelMausi.kext+ Legacy_USB3.kext- ") != NULL);
  Data   = ReadTestFile ("config.plist", &Size);
  Backup = ReadTestFile ("config.plist.bak", &BackupSize);
  CHECK (Data != NULL && Size == OriginalSize - 1);
  CHECK (Backup != NULL && BackupSize == OriginalSize && memcmp (Backup, Original, OriginalSize) == 0);
  {
    UINT32  Same;

    for (Same = 0; Same < Size && ((CONST CHAR8 *)Data)[Same] == ((CONST CHAR8 *)Original)[Same]; ++Same) {
    }

    //
    // After the change the rest of the file is the same, shifted by the one byte of <true/> over <false/>.
    //
    CHECK (Same < Size && memcmp ((CONST CHAR8 *)Data + Same + 7, (CONST CHAR8 *)Original + Same + 8, Size - Same - 7) == 0);
  }

  WriteTestFile ("config.plist", Original, OriginalSize);
  RemoveTestFile ("config.plist.bak");

  //
  // Moving: down with plus, up with minus. Lilu is below VirtualSMC after the first move, which needs it.
  //
  ScriptReset ();
  Press (KeyDown, MENU_KEXTS);
  Press (KeyEnter, 1);
  Press (KeyPlus, 1);
  RunEditorToLook ();
  CHECK (OnScreenNow ("1 with problems"));
  CHECK (OnScreenNow ("1 unsaved changes"));
  CHECK (OnScreenNow ("   1 [x]! VirtualSMC.kext   SMC emulator"));
  CHECK (OnScreenNow (">  2 [x]* Lilu.kext   Patch engine"));

  ScriptReset ();
  Press (KeyDown, MENU_KEXTS);
  Press (KeyEnter, 1);
  Press (KeyPlus, 1);
  Press (KeyUp, 1);                    // VirtualSMC
  RunEditorToLook ();
  CHECK (OnScreenNow ("Loaded too early, F5 fixes it: needs as.vit9696.Lilu"));

  //
  // The order is fixed with F5, and that is the order of the file again.
  //
  ScriptReset ();
  Press (KeyDown, MENU_KEXTS);
  Press (KeyEnter, 1);
  Press (KeyPlus, 1);
  Press (KeyF5, 1);
  RunEditorToLook ();
  CHECK (SeenOnScreen ("1 with problems"));
  CHECK (OnScreenNow ("The order is fixed: every kext comes after the ones it needs"));
  CHECK (!OnScreenNow ("with problems"));
  CHECK (!OnScreenNow ("unsaved"));
  CHECK (OnScreenNow (">  1 [x]  Lilu.kext"));

  ScriptReset ();
  Press (KeyDown, MENU_KEXTS);
  Press (KeyEnter, 1);
  Press (KeyF5, 1);
  RunEditorToLook ();
  CHECK (OnScreenNow ("The order is right already"));

  //
  // The first cannot go up and the last cannot go down.
  //
  ScriptReset ();
  Press (KeyDown, MENU_KEXTS);
  Press (KeyEnter, 1);
  Press (KeyMinus, 1);
  RunEditorToLook ();
  CHECK (OnScreenNow ("This one is the first already"));
  CHECK (!OnScreenNow ("unsaved"));

  ScriptReset ();
  Press (KeyDown, MENU_KEXTS);
  Press (KeyEnter, 1);
  Press (KeyEnd, 1);
  Press (KeyPlus, 1);
  RunEditorToLook ();
  CHECK (OnScreenNow ("This one is the last already"));
  CHECK (OnScreenNow (">"));
  CHECK (!OnScreenNow ("unsaved"));

  //
  // Moving there and back is no change.
  //
  ScriptReset ();
  Press (KeyDown, MENU_KEXTS);
  Press (KeyEnter, 1);
  Press (KeyDown, 2);
  Press (KeyPlus, 3);
  Press (KeyMinus, 3);
  Press (KeyEsc, 1);
  Press (KeyEsc, 1);
  RUN_EDITOR (NULL);
  CHECK (!SeenOnScreen ("There are unsaved changes"));

  //
  // Moving a kext down by two, and saving. The file has the kexts in that order, the same text otherwise.
  //
  ScriptReset ();
  Press (KeyDown, MENU_KEXTS);
  Press (KeyEnter, 1);
  Press (KeyDown, 1);                  // VirtualSMC
  Press (KeyPlus, 2);
  Press (KeyF10, 1);
  Press (KeyEsc, 1);
  Press (KeyEsc, 1);
  RUN_EDITOR (&Restarted);
  CHECK (!Restarted);
  FileKexts ("config.plist", Kexts, sizeof (Kexts));
  CHECK (strncmp (Kexts, "Lilu.kext+ WhateverGreen.kext+ AppleALC.kext+ VirtualSMC.kext+ IntelMausi.kext- ", 80) == 0);
  Data   = ReadTestFile ("config.plist", &Size);
  Backup = ReadTestFile ("config.plist.bak", &BackupSize);
  CHECK (Data != NULL && Size == OriginalSize);
  CHECK (Backup != NULL && BackupSize == OriginalSize && memcmp (Backup, Original, OriginalSize) == 0);
  {
    UINT32  Different;
    UINT32  Index;

    Different = 0;
    for (Index = 0; Data != NULL && Index < Size; ++Index) {
      if (((CONST CHAR8 *)Data)[Index] != ((CONST CHAR8 *)Original)[Index]) {
        ++Different;
      }
    }

    CHECK (Different > 0);
  }

  //
  // What was saved is a configuration which does not have more problems.
  //
  {
    UINT32  ErrorsBefore;
    UINT32  ErrorsAfter;

    CHECK (!EFI_ERROR (EditPlistCountErrors (Original, OriginalSize, &ErrorsBefore)));
    CHECK (!EFI_ERROR (EditPlistCountErrors (Data, Size, &ErrorsAfter)));
    CHECK (ErrorsAfter == ErrorsBefore);
  }

  WriteTestFile ("config.plist", Original, OriginalSize);
  RemoveTestFile ("config.plist.bak");

  //
  // A kext that is needed, and is off.
  //
  ScriptReset ();
  Press (KeyDown, MENU_KEXTS);
  Press (KeyEnter, 1);
  Press (KeySpace, 1);                 // Lilu off
  Press (KeyDown, 1);
  RunEditorToLook ();
  CHECK (OnScreenNow ("3 with problems"));
  CHECK (OnScreenNow ("Needs a kext which is off: as.vit9696.Lilu (Lilu.kext)"));
  CHECK (OnScreenNow ("   1 [ ]* Lilu.kext") || OnScreenNow ("   1 [ ]*"));

  //
  // Plugins that need things below them: switching them on shows what is wrong, and F5 sorts it out.
  //
  ScriptReset ();
  Press (KeyDown, MENU_KEXTS);
  Press (KeyEnter, 1);
  Press (KeyDown, 10);                 // VoodooPS2Trackpad, which needs the controller and VoodooInput
  Press (KeySpace, 1);
  RunEditorToLook ();
  CHECK (OnScreenNow ("Needs a kext which is off  (+1 more): as.acidanthera.voodoo.driver.PS2"));

  ScriptReset ();
  Press (KeyDown, MENU_KEXTS);
  Press (KeyEnter, 1);
  Press (KeyDown, 7);                  // VoodooPS2Controller
  Press (KeySpace, 1);
  Press (KeyDown, 3);                  // VoodooPS2Trackpad
  Press (KeySpace, 1);
  Press (KeyDown, 1);                  // VoodooInput
  Press (KeySpace, 1);
  Press (KeyUp, 1);
  RunEditorToLook ();
  CHECK (OnScreenNow ("Loaded too early, F5 fixes it: needs " INPUT));
  CHECK (OnScreenNow ("1 with problems"));

  ScriptReset ();
  Press (KeyDown, MENU_KEXTS);
  Press (KeyEnter, 1);
  Press (KeyDown, 7);
  Press (KeySpace, 1);
  Press (KeyDown, 3);
  Press (KeySpace, 1);
  Press (KeyDown, 1);
  Press (KeySpace, 1);
  Press (KeyF5, 1);
  Press (KeyF10, 1);
  Press (KeyEsc, 1);
  Press (KeyEsc, 1);
  RUN_EDITOR (NULL);
  CHECK (SeenOnScreen ("The order is fixed: every kext comes after the ones it needs"));
  FileKexts ("config.plist", Kexts, sizeof (Kexts));
  CHECK (strstr (
           Kexts,
           "VoodooPS2Controller.kext+ "
           "VoodooPS2Controller.kext/Contents/PlugIns/VoodooPS2Keyboard.kext- "
           "VoodooPS2Controller.kext/Contents/PlugIns/VoodooPS2Mouse.kext- "
           "VoodooPS2Controller.kext/Contents/PlugIns/VoodooInput.kext+ "
           "VoodooPS2Controller.kext/Contents/PlugIns/VoodooPS2Trackpad.kext+ "
           "AirportBrcmFixup.kext- "
           ) != NULL);

  WriteTestFile ("config.plist", Original, OriginalSize);
  RemoveTestFile ("config.plist.bak");

  //
  // F5 cannot do anything about kexts that are off, and says so.
  //
  ScriptReset ();
  Press (KeyDown, MENU_KEXTS);
  Press (KeyEnter, 1);
  Press (KeyDown, 10);                 // VoodooPS2Trackpad, needs two kexts which are off
  Press (KeySpace, 1);
  Press (KeyF5, 1);
  RunEditorToLook ();
  CHECK (OnScreenNow ("The order is right. Marked kexts need kexts that are off or missing"));

  ScriptReset ();
  Press (KeyDown, MENU_KEXTS);
  Press (KeyEnter, 1);
  Press (KeyDown, 10);
  Press (KeySpace, 1);
  Press (KeyDown, 1);                  // VoodooInput, which is below
  Press (KeySpace, 1);
  Press (KeyF5, 1);
  RunEditorToLook ();
  CHECK (OnScreenNow ("The order is fixed. Marked kexts need kexts that are off or missing"));
  CHECK (OnScreenNow ("11 [x]* VoodooPS2Controller.kext/Contents/PlugIns/VoodooInput.kext"));

  //
  // Files which are not there: OpenCore does not add such kexts.
  //
  RemoveKextFile ("WhateverGreen.kext/Contents/Info.plist");
  RemoveKextFile ("AppleALC.kext/Contents/MacOS/AppleALC");
  ScriptReset ();
  Press (KeyDown, MENU_KEXTS);
  Press (KeyEnter, 1);
  Press (KeyDown, 2);
  RunEditorToLook ();
  CHECK (OnScreenNow ("2 with problems"));
  CHECK (OnScreenNow ("OpenCore will not add this kext, Info.plist is missing or broken"));
  ScriptReset ();
  Press (KeyDown, MENU_KEXTS);
  Press (KeyEnter, 1);
  Press (KeyDown, 3);
  RunEditorToLook ();
  CHECK (OnScreenNow ("OpenCore will not add this kext, the executable is missing"));

  ScriptReset ();
  Press (KeyDown, MENU_KEXTS);
  Press (KeyEnter, 1);
  Press (KeyF5, 1);
  RunEditorToLook ();
  CHECK (OnScreenNow ("The order is right. Marked kexts lack files which OpenCore needs"));

  ScriptReset ();
  Press (KeyDown, MENU_KEXTS);
  Press (KeyEnter, 1);
  Press (KeyDown, 10);
  Press (KeySpace, 1);
  Press (KeyF5, 1);
  RunEditorToLook ();
  CHECK (OnScreenNow ("The order is right. Marked kexts lack files or need kexts that are off/missing"));
  WriteSampleKexts ();

  //
  // Kexts which are broken in the file in a way that is easy to put right.
  //
  {
    CHAR8   *Text;
    CHAR8   *Start;
    CHAR8   *KeyStart;
    CHAR8   *ValueEnd;
    UINT32  TextSize;
    UINT32  ErrorsBefore;
    UINT32  ErrorsAfter;

    ErrorsBefore = 0;
    ErrorsAfter  = MAX_UINT32;
    Text = AllocatePool (OriginalSize + 1);
    CopyMem (Text, Original, OriginalSize);
    Text[OriginalSize] = '\0';
    Start    = strstr (Text, "<string>IntelMausi.kext</string>");
    KeyStart = Start != NULL ? strstr (Start, "<key>Enabled</key>") : NULL;
    ValueEnd = KeyStart != NULL ? strstr (KeyStart, "<false/>") : NULL;
    CHECK (ValueEnd != NULL);
    if (ValueEnd != NULL) {
      //
      // The key is gone with the line which it was on, and the value with its line.
      //
      while (KeyStart > Text && KeyStart[-1] != '\n') {
        --KeyStart;
      }

      ValueEnd += strlen ("<false/>");
      if (*ValueEnd == '\n') {
        ++ValueEnd;
      }

      memmove (KeyStart, ValueEnd, strlen (ValueEnd) + 1);
      TextSize = (UINT32)strlen (Text);
      WriteTestFile ("config.plist", Text, TextSize);
      FileKexts ("config.plist", Kexts, sizeof (Kexts));
      CHECK (strstr (Kexts, "AppleALC.kext+ IntelMausi.kext? Legacy_USB3.kext- ") != NULL);

      ScriptReset ();
      Press (KeyDown, MENU_KEXTS);
      Press (KeyEnter, 1);
      Press (KeyDown, 4);
      RunEditorToLook ();
      CHECK (OnScreenNow ("5 [ ]  IntelMausi.kext"));

      //
      // On, off, on.
      //
      ScriptReset ();
      Press (KeyDown, MENU_KEXTS);
      Press (KeyEnter, 1);
      Press (KeyDown, 4);
      Press (KeySpace, 2);
      RunEditorToLook ();
      CHECK (!OnScreenNow ("unsaved"));
      CHECK (OnScreenNow ("4 of 18 on"));

      ScriptReset ();
      Press (KeyDown, MENU_KEXTS);
      Press (KeyEnter, 1);
      Press (KeyDown, 4);
      Press (KeySpace, 3);
      Press (KeyF10, 1);
      Press (KeyEsc, 1);
      Press (KeyEsc, 1);
      RUN_EDITOR (NULL);
      FileKexts ("config.plist", Kexts, sizeof (Kexts));
      CHECK (strstr (Kexts, "AppleALC.kext+ IntelMausi.kext+ Legacy_USB3.kext- ") != NULL);
      Data = ReadTestFile ("config.plist", &Size);
      CHECK (Data != NULL && !EFI_ERROR (EditPlistCountErrors (Data, Size, &ErrorsAfter)));
      CHECK (!EFI_ERROR (EditPlistCountErrors (Text, TextSize, &ErrorsBefore)));
      CHECK (ErrorsAfter <= ErrorsBefore);       // a missing key is no more of a problem than it was
      Backup = ReadTestFile ("config.plist.bak", &BackupSize);
      CHECK (Backup != NULL && BackupSize == TextSize && memcmp (Backup, Text, TextSize) == 0);
    }

    FreePool (Text);
  }

  WriteTestFile ("config.plist", Original, OriginalSize);
  RemoveTestFile ("config.plist.bak");

  //
  // The configuration which is in use is the one that is worked on.
  //
  WriteTestFile ("config2.plist", Original, OriginalSize);
  CHECK (!EFI_ERROR (OcMiscSetConfigChoice (&mStorage, L"config2.plist", TRUE)));
  LoadConfigTimeout (NULL);
  CHECK (ConfigIs (L"config2.plist", OcConfigSourceKept));
  ScriptReset ();
  Press (KeyDown, MENU_KEXTS);
  Press (KeyEnter, 1);
  Press (KeyDown, 4);
  Press (KeySpace, 1);
  RunEditorToLook ();
  CHECK (OnScreenNow ("config2.plist: 5 of 18 on"));
  ScriptReset ();
  Press (KeyDown, MENU_KEXTS);
  Press (KeyEnter, 1);
  Press (KeyDown, 4);
  Press (KeySpace, 1);
  Press (KeyF10, 1);
  Press (KeyEsc, 1);
  Press (KeyEsc, 1);
  RUN_EDITOR (NULL);
  FileKexts ("config2.plist", Kexts, sizeof (Kexts));
  CHECK (strstr (Kexts, "IntelMausi.kext+ ") != NULL);
  FileKexts ("config.plist", Kexts, sizeof (Kexts));
  CHECK (strcmp (Kexts, EXPECTED_SAMPLE_KEXTS) == 0);
  Backup = ReadTestFile ("config2.plist.bak", &BackupSize);
  CHECK (Backup != NULL && BackupSize == OriginalSize && memcmp (Backup, Original, OriginalSize) == 0);
  CHECK (ReadTestFile ("config.plist.bak", &Size) == NULL);
  RemoveTestFile ("config2.plist");
  RemoveTestFile ("config2.plist.bak");
  RemoveTestFile ("config.choice");
  LoadConfigTimeout (NULL);

  //
  // Nothing is changed when the vault is used.
  //
  mStorage.HasVault = TRUE;
  ScriptReset ();
  Press (KeyDown, MENU_KEXTS);
  Press (KeyEnter, 1);
  Press (KeyEnter, 1);
  Press (KeyEsc, 1);
  RUN_EDITOR (NULL);
  CHECK (SeenOnScreen ("Kexts  (The vault is used, config.plist is protected by it)"));
  mStorage.HasVault = FALSE;

  //
  // Saving and restarting at once.
  //
  ScriptReset ();
  Press (KeyDown, MENU_KEXTS);
  Press (KeyEnter, 1);
  Press (KeyDown, 4);
  Press (KeySpace, 1);
  Press (KeyEsc, 1);
  Press (KeyEnter, 1);                 // save and restart
  RUN_EDITOR (&Restarted);
  CHECK (Restarted);
  FileKexts ("config.plist", Kexts, sizeof (Kexts));
  CHECK (strstr (Kexts, "IntelMausi.kext+ ") != NULL);
  WriteTestFile ("config.plist", Original, OriginalSize);
  RemoveTestFile ("config.plist.bak");

  //
  // A configuration without kexts, and without a place for them.
  //
  {
    CONST CHAR8  Empty[] = "<plist><dict><key>Kernel</key><dict><key>Add</key><array/></dict></dict></plist>";

    WriteTestFile ("config.plist", Empty, sizeof (Empty) - 1);
  }

  ScriptReset ();
  Press (KeyDown, MENU_KEXTS);
  Press (KeyEnter, 1);
  Press (KeySpace, 1);
  Press (KeyPlus, 1);
  Press (KeyMinus, 1);
  Press (KeyF5, 1);
  RunEditorToLook ();
  CHECK (OnScreenNow ("(there is nothing in Kernel > Add)"));
  CHECK (OnScreenNow ("0 of 0 on"));

  {
    CONST CHAR8  Nothing[] = "<plist><dict/></plist>";

    WriteTestFile ("config.plist", Nothing, sizeof (Nothing) - 1);
  }

  ScriptReset ();
  Press (KeyDown, MENU_KEXTS);
  Press (KeyEnter, 1);
  Press (KeyEnter, 1);
  Press (KeyEsc, 1);
  RUN_EDITOR (NULL);
  CHECK (SeenOnScreen ("Kernel > Add is not found in the config"));

  WriteTestFile ("config.plist", Original, OriginalSize);
  RemoveTestFile ("config.plist.bak");
  RemoveKexts ();
}

/**
  Write a configuration of a number of kexts, none of which is anywhere in the files.
**/
STATIC
VOID
WriteKextsConfig (
  IN UINT32  Many
  )
{
  CHAR8   *Text;
  UINTN   Length;
  UINTN   Capacity;
  UINT32  Index;

  Capacity = (UINTN)Many * 600 + 1024;
  Text     = AllocatePool (Capacity);
  Length   = AsciiSPrint (
               Text,
               Capacity,
               "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\">\n<dict>\n\t<key>Kernel</key>\n\t<dict>\n\t\t<key>Add</key>\n\t\t<array>\n"
               );
  for (Index = 0; Index < Many; ++Index) {
    Length += AsciiSPrint (
                &Text[Length],
                Capacity - Length,
                "\t\t\t<dict>\n"
                "\t\t\t\t<key>Arch</key>\n\t\t\t\t<string>Any</string>\n"
                "\t\t\t\t<key>BundlePath</key>\n\t\t\t\t<string>K%02u.kext</string>\n"
                "\t\t\t\t<key>Comment</key>\n\t\t\t\t<string></string>\n"
                "\t\t\t\t<key>Enabled</key>\n\t\t\t\t<true/>\n"
                "\t\t\t\t<key>ExecutablePath</key>\n\t\t\t\t<string>Contents/MacOS/K%02u</string>\n"
                "\t\t\t\t<key>MaxKernel</key>\n\t\t\t\t<string></string>\n"
                "\t\t\t\t<key>MinKernel</key>\n\t\t\t\t<string></string>\n"
                "\t\t\t\t<key>PlistPath</key>\n\t\t\t\t<string>Contents/Info.plist</string>\n"
                "\t\t\t</dict>\n",
                Index,
                Index
                );
  }

  Length += AsciiSPrint (&Text[Length], Capacity - Length, "\t\t</array>\n\t</dict>\n</dict>\n</plist>\n");

  WriteTestFile ("config.plist", Text, (UINT32)Length);
  FreePool (Text);
}

/**
  A configuration of many kexts: each one needs the next one, so they are all in the wrong order.
**/
STATIC
VOID
TestManyKexts (
  VOID
  )
{
  enum { Many = 60 };

  UINT32      Index;
  CHAR8       Kexts[Many * 20 + 64];
  CHAR8       Wanted[Many * 20 + 64];
  CHAR8       Line[64];
  KEXT_FILES  Kext;
  CHAR8       Bundle[32];
  CHAR8       Executable[64];
  CHAR8       Id[32];
  CHAR8       Next[32];
  BOOLEAN     Restarted;

  RemoveKexts ();
  WriteKextsConfig (Many);

  //
  // Every kext needs the next one: K00 needs K01 and so on.
  //
  for (Index = 0; Index < Many; ++Index) {
    AsciiSPrint (Bundle, sizeof (Bundle), "K%02u.kext", Index);
    AsciiSPrint (Executable, sizeof (Executable), "Contents/MacOS/K%02u", Index);
    AsciiSPrint (Id, sizeof (Id), "k.K%02u", Index);
    AsciiSPrint (Next, sizeof (Next), "k.K%02u", Index + 1);
    ZeroMem (&Kext, sizeof (Kext));
    Kext.Bundle     = Bundle;
    Kext.Executable = Executable;
    Kext.Id         = Id;
    Kext.Libs[0]    = Index + 1 < Many ? Next : NULL;
    WriteKextFiles (&Kext);
  }

  ScriptReset ();
  Press (KeyDown, MENU_KEXTS);
  Press (KeyEnter, 1);
  RunEditorToLook ();
  CHECK (OnScreenNow ("59 with problems"));
  CHECK (OnScreenNow ("  1 [x]! K00.kext") || OnScreenNow ("   1 [x]! K00.kext"));

  //
  // To the end of the list, which scrolls.
  //
  ScriptReset ();
  Press (KeyDown, MENU_KEXTS);
  Press (KeyEnter, 1);
  Press (KeyEnd, 1);
  RunEditorToLook ();
  CHECK (OnScreenNow ("> 60 [x]  K59.kext"));
  CHECK (!OnScreenNow ("K00.kext"));
  CHECK (OnScreenNow ("K42.kext"));

  //
  // Up with minus, there is a lot of room to go.
  //
  ScriptReset ();
  Press (KeyDown, MENU_KEXTS);
  Press (KeyEnter, 1);
  Press (KeyEnd, 1);
  Press (KeyMinus, 25);
  RunEditorToLook ();
  CHECK (OnScreenNow ("> 35 [x]* K59.kext"));
  CHECK (OnScreenNow ("K59.kext"));
  CHECK (OnScreenNow ("1 unsaved changes"));

  //
  // F5: all of them go the other way round, and the one selected is still in sight.
  //
  ScriptReset ();
  Press (KeyDown, MENU_KEXTS);
  Press (KeyEnter, 1);
  Press (KeyF5, 1);
  RunEditorToLook ();
  CHECK (OnScreenNow ("The order is fixed: every kext comes after the ones it needs"));
  CHECK (!OnScreenNow ("with problems"));
  CHECK (OnScreenNow ("> 60 [x]* K00.kext"));
  CHECK (!OnScreenNow ("K59.kext"));

  ScriptReset ();
  Press (KeyDown, MENU_KEXTS);
  Press (KeyEnter, 1);
  Press (KeyF5, 1);
  Press (KeyF10, 1);
  Press (KeyEsc, 1);
  Press (KeyEsc, 1);
  RUN_EDITOR (&Restarted);
  FileKexts ("config.plist", Kexts, sizeof (Kexts));
  Wanted[0] = '\0';
  for (Index = 0; Index < Many; ++Index) {
    AsciiSPrint (Line, sizeof (Line), "K%02u.kext+ ", Many - 1 - Index);
    AsciiStrCatS (Wanted, sizeof (Wanted), Line);
  }

  CHECK (strcmp (Kexts, Wanted) == 0);

  //
  // Once it is right, the editor has nothing to say about it.
  //
  ScriptReset ();
  Press (KeyDown, MENU_KEXTS);
  Press (KeyEnter, 1);
  RunEditorToLook ();
  CHECK (!OnScreenNow ("with problems"));
  CHECK (OnScreenNow ("> 1 [x]  K59.kext") || OnScreenNow (">  1 [x]  K59.kext"));

  //
  // Two kexts which need each other: whatever the order, one of them waits.
  //
  RemoveKexts ();
  WriteKextsConfig (2);
  ZeroMem (&Kext, sizeof (Kext));
  Kext.Bundle     = "K00.kext";
  Kext.Executable = "Contents/MacOS/K00";
  Kext.Id         = "k.K00";
  Kext.Libs[0]    = "k.K01";
  WriteKextFiles (&Kext);
  Kext.Bundle     = "K01.kext";
  Kext.Executable = "Contents/MacOS/K01";
  Kext.Id         = "k.K01";
  Kext.Libs[0]    = "k.K00";
  WriteKextFiles (&Kext);

  ScriptReset ();
  Press (KeyDown, MENU_KEXTS);
  Press (KeyEnter, 1);
  Press (KeyF5, 1);
  RunEditorToLook ();
  CHECK (OnScreenNow ("1 with problems"));
  CHECK (OnScreenNow ("Some kexts need each other, their order cannot be fixed"));
  RemoveKexts ();

  //
  // Too many to work with: said at once.
  //
  WriteKextsConfig (1024 + 1);
  ScriptReset ();
  Press (KeyDown, MENU_KEXTS);
  Press (KeyEnter, 1);
  Press (KeyEnter, 1);
  Press (KeyEsc, 1);
  RUN_EDITOR (NULL);
  CHECK (SeenOnScreen ("Too many kexts to work with"));

  RemoveKexts ();
  RemoveTestFile ("config.plist.bak");
}

/**
  Show how the screens look, for a look by a human.
**/
STATIC
VOID
ShowScreens (
  VOID
  )
{
  CONST CHAR8  *Quirks[] = { "Kernel", "Quirks", "AppleCpuPmCfgLock" };

  ClearVariables ();
  PutVariable (L"boot-args", &gAppleBootVariableGuid, mNv, "keepsyms=1 debug=0x100 -v", 25);
  mShowScreens = TRUE;

  ScriptReset ();
  Press (KeyDown, 1);
  Press (KeyEnter, 1);                 // Boot flags
  Press (KeySpace, 1);
  Press (KeyDown, 5);
  Press (KeySpace, 1);
  Press (KeyDown, 1);
  Press (KeySpace, 1);
  Press (KeyEnter, 1);                 // Apply
  Press (KeyEsc, 1);
  Press (KeyEsc, 1);
  Press (KeyEsc, 1);
  RunEditor (NULL);

  ScriptReset ();
  Press (KeyEnter, 1);
  Type (" npci=0x2000");
  Press (KeyEnter, 1);
  Press (KeyEsc, 3);
  RunEditor (NULL);

  ScriptReset ();
  Enter (Quirks, ARRAY_SIZE (Quirks));
  Press (KeyEnter, 1);
  Press (KeyEsc, 3);
  Press (KeyEsc, 1);
  Press (KeyEsc, 1);
  Press (KeyDown, 2);
  Press (KeyEnter, 1);
  Press (KeyEsc, 1);
  RunEditor (NULL);

  RemoveTestFile ("config.choice");
  WriteConfigVariant ("config2.plist", 77, TRUE);
  WriteConfigVariant ("config3.plist", 33, FALSE);
  ScriptReset ();
  Press (KeyDown, MENU_CHOOSER);
  Press (KeyEnter, 1);                 // Config file
  Press (KeyDown, 2);
  Press (KeyEnter, 1);                 // config3.plist
  Press (KeyEsc, 1);
  Press (KeyUp, 1);
  Press (KeyEnter, 1);                 // config2.plist
  Press (KeyEnter, 1);                 // This time only
  RunEditor (NULL);

  mShowScreens = FALSE;
}


STATIC
VOID
TestOtherLists (
  IN CONST VOID  *Original,
  IN UINT32      OriginalSize
  )
{
  UINT32  Size;
  VOID    *Data;
  CHAR8   *Text;

  RemoveKexts ();
  WriteTestFile ("config.plist", Original, OriginalSize);
  RemoveTestFile ("config.plist.bak");

  //
  // The menu of the lists, and drivers: the files are not there, so what is on is marked.
  //
  ScriptReset ();
  Press (KeyDown, MENU_LISTS);
  Press (KeyEnter, 1);
  RunEditorToLook ();
  CHECK (OnScreenNow ("Drivers"));
  CHECK (OnScreenNow ("ACPI files"));
  CHECK (OnScreenNow ("ACPI patches"));
  CHECK (OnScreenNow ("Tools"));
  CHECK (OnScreenNow ("Entries"));

  ScriptReset ();
  Press (KeyDown, MENU_LISTS);
  Press (KeyEnter, 1);
  Press (KeyEnter, 1);                 // Drivers
  RunEditorToLook ();
  CHECK (OnScreenNow ("config.plist: 2 of 51 on, 2 with problems"));
  CHECK (OnScreenNow ("Drivers are loaded from the top to the bottom"));
  CHECK (OnScreenNow ("OpenRuntime.efi"));
  CHECK (OnScreenNow ("F10: save"));
  CHECK (!OnScreenNow ("F5"));

  //
  // With the files there, nothing is wrong. Move OpenRuntime up, switch another on, and save.
  //
  WriteTestFile ("Drivers/OpenRuntime.efi", "x", 1);
  WriteTestFile ("Drivers/HfsPlus.efi", "x", 1);
  WriteTestFile ("Drivers/OpenCanopy.efi", "x", 1);
  ScriptReset ();
  Press (KeyDown, MENU_LISTS);
  Press (KeyEnter, 1);
  Press (KeyEnter, 1);
  Press (KeyDown, 1);                  // OpenRuntime.efi
  Press (KeyMinus, 1);
  Press (KeyDown, 10);
  Press (KeySpace, 1);                 // OpenCanopy.efi on
  Press (KeyF5, 1);                    // does nothing here
  RunEditorToLook ();
  CHECK (OnScreenNow ("3 of 51 on"));
  CHECK (OnScreenNow ("2 unsaved changes"));

  ScriptReset ();
  Press (KeyDown, MENU_LISTS);
  Press (KeyEnter, 1);
  Press (KeyEnter, 1);
  Press (KeyDown, 1);
  Press (KeyMinus, 1);
  Press (KeyDown, 10);
  Press (KeySpace, 1);
  Press (KeyF10, 1);
  Press (KeyEsc, 1);
  Press (KeyEsc, 2);
  RUN_EDITOR (NULL);
  Data = ReadTestFile ("config.plist", &Size);
  CHECK (Data != NULL && Size == OriginalSize - 1);
  {
    EDIT_PLIST  Plist;
    XML_NODE    *Array;

    CHECK (!EFI_ERROR (EditPlistLoad (&Plist, Data, Size)));
    Array = EditPlistFindChild (EditPlistFindChild (Plist.Root, "UEFI"), "Drivers");
    CHECK (AsciiStrCmp (EditPlistGetContent (&Plist, EditPlistFindChild (EditPlistChildValue (Array, 0), "Path")), "OpenRuntime.efi") == 0);
    CHECK (AsciiStrCmp (EditPlistGetContent (&Plist, EditPlistFindChild (EditPlistChildValue (Array, 1), "Path")), "OpenVariableRuntimeDxe.efi") == 0);
    CHECK (EditPlistGetBoolean (&Plist, EditPlistFindChild (EditPlistChildValue (Array, 10), "Enabled")));
    EditPlistFree (&Plist);
  }

  WriteTestFile ("config.plist", Original, OriginalSize);
  RemoveTestFile ("config.plist.bak");
  RemoveKexts ();
  {
    CHAR8  Command[600];

    AsciiSPrint (Command, sizeof (Command), "rm -rf '%a/Drivers'", mDirectory);
    CHECK (system (Command) == 0);
  }

  //
  // ACPI patches have no files, tools and entries are lists as well.
  //
  ScriptReset ();
  Press (KeyDown, MENU_LISTS);
  Press (KeyEnter, 1);
  Press (KeyDown, 2);
  Press (KeyEnter, 1);                 // ACPI patches
  RunEditorToLook ();
  CHECK (OnScreenNow ("ACPI > Patch") || OnScreenNow ("Patches of ACPI tables"));
  CHECK (!OnScreenNow ("with problems"));

  //
  // Quirks: switch two, save, nothing else changes.
  //
  ScriptReset ();
  Press (KeyDown, MENU_QUIRKS);
  Press (KeyEnter, 1);
  RunEditorToLook ();
  CHECK (OnScreenNow ("Quirks and switches"));
  CHECK (OnScreenNow ("Booter > Quirks > AvoidRuntimeDefrag"));

  ScriptReset ();
  Press (KeyDown, MENU_QUIRKS);
  Press (KeyEnter, 1);
  Press (KeySpace, 1);
  Press (KeyDown, 1);
  Press (KeySpace, 1);
  Press (KeyF10, 1);
  Press (KeyEsc, 1);
  Press (KeyEsc, 1);
  RUN_EDITOR (NULL);
  Text = ReadTestFile ("config.plist", &Size);
  CHECK (Text != NULL && (Size != OriginalSize || memcmp (Text, Original, Size) != 0));
  {
    UINT32  Errors1;
    UINT32  Errors2;

    CHECK (!EFI_ERROR (EditPlistCountErrors (Original, OriginalSize, &Errors1)));
    CHECK (!EFI_ERROR (EditPlistCountErrors (Text, Size, &Errors2)));
    CHECK (Errors1 == Errors2);
  }

  //
  // Esc with changes asks, and discarding leaves the file as it is.
  //
  WriteTestFile ("config.plist", Original, OriginalSize);
  RemoveTestFile ("config.plist.bak");
  ScriptReset ();
  Press (KeyDown, MENU_QUIRKS);
  Press (KeyEnter, 1);
  Press (KeySpace, 1);
  Press (KeyEsc, 1);
  Press (KeyDown, 2);
  Press (KeyEnter, 1);
  Press (KeyEsc, 1);
  RUN_EDITOR (NULL);
  CHECK (SeenOnScreen ("There are unsaved changes"));
  Data = ReadTestFile ("config.plist", &Size);
  CHECK (Data != NULL && Size == OriginalSize && memcmp (Data, Original, Size) == 0);

  mStorage.HasVault = TRUE;
  ScriptReset ();
  Press (KeyDown, MENU_QUIRKS);
  Press (KeyEnter, 1);
  Press (KeyEnter, 1);
  Press (KeyEsc, 1);
  RUN_EDITOR (NULL);
  CHECK (SeenOnScreen ("The vault is used"));
  mStorage.HasVault = FALSE;
}


STATIC
VOID
TestCheckAndLog (
  IN CONST VOID  *Original,
  IN UINT32      OriginalSize
  )
{
  CHAR8   Log[2048];
  UINT32  Index;
  UINTN   Length;

  WriteTestFile ("config.plist", Original, OriginalSize);
  RemoveKexts ();

  //
  // No log, and a log that is read from its end.
  //
  mFakeLog = NULL;
  ScriptReset ();
  Press (KeyDown, MENU_LOG);
  Press (KeyEnter, 1);
  Press (KeyEnter, 1);
  Press (KeyEsc, 1);
  RUN_EDITOR (NULL);
  CHECK (SeenOnScreen ("There is no log"));

  Length = 0;
  for (Index = 1; Index <= 60; ++Index) {
    Length += AsciiSPrint (&Log[Length], sizeof (Log) - Length, "OC: line number %u of the log\n", Index);
  }

  mFakeLog = Log;
  ScriptReset ();
  Press (KeyDown, MENU_LOG);
  Press (KeyEnter, 1);
  RunEditorToLook ();
  CHECK (OnScreenNow ("OpenCore log"));
  CHECK (OnScreenNow ("line 60 of 60"));
  CHECK (OnScreenNow ("line number 60 of the log"));
  CHECK (!OnScreenNow ("line number 2 of the log"));

  ScriptReset ();
  Press (KeyDown, MENU_LOG);
  Press (KeyEnter, 1);
  Press (KeyHome, 1);
  RunEditorToLook ();
  CHECK (OnScreenNow ("line number 1 of the log"));
  CHECK (!OnScreenNow ("line number 60 of the log"));
  mFakeLog = NULL;

  //
  // The check: kexts and drivers have no files here, then they have.
  //
  ScriptReset ();
  Press (KeyDown, MENU_CHECK);
  Press (KeyEnter, 1);
  RunEditorToLook ();
  CHECK (OnScreenNow ("Structure: no problems found"));
  CHECK (OnScreenNow ("problems found in config.plist"));
  CHECK (OnScreenNow ("! Kexts: Lilu.kext"));
  CHECK (OnScreenNow ("! Drivers: OpenRuntime.efi"));

  WriteSampleKexts ();
  WriteTestFile ("Drivers/OpenRuntime.efi", "x", 1);
  WriteTestFile ("Drivers/HfsPlus.efi", "x", 1);
  ScriptReset ();
  Press (KeyDown, MENU_CHECK);
  Press (KeyEnter, 1);
  RunEditorToLook ();
  CHECK (OnScreenNow ("No problems found in config.plist"));
  CHECK (!OnScreenNow ("! Kexts"));

  //
  // A kext in the wrong order is told about.
  //
  RemoveKextFile ("Lilu.kext/Contents/Info.plist");
  ScriptReset ();
  Press (KeyDown, MENU_CHECK);
  Press (KeyEnter, 1);
  RunEditorToLook ();
  CHECK (OnScreenNow ("! Kexts: Lilu.kext"));
  CHECK (OnScreenNow ("Info.plist is missing or broken"));

  RemoveKexts ();
  {
    CHAR8  Command[600];

    AsciiSPrint (Command, sizeof (Command), "rm -rf '%a/Drivers'", mDirectory);
    CHECK (system (Command) == 0);
  }
}


STATIC
VOID
TestCompareAndUndo (
  IN CONST VOID  *Original,
  IN UINT32      OriginalSize
  )
{
  UINT32      Size;
  VOID        *Data;
  VOID        *Backup;
  UINT32      BackupSize;
  EDIT_PLIST  Plist;
  XML_NODE    *Array;

  RemoveKexts ();
  RemoveTestFile ("config2.plist");
  WriteTestFile ("config.plist", Original, OriginalSize);
  RemoveTestFile ("config.plist.bak");

  //
  // Nothing to compare with.
  //
  ScriptReset ();
  Press (KeyDown, MENU_COMPARE);
  Press (KeyEnter, 1);
  Press (KeyEnter, 1);
  Press (KeyEsc, 1);
  RUN_EDITOR (NULL);
  CHECK (SeenOnScreen ("There is no other config to compare with"));

  //
  // The same file: no differences. A variant: the timeout and a kext differ.
  //
  WriteTestFile ("config2.plist", Original, OriginalSize);
  ScriptReset ();
  Press (KeyDown, MENU_COMPARE);
  Press (KeyEnter, 1);
  Press (KeyEnter, 1);
  RunEditorToLook ();
  CHECK (OnScreenNow ("config.plist  ->  config2.plist"));
  CHECK (OnScreenNow ("No differences"));

  WriteConfigVariant ("config2.plist", 77, TRUE);
  CHECK (!EFI_ERROR (EditPlistLoad (&Plist, Original, OriginalSize)));
  Array = EditPlistFindChild (EditPlistFindChild (Plist.Root, "Kernel"), "Add");
  CHECK (Array != NULL);
  EditPlistFree (&Plist);

  ScriptReset ();
  Press (KeyDown, MENU_COMPARE);
  Press (KeyEnter, 1);
  Press (KeyEnter, 1);
  RunEditorToLook ();
  CHECK (OnScreenNow ("~ /Misc/Boot/Timeout: 5  ->  77"));
  CHECK (!OnScreenNow ("No differences"));
  CHECK (OnScreenNow ("- only in the first"));

  //
  // Undo: nothing to put back at first, then a saved change can be taken back.
  //
  ScriptReset ();
  Press (KeyDown, MENU_UNDO);
  Press (KeyEnter, 1);
  Press (KeyEnter, 1);
  Press (KeyEsc, 1);
  RUN_EDITOR (NULL);
  CHECK (SeenOnScreen ("There is no config.plist.bak"));

  ScriptReset ();
  Press (KeyDown, MENU_KEXTS);
  Press (KeyEnter, 1);
  Press (KeyDown, 4);
  Press (KeySpace, 1);
  Press (KeyF10, 1);
  Press (KeyEsc, 3);
  RUN_EDITOR (NULL);
  Data = ReadTestFile ("config.plist", &Size);
  CHECK (Data != NULL && Size == OriginalSize - 1);

  ScriptReset ();
  Press (KeyDown, MENU_UNDO);
  Press (KeyEnter, 1);
  Press (KeyEsc, 1);                   // no
  Press (KeyEsc, 1);
  RUN_EDITOR (NULL);
  Data = ReadTestFile ("config.plist", &Size);
  CHECK (Data != NULL && Size == OriginalSize - 1);

  ScriptReset ();
  Press (KeyDown, MENU_UNDO);
  Press (KeyEnter, 1);
  Press (KeyEnter, 1);                 // yes
  Press (KeyEsc, 1);
  RUN_EDITOR (NULL);
  CHECK (SeenOnScreen ("The previous config is back, restart to apply it"));
  Data   = ReadTestFile ("config.plist", &Size);
  Backup = ReadTestFile ("config.plist.bak", &BackupSize);
  CHECK (Data != NULL && Size == OriginalSize && memcmp (Data, Original, Size) == 0);
  CHECK (Backup != NULL && BackupSize == OriginalSize - 1);

  RemoveTestFile ("config2.plist");
  WriteTestFile ("config.plist", Original, OriginalSize);
  RemoveTestFile ("config.plist.bak");
}


STATIC
VOID
TestPanicLog (
  VOID
  )
{
  mFakePanic = NULL;
  ScriptReset ();
  Press (KeyDown, MENU_PANIC);
  Press (KeyEnter, 1);
  Press (KeyEnter, 1);
  Press (KeyEsc, 1);
  RUN_EDITOR (NULL);
  CHECK (SeenOnScreen ("No panic log in NVRAM"));

  mFakePanic = "panic(cpu 0 caller 0xffffff80): Kernel trap\nBacktrace (CPU 0):\n0xfffffe: frame one\n0xfffffe: frame two\n";
  ScriptReset ();
  Press (KeyDown, MENU_PANIC);
  Press (KeyEnter, 1);
  RunEditorToLook ();
  CHECK (OnScreenNow ("Kernel panic log"));
  CHECK (OnScreenNow ("panic(cpu 0 caller 0xffffff80): Kernel trap"));
  CHECK (OnScreenNow ("frame two"));
  mFakePanic = NULL;
}

/**
  Show the kexts screen of the configuration in the directory of PICKER_TEST_DIR, as it is and after F5.
  Nothing is saved.
**/
STATIC
VOID
ShowKexts (
  VOID
  )
{
  ScriptReset ();
  Press (KeyDown, MENU_KEXTS);
  Press (KeyEnter, 1);
  RunEditor (NULL);
  printf ("===== the kexts of the configuration =====\n");
  DumpScreen ();

  ScriptReset ();
  Press (KeyDown, MENU_KEXTS);
  Press (KeyEnter, 1);
  Press (KeyF5, 1);
  RunEditor (NULL);
  printf ("===== after F5 =====\n");
  DumpScreen ();

  ScriptReset ();
  Press (KeyDown, MENU_KEXTS);
  Press (KeyEnter, 1);
  Press (KeyEnd, 1);
  RunEditor (NULL);
  printf ("===== the end of the list =====\n");
  DumpScreen ();

  ScriptReset ();
  Press (KeyDown, MENU_KEXTS);
  Press (KeyEnter, 1);
  Press (KeyPlus, 4);
  Press (KeyDown, 1);
  RunEditor (NULL);
  printf ("===== Lilu moved down by four =====\n");
  DumpScreen ();

  ScriptReset ();
  Press (KeyDown, MENU_KEXTS);
  Press (KeyEnter, 1);
  Press (KeyPlus, 4);
  Press (KeyF5, 1);
  RunEditor (NULL);
  printf ("===== and F5 =====\n");
  DumpScreen ();
}

int
ENTRY_POINT (
  int   argc,
  char  *argv[]
  )
{
  UINT32  Size;
  VOID    *Original;

  if (argc < 2) {
    printf ("usage: PickerEditor Sample.plist\n");
    return 2;
  }

  InitEnvironment (argv[1]);
  mPrivilegeResult = EFI_SUCCESS;

  if (getenv ("PICKER_TEST_SHOW") != NULL) {
    ShowScreens ();
    return 0;
  }

  if (getenv ("PICKER_TEST_KEXTS") != NULL) {
    ShowKexts ();
    return 0;
  }

  TestBootArgsVariables ();
  TestEscapeOnly ();
  TestPrivilege ();
  TestBootArgsEditor ();
  TestBootFlags ();
  TestConfigEditor ();

  //
  // Start over from the sample for the rest.
  //
  Original = UserReadFile (argv[1], &Size);
  WriteTestFile ("config.plist", Original, Size);
  free (Original);
  TestConfigLimits ();
  TestBootArgsToConfig ();

  Original = UserReadFile (argv[1], &Size);
  WriteTestFile ("config.plist", Original, Size);
  TestConfigChoiceLibrary ();
  TestConfigChooser ();
  TestConfigChooserMany ();
  TestKexts (Original, Size);
  TestManyKexts ();
  TestOtherLists (Original, Size);
  TestCheckAndLog (Original, Size);
  TestCompareAndUndo (Original, Size);
  TestPanicLog ();
  free (Original);

  printf ("%u checks, %u failures\n", mChecks, mFailures);

  return mFailures == 0 ? 0 : 1;
}

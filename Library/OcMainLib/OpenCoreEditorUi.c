/** @file
  Text mode user interface helpers of the OpenCore picker options editor.

  Copyright (C) 2026, OpenCore contributors. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-3-Clause
**/

#include "OpenCoreEditorUi.h"

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>

/**
  Rows which are not content: title with its subtitle and a gap below them,
  status and hints (the very last row is not used to never scroll the screen).
**/
#define ED_UI_ROWS_ABOVE  3U
#define ED_UI_ROWS_BELOW  3U

EFI_STATUS
EdUiInit (
  OUT ED_UI              *Ui,
  IN  OC_PICKER_CONTEXT  *Picker
  )
{
  EFI_STATUS  Status;
  UINTN       Columns;
  UINTN       Rows;
  UINTN       Foreground;
  UINTN       Background;

  ZeroMem (Ui, sizeof (*Ui));

  if ((gST->ConOut == NULL) || (Picker->HotKeyContext == NULL)) {
    return EFI_NOT_READY;
  }

  Status = gST->ConOut->QueryMode (gST->ConOut, gST->ConOut->Mode->Mode, &Columns, &Rows);
  if (EFI_ERROR (Status) || (Columns < ED_UI_MIN_COLUMNS) || (Rows < ED_UI_MIN_ROWS)) {
    DEBUG ((DEBUG_INFO, "OCE: Console is not suitable %ux%u - %r\n", (UINT32)Columns, (UINT32)Rows, Status));
    return EFI_UNSUPPORTED;
  }

  Ui->Scratch = AllocatePool ((Columns + 1) * sizeof (CHAR16));
  if (Ui->Scratch == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Ui->Picker             = Picker;
  Ui->Columns            = Columns;
  Ui->Rows               = Rows;
  Ui->SavedMode          = OcConsoleControlSetMode (EfiConsoleControlScreenText);
  Ui->SavedAttribute     = gST->ConOut->Mode->Attribute;
  Ui->SavedCursorVisible = gST->ConOut->Mode->CursorVisible;

  //
  // Follow the console colours which the builtin picker uses.
  //
  if (Picker->ConsoleAttributes != 0) {
    Ui->AttrNormal = Picker->ConsoleAttributes & 0x7FU;
  } else {
    Ui->AttrNormal = EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK);
  }

  Foreground = Ui->AttrNormal & 0x0FU;
  Background = (Ui->AttrNormal >> 4) & 0x07U;

  Ui->AttrSelected = EFI_TEXT_ATTR (Background, Foreground & 0x07U);
  if (Ui->AttrSelected == Ui->AttrNormal) {
    Ui->AttrSelected = EFI_TEXT_ATTR (EFI_BLACK, EFI_LIGHTGRAY);
  }

  Ui->AttrTitle   = EFI_TEXT_ATTR (EFI_WHITE, EFI_BLUE);
  Ui->AttrChanged = EFI_TEXT_ATTR (EFI_YELLOW, Background);
  Ui->AttrError   = EFI_TEXT_ATTR (EFI_LIGHTRED, Background);
  Ui->AttrHint    = EFI_TEXT_ATTR (Background == EFI_BLACK ? EFI_DARKGRAY : EFI_BLACK, Background);

  Ui->Key.OcKeyCode   = OC_INPUT_NO_ACTION;
  Ui->Key.OcModifiers = OC_MODIFIERS_NONE;
  Ui->Key.UnicodeChar = CHAR_NULL;

  gST->ConOut->EnableCursor (gST->ConOut, FALSE);

  //
  // Do not let the key which opened the editor be handled by it.
  //
  Picker->HotKeyContext->FlushTypingBuffer (Picker);

  EdUiClear (Ui);

  return EFI_SUCCESS;
}

VOID
EdUiDone (
  IN OUT ED_UI  *Ui
  )
{
  gST->ConOut->SetAttribute (gST->ConOut, (UINTN)Ui->SavedAttribute);
  gST->ConOut->ClearScreen (gST->ConOut);
  gST->ConOut->EnableCursor (gST->ConOut, Ui->SavedCursorVisible);
  OcConsoleControlSetMode (Ui->SavedMode);

  Ui->Picker->HotKeyContext->FlushTypingBuffer (Ui->Picker);

  if (Ui->Scratch != NULL) {
    FreePool (Ui->Scratch);
    Ui->Scratch = NULL;
  }
}

VOID
EdUiClear (
  IN ED_UI  *Ui
  )
{
  gST->ConOut->SetAttribute (gST->ConOut, Ui->AttrNormal);
  gST->ConOut->ClearScreen (gST->ConOut);
}

VOID
EdUiPut (
  IN ED_UI        *Ui,
  IN UINTN        Col,
  IN UINTN        Row,
  IN UINTN        Attr,
  IN CONST CHAR8  *Text,
  IN UINTN        Width
  )
{
  UINTN    MaxWidth;
  UINTN    Index;
  BOOLEAN  AtEnd;

  //
  // The last column is never written to as that may wrap to the next row.
  //
  if ((Row >= Ui->Rows) || (Col + 1 >= Ui->Columns)) {
    return;
  }

  MaxWidth = Ui->Columns - 1 - Col;
  if ((Width == 0) || (Width > MaxWidth)) {
    Width = MaxWidth;
  }

  AtEnd = FALSE;
  for (Index = 0; Index < Width; ++Index) {
    if (!AtEnd && (Text[Index] == '\0')) {
      AtEnd = TRUE;
    }

    if (AtEnd) {
      Ui->Scratch[Index] = L' ';
    } else if ((Text[Index] >= 0x20) && (Text[Index] <= 0x7E)) {
      Ui->Scratch[Index] = (CHAR16)Text[Index];
    } else {
      Ui->Scratch[Index] = L'?';
    }
  }

  Ui->Scratch[Width] = L'\0';

  gST->ConOut->SetAttribute (gST->ConOut, Attr);
  gST->ConOut->SetCursorPosition (gST->ConOut, Col, Row);
  gST->ConOut->OutputString (gST->ConOut, Ui->Scratch);
}

VOID
EdUiTitle (
  IN ED_UI        *Ui,
  IN CONST CHAR8  *Title,
  IN CONST CHAR8  *Subtitle  OPTIONAL
  )
{
  CHAR8  Line[128];

  AsciiSPrint (Line, sizeof (Line), " %a", Title);
  EdUiPut (Ui, 0, 0, Ui->AttrTitle, Line, 0);
  EdUiPut (Ui, 0, 1, Ui->AttrNormal, Subtitle != NULL ? Subtitle : "", 0);
  EdUiPut (Ui, 0, 2, Ui->AttrNormal, "", 0);
}

VOID
EdUiHint (
  IN ED_UI        *Ui,
  IN CONST CHAR8  *Hint
  )
{
  EdUiPut (Ui, 0, Ui->Rows - 2, Ui->AttrHint, Hint, 0);
}

VOID
EdUiStatus (
  IN ED_UI        *Ui,
  IN UINTN        Attr,
  IN CONST CHAR8  *Text
  )
{
  EdUiPut (Ui, 0, Ui->Rows - 3, Attr, Text, 0);
}

UINTN
EdUiContentRows (
  IN ED_UI  *Ui
  )
{
  return Ui->Rows - ED_UI_ROWS_ABOVE - ED_UI_ROWS_BELOW - 1;
}

STATIC
OC_PICKER_KEY_INFO *
EdUiWaitKeyFiltered (
  IN OUT ED_UI             *Ui,
  IN     OC_PICKER_KEY_MAP  Filter
  )
{
  while (TRUE) {
    //
    // Hotkeys of the picker are not wanted here, so they are not enabled.
    //
    Ui->Picker->HotKeyContext->WaitForKeyInfo (
                                 Ui->Picker,
                                 0,
                                 Filter,
                                 &Ui->Key
                                 );

    if ((Ui->Key.OcKeyCode != OC_INPUT_NO_ACTION) || (Ui->Key.UnicodeChar != CHAR_NULL)) {
      return &Ui->Key;
    }
  }
}

OC_PICKER_KEY_INFO *
EdUiWaitKey (
  IN OUT ED_UI  *Ui,
  IN     BOOLEAN  Typing
  )
{
  return EdUiWaitKeyFiltered (Ui, Typing ? OC_PICKER_KEYS_TYPING : 0);
}

OC_PICKER_KEY_INFO *
EdUiWaitKeyWithPlusMinus (
  IN OUT ED_UI  *Ui
  )
{
  return EdUiWaitKeyFiltered (Ui, OC_PICKER_KEYS_PLUS_MINUS);
}

VOID
EdUiNotify (
  IN OUT ED_UI        *Ui,
  IN     CONST CHAR8  *Message,
  IN     BOOLEAN      IsError
  )
{
  EdUiStatus (Ui, IsError ? Ui->AttrError : Ui->AttrChanged, Message);
  EdUiHint (Ui, "Press any key to continue");
  EdUiWaitKey (Ui, FALSE);
  EdUiStatus (Ui, Ui->AttrNormal, "");
}

BOOLEAN
EdUiConfirm (
  IN OUT ED_UI        *Ui,
  IN     CONST CHAR8  *Question
  )
{
  OC_PICKER_KEY_INFO  *Key;
  BOOLEAN             Result;

  EdUiStatus (Ui, Ui->AttrChanged, Question);
  EdUiHint (Ui, "Enter: yes   Esc: no");

  Result = FALSE;
  while (TRUE) {
    Key = EdUiWaitKey (Ui, FALSE);
    if (Key->OcKeyCode == OC_INPUT_CONTINUE) {
      Result = TRUE;
      break;
    }

    if (Key->OcKeyCode == OC_INPUT_ABORTED) {
      break;
    }
  }

  EdUiStatus (Ui, Ui->AttrNormal, "");
  return Result;
}

VOID
EdListDraw (
  IN ED_UI         *Ui,
  IN UINTN         FirstRow,
  IN UINTN         Visible,
  IN CONST ED_LIST *List,
  IN ED_LIST_ITEM  Item,
  IN VOID          *Context
  )
{
  UINTN    Offset;
  UINTN    Index;
  UINTN    Attr;
  BOOLEAN  Changed;
  BOOLEAN  Dim;
  BOOLEAN  Error;
  CHAR8    Text[256];
  CHAR8    Line[260];

  for (Offset = 0; Offset < Visible; ++Offset) {
    Index = List->Top + Offset;
    if (Index >= List->Count) {
      EdUiPut (Ui, 0, FirstRow + Offset, Ui->AttrNormal, "", 0);
      continue;
    }

    Changed = FALSE;
    Dim     = FALSE;
    Error   = FALSE;
    ZeroMem (Text, sizeof (Text));
    Item (Context, Index, Text, sizeof (Text), &Changed, &Dim, &Error);

    if (Index == List->Selected) {
      Attr = Ui->AttrSelected;
    } else if (Error) {
      Attr = Ui->AttrError;
    } else if (Changed) {
      Attr = Ui->AttrChanged;
    } else if (Dim) {
      Attr = Ui->AttrHint;
    } else {
      Attr = Ui->AttrNormal;
    }

    //
    // The selection is marked with a character too, as colours may not be available.
    //
    AsciiSPrint (Line, sizeof (Line), "%a%a", Index == List->Selected ? "> " : "  ", Text);
    EdUiPut (Ui, 0, FirstRow + Offset, Attr, Line, 0);
  }
}

BOOLEAN
EdListMove (
  IN OUT ED_LIST     *List,
  IN     UINTN       Visible,
  IN     OC_KEY_CODE KeyCode
  )
{
  if (List->Count == 0) {
    return FALSE;
  }

  switch (KeyCode) {
    case OC_INPUT_UP:
      List->Selected = List->Selected == 0 ? List->Count - 1 : List->Selected - 1;
      break;
    case OC_INPUT_DOWN:
      List->Selected = List->Selected + 1 >= List->Count ? 0 : List->Selected + 1;
      break;
    case OC_INPUT_TOP:
      List->Selected = 0;
      break;
    case OC_INPUT_BOTTOM:
      List->Selected = List->Count - 1;
      break;
    default:
      return FALSE;
  }

  if (List->Selected < List->Top) {
    List->Top = List->Selected;
  } else if (List->Selected >= List->Top + Visible) {
    List->Top = List->Selected - Visible + 1;
  }

  return TRUE;
}

INTN
EdUiMenu (
  IN OUT ED_UI           *Ui,
  IN     UINTN           FirstRow,
  IN     CONST CHAR8     **Items,
  IN     CONST BOOLEAN   *Enabled  OPTIONAL,
  IN     UINTN           Count,
  IN OUT UINTN           *Selected
  )
{
  OC_PICKER_KEY_INFO  *Key;
  UINTN               Index;
  UINTN               Attr;
  CHAR8               Line[256];

  while (TRUE) {
    for (Index = 0; Index < Count; ++Index) {
      if (Index == *Selected) {
        Attr = Ui->AttrSelected;
      } else if ((Enabled != NULL) && !Enabled[Index]) {
        Attr = Ui->AttrHint;
      } else {
        Attr = Ui->AttrNormal;
      }

      AsciiSPrint (Line, sizeof (Line), "%a%u. %a", Index == *Selected ? "> " : "  ", (UINT32)(Index + 1), Items[Index]);
      EdUiPut (Ui, 0, FirstRow + Index, Attr, Line, 0);
    }

    Key = EdUiWaitKey (Ui, FALSE);

    switch (Key->OcKeyCode) {
      case OC_INPUT_UP:
        *Selected = *Selected == 0 ? Count - 1 : *Selected - 1;
        break;
      case OC_INPUT_DOWN:
        *Selected = *Selected + 1 >= Count ? 0 : *Selected + 1;
        break;
      case OC_INPUT_TOP:
        *Selected = 0;
        break;
      case OC_INPUT_BOTTOM:
        *Selected = Count - 1;
        break;
      case OC_INPUT_ABORTED:
        return -1;
      case OC_INPUT_CONTINUE:
        if ((Enabled == NULL) || Enabled[*Selected]) {
          return (INTN)*Selected;
        }

        break;
      default:
        //
        // Digits choose the item directly.
        //
        if ((Key->OcKeyCode >= 0) && ((UINTN)Key->OcKeyCode < Count) && ((UINTN)Key->OcKeyCode < 9)) {
          *Selected = (UINTN)Key->OcKeyCode;
          if ((Enabled == NULL) || Enabled[*Selected]) {
            return (INTN)*Selected;
          }
        }

        break;
    }
  }
}

/**
  Draw the edited text and its cursor.

  @param[in]      Ui          Console state.
  @param[in]      FirstRow    First row of the text.
  @param[in]      MaxRows     Rows which can be used.
  @param[in]      Width       Characters per row.
  @param[in]      Buffer      Text.
  @param[in]      Length      Length of Text.
  @param[in]      Cursor      Cursor position, up to Length.
  @param[in,out]  PrevRows    Rows which were used last time, to clear the rest.
**/
STATIC
VOID
EdDrawField (
  IN     ED_UI        *Ui,
  IN     UINTN        FirstRow,
  IN     UINTN        MaxRows,
  IN     UINTN        Width,
  IN     CONST CHAR8  *Buffer,
  IN     UINTN        Length,
  IN     UINTN        Cursor,
  IN OUT UINTN        *PrevRows
  )
{
  UINTN   Rows;
  UINTN   Row;
  UINTN   Col;
  UINTN   Index;
  UINTN   CursorRow;
  UINTN   CursorCol;
  CHAR16  Saved;

  Rows = Length / Width + 1;
  if (Rows > MaxRows) {
    Rows = MaxRows;
  }

  CursorRow = Cursor / Width;
  CursorCol = Cursor % Width;

  for (Row = 0; Row < MAX (Rows, *PrevRows); ++Row) {
    for (Col = 0; Col < Width; ++Col) {
      Index = Row * Width + Col;
      if (Index < Length) {
        Ui->Scratch[Col] = (Buffer[Index] >= 0x20 && Buffer[Index] <= 0x7E) ? (CHAR16)Buffer[Index] : L'?';
      } else {
        Ui->Scratch[Col] = L' ';
      }
    }

    Ui->Scratch[Width] = L'\0';

    if (Row == CursorRow) {
      //
      // The part before the cursor, the cursor itself, the part after it.
      //
      gST->ConOut->SetAttribute (gST->ConOut, Ui->AttrNormal);
      gST->ConOut->SetCursorPosition (gST->ConOut, 0, FirstRow + Row);
      Saved                  = Ui->Scratch[CursorCol];
      Ui->Scratch[CursorCol] = L'\0';
      if (CursorCol > 0) {
        gST->ConOut->OutputString (gST->ConOut, Ui->Scratch);
      }

      Ui->Scratch[CursorCol]     = Saved;
      Saved                      = Ui->Scratch[CursorCol + 1];
      Ui->Scratch[CursorCol + 1] = L'\0';
      gST->ConOut->SetAttribute (gST->ConOut, Ui->AttrSelected);
      gST->ConOut->OutputString (gST->ConOut, &Ui->Scratch[CursorCol]);
      Ui->Scratch[CursorCol + 1] = Saved;

      gST->ConOut->SetAttribute (gST->ConOut, Ui->AttrNormal);
      if (CursorCol + 1 < Width) {
        gST->ConOut->OutputString (gST->ConOut, &Ui->Scratch[CursorCol + 1]);
      }
    } else {
      gST->ConOut->SetAttribute (gST->ConOut, Ui->AttrNormal);
      gST->ConOut->SetCursorPosition (gST->ConOut, 0, FirstRow + Row);
      gST->ConOut->OutputString (gST->ConOut, Ui->Scratch);
    }
  }

  *PrevRows = Rows;
}

ED_LINE_RESULT
EdUiEditLine (
  IN OUT ED_UI           *Ui,
  IN     UINTN           FirstRow,
  IN     UINTN           MaxRows,
  IN OUT CHAR8           *Buffer,
  IN     UINTN           BufferSize,
  IN     ED_CHAR_FILTER  Filter  OPTIONAL
  )
{
  OC_PICKER_KEY_INFO  *Key;
  UINTN               Width;
  UINTN               Length;
  UINTN               MaxLength;
  UINTN               Cursor;
  UINTN               PrevRows;
  CHAR8               Character;

  ASSERT (BufferSize > 1);
  ASSERT (MaxRows > 0);

  //
  // One character less than the row width is used to never touch the last column.
  // There is always room for the cursor after the last character.
  //
  Width     = Ui->Columns - 1;
  MaxLength = MIN (BufferSize - 1, Width * MaxRows - 1);
  Length    = AsciiStrLen (Buffer);
  if (Length > MaxLength) {
    Length         = MaxLength;
    Buffer[Length] = '\0';
  }

  Cursor   = Length;
  PrevRows = 0;

  while (TRUE) {
    EdDrawField (Ui, FirstRow, MaxRows, Width, Buffer, Length, Cursor, &PrevRows);

    Key = EdUiWaitKey (Ui, TRUE);

    switch (Key->OcKeyCode) {
      case OC_INPUT_TYPING_CONFIRM:
        return EdLineAccepted;
      case OC_INPUT_TYPING_CLEAR_ALL:
        if (Length == 0) {
          return EdLineCancelled;
        }

        Buffer[0] = '\0';
        Length    = 0;
        Cursor    = 0;
        break;
      case OC_INPUT_TYPING_LEFT:
        if (Cursor > 0) {
          --Cursor;
        }

        break;
      case OC_INPUT_TYPING_RIGHT:
        if (Cursor < Length) {
          ++Cursor;
        }

        break;
      case OC_INPUT_TYPING_HOME:
        Cursor = 0;
        break;
      case OC_INPUT_TYPING_END:
        Cursor = Length;
        break;
      case OC_INPUT_TYPING_BACKSPACE:
        if (Cursor > 0) {
          CopyMem (&Buffer[Cursor - 1], &Buffer[Cursor], Length - Cursor + 1);
          --Cursor;
          --Length;
        }

        break;
      case OC_INPUT_TYPING_DELETE:
        if (Cursor < Length) {
          CopyMem (&Buffer[Cursor], &Buffer[Cursor + 1], Length - Cursor);
          --Length;
        }

        break;
      default:
        if (  (Key->OcKeyCode == OC_INPUT_NO_ACTION)
           && (Key->UnicodeChar >= 0x20)
           && (Key->UnicodeChar <= 0x7E)
           && (Length < MaxLength))
        {
          Character = (CHAR8)Key->UnicodeChar;
          if ((Filter == NULL) || Filter (Character)) {
            CopyMem (&Buffer[Cursor + 1], &Buffer[Cursor], Length - Cursor + 1);
            Buffer[Cursor] = Character;
            ++Cursor;
            ++Length;
          }
        }

        break;
    }
  }
}

/** @file
  Text mode user interface helpers of the OpenCore picker options editor.

  Copyright (C) 2026, OpenCore contributors. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-3-Clause
**/

#ifndef OPEN_CORE_EDITOR_UI_H
#define OPEN_CORE_EDITOR_UI_H

#include <Uefi.h>

#include <Protocol/SimpleTextOut.h>

#include <Library/OcBootManagementLib.h>
#include <Library/OcConsoleLib.h>

/**
  Smallest console the editor can work with.
**/
#define ED_UI_MIN_COLUMNS  40U
#define ED_UI_MIN_ROWS     12U

/**
  Console state of the editor.
**/
typedef struct {
  OC_PICKER_CONTEXT                  *Picker;
  UINTN                              Columns;
  UINTN                              Rows;
  UINTN                              AttrNormal;
  UINTN                              AttrSelected;
  UINTN                              AttrTitle;
  UINTN                              AttrChanged;
  UINTN                              AttrError;
  UINTN                              AttrHint;
  //
  // Keyboard state, which carries the modifiers to detect their changes.
  //
  OC_PICKER_KEY_INFO                 Key;
  //
  // One row of text.
  //
  CHAR16                             *Scratch;
  //
  // State to restore on leaving.
  //
  EFI_CONSOLE_CONTROL_SCREEN_MODE    SavedMode;
  INT32                              SavedAttribute;
  BOOLEAN                            SavedCursorVisible;
} ED_UI;

/**
  Scrollable list state.
**/
typedef struct {
  UINTN    Count;
  UINTN    Selected;
  UINTN    Top;
} ED_LIST;

/**
  Get the text of a list item.

  @param[in]   Context  Caller context.
  @param[in]   Index    Item index.
  @param[out]  Text     Item text, to be filled.
  @param[in]   Size     Size of Text.
  @param[out]  Changed  Whether to show the item as changed.
  @param[out]  Dim      Whether to show the item as unavailable.
  @param[out]  Error    Whether to show the item as having a problem.
**/
typedef
VOID
(*ED_LIST_ITEM)(
  IN  VOID     *Context,
  IN  UINTN    Index,
  OUT CHAR8    *Text,
  IN  UINTN    Size,
  OUT BOOLEAN  *Changed,
  OUT BOOLEAN  *Dim,
  OUT BOOLEAN  *Error
  );

/**
  Check if a character may be typed.
**/
typedef
BOOLEAN
(*ED_CHAR_FILTER)(
  IN CHAR8  Character
  );

typedef enum {
  EdLineAccepted,
  EdLineCancelled
} ED_LINE_RESULT;

/**
  Take over the console. Needs to be matched with EdUiDone.

  @retval EFI_SUCCESS      The console is ready.
  @retval EFI_UNSUPPORTED  The console is too small or cannot be used.
**/
EFI_STATUS
EdUiInit (
  OUT ED_UI              *Ui,
  IN  OC_PICKER_CONTEXT  *Picker
  );

/**
  Give the console back.
**/
VOID
EdUiDone (
  IN OUT ED_UI  *Ui
  );

/**
  Clear the screen.
**/
VOID
EdUiClear (
  IN ED_UI  *Ui
  );

/**
  Print ASCII text, clipped to the screen and padded with spaces up to Width.

  @param[in]  Ui     Console state.
  @param[in]  Col    First column.
  @param[in]  Row    Row.
  @param[in]  Attr   Text attribute.
  @param[in]  Text   Text. Characters which cannot be printed are shown as ?.
  @param[in]  Width  Width to fill, 0 for the rest of the row.
**/
VOID
EdUiPut (
  IN ED_UI        *Ui,
  IN UINTN        Col,
  IN UINTN        Row,
  IN UINTN        Attr,
  IN CONST CHAR8  *Text,
  IN UINTN        Width
  );

/**
  Show the title (first row) and a line below it.
**/
VOID
EdUiTitle (
  IN ED_UI        *Ui,
  IN CONST CHAR8  *Title,
  IN CONST CHAR8  *Subtitle  OPTIONAL
  );

/**
  Show key hints on the last usable row.
**/
VOID
EdUiHint (
  IN ED_UI        *Ui,
  IN CONST CHAR8  *Hint
  );

/**
  Show a message above the hints, attribute selects the importance.
**/
VOID
EdUiStatus (
  IN ED_UI        *Ui,
  IN UINTN        Attr,
  IN CONST CHAR8  *Text
  );

/**
  Get the number of rows which are free for content below the title and above the status.
**/
UINTN
EdUiContentRows (
  IN ED_UI  *Ui
  );

/**
  Wait for a key.

  @param[in,out]  Ui      Console state.
  @param[in]      Typing  TRUE to receive typed characters, FALSE for navigation keys.

  @return Pointer to the key info in Ui.
**/
OC_PICKER_KEY_INFO *
EdUiWaitKey (
  IN OUT ED_UI  *Ui,
  IN     BOOLEAN  Typing
  );

/**
  Wait for a navigation key, or the plus and minus keys.

  @param[in,out]  Ui  Console state.

  @return Pointer to the key info in Ui, with OC_INPUT_PLUS and OC_INPUT_MINUS possible.
**/
OC_PICKER_KEY_INFO *
EdUiWaitKeyWithPlusMinus (
  IN OUT ED_UI  *Ui
  );

/**
  Show a message and wait for a key.
**/
VOID
EdUiNotify (
  IN OUT ED_UI        *Ui,
  IN     CONST CHAR8  *Message,
  IN     BOOLEAN      IsError
  );

/**
  Ask a question and wait for the answer.

  @retval TRUE   Enter was pressed.
  @retval FALSE  Esc was pressed.
**/
BOOLEAN
EdUiConfirm (
  IN OUT ED_UI        *Ui,
  IN     CONST CHAR8  *Question
  );

/**
  Draw a list.

  @param[in]  Ui        Console state.
  @param[in]  FirstRow  Row of the first visible item.
  @param[in]  Visible   Number of rows to use.
  @param[in]  List      List state.
  @param[in]  Item      Item text callback.
  @param[in]  Context   Callback context.
**/
VOID
EdListDraw (
  IN ED_UI         *Ui,
  IN UINTN         FirstRow,
  IN UINTN         Visible,
  IN CONST ED_LIST *List,
  IN ED_LIST_ITEM  Item,
  IN VOID          *Context
  );

/**
  Move the selection by a navigation key, scrolling when needed.

  @retval TRUE   The key was a navigation key and the list has to be drawn again.
**/
BOOLEAN
EdListMove (
  IN OUT ED_LIST     *List,
  IN     UINTN       Visible,
  IN     OC_KEY_CODE KeyCode
  );

/**
  Show a short menu and let the user choose.

  @param[in,out]  Ui        Console state.
  @param[in]      FirstRow  Row of the first item.
  @param[in]      Items     Item texts.
  @param[in]      Enabled   Which items can be chosen, NULL if all of them.
  @param[in]      Count     Number of items.
  @param[in,out]  Selected  Selected item.

  @return Chosen item or -1 if Esc was pressed.
**/
INTN
EdUiMenu (
  IN OUT ED_UI           *Ui,
  IN     UINTN           FirstRow,
  IN     CONST CHAR8     **Items,
  IN     CONST BOOLEAN   *Enabled  OPTIONAL,
  IN     UINTN           Count,
  IN OUT UINTN           *Selected
  );

/**
  Edit a line of text.

  Left, Right, Home, End, Delete and Backspace work as expected, Esc clears the line
  or leaves it as it is cancelled when it is empty, Enter accepts.

  @param[in,out]  Ui          Console state.
  @param[in]      FirstRow    First row of the edited text, which wraps over several.
  @param[in]      MaxRows     Rows which can be used.
  @param[in,out]  Buffer      Text to edit, ASCII.
  @param[in]      BufferSize  Size of Buffer, including the terminator.
  @param[in]      Filter      Accepts typed characters, optional.
**/
ED_LINE_RESULT
EdUiEditLine (
  IN OUT ED_UI           *Ui,
  IN     UINTN           FirstRow,
  IN     UINTN           MaxRows,
  IN OUT CHAR8           *Buffer,
  IN     UINTN           BufferSize,
  IN     ED_CHAR_FILTER  Filter  OPTIONAL
  );

#endif // OPEN_CORE_EDITOR_UI_H

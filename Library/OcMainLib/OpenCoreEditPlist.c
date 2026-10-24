/** @file
  Text-preserving editing of OpenCore config.plist for the picker options editor.

  Edits are kept as a list of replacements of the original text. Applying them
  changes only the edited elements, so formatting, comments and the order of keys
  of a file which was created by hand or with a plist editor stay exactly as they were.

  Copyright (C) 2026, OpenCore contributors. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-3-Clause
**/

#include "OpenCoreEditorInternal.h"

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/OcConfigurationLib.h>
#include <Library/OcStringLib.h>
#include <Library/PrintLib.h>

/**
  Growing text buffer.
**/
typedef struct {
  CHAR8      *Data;
  UINTN      Length;
  UINTN      Capacity;
  BOOLEAN    Failed;
} EDIT_BUILDER;

/**
  Longest indentation of a line which is repeated for added entries, and room for
  one more level of it.
**/
#define EDIT_MAX_INDENT        32U
#define EDIT_MAX_COPY_INDENT   (EDIT_MAX_INDENT - 4U)

STATIC
VOID
EditBuilderInit (
  OUT EDIT_BUILDER  *Builder,
  IN  UINTN         Capacity
  )
{
  Builder->Length   = 0;
  Builder->Capacity = Capacity + 1;
  Builder->Failed   = FALSE;
  Builder->Data     = AllocatePool (Builder->Capacity);
  if (Builder->Data == NULL) {
    Builder->Failed = TRUE;
  }
}

STATIC
VOID
EditBuilderAppend (
  IN OUT EDIT_BUILDER  *Builder,
  IN     CONST CHAR8   *Text,
  IN     UINTN         Length
  )
{
  CHAR8  *NewData;
  UINTN  NewCapacity;

  if (Builder->Failed || (Length == 0)) {
    return;
  }

  if (Builder->Length + Length + 1 > Builder->Capacity) {
    NewCapacity = (Builder->Length + Length + 1) * 2;
    NewData     = AllocatePool (NewCapacity);
    if (NewData == NULL) {
      Builder->Failed = TRUE;
      return;
    }

    CopyMem (NewData, Builder->Data, Builder->Length);
    FreePool (Builder->Data);
    Builder->Data     = NewData;
    Builder->Capacity = NewCapacity;
  }

  CopyMem (&Builder->Data[Builder->Length], Text, Length);
  Builder->Length += Length;
}

STATIC
VOID
EditBuilderAppendStr (
  IN OUT EDIT_BUILDER  *Builder,
  IN     CONST CHAR8   *Text
  )
{
  EditBuilderAppend (Builder, Text, AsciiStrLen (Text));
}

/**
  Finish the buffer, which transfers it to the caller.

  @return NUL terminated text or NULL on failure.
**/
STATIC
CHAR8 *
EditBuilderFinish (
  IN OUT EDIT_BUILDER  *Builder,
  OUT    UINTN         *Length  OPTIONAL
  )
{
  CHAR8  *Result;

  if (Builder->Failed) {
    if (Builder->Data != NULL) {
      FreePool (Builder->Data);
      Builder->Data = NULL;
    }

    return NULL;
  }

  Builder->Data[Builder->Length] = '\0';
  Result                         = Builder->Data;
  if (Length != NULL) {
    *Length = Builder->Length;
  }

  Builder->Data = NULL;
  return Result;
}

STATIC
VOID
EditBuilderFree (
  IN OUT EDIT_BUILDER  *Builder
  )
{
  if (Builder->Data != NULL) {
    FreePool (Builder->Data);
    Builder->Data = NULL;
  }
}

EFI_STATUS
EditPlistLoad (
  OUT EDIT_PLIST  *Plist,
  IN  CONST VOID  *Data,
  IN  UINT32      Size
  )
{
  ZeroMem (Plist, sizeof (*Plist));

  if ((Data == NULL) || (Size == 0) || (Size == MAX_UINT32)) {
    return EFI_INVALID_PARAMETER;
  }

  Plist->Source = AllocatePool ((UINTN)Size + 1);
  Plist->Work   = AllocatePool ((UINTN)Size + 1);
  if ((Plist->Source == NULL) || (Plist->Work == NULL)) {
    EditPlistFree (Plist);
    return EFI_OUT_OF_RESOURCES;
  }

  CopyMem (Plist->Source, Data, Size);
  CopyMem (Plist->Work, Data, Size);
  Plist->Source[Size] = '\0';
  Plist->Work[Size]   = '\0';
  Plist->SourceSize   = Size;

  //
  // The parser takes over Work and modifies it, thus Source is the only intact copy.
  //
  Plist->Document = XmlDocumentParse (Plist->Work, Size, FALSE);
  if (Plist->Document == NULL) {
    EditPlistFree (Plist);
    return EFI_INVALID_PARAMETER;
  }

  Plist->Root = PlistNodeCast (PlistDocumentRoot (Plist->Document), PLIST_NODE_TYPE_DICT);
  if (Plist->Root == NULL) {
    EditPlistFree (Plist);
    return EFI_INVALID_PARAMETER;
  }

  return EFI_SUCCESS;
}

VOID
EditPlistFree (
  IN OUT EDIT_PLIST  *Plist
  )
{
  UINT32  Index;

  for (Index = 0; Index < Plist->PatchCount; ++Index) {
    if (Plist->Patches[Index].Text != NULL) {
      FreePool (Plist->Patches[Index].Text);
    }
  }

  if (Plist->Patches != NULL) {
    FreePool (Plist->Patches);
  }

  if (Plist->Reorder.Order != NULL) {
    FreePool (Plist->Reorder.Order);
  }

  if (Plist->Document != NULL) {
    XmlDocumentFree (Plist->Document);
  }

  if (Plist->Work != NULL) {
    FreePool (Plist->Work);
  }

  if (Plist->Source != NULL) {
    FreePool (Plist->Source);
  }

  ZeroMem (Plist, sizeof (*Plist));
}

EDIT_PLIST_KIND
EditPlistNodeKind (
  IN  XML_NODE  *Node
  )
{
  CONST CHAR8  *Name;

  if (Node == NULL) {
    return EditPlistKindOther;
  }

  Name = XmlNodeName (Node);

  if (AsciiStrCmp (Name, "dict") == 0) {
    return EditPlistKindDict;
  }

  if (AsciiStrCmp (Name, "array") == 0) {
    return EditPlistKindArray;
  }

  if ((AsciiStrCmp (Name, "true") == 0) || (AsciiStrCmp (Name, "false") == 0)) {
    return EditPlistKindBoolean;
  }

  if (AsciiStrCmp (Name, "integer") == 0) {
    return EditPlistKindInteger;
  }

  if (AsciiStrCmp (Name, "string") == 0) {
    return EditPlistKindString;
  }

  if (AsciiStrCmp (Name, "data") == 0) {
    return EditPlistKindData;
  }

  return EditPlistKindOther;
}

UINT32
EditPlistChildCount (
  IN  XML_NODE  *Node
  )
{
  switch (EditPlistNodeKind (Node)) {
    case EditPlistKindDict:
      //
      // A dangling key (malformed dictionary) is ignored.
      //
      return XmlNodeChildren (Node) / 2;
    case EditPlistKindArray:
      return XmlNodeChildren (Node);
    default:
      return 0;
  }
}

XML_NODE *
EditPlistChildValue (
  IN  XML_NODE  *Node,
  IN  UINT32    Index
  )
{
  if (Index >= EditPlistChildCount (Node)) {
    return NULL;
  }

  if (EditPlistNodeKind (Node) == EditPlistKindDict) {
    return XmlNodeChild (Node, Index * 2 + 1);
  }

  return XmlNodeChild (Node, Index);
}

CONST CHAR8 *
EditPlistChildKey (
  IN  XML_NODE  *Node,
  IN  UINT32    Index
  )
{
  CONST CHAR8  *Key;

  if ((EditPlistNodeKind (Node) != EditPlistKindDict) || (Index >= EditPlistChildCount (Node))) {
    return NULL;
  }

  Key = XmlNodeContent (XmlNodeChild (Node, Index * 2));

  return Key != NULL ? Key : "";
}

XML_NODE *
EditPlistFindChild (
  IN  XML_NODE     *Dict,
  IN  CONST CHAR8  *Key
  )
{
  UINT32  Index;
  UINT32  Count;

  Count = EditPlistChildCount (Dict);
  if (EditPlistNodeKind (Dict) != EditPlistKindDict) {
    return NULL;
  }

  for (Index = 0; Index < Count; ++Index) {
    if (AsciiStrCmp (EditPlistChildKey (Dict, Index), Key) == 0) {
      return EditPlistChildValue (Dict, Index);
    }
  }

  return NULL;
}

STATIC
UINT32
EditFindPatch (
  IN  CONST EDIT_PLIST  *Plist,
  IN  CONST XML_NODE    *Node
  )
{
  UINT32  Index;

  for (Index = 0; Index < Plist->PatchCount; ++Index) {
    if ((Plist->Patches[Index].Node == Node) && (Node != NULL)) {
      return Index;
    }
  }

  return MAX_UINT32;
}

/**
  Append an empty patch.

  @return The patch or NULL when out of memory.
**/
STATIC
EDIT_PLIST_PATCH *
EditAllocPatch (
  IN OUT EDIT_PLIST  *Plist
  )
{
  EDIT_PLIST_PATCH  *NewPatches;
  UINT32            NewCapacity;

  if (Plist->PatchCount == Plist->PatchCapacity) {
    NewCapacity = Plist->PatchCapacity == 0 ? 8 : Plist->PatchCapacity * 2;
    NewPatches  = AllocateZeroPool (NewCapacity * sizeof (*NewPatches));
    if (NewPatches == NULL) {
      return NULL;
    }

    if (Plist->Patches != NULL) {
      CopyMem (NewPatches, Plist->Patches, Plist->PatchCount * sizeof (*NewPatches));
      FreePool (Plist->Patches);
    }

    Plist->Patches       = NewPatches;
    Plist->PatchCapacity = NewCapacity;
  }

  ZeroMem (&Plist->Patches[Plist->PatchCount], sizeof (Plist->Patches[0]));
  return &Plist->Patches[Plist->PatchCount++];
}

STATIC
VOID
EditRemovePatch (
  IN OUT EDIT_PLIST  *Plist,
  IN     UINT32      Index
  )
{
  if (Plist->Patches[Index].Text != NULL) {
    FreePool (Plist->Patches[Index].Text);
  }

  --Plist->PatchCount;
  if (Index < Plist->PatchCount) {
    CopyMem (
      &Plist->Patches[Index],
      &Plist->Patches[Index + 1],
      (Plist->PatchCount - Index) * sizeof (Plist->Patches[0])
      );
  }
}

/**
  Get a patch of a scalar node for modification, creating it when needed.
  Text of an already existing patch is released.

  @retval EFI_UNSUPPORTED  Node cannot be replaced as a whole.
**/
STATIC
EFI_STATUS
EditGetNodePatch (
  IN OUT EDIT_PLIST        *Plist,
  IN     XML_NODE          *Node,
  IN     EDIT_PLIST_KIND   Kind,
  OUT    EDIT_PLIST_PATCH  **Patch
  )
{
  UINT32  Index;
  UINT32  Start;
  UINT32  End;

  Index = EditFindPatch (Plist, Node);
  if (Index != MAX_UINT32) {
    *Patch = &Plist->Patches[Index];
    if ((*Patch)->Text != NULL) {
      FreePool ((*Patch)->Text);
      (*Patch)->Text = NULL;
    }

    (*Patch)->Kind = Kind;
    return EFI_SUCCESS;
  }

  if (!XmlNodeSourceRange (Node, &Start, &End) || (End > Plist->SourceSize)) {
    return EFI_UNSUPPORTED;
  }

  *Patch = EditAllocPatch (Plist);
  if (*Patch == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  (*Patch)->Node  = Node;
  (*Patch)->Kind  = Kind;
  (*Patch)->Start = Start;
  (*Patch)->End   = End;

  return EFI_SUCCESS;
}

UINT32
EditPlistPatchCount (
  IN  CONST EDIT_PLIST  *Plist
  )
{
  return Plist->PatchCount + (Plist->Reorder.Array != NULL ? 1 : 0);
}

BOOLEAN
EditPlistIsPatched (
  IN  CONST EDIT_PLIST  *Plist,
  IN  CONST XML_NODE    *Node
  )
{
  return EditFindPatch (Plist, Node) != MAX_UINT32;
}

BOOLEAN
EditPlistGetBoolean (
  IN  CONST EDIT_PLIST  *Plist,
  IN  XML_NODE          *Node
  )
{
  UINT32  Index;

  Index = EditFindPatch (Plist, Node);
  if ((Index != MAX_UINT32) && (Plist->Patches[Index].Kind == EditPlistKindBoolean)) {
    return Plist->Patches[Index].Boolean;
  }

  return AsciiStrCmp (XmlNodeName (Node), "true") == 0;
}

VOID
EditPlistGetInteger (
  IN  CONST EDIT_PLIST  *Plist,
  IN  XML_NODE          *Node,
  OUT CHAR8             *Buffer,
  IN  UINTN             BufferSize
  )
{
  UINT32       Index;
  CONST CHAR8  *Content;
  UINTN        Length;

  Index = EditFindPatch (Plist, Node);
  if ((Index != MAX_UINT32) && (Plist->Patches[Index].Kind == EditPlistKindInteger)) {
    AsciiSPrint (Buffer, BufferSize, "%Lu", Plist->Patches[Index].Integer);
    return;
  }

  Content = XmlNodeContent (Node);
  if (Content == NULL) {
    Content = "";
  }

  Length = AsciiStrLen (Content);
  if (Length > BufferSize - 1) {
    Length = BufferSize - 1;
  }

  CopyMem (Buffer, Content, Length);
  Buffer[Length] = '\0';
}

CONST CHAR8 *
EditPlistGetContent (
  IN  CONST EDIT_PLIST  *Plist,
  IN  XML_NODE          *Node
  )
{
  UINT32       Index;
  CONST CHAR8  *Content;

  Index = EditFindPatch (Plist, Node);
  if (  (Index != MAX_UINT32)
     && ((Plist->Patches[Index].Kind == EditPlistKindString) || (Plist->Patches[Index].Kind == EditPlistKindData))
     && (Plist->Patches[Index].Text != NULL))
  {
    return Plist->Patches[Index].Text;
  }

  Content = XmlNodeContent (Node);

  return Content != NULL ? Content : "";
}

EFI_STATUS
EditPlistGetData (
  IN  CONST EDIT_PLIST  *Plist,
  IN  XML_NODE          *Node,
  OUT UINT8             **Data,
  OUT UINTN             *Size
  )
{
  RETURN_STATUS  Status;
  CONST CHAR8    *Content;
  UINTN          DataSize;
  UINT8          *Buffer;

  *Data = NULL;
  *Size = 0;

  Content  = EditPlistGetContent (Plist, Node);
  DataSize = 0;
  Status   = Base64Decode (Content, AsciiStrLen (Content), NULL, &DataSize);
  if (Status == RETURN_SUCCESS) {
    return EFI_SUCCESS;
  }

  if (Status != RETURN_BUFFER_TOO_SMALL) {
    return EFI_INVALID_PARAMETER;
  }

  Buffer = AllocatePool (DataSize);
  if (Buffer == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = Base64Decode (Content, AsciiStrLen (Content), Buffer, &DataSize);
  if (RETURN_ERROR (Status)) {
    FreePool (Buffer);
    return EFI_INVALID_PARAMETER;
  }

  *Data = Buffer;
  *Size = DataSize;
  return EFI_SUCCESS;
}

BOOLEAN
EditPlistIsPlainString (
  IN  CONST CHAR8  *Value
  )
{
  UINTN  Index;
  UINTN  Length;

  Length = AsciiStrLen (Value);
  if (Length > EDIT_PLIST_MAX_VALUE_LENGTH) {
    return FALSE;
  }

  for (Index = 0; Index < Length; ++Index) {
    if ((Value[Index] < 0x20) || (Value[Index] > 0x7E) || (Value[Index] == '<') || (Value[Index] == '>')) {
      return FALSE;
    }

    if (Value[Index] == '&') {
      //
      // Only the predefined entities are acceptable. Note, that OpenCore does not
      // decode them in most places, so they are kept as they are written.
      //
      if (  (AsciiStrnCmp (&Value[Index], "&amp;", L_STR_LEN ("&amp;")) != 0)
         && (AsciiStrnCmp (&Value[Index], "&lt;", L_STR_LEN ("&lt;")) != 0)
         && (AsciiStrnCmp (&Value[Index], "&gt;", L_STR_LEN ("&gt;")) != 0)
         && (AsciiStrnCmp (&Value[Index], "&quot;", L_STR_LEN ("&quot;")) != 0)
         && (AsciiStrnCmp (&Value[Index], "&apos;", L_STR_LEN ("&apos;")) != 0))
      {
        return FALSE;
      }
    }
  }

  return TRUE;
}

EFI_STATUS
EditPlistSetBoolean (
  IN OUT EDIT_PLIST  *Plist,
  IN     XML_NODE    *Node,
  IN     BOOLEAN     Value
  )
{
  EFI_STATUS        Status;
  EDIT_PLIST_PATCH  *Patch;
  UINT32            Index;

  if (EditPlistNodeKind (Node) != EditPlistKindBoolean) {
    return EFI_UNSUPPORTED;
  }

  //
  // Going back to the original value is no change at all.
  //
  Index = EditFindPatch (Plist, Node);
  if ((AsciiStrCmp (XmlNodeName (Node), "true") == 0) == Value) {
    if (Index != MAX_UINT32) {
      EditRemovePatch (Plist, Index);
    }

    return EFI_SUCCESS;
  }

  Status = EditGetNodePatch (Plist, Node, EditPlistKindBoolean, &Patch);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Patch->Boolean = Value;
  return EFI_SUCCESS;
}

EFI_STATUS
EditPlistSetInteger (
  IN OUT EDIT_PLIST  *Plist,
  IN     XML_NODE    *Node,
  IN     UINT64      Value
  )
{
  EFI_STATUS        Status;
  EDIT_PLIST_PATCH  *Patch;

  if (EditPlistNodeKind (Node) != EditPlistKindInteger) {
    return EFI_UNSUPPORTED;
  }

  Status = EditGetNodePatch (Plist, Node, EditPlistKindInteger, &Patch);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Patch->Integer = Value;
  return EFI_SUCCESS;
}

EFI_STATUS
EditPlistSetString (
  IN OUT EDIT_PLIST   *Plist,
  IN     XML_NODE     *Node,
  IN     CONST CHAR8  *Value
  )
{
  EFI_STATUS        Status;
  EDIT_PLIST_PATCH  *Patch;
  EDIT_PLIST_KIND   Kind;
  UINT32            Index;
  CHAR8             *Copy;

  Kind = EditPlistNodeKind (Node);
  if ((Kind != EditPlistKindString) && (Kind != EditPlistKindData) && (Kind != EditPlistKindInteger)) {
    return EFI_UNSUPPORTED;
  }

  if (!EditPlistIsPlainString (Value)) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Going back to the original value is no change at all.
  //
  Index = EditFindPatch (Plist, Node);
  if (  (Kind == EditPlistKindString)
     && (AsciiStrCmp (XmlNodeContent (Node) != NULL ? XmlNodeContent (Node) : "", Value) == 0))
  {
    if (Index != MAX_UINT32) {
      EditRemovePatch (Plist, Index);
    }

    return EFI_SUCCESS;
  }

  Copy = AllocateCopyPool (AsciiStrSize (Value), Value);
  if (Copy == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = EditGetNodePatch (Plist, Node, EditPlistKindString, &Patch);
  if (EFI_ERROR (Status)) {
    FreePool (Copy);
    return Status;
  }

  Patch->Text = Copy;
  return EFI_SUCCESS;
}

EFI_STATUS
EditPlistSetData (
  IN OUT EDIT_PLIST   *Plist,
  IN     XML_NODE     *Node,
  IN     CONST UINT8  *Data,
  IN     UINTN        Size
  )
{
  EFI_STATUS        Status;
  RETURN_STATUS     EncodeStatus;
  EDIT_PLIST_PATCH  *Patch;
  UINTN             TextSize;
  CHAR8             *Text;

  if (EditPlistNodeKind (Node) != EditPlistKindData) {
    return EFI_UNSUPPORTED;
  }

  if (Size == 0) {
    Text = AllocateZeroPool (1);
  } else {
    TextSize     = 0;
    EncodeStatus = Base64Encode (Data, Size, NULL, &TextSize);
    if (EncodeStatus != RETURN_BUFFER_TOO_SMALL) {
      return EFI_INVALID_PARAMETER;
    }

    Text = AllocatePool (TextSize);
    if (Text != NULL) {
      EncodeStatus = Base64Encode (Data, Size, Text, &TextSize);
      if (RETURN_ERROR (EncodeStatus)) {
        FreePool (Text);
        return EFI_INVALID_PARAMETER;
      }
    }
  }

  if (Text == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = EditGetNodePatch (Plist, Node, EditPlistKindData, &Patch);
  if (EFI_ERROR (Status)) {
    FreePool (Text);
    return Status;
  }

  Patch->Text = Text;
  return EFI_SUCCESS;
}

VOID
EditPlistRevert (
  IN OUT EDIT_PLIST  *Plist,
  IN     XML_NODE    *Node
  )
{
  UINT32  Index;

  Index = EditFindPatch (Plist, Node);
  if (Index != MAX_UINT32) {
    EditRemovePatch (Plist, Index);
  }
}

/**
  Check if the text before a position on its line is only indentation.

  @param[in]   Plist       Document.
  @param[in]   Position    Offset into the original text.
  @param[out]  LineStart   Offset of the line which Position is on.

  @retval TRUE   Nothing but spaces and tabs is in front of Position on its line.
**/
STATIC
BOOLEAN
EditIsIndented (
  IN  CONST EDIT_PLIST  *Plist,
  IN  UINT32            Position,
  OUT UINT32            *LineStart
  )
{
  UINT32  Start;
  UINT32  Index;

  Start = Position;
  while ((Start > 0) && (Plist->Source[Start - 1] != '\n') && (Plist->Source[Start - 1] != '\r')) {
    --Start;
  }

  *LineStart = Start;

  for (Index = Start; Index < Position; ++Index) {
    if ((Plist->Source[Index] != ' ') && (Plist->Source[Index] != '\t')) {
      return FALSE;
    }
  }

  return TRUE;
}

STATIC
CONST CHAR8 *
EditNewLine (
  IN  CONST EDIT_PLIST  *Plist
  )
{
  UINT32  Index;

  for (Index = 0; Index < Plist->SourceSize; ++Index) {
    if (Plist->Source[Index] == '\n') {
      return (Index > 0 && Plist->Source[Index - 1] == '\r') ? "\r\n" : "\n";
    }
  }

  return "\n";
}

/**
  Layout of a dictionary which a new entry is going to be added to.
**/
typedef struct {
  //
  // Where the text goes, and whether it replaces the (empty) dictionary.
  //
  UINT32         Start;
  UINT32         End;
  BOOLEAN        ReplaceDict;
  //
  // Entries on separate lines, indented with Indent for the key.
  // Both strings are empty for dictionaries which are on a single line.
  //
  CONST CHAR8    *NewLine;
  CHAR8          Indent[EDIT_MAX_INDENT + 1];
  CHAR8          Unit[EDIT_MAX_INDENT + 1];
  //
  // Indentation of the closing tag for a replaced dictionary.
  //
  CHAR8          OuterIndent[EDIT_MAX_INDENT + 1];
} EDIT_DICT_LAYOUT;

STATIC
BOOLEAN
EditCopyIndent (
  IN  CONST EDIT_PLIST  *Plist,
  IN  UINT32            LineStart,
  IN  UINT32            Position,
  OUT CHAR8             *Indent
  )
{
  if (Position - LineStart > EDIT_MAX_COPY_INDENT) {
    return FALSE;
  }

  CopyMem (Indent, &Plist->Source[LineStart], Position - LineStart);
  Indent[Position - LineStart] = '\0';
  return TRUE;
}

STATIC
EFI_STATUS
EditGetDictLayout (
  IN  CONST EDIT_PLIST  *Plist,
  IN  XML_NODE          *Dict,
  OUT EDIT_DICT_LAYOUT  *Layout
  )
{
  UINT32    Count;
  UINT32    DictStart;
  UINT32    DictEnd;
  UINT32    KeyStart;
  UINT32    KeyEnd;
  UINT32    LastStart;
  UINT32    LastEnd;
  UINT32    LineStart;
  BOOLEAN   Multiline;

  ZeroMem (Layout, sizeof (*Layout));

  if (  (EditPlistNodeKind (Dict) != EditPlistKindDict)
     || !XmlNodeSourceRange (Dict, &DictStart, &DictEnd)
     || (DictEnd > Plist->SourceSize))
  {
    return EFI_UNSUPPORTED;
  }

  Count = EditPlistChildCount (Dict);

  if (Count == 0) {
    //
    // Empty dictionary is written either as <dict/> or <dict></dict>, replace it.
    //
    Layout->ReplaceDict = TRUE;
    Layout->Start       = DictStart;
    Layout->End         = DictEnd;
    Multiline           = EditIsIndented (Plist, DictStart, &LineStart);
    if (Multiline && !EditCopyIndent (Plist, LineStart, DictStart, Layout->OuterIndent)) {
      Multiline = FALSE;
    }

    if (Multiline) {
      Layout->NewLine = EditNewLine (Plist);
      AsciiStrCpyS (Layout->Indent, sizeof (Layout->Indent), Layout->OuterIndent);
      if ((Layout->OuterIndent[0] == '\0') || (AsciiStrStr (Layout->OuterIndent, "\t") != NULL)) {
        AsciiStrCatS (Layout->Indent, sizeof (Layout->Indent), "\t");
        AsciiStrCpyS (Layout->Unit, sizeof (Layout->Unit), "\t");
      } else {
        AsciiStrCatS (Layout->Indent, sizeof (Layout->Indent), "    ");
        AsciiStrCpyS (Layout->Unit, sizeof (Layout->Unit), "    ");
      }

    }

    if (!Multiline) {
      Layout->NewLine        = "";
      Layout->Indent[0]      = '\0';
      Layout->Unit[0]        = '\0';
      Layout->OuterIndent[0] = '\0';
    }

    return EFI_SUCCESS;
  }

  //
  // Add after the last value, formatted like the first key.
  //
  if (  !XmlNodeSourceRange (XmlNodeChild (Dict, 0), &KeyStart, &KeyEnd)
     || !XmlNodeSourceRange (XmlNodeChild (Dict, Count * 2 - 1), &LastStart, &LastEnd)
     || (LastEnd > Plist->SourceSize))
  {
    return EFI_UNSUPPORTED;
  }

  Layout->Start = LastEnd;
  Layout->End   = LastEnd;

  Multiline = EditIsIndented (Plist, KeyStart, &LineStart);
  if (Multiline && EditCopyIndent (Plist, LineStart, KeyStart, Layout->Indent)) {
    Layout->NewLine = EditNewLine (Plist);
    //
    // Indentation of a nested dictionary is one unit deeper than the key.
    //
    if (AsciiStrStr (Layout->Indent, "\t") != NULL) {
      AsciiStrCpyS (Layout->Unit, sizeof (Layout->Unit), "\t");
    } else {
      AsciiStrCpyS (Layout->Unit, sizeof (Layout->Unit), "    ");
    }
  } else {
    Layout->NewLine   = "";
    Layout->Indent[0] = '\0';
    Layout->Unit[0]   = '\0';
  }

  return EFI_SUCCESS;
}

/**
  Check that a key can be added: a plain non-empty string, which is not there yet.
**/
STATIC
EFI_STATUS
EditCheckNewKey (
  IN  XML_NODE     *Dict,
  IN  CONST CHAR8  *Key
  )
{
  if ((Key[0] == '\0') || !EditPlistIsPlainString (Key)) {
    return EFI_INVALID_PARAMETER;
  }

  if (EditPlistFindChild (Dict, Key) != NULL) {
    return EFI_ALREADY_STARTED;
  }

  return EFI_SUCCESS;
}

/**
  Add the text which holds a new entry, built by the callback-less builder below, as a raw patch.
**/
STATIC
EFI_STATUS
EditAddRawPatch (
  IN OUT EDIT_PLIST  *Plist,
  IN     UINT32      Start,
  IN     UINT32      End,
  IN     CHAR8       *Text
  )
{
  EDIT_PLIST_PATCH  *Patch;

  Patch = EditAllocPatch (Plist);
  if (Patch == NULL) {
    FreePool (Text);
    return EFI_OUT_OF_RESOURCES;
  }

  Patch->Node  = NULL;
  Patch->Kind  = EditPlistKindOther;
  Patch->Start = Start;
  Patch->End   = End;
  Patch->Text  = Text;
  return EFI_SUCCESS;
}

/**
  Build and add the text for a new entry of a dictionary.

  @param[in,out]  Plist       Document.
  @param[in]      Dict        Dictionary to add to.
  @param[in]      Key         New key.
  @param[in]      InnerKey    Key of an entry of a dictionary value, NULL if the value is not a dictionary.
  @param[in]      Value       String value, or string value of the entry of a dictionary value.
                              Ignored when BooleanValue is given.
  @param[in]      Boolean     TRUE if the value is a boolean (given by BooleanValue) and not a string.
  @param[in]      BooleanValue  The boolean value.
**/
STATIC
EFI_STATUS
EditAddEntry (
  IN OUT EDIT_PLIST   *Plist,
  IN     XML_NODE     *Dict,
  IN     CONST CHAR8  *Key,
  IN     CONST CHAR8  *InnerKey OPTIONAL,
  IN     CONST CHAR8  *Value,
  IN     BOOLEAN      Boolean,
  IN     BOOLEAN      BooleanValue
  )
{
  EFI_STATUS        Status;
  EDIT_DICT_LAYOUT  Layout;
  EDIT_BUILDER      Builder;
  CHAR8             *Text;

  Status = EditCheckNewKey (Dict, Key);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (  (!Boolean && !EditPlistIsPlainString (Value))
     || ((InnerKey != NULL) && ((InnerKey[0] == '\0') || !EditPlistIsPlainString (InnerKey))))
  {
    return EFI_INVALID_PARAMETER;
  }

  Status = EditGetDictLayout (Plist, Dict, &Layout);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  EditBuilderInit (&Builder, AsciiStrLen (Key) + (Boolean ? 0 : AsciiStrLen (Value)) + (InnerKey != NULL ? AsciiStrLen (InnerKey) : 0) + 256);

  if (Layout.ReplaceDict) {
    EditBuilderAppendStr (&Builder, "<dict>");
  }

  //
  // Entry: newline and indentation, the key, newline and indentation, the value.
  //
  EditBuilderAppendStr (&Builder, Layout.NewLine);
  EditBuilderAppendStr (&Builder, Layout.Indent);
  EditBuilderAppendStr (&Builder, "<key>");
  EditBuilderAppendStr (&Builder, Key);
  EditBuilderAppendStr (&Builder, "</key>");
  EditBuilderAppendStr (&Builder, Layout.NewLine);
  EditBuilderAppendStr (&Builder, Layout.Indent);

  if (InnerKey != NULL) {
    EditBuilderAppendStr (&Builder, "<dict>");
    EditBuilderAppendStr (&Builder, Layout.NewLine);
    EditBuilderAppendStr (&Builder, Layout.Indent);
    EditBuilderAppendStr (&Builder, Layout.Unit);
    EditBuilderAppendStr (&Builder, "<key>");
    EditBuilderAppendStr (&Builder, InnerKey);
    EditBuilderAppendStr (&Builder, "</key>");
    EditBuilderAppendStr (&Builder, Layout.NewLine);
    EditBuilderAppendStr (&Builder, Layout.Indent);
    EditBuilderAppendStr (&Builder, Layout.Unit);
  }

  if (Boolean) {
    EditBuilderAppendStr (&Builder, BooleanValue ? "<true/>" : "<false/>");
  } else {
    EditBuilderAppendStr (&Builder, "<string>");
    EditBuilderAppendStr (&Builder, Value);
    EditBuilderAppendStr (&Builder, "</string>");
  }

  if (InnerKey != NULL) {
    EditBuilderAppendStr (&Builder, Layout.NewLine);
    EditBuilderAppendStr (&Builder, Layout.Indent);
    EditBuilderAppendStr (&Builder, "</dict>");
  }

  if (Layout.ReplaceDict) {
    EditBuilderAppendStr (&Builder, Layout.NewLine);
    EditBuilderAppendStr (&Builder, Layout.OuterIndent);
    EditBuilderAppendStr (&Builder, "</dict>");
  }

  Text = EditBuilderFinish (&Builder, NULL);
  if (Text == NULL) {
    EditBuilderFree (&Builder);
    return EFI_OUT_OF_RESOURCES;
  }

  return EditAddRawPatch (Plist, Layout.Start, Layout.End, Text);
}

EFI_STATUS
EditPlistAddString (
  IN OUT EDIT_PLIST   *Plist,
  IN     XML_NODE     *Dict,
  IN     CONST CHAR8  *Key,
  IN     CONST CHAR8  *Value
  )
{
  return EditAddEntry (Plist, Dict, Key, NULL, Value, FALSE, FALSE);
}

EFI_STATUS
EditPlistAddBoolean (
  IN OUT EDIT_PLIST   *Plist,
  IN     XML_NODE     *Dict,
  IN     CONST CHAR8  *Key,
  IN     BOOLEAN      Value
  )
{
  return EditAddEntry (Plist, Dict, Key, NULL, "", TRUE, Value);
}

VOID
EditPlistRevertAdded (
  IN OUT EDIT_PLIST  *Plist,
  IN     XML_NODE    *Dict
  )
{
  UINT32  Index;
  UINT32  Start;
  UINT32  End;

  if (!XmlNodeSourceRange (Dict, &Start, &End)) {
    return;
  }

  //
  // Raw patches are the only additions, and they are inside of the dictionary.
  //
  Index = 0;
  while (Index < Plist->PatchCount) {
    if (  (Plist->Patches[Index].Node == NULL)
       && (Plist->Patches[Index].Start >= Start)
       && (Plist->Patches[Index].Start < End))
    {
      EditRemovePatch (Plist, Index);
    } else {
      ++Index;
    }
  }
}

EFI_STATUS
EditPlistAddDictWithString (
  IN OUT EDIT_PLIST   *Plist,
  IN     XML_NODE     *Dict,
  IN     CONST CHAR8  *Key,
  IN     CONST CHAR8  *InnerKey,
  IN     CONST CHAR8  *InnerValue
  )
{
  return EditAddEntry (Plist, Dict, Key, InnerKey, InnerValue, FALSE, FALSE);
}

/**
  Produce the text which replaces a range of the original text.

  @param[in]   Patch   Patch to render.
  @param[out]  Length  Length of the text.

  @return Text from pool or NULL on failure.
**/
STATIC
CHAR8 *
EditRenderPatch (
  IN  CONST EDIT_PLIST_PATCH  *Patch,
  OUT UINTN                   *Length
  )
{
  EDIT_BUILDER  Builder;
  CHAR8         Number[24];

  EditBuilderInit (&Builder, (Patch->Text != NULL ? AsciiStrLen (Patch->Text) : 0) + 64);

  switch (Patch->Kind) {
    case EditPlistKindBoolean:
      EditBuilderAppendStr (&Builder, Patch->Boolean ? "<true/>" : "<false/>");
      break;
    case EditPlistKindInteger:
      AsciiSPrint (Number, sizeof (Number), "%Lu", Patch->Integer);
      EditBuilderAppendStr (&Builder, "<integer>");
      EditBuilderAppendStr (&Builder, Number);
      EditBuilderAppendStr (&Builder, "</integer>");
      break;
    case EditPlistKindString:
      EditBuilderAppendStr (&Builder, "<string>");
      EditBuilderAppendStr (&Builder, Patch->Text != NULL ? Patch->Text : "");
      EditBuilderAppendStr (&Builder, "</string>");
      break;
    case EditPlistKindData:
      EditBuilderAppendStr (&Builder, "<data>");
      EditBuilderAppendStr (&Builder, Patch->Text != NULL ? Patch->Text : "");
      EditBuilderAppendStr (&Builder, "</data>");
      break;
    default:
      //
      // Raw text, verbatim.
      //
      EditBuilderAppendStr (&Builder, Patch->Text != NULL ? Patch->Text : "");
      break;
  }

  return EditBuilderFinish (&Builder, Length);
}

/**
  Part of the output: text which replaces a range of the original.
**/
typedef struct {
  UINT32    Start;
  UINT32    End;
  CHAR8     *Text;
  UINTN     Length;
} EDIT_PIECE;

/**
  Check if a patch is inside of a range of the original text. An insertion on the border of
  the range is not: it stays where it is whatever happens to the range.
**/
STATIC
BOOLEAN
EditPatchInRange (
  IN  CONST EDIT_PLIST_PATCH  *Patch,
  IN  UINT32                  Start,
  IN  UINT32                  End
  )
{
  if (Patch->Start == Patch->End) {
    return Patch->Start > Start && Patch->Start < End;
  }

  return Patch->Start >= Start && Patch->End <= End;
}

/**
  Get the locations of the elements of the rearranged array in the original text.

  @param[in]   Plist   Document.
  @param[out]  Ranges  Start and end of every element, from the pool.

  @retval EFI_UNSUPPORTED  Elements cannot be told apart in the text.
**/
STATIC
EFI_STATUS
EditGetElementRanges (
  IN  CONST EDIT_PLIST  *Plist,
  IN  XML_NODE          *Array,
  IN  UINT32            Count,
  OUT UINT32            **Ranges
  )
{
  UINT32  Index;
  UINT32  Position;
  UINT32  *Result;

  if (EditPlistChildCount (Array) != Count) {
    return EFI_UNSUPPORTED;
  }

  Result = AllocatePool (((UINTN)Count * 2 + 1) * sizeof (*Result));
  if (Result == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Position = 0;
  for (Index = 0; Index < Count; ++Index) {
    if (  !XmlNodeSourceRange (XmlNodeChild (Array, Index), &Result[Index * 2], &Result[Index * 2 + 1])
       || (Result[Index * 2] < Position)
       || (Result[Index * 2 + 1] <= Result[Index * 2])
       || (Result[Index * 2 + 1] > Plist->SourceSize))
    {
      FreePool (Result);
      return EFI_UNSUPPORTED;
    }

    Position = Result[Index * 2 + 1];
  }

  *Ranges = Result;
  return EFI_SUCCESS;
}

STATIC
BOOLEAN
EditIsIdentityOrder (
  IN  CONST EDIT_PLIST_ORDER  *Order
  )
{
  UINT32  Index;

  for (Index = 0; Index < Order->Count; ++Index) {
    if (Order->Order[Index] != Index) {
      return FALSE;
    }
  }

  return TRUE;
}

STATIC
VOID
EditDropOrder (
  IN OUT EDIT_PLIST  *Plist
  )
{
  if (Plist->Reorder.Order != NULL) {
    FreePool (Plist->Reorder.Order);
  }

  ZeroMem (&Plist->Reorder, sizeof (Plist->Reorder));
}

/**
  Make the array the one which is rearranged, with its elements where they are.
**/
STATIC
EFI_STATUS
EditStartOrder (
  IN OUT EDIT_PLIST  *Plist,
  IN     XML_NODE    *Array,
  IN     UINT32      Count
  )
{
  EFI_STATUS  Status;
  UINT32      *Ranges;
  UINT32      *Order;
  UINT32      Index;

  if (Plist->Reorder.Array == Array) {
    return EFI_SUCCESS;
  }

  if ((Plist->Reorder.Array != NULL) || (Count > EDIT_PLIST_MAX_ORDER)) {
    return EFI_UNSUPPORTED;
  }

  //
  // It is found out now whether the elements can be moved, and not when the text is made.
  //
  Status = EditGetElementRanges (Plist, Array, Count, &Ranges);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  FreePool (Ranges);

  Order = AllocatePool (((UINTN)Count + 1) * sizeof (*Order));
  if (Order == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  for (Index = 0; Index < Count; ++Index) {
    Order[Index] = Index;
  }

  Plist->Reorder.Array = Array;
  Plist->Reorder.Count = Count;
  Plist->Reorder.Order = Order;

  return EFI_SUCCESS;
}

EFI_STATUS
EditPlistArrayMove (
  IN OUT EDIT_PLIST  *Plist,
  IN     XML_NODE    *Array,
  IN     UINT32      From,
  IN     UINT32      To
  )
{
  EFI_STATUS  Status;
  UINT32      Count;
  UINT32      Index;
  UINT32      Moved;
  UINT32      *Order;

  if (EditPlistNodeKind (Array) != EditPlistKindArray) {
    return EFI_INVALID_PARAMETER;
  }

  Count = EditPlistChildCount (Array);
  if ((From >= Count) || (To >= Count)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((Plist->Reorder.Array != NULL) && (Plist->Reorder.Array != Array)) {
    return EFI_UNSUPPORTED;
  }

  if (From == To) {
    return EFI_SUCCESS;
  }

  Status = EditStartOrder (Plist, Array, Count);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Order = Plist->Reorder.Order;
  Moved = Order[From];
  if (From < To) {
    for (Index = From; Index < To; ++Index) {
      Order[Index] = Order[Index + 1];
    }
  } else {
    for (Index = From; Index > To; --Index) {
      Order[Index] = Order[Index - 1];
    }
  }

  Order[To] = Moved;

  if (EditIsIdentityOrder (&Plist->Reorder)) {
    EditDropOrder (Plist);
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EditPlistArraySetOrder (
  IN OUT EDIT_PLIST    *Plist,
  IN     XML_NODE      *Array,
  IN     CONST UINT32  *Order,
  IN     UINT32        Count
  )
{
  EFI_STATUS  Status;
  UINT32      Index;
  BOOLEAN     *Seen;

  if ((EditPlistNodeKind (Array) != EditPlistKindArray) || (EditPlistChildCount (Array) != Count)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((Plist->Reorder.Array != NULL) && (Plist->Reorder.Array != Array)) {
    return EFI_UNSUPPORTED;
  }

  //
  // Every element has to be somewhere, and only once.
  //
  Seen = AllocateZeroPool (((UINTN)Count + 1) * sizeof (*Seen));
  if (Seen == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  for (Index = 0; Index < Count; ++Index) {
    if ((Order[Index] >= Count) || Seen[Order[Index]]) {
      FreePool (Seen);
      return EFI_INVALID_PARAMETER;
    }

    Seen[Order[Index]] = TRUE;
  }

  FreePool (Seen);

  Status = EditStartOrder (Plist, Array, Count);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  CopyMem (Plist->Reorder.Order, Order, (UINTN)Count * sizeof (*Order));

  if (EditIsIdentityOrder (&Plist->Reorder)) {
    EditDropOrder (Plist);
  }

  return EFI_SUCCESS;
}

UINT32
EditPlistArrayOriginalIndex (
  IN  CONST EDIT_PLIST  *Plist,
  IN  XML_NODE          *Array,
  IN  UINT32            Position
  )
{
  if ((Plist->Reorder.Array == Array) && (Array != NULL) && (Position < Plist->Reorder.Count)) {
    return Plist->Reorder.Order[Position];
  }

  return Position;
}

BOOLEAN
EditPlistArrayIsRearranged (
  IN  CONST EDIT_PLIST  *Plist,
  IN  XML_NODE          *Array
  )
{
  return Array != NULL && Plist->Reorder.Array == Array;
}

VOID
EditPlistArrayRevertOrder (
  IN OUT EDIT_PLIST  *Plist,
  IN     XML_NODE    *Array
  )
{
  if (EditPlistArrayIsRearranged (Plist, Array)) {
    EditDropOrder (Plist);
  }
}

/**
  Insert a piece in the order of the text, after the ones that start at the same place.
**/
STATIC
VOID
EditSortPieces (
  IN OUT EDIT_PIECE  *Pieces,
  IN     UINT32      Count
  )
{
  UINT32      Index;
  UINT32      Other;
  EDIT_PIECE  Temp;

  for (Index = 1; Index < Count; ++Index) {
    for (Other = Index; Other > 0; --Other) {
      if (Pieces[Other - 1].Start <= Pieces[Other].Start) {
        break;
      }

      CopyMem (&Temp, &Pieces[Other - 1], sizeof (Temp));
      CopyMem (&Pieces[Other - 1], &Pieces[Other], sizeof (Temp));
      CopyMem (&Pieces[Other], &Temp, sizeof (Temp));
    }
  }
}

/**
  Produce the text of a range of the original with the patches inside of it applied.

  @param[in]   Plist   Document.
  @param[in]   Start   Start of the range.
  @param[in]   End     End of the range.
  @param[out]  Length  Length of the text.

  @return Text from pool or NULL on failure.
**/
STATIC
CHAR8 *
EditRenderRange (
  IN  CONST EDIT_PLIST  *Plist,
  IN  UINT32            Start,
  IN  UINT32            End,
  OUT UINTN             *Length
  )
{
  EDIT_PIECE    *Pieces;
  UINT32        Count;
  UINT32        Index;
  UINT32        Position;
  CHAR8         *Text;
  EDIT_BUILDER  Builder;

  Pieces = AllocateZeroPool (((UINTN)Plist->PatchCount + 1) * sizeof (*Pieces));
  if (Pieces == NULL) {
    return NULL;
  }

  Count = 0;
  for (Index = 0; Index < Plist->PatchCount; ++Index) {
    if (!EditPatchInRange (&Plist->Patches[Index], Start, End)) {
      continue;
    }

    Pieces[Count].Start = Plist->Patches[Index].Start;
    Pieces[Count].End   = Plist->Patches[Index].End;
    Pieces[Count].Text  = EditRenderPatch (&Plist->Patches[Index], &Pieces[Count].Length);
    if (Pieces[Count].Text == NULL) {
      goto Fail;
    }

    ++Count;
  }

  EditSortPieces (Pieces, Count);

  EditBuilderInit (&Builder, End - Start + 64);

  Position = Start;
  for (Index = 0; Index < Count; ++Index) {
    if (Pieces[Index].Start < Position) {
      EditBuilderFree (&Builder);
      goto Fail;
    }

    EditBuilderAppend (&Builder, &Plist->Source[Position], Pieces[Index].Start - Position);
    EditBuilderAppend (&Builder, Pieces[Index].Text, Pieces[Index].Length);
    Position = Pieces[Index].End;
  }

  EditBuilderAppend (&Builder, &Plist->Source[Position], End - Position);
  Text = EditBuilderFinish (&Builder, Length);

  for (Index = 0; Index < Count; ++Index) {
    FreePool (Pieces[Index].Text);
  }

  FreePool (Pieces);
  return Text;

Fail:
  for (Index = 0; Index < Count; ++Index) {
    if (Pieces[Index].Text != NULL) {
      FreePool (Pieces[Index].Text);
    }
  }

  FreePool (Pieces);
  return NULL;
}

EFI_STATUS
EditPlistRender (
  IN  CONST EDIT_PLIST  *Plist,
  OUT CHAR8             **Text,
  OUT UINT32            *Size
  )
{
  EFI_STATUS    Status;
  EDIT_PIECE    *Pieces;
  UINT32        *Ranges;
  UINT32        Count;
  UINT32        Index;
  UINT32        Other;
  UINT32        Original;
  UINT32        Position;
  UINTN         Total;
  BOOLEAN       Shadowed;
  EDIT_BUILDER  Output;

  *Text = NULL;
  *Size = 0;

  Status = EFI_SUCCESS;
  Ranges = NULL;
  Count  = 0;
  Pieces = AllocateZeroPool (((UINTN)Plist->PatchCount + Plist->Reorder.Count + 1) * sizeof (*Pieces));
  if (Pieces == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  if (Plist->Reorder.Array != NULL) {
    Status = EditGetElementRanges (Plist, Plist->Reorder.Array, Plist->Reorder.Count, &Ranges);
    if (EFI_ERROR (Status)) {
      goto Done;
    }
  }

  //
  // Patches inside of an element which is not where it was are made as part of the element.
  //
  for (Index = 0; Index < Plist->PatchCount; ++Index) {
    Shadowed = FALSE;
    for (Other = 0; (Ranges != NULL) && (Other < Plist->Reorder.Count); ++Other) {
      if (  (Plist->Reorder.Order[Other] != Other)
         && EditPatchInRange (&Plist->Patches[Index], Ranges[Other * 2], Ranges[Other * 2 + 1]))
      {
        Shadowed = TRUE;
        break;
      }
    }

    if (Shadowed) {
      continue;
    }

    Pieces[Count].Start = Plist->Patches[Index].Start;
    Pieces[Count].End   = Plist->Patches[Index].End;
    Pieces[Count].Text  = EditRenderPatch (&Plist->Patches[Index], &Pieces[Count].Length);
    if (Pieces[Count].Text == NULL) {
      Status = EFI_OUT_OF_RESOURCES;
      goto Done;
    }

    ++Count;
  }

  //
  // Every place which holds another element now gets its text.
  //
  for (Index = 0; (Ranges != NULL) && (Index < Plist->Reorder.Count); ++Index) {
    Original = Plist->Reorder.Order[Index];
    if (Original == Index) {
      continue;
    }

    Pieces[Count].Start = Ranges[Index * 2];
    Pieces[Count].End   = Ranges[Index * 2 + 1];
    Pieces[Count].Text  = EditRenderRange (Plist, Ranges[Original * 2], Ranges[Original * 2 + 1], &Pieces[Count].Length);
    if (Pieces[Count].Text == NULL) {
      Status = EFI_OUT_OF_RESOURCES;
      goto Done;
    }

    ++Count;
  }

  EditSortPieces (Pieces, Count);

  Position = 0;
  Total    = Plist->SourceSize;
  for (Index = 0; Index < Count; ++Index) {
    if (  (Pieces[Index].Start < Position)
       || (Pieces[Index].End < Pieces[Index].Start)
       || (Pieces[Index].End > Plist->SourceSize))
    {
      Status = EFI_INVALID_PARAMETER;
      goto Done;
    }

    Position = Pieces[Index].End;
    Total    = Total - (Pieces[Index].End - Pieces[Index].Start) + Pieces[Index].Length;
  }

  EditBuilderInit (&Output, Total);

  Position = 0;
  for (Index = 0; Index < Count; ++Index) {
    EditBuilderAppend (&Output, &Plist->Source[Position], Pieces[Index].Start - Position);
    EditBuilderAppend (&Output, Pieces[Index].Text, Pieces[Index].Length);
    Position = Pieces[Index].End;
  }

  EditBuilderAppend (&Output, &Plist->Source[Position], Plist->SourceSize - Position);

  *Text = EditBuilderFinish (&Output, &Total);
  if (*Text == NULL) {
    EditBuilderFree (&Output);
    Status = EFI_OUT_OF_RESOURCES;
    goto Done;
  }

  *Size = (UINT32)Total;

Done:
  for (Index = 0; Index < Count; ++Index) {
    if (Pieces[Index].Text != NULL) {
      FreePool (Pieces[Index].Text);
    }
  }

  FreePool (Pieces);

  if (Ranges != NULL) {
    FreePool (Ranges);
  }

  return Status;
}

EFI_STATUS
EditPlistCountErrors (
  IN  CONST CHAR8  *Text,
  IN  UINT32       Size,
  OUT UINT32       *Errors
  )
{
  EFI_STATUS        Status;
  OC_GLOBAL_CONFIG  *Config;
  VOID              *Copy;

  *Errors = 0;

  //
  // The parser destroys its input, and the configuration is large to be on the stack.
  //
  Config = AllocatePool (sizeof (*Config));
  Copy   = AllocatePool ((UINTN)Size + 1);
  if ((Config == NULL) || (Copy == NULL)) {
    if (Config != NULL) {
      FreePool (Config);
    }

    if (Copy != NULL) {
      FreePool (Copy);
    }

    return EFI_OUT_OF_RESOURCES;
  }

  CopyMem (Copy, Text, Size);
  ((CHAR8 *)Copy)[Size] = '\0';

  Status = OcConfigurationInit (Config, Copy, Size, Errors);
  if (!EFI_ERROR (Status)) {
    OcConfigurationFree (Config);
  }

  FreePool (Copy);
  FreePool (Config);

  return Status;
}

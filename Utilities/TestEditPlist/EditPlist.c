/** @file
  Tests of the text-preserving plist editing used by the picker options editor.

  Copyright (C) 2026, OpenCore contributors. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-3-Clause
**/

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>

#include <UserFile.h>

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

#define CHECK_TEXT(Plist, Expected)  do { \
  CHAR8   *_Text; \
  UINT32  _Size; \
  ++mChecks; \
  if (EFI_ERROR (EditPlistRender (Plist, &_Text, &_Size))) { \
    printf ("FAIL %s:%d: render failed\n", __FILE__, __LINE__); \
    ++mFailures; \
  } else { \
    if ((_Size != AsciiStrLen (Expected)) || (CompareMem (_Text, Expected, _Size) != 0)) { \
      printf ("FAIL %s:%d: unexpected text\n--- got (%u) ---\n%s\n--- expected ---\n%s\n", __FILE__, __LINE__, _Size, _Text, Expected); \
      ++mFailures; \
    } \
    FreePool (_Text); \
  } \
} while (0)

#define HEAD  \
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n" \
  "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n" \
  "<plist version=\"1.0\">\n"

STATIC CONST CHAR8  mTabbed[] =
  HEAD
  "<dict>\n"
  "\t<!-- a comment which must survive -->\n"
  "\t<key>Flag</key>\n"
  "\t<true/>\n"
  "\t<key>Num</key>\n"
  "\t<integer>5</integer>\n"
  "\t<key>Str</key>\n"
  "\t<string>abc</string>\n"
  "\t<key>Bin</key>\n"
  "\t<data>AAAA</data>\n"
  "\t<key>Empty</key>\n"
  "\t<dict/>\n"
  "\t<key>Sub</key>\n"
  "\t<dict>\n"
  "\t\t<key>X</key>\n"
  "\t\t<string>1</string>\n"
  "\t</dict>\n"
  "\t<key>List</key>\n"
  "\t<array>\n"
  "\t\t<string>one</string>\n"
  "\t\t<false/>\n"
  "\t</array>\n"
  "</dict>\n"
  "</plist>\n";

STATIC
XML_NODE *
Find (
  IN EDIT_PLIST  *Plist,
  IN CONST CHAR8 *Key
  )
{
  XML_NODE  *Node;

  Node = EditPlistFindChild (Plist->Root, Key);
  if (Node == NULL) {
    printf ("FAIL: no key %s\n", Key);
    ++mFailures;
  }

  return Node;
}

STATIC
VOID
TestBasics (
  VOID
  )
{
  EDIT_PLIST  Plist;
  XML_NODE    *Node;
  CHAR8       Number[32];
  UINT8       *Data;
  UINTN       DataSize;
  UINT8       NewData[4] = { 0xE7, 0x03, 0x00, 0x00 };

  CHECK (!EFI_ERROR (EditPlistLoad (&Plist, mTabbed, sizeof (mTabbed) - 1)));

  CHECK (EditPlistChildCount (Plist.Root) == 7);
  CHECK (AsciiStrCmp (EditPlistChildKey (Plist.Root, 0), "Flag") == 0);
  CHECK (AsciiStrCmp (EditPlistChildKey (Plist.Root, 6), "List") == 0);
  CHECK (EditPlistNodeKind (Find (&Plist, "Flag")) == EditPlistKindBoolean);
  CHECK (EditPlistNodeKind (Find (&Plist, "Num")) == EditPlistKindInteger);
  CHECK (EditPlistNodeKind (Find (&Plist, "Str")) == EditPlistKindString);
  CHECK (EditPlistNodeKind (Find (&Plist, "Bin")) == EditPlistKindData);
  CHECK (EditPlistNodeKind (Find (&Plist, "Empty")) == EditPlistKindDict);
  CHECK (EditPlistNodeKind (Find (&Plist, "List")) == EditPlistKindArray);
  CHECK (EditPlistChildCount (Find (&Plist, "List")) == 2);
  CHECK (EditPlistChildKey (Find (&Plist, "List"), 0) == NULL);
  CHECK (EditPlistChildCount (Find (&Plist, "Empty")) == 0);
  CHECK (EditPlistFindChild (Plist.Root, "Missing") == NULL);
  CHECK (EditPlistFindChild (NULL, "Missing") == NULL);
  CHECK (EditPlistFindChild (EditPlistFindChild (Plist.Root, "Sub"), "X") != NULL);

  //
  // Unchanged document renders to the very same text.
  //
  CHECK_TEXT (&Plist, (CONST CHAR8 *)mTabbed);

  //
  // Boolean.
  //
  Node = Find (&Plist, "Flag");
  CHECK (EditPlistGetBoolean (&Plist, Node));
  CHECK (!EFI_ERROR (EditPlistSetBoolean (&Plist, Node, FALSE)));
  CHECK (!EditPlistGetBoolean (&Plist, Node));
  CHECK (EditPlistIsPatched (&Plist, Node));
  CHECK (EditPlistPatchCount (&Plist) == 1);
  CHECK (EditPlistSetBoolean (&Plist, Find (&Plist, "Num"), TRUE) == EFI_UNSUPPORTED);
  //
  // Setting back the original value is no change.
  //
  CHECK (!EFI_ERROR (EditPlistSetBoolean (&Plist, Node, TRUE)));
  CHECK (EditPlistPatchCount (&Plist) == 0);
  CHECK (!EditPlistIsPatched (&Plist, Node));
  CHECK (!EFI_ERROR (EditPlistSetBoolean (&Plist, Node, FALSE)));
  EditPlistRevert (&Plist, Node);
  CHECK (EditPlistPatchCount (&Plist) == 0);
  CHECK (EditPlistGetBoolean (&Plist, Node));

  //
  // Integer.
  //
  Node = Find (&Plist, "Num");
  EditPlistGetInteger (&Plist, Node, Number, sizeof (Number));
  CHECK (AsciiStrCmp (Number, "5") == 0);
  CHECK (!EFI_ERROR (EditPlistSetInteger (&Plist, Node, 18446744073709551615ULL)));
  EditPlistGetInteger (&Plist, Node, Number, sizeof (Number));
  CHECK (AsciiStrCmp (Number, "18446744073709551615") == 0);
  CHECK (!EFI_ERROR (EditPlistSetInteger (&Plist, Node, 12345)));
  CHECK (EditPlistPatchCount (&Plist) == 1);
  EditPlistGetInteger (&Plist, Node, Number, 4);
  CHECK (AsciiStrCmp (Number, "123") == 0);

  //
  // String.
  //
  Node = Find (&Plist, "Str");
  CHECK (AsciiStrCmp (EditPlistGetContent (&Plist, Node), "abc") == 0);
  CHECK (!EFI_ERROR (EditPlistSetString (&Plist, Node, "x y &amp; z")));
  CHECK (AsciiStrCmp (EditPlistGetContent (&Plist, Node), "x y &amp; z") == 0);
  CHECK (EditPlistSetString (&Plist, Node, "a<b") == EFI_INVALID_PARAMETER);
  CHECK (EditPlistSetString (&Plist, Node, "a>b") == EFI_INVALID_PARAMETER);
  CHECK (EditPlistSetString (&Plist, Node, "a&b") == EFI_INVALID_PARAMETER);
  CHECK (EditPlistSetString (&Plist, Node, "a&bogus;b") == EFI_INVALID_PARAMETER);
  CHECK (EditPlistSetString (&Plist, Node, "tab\there") == EFI_INVALID_PARAMETER);
  CHECK (EditPlistSetString (&Plist, Node, "caf\xC3\xA9") == EFI_INVALID_PARAMETER);
  //
  // A failed attempt keeps the previous edit.
  //
  CHECK (AsciiStrCmp (EditPlistGetContent (&Plist, Node), "x y &amp; z") == 0);
  CHECK (!EFI_ERROR (EditPlistSetString (&Plist, Node, "")));
  CHECK (AsciiStrCmp (EditPlistGetContent (&Plist, Node), "") == 0);
  CHECK (!EFI_ERROR (EditPlistSetString (&Plist, Node, "abc")));
  CHECK (!EditPlistIsPatched (&Plist, Node));

  //
  // Data.
  //
  Node = Find (&Plist, "Bin");
  CHECK (!EFI_ERROR (EditPlistGetData (&Plist, Node, &Data, &DataSize)));
  CHECK (DataSize == 3);
  CHECK (Data != NULL && Data[0] == 0 && Data[1] == 0 && Data[2] == 0);
  FreePool (Data);
  CHECK (!EFI_ERROR (EditPlistSetData (&Plist, Node, NewData, sizeof (NewData))));
  CHECK (AsciiStrCmp (EditPlistGetContent (&Plist, Node), "5wMAAA==") == 0);
  CHECK (!EFI_ERROR (EditPlistGetData (&Plist, Node, &Data, &DataSize)));
  CHECK (DataSize == 4 && CompareMem (Data, NewData, 4) == 0);
  FreePool (Data);

  CHECK_TEXT (
    &Plist,
    HEAD
    "<dict>\n"
    "\t<!-- a comment which must survive -->\n"
    "\t<key>Flag</key>\n"
    "\t<true/>\n"
    "\t<key>Num</key>\n"
    "\t<integer>12345</integer>\n"
    "\t<key>Str</key>\n"
    "\t<string>abc</string>\n"
    "\t<key>Bin</key>\n"
    "\t<data>5wMAAA==</data>\n"
    "\t<key>Empty</key>\n"
    "\t<dict/>\n"
    "\t<key>Sub</key>\n"
    "\t<dict>\n"
    "\t\t<key>X</key>\n"
    "\t\t<string>1</string>\n"
    "\t</dict>\n"
    "\t<key>List</key>\n"
    "\t<array>\n"
    "\t\t<string>one</string>\n"
    "\t\t<false/>\n"
    "\t</array>\n"
    "</dict>\n"
    "</plist>\n"
    );

  //
  // Array elements can be changed too, as they are just nodes.
  //
  Node = Find (&Plist, "List");
  CHECK (!EFI_ERROR (EditPlistSetBoolean (&Plist, EditPlistChildValue (Node, 1), TRUE)));
  CHECK (!EFI_ERROR (EditPlistSetString (&Plist, EditPlistChildValue (Node, 0), "two")));
  CHECK (EditPlistChildValue (Node, 2) == NULL);

  EditPlistFree (&Plist);
  CHECK (Plist.Source == NULL && Plist.Document == NULL);
}

STATIC
VOID
TestInsertions (
  VOID
  )
{
  EDIT_PLIST  Plist;
  XML_NODE    *Sub;

  //
  // Into a populated dictionary, below the last entry with the indentation of the first.
  //
  CHECK (!EFI_ERROR (EditPlistLoad (&Plist, mTabbed, sizeof (mTabbed) - 1)));
  Sub = Find (&Plist, "Sub");
  CHECK (!EFI_ERROR (EditPlistAddString (&Plist, Sub, "Y", "2")));
  CHECK (EditPlistAddString (&Plist, Sub, "X", "again") == EFI_ALREADY_STARTED);
  CHECK (EditPlistAddString (&Plist, Sub, "", "again") == EFI_INVALID_PARAMETER);
  CHECK (EditPlistAddString (&Plist, Sub, "a<b", "again") == EFI_INVALID_PARAMETER);
  CHECK (EditPlistAddString (&Plist, Sub, "Z", "a<b") == EFI_INVALID_PARAMETER);
  CHECK (EditPlistAddString (&Plist, Find (&Plist, "Flag"), "Z", "1") == EFI_UNSUPPORTED);
  CHECK_TEXT (
    &Plist,
    HEAD
    "<dict>\n"
    "\t<!-- a comment which must survive -->\n"
    "\t<key>Flag</key>\n"
    "\t<true/>\n"
    "\t<key>Num</key>\n"
    "\t<integer>5</integer>\n"
    "\t<key>Str</key>\n"
    "\t<string>abc</string>\n"
    "\t<key>Bin</key>\n"
    "\t<data>AAAA</data>\n"
    "\t<key>Empty</key>\n"
    "\t<dict/>\n"
    "\t<key>Sub</key>\n"
    "\t<dict>\n"
    "\t\t<key>X</key>\n"
    "\t\t<string>1</string>\n"
    "\t\t<key>Y</key>\n"
    "\t\t<string>2</string>\n"
    "\t</dict>\n"
    "\t<key>List</key>\n"
    "\t<array>\n"
    "\t\t<string>one</string>\n"
    "\t\t<false/>\n"
    "\t</array>\n"
    "</dict>\n"
    "</plist>\n"
    );

  //
  // Nested dictionary, plus a second entry at the same place which has to come after the first.
  //
  CHECK (!EFI_ERROR (EditPlistAddDictWithString (&Plist, Sub, "Z", "Inner", "Value")));
  CHECK (!EFI_ERROR (EditPlistAddString (&Plist, Sub, "W", "3")));
  CHECK_TEXT (
    &Plist,
    HEAD
    "<dict>\n"
    "\t<!-- a comment which must survive -->\n"
    "\t<key>Flag</key>\n"
    "\t<true/>\n"
    "\t<key>Num</key>\n"
    "\t<integer>5</integer>\n"
    "\t<key>Str</key>\n"
    "\t<string>abc</string>\n"
    "\t<key>Bin</key>\n"
    "\t<data>AAAA</data>\n"
    "\t<key>Empty</key>\n"
    "\t<dict/>\n"
    "\t<key>Sub</key>\n"
    "\t<dict>\n"
    "\t\t<key>X</key>\n"
    "\t\t<string>1</string>\n"
    "\t\t<key>Y</key>\n"
    "\t\t<string>2</string>\n"
    "\t\t<key>Z</key>\n"
    "\t\t<dict>\n"
    "\t\t\t<key>Inner</key>\n"
    "\t\t\t<string>Value</string>\n"
    "\t\t</dict>\n"
    "\t\t<key>W</key>\n"
    "\t\t<string>3</string>\n"
    "\t</dict>\n"
    "\t<key>List</key>\n"
    "\t<array>\n"
    "\t\t<string>one</string>\n"
    "\t\t<false/>\n"
    "\t</array>\n"
    "</dict>\n"
    "</plist>\n"
    );
  EditPlistFree (&Plist);

  //
  // Into an empty dictionary written as <dict/>.
  //
  CHECK (!EFI_ERROR (EditPlistLoad (&Plist, mTabbed, sizeof (mTabbed) - 1)));
  CHECK (!EFI_ERROR (EditPlistAddString (&Plist, Find (&Plist, "Empty"), "Y", "2")));
  CHECK_TEXT (
    &Plist,
    HEAD
    "<dict>\n"
    "\t<!-- a comment which must survive -->\n"
    "\t<key>Flag</key>\n"
    "\t<true/>\n"
    "\t<key>Num</key>\n"
    "\t<integer>5</integer>\n"
    "\t<key>Str</key>\n"
    "\t<string>abc</string>\n"
    "\t<key>Bin</key>\n"
    "\t<data>AAAA</data>\n"
    "\t<key>Empty</key>\n"
    "\t<dict>\n"
    "\t\t<key>Y</key>\n"
    "\t\t<string>2</string>\n"
    "\t</dict>\n"
    "\t<key>Sub</key>\n"
    "\t<dict>\n"
    "\t\t<key>X</key>\n"
    "\t\t<string>1</string>\n"
    "\t</dict>\n"
    "\t<key>List</key>\n"
    "\t<array>\n"
    "\t\t<string>one</string>\n"
    "\t\t<false/>\n"
    "\t</array>\n"
    "</dict>\n"
    "</plist>\n"
    );
  EditPlistFree (&Plist);
}

STATIC
VOID
TestLayouts (
  VOID
  )
{
  EDIT_PLIST  Plist;
  XML_NODE    *Sub;

  //
  // Windows line endings are kept.
  //
  STATIC CONST CHAR8  Crlf[] =
    "<plist version=\"1.0\">\r\n"
    "<dict>\r\n"
    "  <key>Sub</key>\r\n"
    "  <dict>\r\n"
    "    <key>X</key>\r\n"
    "    <string>1</string>\r\n"
    "  </dict>\r\n"
    "  <key>E</key>\r\n"
    "  <dict/>\r\n"
    "</dict>\r\n"
    "</plist>\r\n";

  CHECK (!EFI_ERROR (EditPlistLoad (&Plist, Crlf, sizeof (Crlf) - 1)));
  CHECK (!EFI_ERROR (EditPlistAddString (&Plist, Find (&Plist, "Sub"), "Y", "2")));
  CHECK (!EFI_ERROR (EditPlistAddString (&Plist, Find (&Plist, "E"), "Y", "2")));
  CHECK_TEXT (
    &Plist,
    "<plist version=\"1.0\">\r\n"
    "<dict>\r\n"
    "  <key>Sub</key>\r\n"
    "  <dict>\r\n"
    "    <key>X</key>\r\n"
    "    <string>1</string>\r\n"
    "    <key>Y</key>\r\n"
    "    <string>2</string>\r\n"
    "  </dict>\r\n"
    "  <key>E</key>\r\n"
    "  <dict>\r\n"
    "      <key>Y</key>\r\n"
    "      <string>2</string>\r\n"
    "  </dict>\r\n"
    "</dict>\r\n"
    "</plist>\r\n"
    );
  EditPlistFree (&Plist);

  //
  // Everything on one line.
  //
  STATIC CONST CHAR8  Inline[] =
    "<plist version=\"1.0\"><dict><key>Sub</key><dict><key>X</key><string>1</string></dict>"
    "<key>E</key><dict></dict><key>F</key><dict/></dict></plist>";

  CHECK (!EFI_ERROR (EditPlistLoad (&Plist, Inline, sizeof (Inline) - 1)));
  Sub = Find (&Plist, "Sub");
  CHECK (!EFI_ERROR (EditPlistAddString (&Plist, Sub, "Y", "2")));
  CHECK (!EFI_ERROR (EditPlistAddString (&Plist, Find (&Plist, "E"), "Y", "2")));
  CHECK (!EFI_ERROR (EditPlistAddDictWithString (&Plist, Find (&Plist, "F"), "Y", "I", "V")));
  CHECK_TEXT (
    &Plist,
    "<plist version=\"1.0\"><dict><key>Sub</key><dict><key>X</key><string>1</string><key>Y</key><string>2</string></dict>"
    "<key>E</key><dict><key>Y</key><string>2</string></dict>"
    "<key>F</key><dict><key>Y</key><dict><key>I</key><string>V</string></dict></dict></dict></plist>"
    );
  EditPlistFree (&Plist);
}

STATIC
VOID
TestMalformed (
  VOID
  )
{
  EDIT_PLIST  Plist;

  CHECK (EditPlistLoad (&Plist, "", 0) == EFI_INVALID_PARAMETER);
  CHECK (EditPlistLoad (&Plist, "<plist><dict>", 13) == EFI_INVALID_PARAMETER);
  CHECK (EditPlistLoad (&Plist, "<plist><array></array></plist>", 30) == EFI_INVALID_PARAMETER);
  CHECK (EditPlistLoad (&Plist, "<html><dict/></html>", 20) == EFI_INVALID_PARAMETER);
  //
  // A dangling key in the root is a broken document, but deeper it is tolerated.
  //
  STATIC CONST CHAR8  Dangling[] = "<plist><dict><key>A</key><true/><key>B</key></dict></plist>";
  STATIC CONST CHAR8  Nested[]   = "<plist><dict><key>N</key><dict><key>A</key><true/><key>B</key></dict></dict></plist>";

  CHECK (EditPlistLoad (&Plist, Dangling, sizeof (Dangling) - 1) == EFI_INVALID_PARAMETER);
  CHECK (!EFI_ERROR (EditPlistLoad (&Plist, Nested, sizeof (Nested) - 1)));
  CHECK (EditPlistChildCount (EditPlistFindChild (Plist.Root, "N")) == 1);
  EditPlistFree (&Plist);
}

/**
  Visit all leaves.
**/
typedef struct {
  UINT32    Path[32];
  UINT32    Depth;
} LEAF_PATH;

STATIC
XML_NODE *
FollowPath (
  IN EDIT_PLIST       *Plist,
  IN CONST LEAF_PATH  *Path
  )
{
  XML_NODE  *Node;
  UINT32    Index;

  Node = Plist->Root;
  for (Index = 0; Index < Path->Depth; ++Index) {
    Node = EditPlistChildValue (Node, Path->Path[Index]);
  }

  return Node;
}

STATIC UINT32  mBooleans;
STATIC UINT32  mEdited;

STATIC
VOID
CollectBooleans (
  IN     EDIT_PLIST  *Plist,
  IN     XML_NODE    *Node,
  IN OUT LEAF_PATH   *Path,
  IN     UINT32      BaselineErrors,
  IN     BOOLEAN     Verbose
  )
{
  UINT32      Index;
  UINT32      Count;
  EDIT_PLIST  Copy;
  XML_NODE    *Leaf;
  CHAR8       *Text;
  UINT32      Size;
  UINT32      Start;
  UINT32      End;
  UINT32      Errors;
  CHAR8       *Expected;
  UINT32      ExpectedSize;
  CONST CHAR8 *NewElement;

  if (EditPlistNodeKind (Node) == EditPlistKindBoolean) {
    ++mBooleans;

    //
    // Toggle this very boolean in a fresh copy and verify that the result is the
    // original with nothing but this element replaced, and that it is still loadable.
    //
    CHECK (!EFI_ERROR (EditPlistLoad (&Copy, Plist->Source, Plist->SourceSize)));
    Leaf = FollowPath (&Copy, Path);
    CHECK (Leaf != NULL && EditPlistNodeKind (Leaf) == EditPlistKindBoolean);
    CHECK (XmlNodeSourceRange (Leaf, &Start, &End));
    NewElement = EditPlistGetBoolean (&Copy, Leaf) ? "<false/>" : "<true/>";
    CHECK (!EFI_ERROR (EditPlistSetBoolean (&Copy, Leaf, !EditPlistGetBoolean (&Copy, Leaf))));
    CHECK (!EFI_ERROR (EditPlistRender (&Copy, &Text, &Size)));

    //
    // Nothing but the element itself, whatever the ranges say.
    //
    CHECK (Size == Plist->SourceSize + AsciiStrLen (NewElement) - AsciiStrLen (EditPlistGetBoolean (Plist, FollowPath (Plist, Path)) ? "<true/>" : "<false/>"));
    CHECK (CompareMem (&Plist->Source[Start], EditPlistGetBoolean (Plist, FollowPath (Plist, Path)) ? "<true/>" : "<false/>", AsciiStrLen (NewElement) - 1) == 0);

    ExpectedSize = Start + (UINT32)AsciiStrLen (NewElement) + (Plist->SourceSize - End);
    Expected     = AllocatePool (ExpectedSize + 1);
    CopyMem (Expected, Plist->Source, Start);
    CopyMem (Expected + Start, NewElement, AsciiStrLen (NewElement));
    CopyMem (Expected + Start + AsciiStrLen (NewElement), Plist->Source + End, Plist->SourceSize - End);
    Expected[ExpectedSize] = '\0';
    CHECK (Size == ExpectedSize && CompareMem (Text, Expected, Size) == 0);

    Errors = 0;
    CHECK (!EFI_ERROR (EditPlistCountErrors (Text, Size, &Errors)));
    CHECK (Errors == BaselineErrors);

    ++mEdited;
    FreePool (Expected);
    FreePool (Text);
    EditPlistFree (&Copy);
    return;
  }

  Count = EditPlistChildCount (Node);
  for (Index = 0; Index < Count; ++Index) {
    Path->Path[Path->Depth++] = Index;
    CollectBooleans (Plist, EditPlistChildValue (Node, Index), Path, BaselineErrors, Verbose);
    --Path->Depth;
  }
}

//
// Moving the elements of arrays around.
//

#define KEXTS_HEAD  \
  HEAD \
  "<dict>\n" \
  "\t<key>Kernel</key>\n" \
  "\t<dict>\n" \
  "\t\t<key>Add</key>\n" \
  "\t\t<array>\n"

#define KEXTS_TAIL  \
  "\t\t</array>\n" \
  "\t</dict>\n" \
  "</dict>\n" \
  "</plist>\n"

#define KEXT_A(Enabled)  \
  "\t\t\t<dict>\n" \
  "\t\t\t\t<key>BundlePath</key>\n" \
  "\t\t\t\t<string>A.kext</string>\n" \
  "\t\t\t\t<key>Enabled</key>\n" \
  "\t\t\t\t<" Enabled "/>\n" \
  "\t\t\t</dict>\n"

#define KEXT_B(Enabled)  \
  "\t\t\t<dict>\n" \
  "\t\t\t\t<key>BundlePath</key>\n" \
  "\t\t\t\t<string>B.kext</string>\n" \
  "\t\t\t\t<key>Enabled</key>\n" \
  "\t\t\t\t<" Enabled "/>\n" \
  "\t\t\t</dict>\n"

#define KEXT_C_NO_ENABLED  \
  "\t\t\t<dict>\n" \
  "\t\t\t\t<key>BundlePath</key>\n" \
  "\t\t\t\t<string>C.kext</string>\n" \
  "\t\t\t</dict>\n"

#define KEXT_C(Enabled)  \
  "\t\t\t<dict>\n" \
  "\t\t\t\t<key>BundlePath</key>\n" \
  "\t\t\t\t<string>C.kext</string>\n" \
  "\t\t\t\t<key>Enabled</key>\n" \
  "\t\t\t\t<" Enabled "/>\n" \
  "\t\t\t</dict>\n"

STATIC CONST CHAR8  mKexts[] =
  KEXTS_HEAD
  "\t\t\t<!-- first -->\n"
  KEXT_A ("true")
  "\t\t\t<!-- between -->\n"
  KEXT_B ("false")
  KEXT_C_NO_ENABLED
  KEXTS_TAIL;

STATIC
XML_NODE *
KextsArray (
  IN EDIT_PLIST  *Plist
  )
{
  return EditPlistFindChild (EditPlistFindChild (Plist->Root, "Kernel"), "Add");
}

/**
  Get the BundlePath of the element at a position of the rendered text, or "" if there is none.
**/
STATIC
VOID
RenderedPaths (
  IN  EDIT_PLIST  *Plist,
  OUT CHAR8       *Paths,
  IN  UINTN       Size
  )
{
  CHAR8       *Text;
  UINT32      TextSize;
  EDIT_PLIST  Again;
  XML_NODE    *Array;
  UINT32      Index;

  Paths[0] = '\0';
  if (EFI_ERROR (EditPlistRender (Plist, &Text, &TextSize))) {
    AsciiStrCpyS (Paths, Size, "render failed");
    return;
  }

  if (EFI_ERROR (EditPlistLoad (&Again, Text, TextSize))) {
    AsciiStrCpyS (Paths, Size, "cannot load");
    FreePool (Text);
    return;
  }

  Array = KextsArray (&Again);
  for (Index = 0; Index < EditPlistChildCount (Array); ++Index) {
    AsciiStrCatS (Paths, Size, EditPlistGetContent (&Again, EditPlistFindChild (EditPlistChildValue (Array, Index), "BundlePath")));
    AsciiStrCatS (Paths, Size, " ");
  }

  EditPlistFree (&Again);
  FreePool (Text);
}

STATIC
VOID
TestReorder (
  VOID
  )
{
  EDIT_PLIST  Plist;
  XML_NODE    *Array;
  XML_NODE    *Other;
  CHAR8       Paths[128];
  UINT32      Order[3];
  CHAR8       *Text;
  UINT32      Size;

  CHECK (!EFI_ERROR (EditPlistLoad (&Plist, mKexts, sizeof (mKexts) - 1)));
  Array = KextsArray (&Plist);
  CHECK (Array != NULL && EditPlistChildCount (Array) == 3);

  //
  // Nothing has been moved: nothing changes.
  //
  CHECK (EditPlistPatchCount (&Plist) == 0);
  CHECK (!EditPlistArrayIsRearranged (&Plist, Array));
  CHECK_TEXT (&Plist, mKexts);
  CHECK (EditPlistArrayOriginalIndex (&Plist, Array, 1) == 1);

  //
  // The first goes to the end, the comments stay where they were.
  //
  CHECK (!EFI_ERROR (EditPlistArrayMove (&Plist, Array, 0, 2)));
  CHECK (EditPlistPatchCount (&Plist) == 1);
  CHECK (EditPlistArrayIsRearranged (&Plist, Array));
  CHECK (EditPlistArrayOriginalIndex (&Plist, Array, 0) == 1);
  CHECK (EditPlistArrayOriginalIndex (&Plist, Array, 1) == 2);
  CHECK (EditPlistArrayOriginalIndex (&Plist, Array, 2) == 0);
  CHECK_TEXT (
    &Plist,
    KEXTS_HEAD
    "\t\t\t<!-- first -->\n"
    KEXT_B ("false")
    "\t\t\t<!-- between -->\n"
    KEXT_C_NO_ENABLED
    KEXT_A ("true")
    KEXTS_TAIL
    );

  //
  // Moving it back brings the text back too.
  //
  CHECK (!EFI_ERROR (EditPlistArrayMove (&Plist, Array, 2, 0)));
  CHECK (!EditPlistArrayIsRearranged (&Plist, Array));
  CHECK (EditPlistPatchCount (&Plist) == 0);
  CHECK_TEXT (&Plist, mKexts);

  //
  // Changes to the elements go with them.
  //
  CHECK (!EFI_ERROR (EditPlistSetBoolean (&Plist, EditPlistFindChild (EditPlistChildValue (Array, 1), "Enabled"), TRUE)));
  CHECK (!EFI_ERROR (EditPlistSetBoolean (&Plist, EditPlistFindChild (EditPlistChildValue (Array, 0), "Enabled"), FALSE)));
  CHECK (!EFI_ERROR (EditPlistArrayMove (&Plist, Array, 1, 0)));
  CHECK (EditPlistPatchCount (&Plist) == 3);
  CHECK_TEXT (
    &Plist,
    KEXTS_HEAD
    "\t\t\t<!-- first -->\n"
    KEXT_B ("true")
    "\t\t\t<!-- between -->\n"
    KEXT_A ("false")
    KEXT_C_NO_ENABLED
    KEXTS_TAIL
    );

  //
  // What is added to an element goes with it as well, and can be taken back.
  //
  CHECK (!EFI_ERROR (EditPlistAddBoolean (&Plist, EditPlistChildValue (Array, 2), "Enabled", TRUE)));
  CHECK (!EFI_ERROR (EditPlistArrayMove (&Plist, Array, 2, 0)));
  RenderedPaths (&Plist, Paths, sizeof (Paths));
  CHECK (AsciiStrCmp (Paths, "C.kext B.kext A.kext ") == 0);
  CHECK (!EFI_ERROR (EditPlistArrayMove (&Plist, Array, 0, 2)));
  RenderedPaths (&Plist, Paths, sizeof (Paths));
  CHECK (AsciiStrCmp (Paths, "B.kext A.kext C.kext ") == 0);
  EditPlistRevertAdded (&Plist, EditPlistChildValue (Array, 2));
  CHECK_TEXT (
    &Plist,
    KEXTS_HEAD
    "\t\t\t<!-- first -->\n"
    KEXT_B ("true")
    "\t\t\t<!-- between -->\n"
    KEXT_A ("false")
    KEXT_C_NO_ENABLED
    KEXTS_TAIL
    );

  //
  // A different order can be given as a whole.
  //
  Order[0] = 2;
  Order[1] = 0;
  Order[2] = 1;
  CHECK (!EFI_ERROR (EditPlistArraySetOrder (&Plist, Array, Order, 3)));
  CHECK (EditPlistArrayOriginalIndex (&Plist, Array, 0) == 2);
  RenderedPaths (&Plist, Paths, sizeof (Paths));
  CHECK (AsciiStrCmp (Paths, "C.kext A.kext B.kext ") == 0);

  Order[1] = 2;
  CHECK (EditPlistArraySetOrder (&Plist, Array, Order, 3) == EFI_INVALID_PARAMETER);
  Order[1] = 3;
  CHECK (EditPlistArraySetOrder (&Plist, Array, Order, 3) == EFI_INVALID_PARAMETER);
  CHECK (EditPlistArraySetOrder (&Plist, Array, Order, 2) == EFI_INVALID_PARAMETER);
  RenderedPaths (&Plist, Paths, sizeof (Paths));
  CHECK (AsciiStrCmp (Paths, "C.kext A.kext B.kext ") == 0);

  //
  // Wrong requests.
  //
  CHECK (EditPlistArrayMove (&Plist, Array, 3, 0) == EFI_INVALID_PARAMETER);
  CHECK (EditPlistArrayMove (&Plist, Array, 0, 3) == EFI_INVALID_PARAMETER);
  CHECK (EditPlistArrayMove (&Plist, EditPlistChildValue (Array, 0), 0, 0) == EFI_INVALID_PARAMETER);
  CHECK (EditPlistArrayMove (&Plist, Plist.Root, 0, 0) == EFI_INVALID_PARAMETER);
  Other = EditPlistFindChild (Plist.Root, "Kernel");
  CHECK (EditPlistArrayMove (&Plist, Other, 0, 1) == EFI_INVALID_PARAMETER);

  //
  // Everything can be taken back at once.
  //
  EditPlistArrayRevertOrder (&Plist, Array);
  CHECK (!EditPlistArrayIsRearranged (&Plist, Array));
  CHECK (EditPlistPatchCount (&Plist) == 2);
  EditPlistRevert (&Plist, EditPlistFindChild (EditPlistChildValue (Array, 0), "Enabled"));
  EditPlistRevert (&Plist, EditPlistFindChild (EditPlistChildValue (Array, 1), "Enabled"));
  CHECK (EditPlistPatchCount (&Plist) == 0);
  CHECK_TEXT (&Plist, mKexts);

  EditPlistFree (&Plist);

  //
  // Only one array at a time, and no array in the document that cannot be told apart.
  //
  {
    STATIC CONST CHAR8  Two[] =
      HEAD
      "<dict>\n"
      "<key>One</key><array><string>1</string><string>2</string></array>\n"
      "<key>Two</key><array><string>a</string><string>b</string></array>\n"
      "<key>Empty</key><array/>\n"
      "<key>Single</key><array><true/></array>\n"
      "</dict>\n"
      "</plist>\n";

    CHECK (!EFI_ERROR (EditPlistLoad (&Plist, Two, sizeof (Two) - 1)));
    CHECK (!EFI_ERROR (EditPlistArrayMove (&Plist, Find (&Plist, "One"), 0, 1)));
    CHECK (EditPlistArrayMove (&Plist, Find (&Plist, "Two"), 0, 1) == EFI_UNSUPPORTED);
    CHECK (EditPlistArraySetOrder (&Plist, Find (&Plist, "Two"), (UINT32[]) { 1, 0 }, 2) == EFI_UNSUPPORTED);
    CHECK (EditPlistArrayMove (&Plist, Find (&Plist, "Empty"), 0, 0) == EFI_INVALID_PARAMETER);
    CHECK (EditPlistArrayOriginalIndex (&Plist, Find (&Plist, "Two"), 0) == 0);
    CHECK (!EFI_ERROR (EditPlistRender (&Plist, &Text, &Size)));
    CHECK (AsciiStrStr (Text, "<array><string>2</string><string>1</string></array>") != NULL);
    CHECK (AsciiStrStr (Text, "<array><string>a</string><string>b</string></array>") != NULL);
    FreePool (Text);

    //
    // A single element has nowhere to go, and nothing is recorded for it.
    //
    EditPlistArrayRevertOrder (&Plist, Find (&Plist, "One"));
    CHECK (!EFI_ERROR (EditPlistArrayMove (&Plist, Find (&Plist, "Single"), 0, 0)));
    CHECK (!EditPlistArrayIsRearranged (&Plist, Find (&Plist, "Single")));
    CHECK (EditPlistPatchCount (&Plist) == 0);
    EditPlistFree (&Plist);
  }

  //
  // Windows line breaks and spaces are kept as they are.
  //
  {
    STATIC CONST CHAR8  Crlf[] =
      HEAD
      "<dict>\r\n"
      "  <key>L</key>\r\n"
      "  <array>\r\n"
      "    <dict><key>N</key><string>x</string></dict>\r\n"
      "    <dict>\r\n"
      "      <key>N</key>\r\n"
      "      <string>y</string>\r\n"
      "    </dict>\r\n"
      "  </array>\r\n"
      "</dict>\r\n"
      "</plist>\r\n";

    CHECK (!EFI_ERROR (EditPlistLoad (&Plist, Crlf, sizeof (Crlf) - 1)));
    CHECK (!EFI_ERROR (EditPlistArrayMove (&Plist, Find (&Plist, "L"), 0, 1)));
    CHECK_TEXT (
      &Plist,
      HEAD
      "<dict>\r\n"
      "  <key>L</key>\r\n"
      "  <array>\r\n"
      "    <dict>\r\n"
      "      <key>N</key>\r\n"
      "      <string>y</string>\r\n"
      "    </dict>\r\n"
      "    <dict><key>N</key><string>x</string></dict>\r\n"
      "  </array>\r\n"
      "</dict>\r\n"
      "</plist>\r\n"
      );
    EditPlistFree (&Plist);
  }
}

/**
  Pseudo random numbers, which are the same everywhere.
**/
STATIC
UINT32
NextRandom (
  IN OUT UINT32  *State
  )
{
  *State = *State * 1103515245U + 12345U;
  return (*State >> 8) & 0xFFFFFFU;
}

/**
  Move and switch the kexts of a configuration in every which way, and compare what comes out
  with the text put together by hand from the pieces of the original.
**/
STATIC
VOID
TestReorderFuzz (
  IN CONST UINT8  *Data,
  IN UINT32       Size,
  IN CONST CHAR8  *FileName
  )
{
  EDIT_PLIST  Plist;
  XML_NODE    *Array;
  XML_NODE    *Enabled[64];
  UINT32      Starts[64];
  UINT32      Ends[64];
  UINT32      ValueStarts[64];
  UINT32      ValueEnds[64];
  BOOLEAN     Value[64];
  BOOLEAN     Touched[64];
  UINT32      Order[64];
  UINT32      Count;
  UINT32      Index;
  UINT32      Round;
  UINT32      Seed;
  UINT32      From;
  UINT32      To;
  UINT32      Position;
  UINT32      Element;
  CHAR8       *Text;
  UINT32      TextSize;
  CHAR8       *Expected;
  UINTN       ExpectedSize;
  UINTN       Capacity;
  UINTN       Used;
  CHAR8       Paths[2048];
  CHAR8       Wanted[2048];
  UINT32      Moves;
  UINT32      Flips;

  CHECK (!EFI_ERROR (EditPlistLoad (&Plist, Data, Size)));
  Array = KextsArray (&Plist);
  CHECK (Array != NULL);
  Count = Array != NULL ? EditPlistChildCount (Array) : 0;
  CHECK (Count > 3 && Count <= 64);
  if (Count <= 3 || Count > 64) {
    EditPlistFree (&Plist);
    return;
  }

  for (Index = 0; Index < Count; ++Index) {
    Enabled[Index] = EditPlistFindChild (EditPlistChildValue (Array, Index), "Enabled");
    CHECK (Enabled[Index] != NULL);
    CHECK (XmlNodeSourceRange (EditPlistChildValue (Array, Index), &Starts[Index], &Ends[Index]));
    CHECK (XmlNodeSourceRange (Enabled[Index], &ValueStarts[Index], &ValueEnds[Index]));
    Value[Index]   = EditPlistGetBoolean (&Plist, Enabled[Index]);
    Touched[Index] = FALSE;
    Order[Index]   = Index;
  }

  Capacity = Size * 2 + 1024;
  Expected = AllocatePool (Capacity);
  Seed     = 12345;
  Moves    = 0;
  Flips    = 0;

  for (Round = 0; Round < 3000; ++Round) {
    switch (NextRandom (&Seed) % 8) {
      case 0:
      case 1:
      case 2:
      case 3:
        From = NextRandom (&Seed) % Count;
        To   = NextRandom (&Seed) % Count;
        CHECK (!EFI_ERROR (EditPlistArrayMove (&Plist, Array, From, To)));
        Element = Order[From];
        if (From < To) {
          for (Index = From; Index < To; ++Index) {
            Order[Index] = Order[Index + 1];
          }
        } else {
          for (Index = From; Index > To; --Index) {
            Order[Index] = Order[Index - 1];
          }
        }

        Order[To] = Element;
        ++Moves;
        break;
      case 4:
      case 5:
      case 6:
        Element = NextRandom (&Seed) % Count;
        Value[Element]   = !Value[Element];
        Touched[Element] = TRUE;
        CHECK (!EFI_ERROR (EditPlistSetBoolean (&Plist, Enabled[Element], Value[Element])));
        ++Flips;
        break;
      default:
        //
        // Starting over now and then.
        //
        EditPlistArrayRevertOrder (&Plist, Array);
        for (Index = 0; Index < Count; ++Index) {
          Order[Index] = Index;
        }

        break;
    }

    //
    // What is put together by hand.
    //
    Used     = 0;
    Position = 0;
    for (Index = 0; Index < Count; ++Index) {
      CopyMem (&Expected[Used], &Data[Position], Starts[Index] - Position);
      Used    += Starts[Index] - Position;
      Element  = Order[Index];

      if (Touched[Element]) {
        CopyMem (&Expected[Used], &Data[Starts[Element]], ValueStarts[Element] - Starts[Element]);
        Used += ValueStarts[Element] - Starts[Element];
        AsciiStrCpyS (&Expected[Used], 16, Value[Element] ? "<true/>" : "<false/>");
        Used += Value[Element] ? 7 : 8;
        CopyMem (&Expected[Used], &Data[ValueEnds[Element]], Ends[Element] - ValueEnds[Element]);
        Used += Ends[Element] - ValueEnds[Element];
      } else {
        CopyMem (&Expected[Used], &Data[Starts[Element]], Ends[Element] - Starts[Element]);
        Used += Ends[Element] - Starts[Element];
      }

      Position = Ends[Index];
    }

    CopyMem (&Expected[Used], &Data[Position], Size - Position);
    Used += Size - Position;
    Expected[Used] = '\0';
    ExpectedSize = Used;

    CHECK (!EFI_ERROR (EditPlistRender (&Plist, &Text, &TextSize)));
    CHECK (TextSize == ExpectedSize && CompareMem (Text, Expected, ExpectedSize) == 0);
    if (TextSize != ExpectedSize || CompareMem (Text, Expected, ExpectedSize) != 0) {
      printf ("%s: the text is wrong after %u rounds\n", FileName, Round);
      FreePool (Text);
      break;
    }

    FreePool (Text);

    //
    // The order that the document tells.
    //
    Paths[0]  = '\0';
    Wanted[0] = '\0';
    RenderedPaths (&Plist, Paths, sizeof (Paths));
    for (Index = 0; Index < Count; ++Index) {
      CHECK (EditPlistArrayOriginalIndex (&Plist, Array, Index) == Order[Index]);
      AsciiStrCatS (Wanted, sizeof (Wanted), EditPlistGetContent (&Plist, EditPlistFindChild (EditPlistChildValue (Array, Order[Index]), "BundlePath")));
      AsciiStrCatS (Wanted, sizeof (Wanted), " ");
    }

    CHECK (AsciiStrCmp (Paths, Wanted) == 0);
  }

  printf ("%s: moved %u times and switched %u kexts, the text was right every time\n", FileName, Moves, Flips);

  FreePool (Expected);
  EditPlistFree (&Plist);
}

//
// Which kexts need which.
//

#define INFO_HEAD  \
  HEAD \
  "<dict>\n"

#define INFO_TAIL  \
  "</dict>\n" \
  "</plist>\n"

typedef struct {
  CONST CHAR8    *Id;
  CONST CHAR8    *Libs[5];
  BOOLEAN        Enabled;
  BOOLEAN        HasInfo;
  BOOLEAN        FilesMissing;
} KEXT_SPEC;

/**
  Make what the analysis works with from short descriptions.
**/
STATIC
VOID
SpecToInfo (
  IN  CONST KEXT_SPEC  *Specs,
  IN  UINT32           Count,
  OUT ED_KEXT_INFO     *Infos,
  OUT BOOLEAN          *Enabled
  )
{
  UINT32  Index;

  for (Index = 0; Index < Count; ++Index) {
    ZeroMem (&Infos[Index], sizeof (Infos[Index]));
    Infos[Index].Identifier   = (CHAR8 *)Specs[Index].Id;
    Infos[Index].Libraries    = (CHAR8 **)Specs[Index].Libs;
    Infos[Index].HasInfo      = Specs[Index].HasInfo;
    Infos[Index].FilesMissing = Specs[Index].FilesMissing;
    while ((Infos[Index].LibraryCount < ARRAY_SIZE (Specs[Index].Libs)) && (Specs[Index].Libs[Infos[Index].LibraryCount] != NULL)) {
      ++Infos[Index].LibraryCount;
    }

    Enabled[Index] = Specs[Index].Enabled;
  }
}

STATIC
VOID
TestKextInfo (
  VOID
  )
{
  STATIC CONST CHAR8  Plain[] =
    INFO_HEAD
    "\t<key>CFBundleIdentifier</key>\n"
    "\t<string>as.vit9696.VirtualSMC</string>\n"
    "\t<key>OSBundleLibraries</key>\n"
    "\t<dict>\n"
    "\t\t<key>as.vit9696.Lilu</key>\n"
    "\t\t<string>1.0</string>\n"
    "\t\t<key>com.apple.kpi.iokit</key>\n"
    "\t\t<string>8.0.0</string>\n"
    "\t\t<key>com.apple.driver.AppleSMC</key>\n"
    "\t\t<string>1.0</string>\n"
    "\t\t<key>org.example.Other</key>\n"
    "\t\t<string>1.0</string>\n"
    "\t</dict>\n"
    "\t<key>OSBundleLibraries_x86_64</key>\n"
    "\t<dict>\n"
    "\t\t<key>as.vit9696.Lilu</key>\n"
    "\t\t<string>1.0</string>\n"
    "\t\t<key>org.example.More</key>\n"
    "\t\t<string>1.0</string>\n"
    "\t</dict>\n"
    INFO_TAIL;
  STATIC CONST CHAR8  Bare[] =
    INFO_HEAD
    "\t<key>CFBundleName</key>\n"
    "\t<string>Nothing</string>\n"
    INFO_TAIL;
  STATIC CONST CHAR8  Odd[] =
    INFO_HEAD
    "\t<key>CFBundleIdentifier</key>\n"
    "\t<integer>5</integer>\n"
    "\t<key>OSBundleLibraries</key>\n"
    "\t<array/>\n"
    INFO_TAIL;

  ED_KEXT_INFO  Info;

  CHECK (!EFI_ERROR (EdKextParseInfo (Plain, sizeof (Plain) - 1, &Info)));
  CHECK (Info.HasInfo && !Info.FilesMissing);
  CHECK (Info.Identifier != NULL && AsciiStrCmp (Info.Identifier, "as.vit9696.VirtualSMC") == 0);
  //
  // What the system provides is left out, and what is said twice is counted once.
  //
  CHECK (Info.LibraryCount == 3);
  if (Info.LibraryCount == 3) {
    CHECK (AsciiStrCmp (Info.Libraries[0], "as.vit9696.Lilu") == 0);
    CHECK (AsciiStrCmp (Info.Libraries[1], "org.example.Other") == 0);
    CHECK (AsciiStrCmp (Info.Libraries[2], "org.example.More") == 0);
  }

  EdKextFreeInfo (&Info);
  CHECK (Info.Identifier == NULL && Info.Libraries == NULL && Info.LibraryCount == 0 && !Info.HasInfo);

  CHECK (!EFI_ERROR (EdKextParseInfo (Bare, sizeof (Bare) - 1, &Info)));
  CHECK (Info.HasInfo && Info.Identifier == NULL && Info.LibraryCount == 0);
  EdKextFreeInfo (&Info);

  //
  // Things of a wrong kind are not understood, which is not a failure.
  //
  CHECK (!EFI_ERROR (EdKextParseInfo (Odd, sizeof (Odd) - 1, &Info)));
  CHECK (Info.HasInfo && Info.Identifier == NULL && Info.LibraryCount == 0);
  EdKextFreeInfo (&Info);

  CHECK (EdKextParseInfo ("not a plist", 11, &Info) == EFI_INVALID_PARAMETER);
  CHECK (!Info.HasInfo && Info.Identifier == NULL);
  CHECK (EdKextParseInfo ("<plist><array/></plist>", 23, &Info) == EFI_INVALID_PARAMETER);
  CHECK (EdKextParseInfo (Plain, 0, &Info) == EFI_INVALID_PARAMETER);

  //
  // Many libraries are cut at some point.
  //
  {
    CHAR8   *Many;
    UINT32  Index;
    UINTN   Length;

    Many   = AllocatePool (65536);
    Length = (UINTN)AsciiSPrint (Many, 65536, "%a<key>OSBundleLibraries</key><dict>", INFO_HEAD);
    for (Index = 0; Index < ED_KEXT_MAX_LIBRARIES + 10; ++Index) {
      Length += AsciiSPrint (&Many[Length], 65536 - Length, "<key>org.example.Lib%u</key><string>1</string>", Index);
    }

    Length += AsciiSPrint (&Many[Length], 65536 - Length, "</dict>%a", INFO_TAIL);
    CHECK (!EFI_ERROR (EdKextParseInfo (Many, (UINT32)Length, &Info)));
    CHECK (Info.LibraryCount == ED_KEXT_MAX_LIBRARIES);
    CHECK (Info.LibraryCount > 0 && AsciiStrCmp (Info.Libraries[0], "org.example.Lib0") == 0);
    EdKextFreeInfo (&Info);
    FreePool (Many);
  }
}

STATIC
VOID
TestKextProblems (
  VOID
  )
{
  STATIC CONST KEXT_SPEC  Good[] = {
    { "L", { NULL },        TRUE, TRUE, FALSE },  // 0 Lilu
    { "V", { "L", NULL },   TRUE, TRUE, FALSE },  // 1 VirtualSMC
    { "P", { "L", "V" },    TRUE, TRUE, FALSE },  // 2 a plugin of both
    { "W", { "L", NULL },   TRUE, TRUE, FALSE },  // 3 something else
    { "N", { NULL },        FALSE, TRUE, FALSE }, // 4 a kext with a plugin, off
    { "NP", { "N", NULL },  FALSE, TRUE, FALSE }  // 5 the plugin, off
  };

  ED_KEXT_INFO     Infos[6];
  BOOLEAN          Enabled[6];
  ED_KEXT_PROBLEM  Problems[6];
  UINT32           Order[6];
  UINT32           Index;
  UINT32           Total;

  SpecToInfo (Good, ARRAY_SIZE (Good), Infos, Enabled);
  for (Index = 0; Index < 6; ++Index) {
    Order[Index] = Index;
  }

  //
  // All is right when what is needed is above, and what is off needs nothing.
  //
  Total = EdKextFindProblems (Infos, Enabled, 6, Order, NULL, 0, Problems);
  CHECK (Total == 0);
  for (Index = 0; Index < 6; ++Index) {
    CHECK (Problems[Index].Kind == EdKextOk && Problems[Index].LibraryProblems == 0 && Problems[Index].Provider == MAX_UINT32);
  }

  //
  // The needed one below: only the one who needs is blamed.
  //
  Order[0] = 3;
  Order[1] = 1;
  Order[2] = 2;
  Order[3] = 0;
  Total    = EdKextFindProblems (Infos, Enabled, 6, Order, NULL, 0, Problems);
  CHECK (Total == 3);
  CHECK (Problems[0].Kind == EdKextOk);
  CHECK (Problems[1].Kind == EdKextOrder && Problems[1].Provider == 0 && Problems[1].Library == 0 && Problems[1].LibraryProblems == 1);
  CHECK (Problems[2].Kind == EdKextOrder && Problems[2].LibraryProblems == 1);
  CHECK (Problems[3].Kind == EdKextOrder && Problems[3].Provider == 0);
  CHECK (Problems[4].Kind == EdKextOk && Problems[5].Kind == EdKextOk);

  //
  // Another position of the same thing is fine as well: the order in the file does not matter,
  // the places in the order do.
  //
  Order[0] = 4;
  Order[1] = 0;
  Order[2] = 1;
  Order[3] = 3;
  Order[4] = 2;
  Order[5] = 5;
  CHECK (EdKextFindProblems (Infos, Enabled, 6, Order, NULL, 0, Problems) == 0);

  //
  // Switched off and not there at all.
  //
  Enabled[0] = FALSE;
  CHECK (EdKextFindProblems (Infos, Enabled, 6, Order, NULL, 0, Problems) == 3);
  CHECK (Problems[1].Kind == EdKextDisabled && Problems[1].Provider == 0);
  CHECK (Problems[2].Kind == EdKextDisabled && Problems[2].Library == 0 && Problems[2].LibraryProblems == 1);
  CHECK (Problems[3].Kind == EdKextDisabled);
  Enabled[0] = TRUE;

  Infos[0].Identifier = (CHAR8 *)"Renamed";
  CHECK (EdKextFindProblems (Infos, Enabled, 6, Order, NULL, 0, Problems) == 3);
  CHECK (Problems[1].Kind == EdKextMissing && Problems[1].Provider == MAX_UINT32);
  CHECK (Problems[2].Kind == EdKextMissing && Problems[2].Library == 0 && Problems[2].LibraryProblems == 1);

  //
  // Something which is there from the start (Kernel > Force) is not missing.
  //
  {
    CONST CHAR8  *Provided[] = { "other", "L" };
    CONST CHAR8  *Elsewhere[] = { "other" };

    CHECK (EdKextFindProblems (Infos, Enabled, 6, Order, Provided, 2, Problems) == 0);
    CHECK (Problems[1].Kind == EdKextOk && Problems[2].Kind == EdKextOk && Problems[3].Kind == EdKextOk);
    CHECK (EdKextFindProblems (Infos, Enabled, 6, Order, Elsewhere, 1, Problems) == 3);
    CHECK (EdKextFindProblems (Infos, Enabled, 6, Order, NULL, 7, Problems) == 3);
  }

  Infos[0].Identifier = (CHAR8 *)"L";

  //
  // Another kext with the same identifier does the job.
  //
  Infos[4].Identifier = (CHAR8 *)"L";
  Enabled[4]          = TRUE;
  Order[0]            = 1;
  Order[1]            = 4;
  Order[2]            = 0;
  Order[3]            = 2;
  Order[4]            = 3;
  Order[5]            = 5;
  Total = EdKextFindProblems (Infos, Enabled, 6, Order, NULL, 0, Problems);
  CHECK (Problems[1].Kind == EdKextOrder);
  CHECK (Problems[2].Kind == EdKextOk);
  CHECK (Problems[3].Kind == EdKextOk);
  CHECK (Problems[5].Kind == EdKextOk);   // off, thus not looked at
  CHECK (Total == 1);
  Order[0] = 4;
  Order[1] = 1;
  CHECK (EdKextFindProblems (Infos, Enabled, 6, Order, NULL, 0, Problems) == 0);
  CHECK (Problems[1].Kind == EdKextOk);

  //
  // Kexts whose files are not there are not added, and add nothing.
  //
  Infos[4].FilesMissing = TRUE;
  CHECK (EdKextFindProblems (Infos, Enabled, 6, Order, NULL, 0, Problems) == 2);
  CHECK (Problems[4].Kind == EdKextNoFiles);
  CHECK (Problems[1].Kind == EdKextOrder);
  Infos[4].FilesMissing = FALSE;
  Infos[3].HasInfo      = FALSE;
  CHECK (EdKextFindProblems (Infos, Enabled, 6, Order, NULL, 0, Problems) == 1);
  CHECK (Problems[3].Kind == EdKextNoFiles);

  //
  // What is off is not looked at.
  //
  Enabled[3] = FALSE;
  CHECK (EdKextFindProblems (Infos, Enabled, 6, Order, NULL, 0, Problems) == 0);
  CHECK (Problems[3].Kind == EdKextOk);

  //
  // A kext does not wait for itself.
  //
  {
    STATIC CONST KEXT_SPEC  Self[] = {
      { "S", { "S", NULL }, TRUE, TRUE, FALSE }
    };

    SpecToInfo (Self, 1, Infos, Enabled);
    Order[0] = 0;
    CHECK (EdKextFindProblems (Infos, Enabled, 1, Order, NULL, 0, Problems) == 1);
    CHECK (Problems[0].Kind == EdKextMissing);
    CHECK (EdKextSort (Infos, Enabled, 1, Order) == 0);
  }

  CHECK (EdKextFindProblems (Infos, Enabled, 0, Order, NULL, 0, Problems) == 0);
}

STATIC
VOID
TestKextSort (
  VOID
  )
{
  STATIC CONST KEXT_SPEC  Wrong[] = {
    { "P", { "V", "L" },  TRUE, TRUE, FALSE },   // 0 needs V and L
    { "W", { "L", NULL }, TRUE, TRUE, FALSE },   // 1
    { "V", { "L", NULL }, TRUE, TRUE, FALSE },   // 2
    { "X", { NULL },      TRUE, TRUE, FALSE },   // 3 needs nothing
    { "L", { NULL },      TRUE, TRUE, FALSE }    // 4 needed by the rest
  };

  ED_KEXT_INFO     Infos[8];
  BOOLEAN          Enabled[8];
  ED_KEXT_PROBLEM  Problems[8];
  UINT32           Order[8];
  UINT32           Changed;
  UINT32           Index;

  SpecToInfo (Wrong, ARRAY_SIZE (Wrong), Infos, Enabled);
  for (Index = 0; Index < 5; ++Index) {
    Order[Index] = Index;
  }

  //
  // What is needed goes up in front of the first that needs it, the rest stays as it was.
  //
  Changed = EdKextSort (Infos, Enabled, 5, Order);
  CHECK (Changed == 5);
  CHECK (Order[0] == 4 && Order[1] == 2 && Order[2] == 0 && Order[3] == 1 && Order[4] == 3);
  CHECK (EdKextFindProblems (Infos, Enabled, 5, Order, NULL, 0, Problems) == 0);

  //
  // Once it is right nothing changes.
  //
  CHECK (EdKextSort (Infos, Enabled, 5, Order) == 0);
  CHECK (Order[0] == 4 && Order[1] == 2 && Order[2] == 0 && Order[3] == 1 && Order[4] == 3);

  //
  // What is switched off does not move, and does not make anything move.
  //
  {
    STATIC CONST KEXT_SPEC  Off[] = {
      { "P", { "L", NULL }, FALSE, TRUE, FALSE },   // off and needs L which is below
      { "Q", { "L", NULL }, TRUE, TRUE, FALSE },    // on and needs L which is off
      { "R", { "S", NULL }, TRUE, TRUE, FALSE },    // on and needs S which is below
      { "L", { NULL },      FALSE, TRUE, FALSE },   // off
      { "S", { NULL },      TRUE, TRUE, FALSE }     // on
    };

    SpecToInfo (Off, ARRAY_SIZE (Off), Infos, Enabled);
    for (Index = 0; Index < 5; ++Index) {
      Order[Index] = Index;
    }

    CHECK (EdKextSort (Infos, Enabled, 5, Order) == 3);
    CHECK (Order[0] == 0 && Order[1] == 1 && Order[2] == 4 && Order[3] == 2 && Order[4] == 3);
    CHECK (EdKextFindProblems (Infos, Enabled, 5, Order, NULL, 0, Problems) == 1);
    CHECK (Problems[1].Kind == EdKextDisabled);
    CHECK (EdKextSort (Infos, Enabled, 5, Order) == 0);
  }

  //
  // Cycles cannot be solved, but they do not make a mess either.
  //
  {
    STATIC CONST KEXT_SPEC  Loop[] = {
      { "A", { "B", NULL }, TRUE, TRUE, FALSE },
      { "B", { "C", NULL }, TRUE, TRUE, FALSE },
      { "C", { "A", NULL }, TRUE, TRUE, FALSE },
      { "D", { NULL },      TRUE, TRUE, FALSE }
    };

    SpecToInfo (Loop, ARRAY_SIZE (Loop), Infos, Enabled);
    for (Index = 0; Index < 4; ++Index) {
      Order[Index] = Index;
    }

    EdKextSort (Infos, Enabled, 4, Order);
    {
      BOOLEAN  Seen[4] = { FALSE, FALSE, FALSE, FALSE };

      for (Index = 0; Index < 4; ++Index) {
        CHECK (Order[Index] < 4 && !Seen[Order[Index]]);
        Seen[Order[Index] % 4] = TRUE;
      }
    }
  }

  //
  // The kexts that are in the list twice are both put in front.
  //
  {
    STATIC CONST KEXT_SPEC  Twice[] = {
      { "U", { "L", NULL }, TRUE, TRUE, FALSE },
      { "L", { NULL },      TRUE, TRUE, FALSE },
      { "T", { NULL },      TRUE, TRUE, FALSE },
      { "L", { NULL },      TRUE, TRUE, FALSE }
    };

    SpecToInfo (Twice, ARRAY_SIZE (Twice), Infos, Enabled);
    for (Index = 0; Index < 4; ++Index) {
      Order[Index] = Index;
    }

    CHECK (EdKextSort (Infos, Enabled, 4, Order) == 4);
    CHECK (Order[0] == 1 && Order[1] == 3 && Order[2] == 0 && Order[3] == 2);
  }

  //
  // Chains that are the other way round, and a lot of them.
  //
  {
    ED_KEXT_INFO  *Many;
    BOOLEAN       *On;
    UINT32        *Big;
    CHAR8         (*Names)[16];
    CHAR8         **Libs;
    UINT32        Count;
    ED_KEXT_PROBLEM *Found;

    Count = 500;
    Many  = AllocateZeroPool (Count * sizeof (*Many));
    On    = AllocatePool (Count * sizeof (*On));
    Big   = AllocatePool (Count * sizeof (*Big));
    Found = AllocatePool (Count * sizeof (*Found));
    Names = AllocatePool (Count * sizeof (*Names));
    Libs  = AllocatePool (Count * sizeof (*Libs));

    //
    // Every kext needs the one which comes after it in the list, the last one needs nothing.
    //
    for (Index = 0; Index < Count; ++Index) {
      AsciiSPrint (Names[Index], sizeof (Names[Index]), "k%u", Index);
      Many[Index].Identifier = Names[Index];
      Many[Index].HasInfo    = TRUE;
      On[Index]              = TRUE;
      Big[Index]             = Index;
    }

    for (Index = 0; Index + 1 < Count; ++Index) {
      Libs[Index]            = Names[Index + 1];
      Many[Index].Libraries    = &Libs[Index];
      Many[Index].LibraryCount = 1;
    }

    CHECK (EdKextFindProblems (Many, On, Count, Big, NULL, 0, Found) == Count - 1);
    EdKextSort (Many, On, Count, Big);
    CHECK (EdKextFindProblems (Many, On, Count, Big, NULL, 0, Found) == 0);
    CHECK (Big[0] == Count - 1 && Big[Count - 1] == 0);
    CHECK (EdKextSort (Many, On, Count, Big) == 0);

    FreePool (Many);
    FreePool (On);
    FreePool (Big);
    FreePool (Found);
    FreePool (Names);
    FreePool (Libs);
  }
}

/**
  Sort random sets of kexts and see that all of them come out right, and stay that way.
**/
STATIC
VOID
TestKextSortFuzz (
  VOID
  )
{
  ED_KEXT_INFO     Infos[24];
  CHAR8            Names[24][8];
  CHAR8            *Libs[24][4];
  UINT32           Deps[24][4];
  BOOLEAN          Enabled[24];
  BOOLEAN          Placed[24];
  BOOLEAN          Seen[24];
  ED_KEXT_PROBLEM  Problems[24];
  UINT32           Order[24];
  UINT32           Good[24];
  UINT32           Candidates[24];
  UINT32           Seed;
  UINT32           Round;
  UINT32           Count;
  UINT32           Index;
  UINT32           Other;
  UINT32           Swap;
  UINT32           Temp;
  UINT32           Found;
  BOOLEAN          Ready;
  UINT32           Checked;

  Seed    = 777;
  Checked = 0;

  for (Round = 0; Round < 500; ++Round) {
    Count = 2 + NextRandom (&Seed) % 22;

    //
    // Kexts can only need kexts with a lower number, so there are no cycles.
    //
    for (Index = 0; Index < Count; ++Index) {
      ZeroMem (&Infos[Index], sizeof (Infos[Index]));
      AsciiSPrint (Names[Index], sizeof (Names[Index]), "n%u", Index);
      Infos[Index].Identifier = Names[Index];
      Infos[Index].HasInfo    = TRUE;
      Infos[Index].Libraries  = Libs[Index];
      Enabled[Index]          = TRUE;

      for (Other = 0; (Index > 0) && (Other < 1 + NextRandom (&Seed) % 3); ++Other) {
        Deps[Index][Infos[Index].LibraryCount]   = NextRandom (&Seed) % Index;
        Libs[Index][Infos[Index].LibraryCount]   = Names[Deps[Index][Infos[Index].LibraryCount]];
        ++Infos[Index].LibraryCount;
      }

      Order[Index]  = Index;
      Placed[Index] = FALSE;
    }

    //
    // An order in which everything is right, picked at random.
    //
    for (Index = 0; Index < Count; ++Index) {
      Found = 0;
      for (Other = 0; Other < Count; ++Other) {
        if (Placed[Other]) {
          continue;
        }

        Ready = TRUE;
        for (Temp = 0; Temp < Infos[Other].LibraryCount; ++Temp) {
          if (!Placed[Deps[Other][Temp]]) {
            Ready = FALSE;
          }
        }

        if (Ready) {
          Candidates[Found++] = Other;
        }
      }

      CHECK (Found > 0);
      Good[Index] = Candidates[NextRandom (&Seed) % Found];
      Placed[Good[Index]] = TRUE;
    }

    CHECK (EdKextFindProblems (Infos, Enabled, Count, Good, NULL, 0, Problems) == 0);

    //
    // It is right, so it stays as it is.
    //
    CopyMem (Order, Good, sizeof (Order));
    CHECK (EdKextSort (Infos, Enabled, Count, Order) == 0);
    CHECK (CompareMem (Order, Good, Count * sizeof (Order[0])) == 0);

    //
    // Any order at all is brought to a right one, and that one stays then.
    //
    for (Index = Count - 1; Index > 0; --Index) {
      Swap         = NextRandom (&Seed) % (Index + 1);
      Temp         = Order[Index];
      Order[Index] = Order[Swap];
      Order[Swap]  = Temp;
    }

    EdKextSort (Infos, Enabled, Count, Order);

    ZeroMem (Seen, sizeof (Seen));
    for (Index = 0; Index < Count; ++Index) {
      CHECK (Order[Index] < Count && !Seen[Order[Index]]);
      Seen[Order[Index] % 24] = TRUE;
    }

    CHECK (EdKextFindProblems (Infos, Enabled, Count, Order, NULL, 0, Problems) == 0);
    CHECK (EdKextSort (Infos, Enabled, Count, Order) == 0);
    ++Checked;
  }

  printf ("sorted %u random sets of kexts\n", Checked);
}

STATIC
VOID
TestFile (
  IN CONST CHAR8  *FileName
  )
{
  UINT8       *Data;
  UINT32      Size;
  EDIT_PLIST  Plist;
  LEAF_PATH   Path;
  UINT32      Errors;
  CHAR8       *Text;
  UINT32      RenderSize;
  XML_NODE    *Node;
  UINT32      EditorErrors;
  UINT8       Bytes[4];

  Data = UserReadFile (FileName, &Size);
  if (Data == NULL) {
    printf ("FAIL: cannot read %s\n", FileName);
    ++mFailures;
    return;
  }

  CHECK (!EFI_ERROR (EditPlistLoad (&Plist, Data, Size)));
  CHECK (!EFI_ERROR (EditPlistRender (&Plist, &Text, &RenderSize)));
  CHECK (RenderSize == Size && CompareMem (Text, Data, Size) == 0);
  FreePool (Text);

  CHECK (!EFI_ERROR (EditPlistCountErrors ((CONST CHAR8 *)Data, Size, &Errors)));
  printf ("%s: %u bytes, %u configuration problems before editing\n", FileName, Size, Errors);

  TestReorderFuzz (Data, Size, FileName);

  ZeroMem (&Path, sizeof (Path));
  mBooleans = 0;
  mEdited   = 0;
  CollectBooleans (&Plist, Plist.Root, &Path, Errors, FALSE);
  printf ("%s: toggled %u of %u booleans one by one\n", FileName, mEdited, mBooleans);
  CHECK (mBooleans > 10);

  //
  // Something typical: switch SIP and boot-args like the editor does.
  //
  Node = EditPlistFindChild (EditPlistFindChild (EditPlistFindChild (Plist.Root, "NVRAM"), "Add"), "7C436110-AB2A-4BBB-A880-FE41995C9F82");
  CHECK (Node != NULL);
  if (Node != NULL) {
    CHECK (EditPlistFindChild (Node, "boot-args") != NULL);
    CHECK (!EFI_ERROR (EditPlistSetString (&Plist, EditPlistFindChild (Node, "boot-args"), "-v debug=0x100")));
    Bytes[0] = 0xE7; Bytes[1] = 0x03; Bytes[2] = 0; Bytes[3] = 0;
    CHECK (!EFI_ERROR (EditPlistSetData (&Plist, EditPlistFindChild (Node, "csr-active-config"), Bytes, 4)));
    CHECK (!EFI_ERROR (EditPlistAddString (&Plist, Node, "test-added-key", "test value")));
  }

  Node = EditPlistFindChild (EditPlistFindChild (EditPlistFindChild (Plist.Root, "Misc"), "Boot"), "Timeout");
  CHECK (Node != NULL);
  if (Node != NULL) {
    CHECK (!EFI_ERROR (EditPlistSetInteger (&Plist, Node, 7)));
  }

  CHECK (!EFI_ERROR (EditPlistRender (&Plist, &Text, &RenderSize)));
  CHECK (!EFI_ERROR (EditPlistCountErrors (Text, RenderSize, &EditorErrors)));
  CHECK (EditorErrors == Errors);
  {
    EDIT_PLIST  Again;
    XML_NODE    *Added;

    CHECK (!EFI_ERROR (EditPlistLoad (&Again, Text, RenderSize)));
    Added = EditPlistFindChild (EditPlistFindChild (EditPlistFindChild (Again.Root, "NVRAM"), "Add"), "7C436110-AB2A-4BBB-A880-FE41995C9F82");
    CHECK (Added != NULL);
    if (Added != NULL) {
      CHECK (AsciiStrCmp (EditPlistGetContent (&Again, EditPlistFindChild (Added, "boot-args")), "-v debug=0x100") == 0);
      CHECK (AsciiStrCmp (EditPlistGetContent (&Again, EditPlistFindChild (Added, "test-added-key")), "test value") == 0);
      CHECK (AsciiStrCmp (EditPlistGetContent (&Again, EditPlistFindChild (Added, "csr-active-config")), "5wMAAA==") == 0);
    }

    EditPlistFree (&Again);
  }

  //
  // A value of a wrong type is something configuration loading has to notice.
  //
  {
    CHAR8   *Broken;
    UINT32  BrokenErrors;
    CHAR8   *Position;

    Broken = AllocatePool (Size + 64);
    CopyMem (Broken, Data, Size);
    Broken[Size] = '\0';
    Position     = AsciiStrStr (Broken, "<key>AppleCpuPmCfgLock</key>");
    CHECK (Position != NULL);
    if (Position != NULL) {
      Position = AsciiStrStr (Position, "<false/>");
      CHECK (Position != NULL);
      if (Position != NULL) {
        CopyMem (Position, "<true/> ", 8);
        CHECK (!EFI_ERROR (EditPlistCountErrors (Broken, Size, &BrokenErrors)));
        CHECK (BrokenErrors == Errors);
        CopyMem (Position, "<null/> ", 8);
        CHECK (!EFI_ERROR (EditPlistCountErrors (Broken, Size, &BrokenErrors)));
        printf ("%s: %u problems when a boolean is replaced with a different element\n", FileName, BrokenErrors);
        CHECK (BrokenErrors > Errors);
      }
    }

    FreePool (Broken);
  }

  FreePool (Text);
  EditPlistFree (&Plist);
  FreePool (Data);
}

int
ENTRY_POINT (
  int   argc,
  char  *argv[]
  )
{
  TestBasics ();
  TestInsertions ();
  TestLayouts ();
  TestMalformed ();
  TestReorder ();
  TestKextInfo ();
  TestKextProblems ();
  TestKextSort ();
  TestKextSortFuzz ();

  if (argc > 1) {
    TestFile (argv[1]);
  }

  printf ("%u checks, %u failures\n", mChecks, mFailures);

  return mFailures == 0 ? 0 : 1;
}

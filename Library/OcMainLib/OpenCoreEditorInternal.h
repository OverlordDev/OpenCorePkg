/** @file
  Internal definitions of the OpenCore picker options editor.

  Copyright (C) 2026, OpenCore contributors. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-3-Clause
**/

#ifndef OPEN_CORE_EDITOR_INTERNAL_H
#define OPEN_CORE_EDITOR_INTERNAL_H

#include <Uefi.h>

#include <Library/OcXmlLib.h>

/**
  Kinds of plist nodes the editor distinguishes.
**/
typedef enum {
  EditPlistKindDict,
  EditPlistKindArray,
  EditPlistKindBoolean,
  EditPlistKindInteger,
  EditPlistKindString,
  EditPlistKindData,
  EditPlistKindOther
} EDIT_PLIST_KIND;

/**
  Pending change of the original text.

  Replacements of scalar nodes are kept in their semantic form, so the editor can show
  what a node currently holds, raw ones are inserted verbatim.
**/
typedef struct {
  //
  // Node a semantic patch is about, NULL for raw patches.
  //
  XML_NODE           *Node;
  //
  // How to render the replacement: a scalar kind, or EditPlistKindOther for raw text.
  //
  EDIT_PLIST_KIND    Kind;
  //
  // Half-open range of the original text to replace, empty for insertions.
  //
  UINT32             Start;
  UINT32             End;
  //
  // Boolean and integer value of semantic patches.
  //
  BOOLEAN            Boolean;
  UINT64             Integer;
  //
  // Pool allocated NUL terminated text: element content of string and data patches,
  // the whole replacement of raw ones.
  //
  CHAR8              *Text;
} EDIT_PLIST_PATCH;

/**
  Rearranged elements of an array. The elements keep their text, including the changes made
  to what they hold, only their places are exchanged when rendering.
**/
typedef struct {
  //
  // The array which is rearranged, NULL if none is.
  //
  XML_NODE    *Array;
  UINT32      Count;
  //
  // Order[Position] is the index in the original array of the element which is at Position now.
  //
  UINT32      *Order;
} EDIT_PLIST_ORDER;

/**
  Plist document which can be modified without reformatting it.

  The original text is never changed, pending changes are only applied when rendering.
  Nodes of Root stay valid for as long as the document is loaded.
**/
typedef struct {
  //
  // Pristine copy of the text with an extra NUL terminator.
  //
  CHAR8               *Source;
  UINT32              SourceSize;
  //
  // Scratch copy which the parser has taken over and destroyed.
  //
  CHAR8               *Work;
  XML_DOCUMENT        *Document;
  //
  // Top level dictionary.
  //
  XML_NODE            *Root;
  EDIT_PLIST_PATCH    *Patches;
  UINT32              PatchCount;
  UINT32              PatchCapacity;
  //
  // Elements of one array may be moved around, see EditPlistArrayMove.
  //
  EDIT_PLIST_ORDER    Reorder;
} EDIT_PLIST;

/**
  Most elements of an array which can be rearranged.
**/
#define EDIT_PLIST_MAX_ORDER  4096U

/**
  Longest string, in characters, which the editor accepts for a single value.
**/
#define EDIT_PLIST_MAX_VALUE_LENGTH  1024U

/**
  Load plist text.

  @param[out]  Plist  Document to initialise.
  @param[in]   Data   Plist text, not necessarily NUL terminated.
  @param[in]   Size   Size of Data.

  @retval EFI_SUCCESS            The document has been loaded and has a top level dictionary.
  @retval EFI_INVALID_PARAMETER  The text is not a plist with a top level dictionary.
  @retval EFI_OUT_OF_RESOURCES   Memory allocation failed.
**/
EFI_STATUS
EditPlistLoad (
  OUT EDIT_PLIST  *Plist,
  IN  CONST VOID  *Data,
  IN  UINT32      Size
  );

/**
  Release the document and all pending changes.

  @param[in,out]  Plist  Document, may be zeroed or loaded.
**/
VOID
EditPlistFree (
  IN OUT EDIT_PLIST  *Plist
  );

/**
  Classify a node.
**/
EDIT_PLIST_KIND
EditPlistNodeKind (
  IN  XML_NODE  *Node
  );

/**
  Get the number of entries of a dictionary (key value pairs) or array (elements), 0 for others.
**/
UINT32
EditPlistChildCount (
  IN  XML_NODE  *Node
  );

/**
  Get an entry value of a dictionary or array.
**/
XML_NODE *
EditPlistChildValue (
  IN  XML_NODE  *Node,
  IN  UINT32    Index
  );

/**
  Get an entry key of a dictionary.

  @return The key, which can be empty, or NULL for anything but dictionaries.
**/
CONST CHAR8 *
EditPlistChildKey (
  IN  XML_NODE  *Node,
  IN  UINT32    Index
  );

/**
  Find a dictionary entry by key.

  @return The value or NULL if there is no such entry (or Dict is NULL or not a dictionary),
          so lookups of nested entries can be chained.
**/
XML_NODE *
EditPlistFindChild (
  IN  XML_NODE     *Dict,
  IN  CONST CHAR8  *Key
  );

/**
  Check if a node has pending changes.
**/
BOOLEAN
EditPlistIsPatched (
  IN  CONST EDIT_PLIST  *Plist,
  IN  CONST XML_NODE    *Node
  );

/**
  Get the effective value of a boolean node.
**/
BOOLEAN
EditPlistGetBoolean (
  IN  CONST EDIT_PLIST  *Plist,
  IN  XML_NODE          *Node
  );

/**
  Get the effective text of an integer node.

  @param[in]   Plist       Document.
  @param[in]   Node        Integer node.
  @param[out]  Buffer      Resulting NUL terminated text.
  @param[in]   BufferSize  Size of Buffer.
**/
VOID
EditPlistGetInteger (
  IN  CONST EDIT_PLIST  *Plist,
  IN  XML_NODE          *Node,
  OUT CHAR8             *Buffer,
  IN  UINTN             BufferSize
  );

/**
  Get the effective content of a string or data node as it is in the file: not unescaped,
  base64 for data.

  @return The content, empty but never NULL.
**/
CONST CHAR8 *
EditPlistGetContent (
  IN  CONST EDIT_PLIST  *Plist,
  IN  XML_NODE          *Node
  );

/**
  Get the effective value of a data node.

  @param[in]   Plist  Document.
  @param[in]   Node   Data node.
  @param[out]  Data   Decoded data allocated from pool, NULL for empty data.
  @param[out]  Size   Size of the data.

  @retval EFI_SUCCESS  The data has been decoded.
  @retval other        The content is not valid base64.
**/
EFI_STATUS
EditPlistGetData (
  IN  CONST EDIT_PLIST  *Plist,
  IN  XML_NODE          *Node,
  OUT UINT8             **Data,
  OUT UINTN             *Size
  );

/**
  Check if a string can be stored by the editor: printable ASCII only, no markup
  characters, and ampersands only as part of the predefined XML entities.
**/
BOOLEAN
EditPlistIsPlainString (
  IN  CONST CHAR8  *Value
  );

/**
  Replace the value of a boolean node.
**/
EFI_STATUS
EditPlistSetBoolean (
  IN OUT EDIT_PLIST  *Plist,
  IN     XML_NODE    *Node,
  IN     BOOLEAN     Value
  );

/**
  Replace the value of an integer node.
**/
EFI_STATUS
EditPlistSetInteger (
  IN OUT EDIT_PLIST  *Plist,
  IN     XML_NODE    *Node,
  IN     UINT64      Value
  );

/**
  Replace the value of a string node. Data and integer nodes are turned into strings.

  @retval EFI_INVALID_PARAMETER  The value is not accepted by EditPlistIsPlainString.
**/
EFI_STATUS
EditPlistSetString (
  IN OUT EDIT_PLIST  *Plist,
  IN     XML_NODE    *Node,
  IN     CONST CHAR8 *Value
  );

/**
  Replace the value of a data node.
**/
EFI_STATUS
EditPlistSetData (
  IN OUT EDIT_PLIST   *Plist,
  IN     XML_NODE     *Node,
  IN     CONST UINT8  *Data,
  IN     UINTN        Size
  );

/**
  Drop pending changes of a node.
**/
VOID
EditPlistRevert (
  IN OUT EDIT_PLIST  *Plist,
  IN     XML_NODE    *Node
  );

/**
  Add a key and a string value to the end of a dictionary.

  @param[in,out]  Plist  Document.
  @param[in]      Dict   Dictionary to add to.
  @param[in]      Key    New key, printable ASCII without markup characters.
  @param[in]      Value  New value as a string, rules of EditPlistIsPlainString apply.
**/
EFI_STATUS
EditPlistAddString (
  IN OUT EDIT_PLIST   *Plist,
  IN     XML_NODE     *Dict,
  IN     CONST CHAR8  *Key,
  IN     CONST CHAR8  *Value
  );

/**
  Add a key and a dictionary holding a single string entry to the end of a dictionary.
**/
EFI_STATUS
EditPlistAddDictWithString (
  IN OUT EDIT_PLIST   *Plist,
  IN     XML_NODE     *Dict,
  IN     CONST CHAR8  *Key,
  IN     CONST CHAR8  *InnerKey,
  IN     CONST CHAR8  *InnerValue
  );

/**
  Add a key and a boolean value to the end of a dictionary.

  @param[in,out]  Plist  Document.
  @param[in]      Dict   Dictionary to add to.
  @param[in]      Key    New key, printable ASCII without markup characters.
  @param[in]      Value  New value.
**/
EFI_STATUS
EditPlistAddBoolean (
  IN OUT EDIT_PLIST   *Plist,
  IN     XML_NODE     *Dict,
  IN     CONST CHAR8  *Key,
  IN     BOOLEAN      Value
  );

/**
  Take back everything which was added to a dictionary with the functions above.
**/
VOID
EditPlistRevertAdded (
  IN OUT EDIT_PLIST  *Plist,
  IN     XML_NODE    *Dict
  );

/**
  Move an element of an array to another place, the elements in between shift by one.
  Only the elements of a single array of the document can be rearranged.

  @param[in,out]  Plist  Document.
  @param[in]      Array  Array.
  @param[in]      From   Current position of the element.
  @param[in]      To     Position the element is to have.

  @retval EFI_SUCCESS            The element has been moved.
  @retval EFI_INVALID_PARAMETER  Array is not an array, or a position is out of its range.
  @retval EFI_UNSUPPORTED        Another array is rearranged already, or the array is too large
                                 or its elements cannot be told apart in the text.
**/
EFI_STATUS
EditPlistArrayMove (
  IN OUT EDIT_PLIST  *Plist,
  IN     XML_NODE    *Array,
  IN     UINT32      From,
  IN     UINT32      To
  );

/**
  Put the elements of an array in a given order.

  @param[in,out]  Plist  Document.
  @param[in]      Array  Array.
  @param[in]      Order  For every position, the index in the original array of the element it is to hold.
  @param[in]      Count  Number of elements, which has to be that of the array.

  @retval EFI_INVALID_PARAMETER  Order is not a permutation of the elements.
**/
EFI_STATUS
EditPlistArraySetOrder (
  IN OUT EDIT_PLIST    *Plist,
  IN     XML_NODE      *Array,
  IN     CONST UINT32  *Order,
  IN     UINT32        Count
  );

/**
  Get the index in the original array of the element which is at a position now.
  It is Position itself for arrays which have not been rearranged.
**/
UINT32
EditPlistArrayOriginalIndex (
  IN  CONST EDIT_PLIST  *Plist,
  IN  XML_NODE          *Array,
  IN  UINT32            Position
  );

/**
  Check if the elements of an array are not where they were.
**/
BOOLEAN
EditPlistArrayIsRearranged (
  IN  CONST EDIT_PLIST  *Plist,
  IN  XML_NODE          *Array
  );

/**
  Put the elements of an array back to where they were.
**/
VOID
EditPlistArrayRevertOrder (
  IN OUT EDIT_PLIST  *Plist,
  IN     XML_NODE    *Array
  );

/**
  Get the number of pending changes: replacements and added entries, and a rearranged array as one.
**/
UINT32
EditPlistPatchCount (
  IN  CONST EDIT_PLIST  *Plist
  );

/**
  Render the document with pending changes applied.

  @param[in]   Plist  Document.
  @param[out]  Text   Resulting NUL terminated text allocated from pool.
  @param[out]  Size   Size of Text without the terminator.

  @retval EFI_SUCCESS  The text has been produced.
**/
EFI_STATUS
EditPlistRender (
  IN  CONST EDIT_PLIST  *Plist,
  OUT CHAR8             **Text,
  OUT UINT32            *Size
  );

/**
  Count errors which OpenCore finds when it parses configuration text.

  @param[in]   Text    Configuration text.
  @param[in]   Size    Size of Text.
  @param[out]  Errors  Number of problems found (unknown keys, mismatching types and so on).

  @retval EFI_SUCCESS      The text is a loadable configuration, which may still have problems.
  @retval EFI_UNSUPPORTED  OpenCore could not load the text at all.
**/
EFI_STATUS
EditPlistCountErrors (
  IN  CONST CHAR8  *Text,
  IN  UINT32       Size,
  OUT UINT32       *Errors
  );

//
// Kexts: what they need, and in which order they can be loaded.
//

/**
  Most libraries of a kext that are taken into account.
**/
#define ED_KEXT_MAX_LIBRARIES  64U

/**
  What the editor knows about a kext from its Info.plist.
**/
typedef struct {
  //
  // CFBundleIdentifier, NULL when it is not known.
  //
  CHAR8      *Identifier;
  //
  // Identifiers of the kexts which it needs (OSBundleLibraries), without the ones which
  // the system itself provides. Both of the pool.
  //
  CHAR8      **Libraries;
  UINT32     LibraryCount;
  //
  // The Info.plist has been read and understood.
  //
  BOOLEAN    HasInfo;
  //
  // A file of the kext which OpenCore needs is not there, so it does not add the kext.
  //
  BOOLEAN    FilesMissing;
} ED_KEXT_INFO;

/**
  What is wrong with a kext in its place.
**/
typedef enum {
  EdKextOk,
  ///
  /// The files of the kext cannot be read, OpenCore does not add the kext then.
  ///
  EdKextNoFiles,
  ///
  /// Needs a kext which is listed after it.
  ///
  EdKextOrder,
  ///
  /// Needs a kext which is listed, but is switched off.
  ///
  EdKextDisabled,
  ///
  /// Needs a kext which is not listed at all.
  ///
  EdKextMissing
} ED_KEXT_PROBLEM_KIND;

typedef struct {
  ED_KEXT_PROBLEM_KIND    Kind;
  //
  // The first library that has the problem (index into Libraries), and a kext which
  // provides it when there is one (MAX_UINT32 otherwise). Both are about EdKextOrder,
  // EdKextDisabled and EdKextMissing.
  //
  UINT32                  Library;
  UINT32                  Provider;
  //
  // How many libraries have a problem.
  //
  UINT32                  LibraryProblems;
} ED_KEXT_PROBLEM;

/**
  Understand the Info.plist of a kext.

  @param[in]   Data  Contents of Info.plist.
  @param[in]   Size  Size of Data.
  @param[out]  Info  What was found, to be released with EdKextFreeInfo.

  @retval EFI_SUCCESS            The file is a plist, whether it has identifiers or not.
  @retval EFI_INVALID_PARAMETER  The file is not a plist with a dictionary.
**/
EFI_STATUS
EdKextParseInfo (
  IN  CONST VOID    *Data,
  IN  UINT32        Size,
  OUT ED_KEXT_INFO  *Info
  );

VOID
EdKextFreeInfo (
  IN OUT ED_KEXT_INFO  *Info
  );

/**
  Find the kexts that cannot do their job where they are, if the kexts are loaded in the given order.
  A kext needs everything it lists to be loaded before it, unless the system provides that.

  @param[in]   Kexts          Information on the kexts, by their index in the original array.
  @param[in]   Enabled        Whether the kexts are switched on, by their index in the original array.
  @param[in]   Count          Number of kexts.
  @param[in]   Order          For every position of the load order, the index of the kext there.
  @param[in]   Provided       Identifiers of the kexts which are there from the start, optional.
  @param[in]   ProvidedCount  Number of Provided.
  @param[out]  Problems       What is wrong with every kext, by index in the original array.

  @return Number of kexts with a problem.
**/
UINT32
EdKextFindProblems (
  IN  CONST ED_KEXT_INFO  *Kexts,
  IN  CONST BOOLEAN       *Enabled,
  IN  UINT32              Count,
  IN  CONST UINT32        *Order,
  IN  CONST CHAR8         **Provided OPTIONAL,
  IN  UINT32              ProvidedCount,
  IN OUT ED_KEXT_PROBLEM  *Problems
  );

/**
  Put the kexts in an order in which each of them comes after the kexts that it needs. The kexts
  which are in a good place already keep it as much as it is possible: a kext that is needed
  is moved up just in front of the first one that needs it. Only the kexts which are switched on
  are looked at, those which are off do not need anything and are not needed.

  @param[in]      Kexts    Information on the kexts, by their index in the original array.
  @param[in]      Enabled  Whether the kexts are switched on, by their index in the original array.
  @param[in]      Count    Number of kexts.
  @param[in,out]  Order    For every position of the load order, the index of the kext there.

  @return Number of positions that got another kext.
**/
UINT32
EdKextSort (
  IN     CONST ED_KEXT_INFO  *Kexts,
  IN     CONST BOOLEAN       *Enabled,
  IN     UINT32              Count,
  IN OUT UINT32              *Order
  );

#endif // OPEN_CORE_EDITOR_INTERNAL_H

#include "WolfSshUefi.h"

#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextOut.h>

#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#define TERMINAL_MAX_PARAMS 16
#define TERMINAL_REPLY_SIZE 128

typedef struct {
  CHAR16 Character;
  UINTN  Attribute;
} TERMINAL_CELL;

typedef enum {
  ParserGround,
  ParserEscape,
  ParserCsi,
  ParserOsc,
  ParserOscEscape
} TERMINAL_PARSER_STATE;

struct WOLFSSH_UEFI_TERMINAL {
  UINTN                 Columns;
  UINTN                 Rows;
  TERMINAL_CELL         *Primary;
  TERMINAL_CELL         *Alternate;
  TERMINAL_CELL         *Cells;
  UINT16                *DirtyStart;
  UINT16                *DirtyEnd;
  CHAR16                *RenderBuffer;
  UINTN                 Row;
  UINTN                 Column;
  UINTN                 SavedRow;
  UINTN                 SavedColumn;
  UINTN                 PrimaryRow;
  UINTN                 PrimaryColumn;
  UINTN                 ScrollTop;
  UINTN                 ScrollBottom;
  UINTN                 DefaultForeground;
  UINTN                 DefaultBackground;
  UINTN                 Foreground;
  UINTN                 Background;
  BOOLEAN               Bold;
  BOOLEAN               Reverse;
  BOOLEAN               CursorVisible;
  BOOLEAN               ApplicationCursor;
  BOOLEAN               InsertMode;
  BOOLEAN               WrapPending;
  BOOLEAN               AlternateActive;
  TERMINAL_PARSER_STATE ParserState;
  INT32                 Parameters[TERMINAL_MAX_PARAMS];
  UINTN                 ParameterCount;
  BOOLEAN               PrivateCsi;
  UINT8                 Utf8Remaining;
  UINT32                Utf8CodePoint;
  UINT8                 Replies[TERMINAL_REPLY_SIZE];
  UINTN                 ReplyLength;
};

STATIC
UINTN
TerminalAttribute (
  IN CONST WOLFSSH_UEFI_TERMINAL  *Terminal
  )
{
  UINTN Foreground;
  UINTN Background;

  Foreground = Terminal->Foreground;
  Background = Terminal->Background;
  if (Terminal->Bold && (Foreground < 8)) {
    Foreground += 8;
  }
  if (Terminal->Reverse) {
    UINTN Temporary;
    Temporary = Foreground;
    Foreground = Background;
    Background = Temporary;
  }
  return EFI_TEXT_ATTR (Foreground & 0x0f, Background & 0x07);
}

STATIC
TERMINAL_CELL *
CellAt (
  IN WOLFSSH_UEFI_TERMINAL  *Terminal,
  IN UINTN                  Row,
  IN UINTN                  Column
  )
{
  return &Terminal->Cells[Row * Terminal->Columns + Column];
}

STATIC
VOID
MarkDirty (
  IN OUT WOLFSSH_UEFI_TERMINAL  *Terminal,
  IN UINTN                      Row,
  IN UINTN                      Start,
  IN UINTN                      End
  )
{
  if ((Row >= Terminal->Rows) || (Start >= Terminal->Columns)) {
    return;
  }
  End = MIN (End, Terminal->Columns - 1);
  Terminal->DirtyStart[Row] = (UINT16)MIN (
                                       (UINTN)Terminal->DirtyStart[Row],
                                       Start
                                       );
  Terminal->DirtyEnd[Row] = (UINT16)MAX (
                                     (UINTN)Terminal->DirtyEnd[Row],
                                     End
                                     );
}

STATIC
VOID
MarkDirtyRows (
  IN OUT WOLFSSH_UEFI_TERMINAL  *Terminal,
  IN UINTN                      First,
  IN UINTN                      Last
  )
{
  UINTN Row;

  Last = MIN (Last, Terminal->Rows - 1);
  for (Row = First; Row <= Last; Row++) {
    MarkDirty (Terminal, Row, 0, Terminal->Columns - 1);
  }
}

STATIC
VOID
FillCells (
  IN OUT WOLFSSH_UEFI_TERMINAL  *Terminal,
  IN UINTN                      Row,
  IN UINTN                      Start,
  IN UINTN                      End,
  IN UINTN                      Attribute
  )
{
  UINTN Column;

  if ((Row >= Terminal->Rows) || (Start >= Terminal->Columns)) {
    return;
  }
  End = MIN (End, Terminal->Columns - 1);
  for (Column = Start; Column <= End; Column++) {
    CellAt (Terminal, Row, Column)->Character = L' ';
    CellAt (Terminal, Row, Column)->Attribute = Attribute;
  }
  MarkDirty (Terminal, Row, Start, End);
}

STATIC
VOID
ClearBuffer (
  IN OUT WOLFSSH_UEFI_TERMINAL  *Terminal,
  IN TERMINAL_CELL              *Buffer
  )
{
  UINTN Index;
  UINTN Attribute;

  Attribute = EFI_TEXT_ATTR (
                Terminal->DefaultForeground,
                Terminal->DefaultBackground
                );
  for (Index = 0; Index < Terminal->Rows * Terminal->Columns; Index++) {
    Buffer[Index].Character = L' ';
    Buffer[Index].Attribute = Attribute;
  }
}

STATIC
VOID
ScrollUp (
  IN OUT WOLFSSH_UEFI_TERMINAL  *Terminal,
  IN UINTN                      Count
  )
{
  UINTN RegionRows;
  UINTN Row;
  UINTN Attribute;

  RegionRows = Terminal->ScrollBottom - Terminal->ScrollTop + 1;
  Count = MIN (Count, RegionRows);
  if (Count == 0) {
    return;
  }

  for (Row = Terminal->ScrollTop; Row + Count <= Terminal->ScrollBottom; Row++) {
    CopyMem (
      CellAt (Terminal, Row, 0),
      CellAt (Terminal, Row + Count, 0),
      Terminal->Columns * sizeof (TERMINAL_CELL)
      );
  }
  Attribute = TerminalAttribute (Terminal);
  for (; Row <= Terminal->ScrollBottom; Row++) {
    FillCells (Terminal, Row, 0, Terminal->Columns - 1, Attribute);
  }
  MarkDirtyRows (Terminal, Terminal->ScrollTop, Terminal->ScrollBottom);
}

STATIC
VOID
ScrollDown (
  IN OUT WOLFSSH_UEFI_TERMINAL  *Terminal,
  IN UINTN                      Count
  )
{
  UINTN RegionRows;
  UINTN Row;
  UINTN Attribute;

  RegionRows = Terminal->ScrollBottom - Terminal->ScrollTop + 1;
  Count = MIN (Count, RegionRows);
  if (Count == 0) {
    return;
  }

  Row = Terminal->ScrollBottom + 1;
  while (Row-- > Terminal->ScrollTop + Count) {
    CopyMem (
      CellAt (Terminal, Row, 0),
      CellAt (Terminal, Row - Count, 0),
      Terminal->Columns * sizeof (TERMINAL_CELL)
      );
  }
  Attribute = TerminalAttribute (Terminal);
  for (Row = Terminal->ScrollTop; Row < Terminal->ScrollTop + Count; Row++) {
    FillCells (Terminal, Row, 0, Terminal->Columns - 1, Attribute);
  }
  MarkDirtyRows (Terminal, Terminal->ScrollTop, Terminal->ScrollBottom);
}

STATIC
VOID
IndexDown (
  IN OUT WOLFSSH_UEFI_TERMINAL  *Terminal
  )
{
  Terminal->WrapPending = FALSE;
  if (Terminal->Row == Terminal->ScrollBottom) {
    ScrollUp (Terminal, 1);
  } else if (Terminal->Row + 1 < Terminal->Rows) {
    Terminal->Row++;
  }
}

STATIC
VOID
IndexUp (
  IN OUT WOLFSSH_UEFI_TERMINAL  *Terminal
  )
{
  Terminal->WrapPending = FALSE;
  if (Terminal->Row == Terminal->ScrollTop) {
    ScrollDown (Terminal, 1);
  } else if (Terminal->Row != 0) {
    Terminal->Row--;
  }
}

STATIC
VOID
PutCharacter (
  IN OUT WOLFSSH_UEFI_TERMINAL  *Terminal,
  IN CHAR16                     Character
  )
{
  UINTN Column;

  if (Terminal->WrapPending) {
    Terminal->Column = 0;
    IndexDown (Terminal);
  }
  if (Terminal->InsertMode && (Terminal->Column + 1 < Terminal->Columns)) {
    for (Column = Terminal->Columns - 1; Column > Terminal->Column; Column--) {
      *CellAt (Terminal, Terminal->Row, Column) =
        *CellAt (Terminal, Terminal->Row, Column - 1);
    }
    MarkDirty (
      Terminal,
      Terminal->Row,
      Terminal->Column,
      Terminal->Columns - 1
      );
  }

  CellAt (Terminal, Terminal->Row, Terminal->Column)->Character = Character;
  CellAt (Terminal, Terminal->Row, Terminal->Column)->Attribute =
    TerminalAttribute (Terminal);
  MarkDirty (
    Terminal,
    Terminal->Row,
    Terminal->Column,
    Terminal->Column
    );
  if (Terminal->Column + 1 == Terminal->Columns) {
    Terminal->WrapPending = TRUE;
  } else {
    Terminal->Column++;
  }
}

STATIC
INT32
Parameter (
  IN CONST WOLFSSH_UEFI_TERMINAL  *Terminal,
  IN UINTN                        Index,
  IN INT32                        DefaultValue
  )
{
  if ((Index >= Terminal->ParameterCount) ||
      (Terminal->Parameters[Index] < 0)) {
    return DefaultValue;
  }
  return Terminal->Parameters[Index];
}

STATIC
UINTN
NonZeroParameter (
  IN CONST WOLFSSH_UEFI_TERMINAL  *Terminal,
  IN UINTN                        Index
  )
{
  INT32 Value;

  Value = Parameter (Terminal, Index, 1);
  return (Value <= 0) ? 1 : (UINTN)Value;
}

STATIC
VOID
QueueReply (
  IN OUT WOLFSSH_UEFI_TERMINAL  *Terminal,
  IN CONST UINT8                *Reply,
  IN UINTN                      Length
  )
{
  UINTN Available;

  Available = sizeof (Terminal->Replies) - Terminal->ReplyLength;
  Length = MIN (Length, Available);
  if (Length != 0) {
    CopyMem (Terminal->Replies + Terminal->ReplyLength, Reply, Length);
    Terminal->ReplyLength += Length;
  }
}

STATIC
UINTN
Map256Color (
  IN UINTN Color
  )
{
  UINTN Red;
  UINTN Green;
  UINTN Blue;
  UINTN Result;

  if (Color < 16) {
    return Color;
  }
  if (Color >= 232) {
    return (Color >= 244) ? 15 : 8;
  }
  Color -= 16;
  Red = Color / 36;
  Green = (Color / 6) % 6;
  Blue = Color % 6;
  Result = 0;
  if (Blue >= 3) {
    Result |= 1;
  }
  if (Green >= 3) {
    Result |= 2;
  }
  if (Red >= 3) {
    Result |= 4;
  }
  if (MAX (Red, MAX (Green, Blue)) >= 5) {
    Result |= 8;
  }
  return Result;
}

STATIC
UINTN
MapRgbColor (
  IN UINTN Red,
  IN UINTN Green,
  IN UINTN Blue
  )
{
  UINTN Result;

  Result = 0;
  if (Blue >= 128) {
    Result |= 1;
  }
  if (Green >= 128) {
    Result |= 2;
  }
  if (Red >= 128) {
    Result |= 4;
  }
  if (MAX (Red, MAX (Green, Blue)) >= 192) {
    Result |= 8;
  }
  return Result;
}

STATIC
VOID
SelectGraphicRendition (
  IN OUT WOLFSSH_UEFI_TERMINAL  *Terminal
  )
{
  UINTN Index;
  INT32 Value;

  for (Index = 0; Index < Terminal->ParameterCount; Index++) {
    Value = Parameter (Terminal, Index, 0);
    if (Value == 0) {
      Terminal->Foreground = Terminal->DefaultForeground;
      Terminal->Background = Terminal->DefaultBackground;
      Terminal->Bold = FALSE;
      Terminal->Reverse = FALSE;
    } else if (Value == 1) {
      Terminal->Bold = TRUE;
    } else if (Value == 22) {
      Terminal->Bold = FALSE;
    } else if (Value == 7) {
      Terminal->Reverse = TRUE;
    } else if (Value == 27) {
      Terminal->Reverse = FALSE;
    } else if ((Value >= 30) && (Value <= 37)) {
      Terminal->Foreground = (UINTN)(Value - 30);
    } else if ((Value >= 90) && (Value <= 97)) {
      Terminal->Foreground = (UINTN)(Value - 90 + 8);
    } else if (Value == 39) {
      Terminal->Foreground = Terminal->DefaultForeground;
    } else if ((Value >= 40) && (Value <= 47)) {
      Terminal->Background = (UINTN)(Value - 40);
    } else if ((Value >= 100) && (Value <= 107)) {
      Terminal->Background = (UINTN)(Value - 100 + 8);
    } else if (Value == 49) {
      Terminal->Background = Terminal->DefaultBackground;
    } else if (((Value == 38) || (Value == 48)) &&
               (Index + 2 < Terminal->ParameterCount) &&
               (Parameter (Terminal, Index + 1, -1) == 5)) {
      UINTN Color;
      Color = Map256Color ((UINTN)Parameter (Terminal, Index + 2, 0));
      if (Value == 38) {
        Terminal->Foreground = Color;
      } else {
        Terminal->Background = Color;
      }
      Index += 2;
    } else if (((Value == 38) || (Value == 48)) &&
               (Index + 4 < Terminal->ParameterCount) &&
               (Parameter (Terminal, Index + 1, -1) == 2)) {
      UINTN Color;
      Color = MapRgbColor (
                (UINTN)Parameter (Terminal, Index + 2, 0),
                (UINTN)Parameter (Terminal, Index + 3, 0),
                (UINTN)Parameter (Terminal, Index + 4, 0)
                );
      if (Value == 38) {
        Terminal->Foreground = Color;
      } else {
        Terminal->Background = Color;
      }
      Index += 4;
    }
  }
}

STATIC
VOID
EraseDisplay (
  IN OUT WOLFSSH_UEFI_TERMINAL  *Terminal,
  IN INT32                      Mode
  )
{
  UINTN Row;
  UINTN Attribute;

  Attribute = TerminalAttribute (Terminal);
  if ((Mode == 2) || (Mode == 3)) {
    ClearBuffer (Terminal, Terminal->Cells);
    gST->ConOut->ClearScreen (gST->ConOut);
    for (Row = 0; Row < Terminal->Rows; Row++) {
      Terminal->DirtyStart[Row] = (UINT16)Terminal->Columns;
      Terminal->DirtyEnd[Row] = 0;
    }
    return;
  }
  if (Mode == 1) {
    for (Row = 0; Row < Terminal->Row; Row++) {
      FillCells (Terminal, Row, 0, Terminal->Columns - 1, Attribute);
    }
    FillCells (Terminal, Terminal->Row, 0, Terminal->Column, Attribute);
    return;
  }
  FillCells (
    Terminal,
    Terminal->Row,
    Terminal->Column,
    Terminal->Columns - 1,
    Attribute
    );
  for (Row = Terminal->Row + 1; Row < Terminal->Rows; Row++) {
    FillCells (Terminal, Row, 0, Terminal->Columns - 1, Attribute);
  }
}

STATIC
VOID
EraseLine (
  IN OUT WOLFSSH_UEFI_TERMINAL  *Terminal,
  IN INT32                      Mode
  )
{
  UINTN Attribute;

  Attribute = TerminalAttribute (Terminal);
  if (Mode == 1) {
    FillCells (Terminal, Terminal->Row, 0, Terminal->Column, Attribute);
  } else if (Mode == 2) {
    FillCells (
      Terminal,
      Terminal->Row,
      0,
      Terminal->Columns - 1,
      Attribute
      );
  } else {
    FillCells (
      Terminal,
      Terminal->Row,
      Terminal->Column,
      Terminal->Columns - 1,
      Attribute
      );
  }
}

STATIC
VOID
SetAlternateScreen (
  IN OUT WOLFSSH_UEFI_TERMINAL  *Terminal,
  IN BOOLEAN                    Enable
  )
{
  if (Enable && !Terminal->AlternateActive) {
    Terminal->PrimaryRow = Terminal->Row;
    Terminal->PrimaryColumn = Terminal->Column;
    Terminal->Cells = Terminal->Alternate;
    Terminal->AlternateActive = TRUE;
    Terminal->Row = 0;
    Terminal->Column = 0;
    ClearBuffer (Terminal, Terminal->Alternate);
    Terminal->ScrollTop = 0;
    Terminal->ScrollBottom = Terminal->Rows - 1;
    gST->ConOut->ClearScreen (gST->ConOut);
  } else if (!Enable && Terminal->AlternateActive) {
    Terminal->Cells = Terminal->Primary;
    Terminal->AlternateActive = FALSE;
    Terminal->Row = MIN (Terminal->PrimaryRow, Terminal->Rows - 1);
    Terminal->Column = MIN (Terminal->PrimaryColumn, Terminal->Columns - 1);
    Terminal->ScrollTop = 0;
    Terminal->ScrollBottom = Terminal->Rows - 1;
    MarkDirtyRows (Terminal, 0, Terminal->Rows - 1);
  }
  Terminal->WrapPending = FALSE;
}

STATIC
VOID
SetModes (
  IN OUT WOLFSSH_UEFI_TERMINAL  *Terminal,
  IN BOOLEAN                    Enable
  )
{
  UINTN Index;
  INT32 Value;

  for (Index = 0; Index < Terminal->ParameterCount; Index++) {
    Value = Parameter (Terminal, Index, 0);
    if (Terminal->PrivateCsi) {
      if (Value == 1) {
        Terminal->ApplicationCursor = Enable;
      } else if (Value == 25) {
        Terminal->CursorVisible = Enable;
      } else if ((Value == 47) || (Value == 1047) || (Value == 1049)) {
        SetAlternateScreen (Terminal, Enable);
      }
    } else if (Value == 4) {
      Terminal->InsertMode = Enable;
    }
  }
}

STATIC
VOID
InsertLines (
  IN OUT WOLFSSH_UEFI_TERMINAL  *Terminal,
  IN UINTN                      Count
  )
{
  UINTN Row;
  UINTN Available;
  UINTN Attribute;

  if ((Terminal->Row < Terminal->ScrollTop) ||
      (Terminal->Row > Terminal->ScrollBottom)) {
    return;
  }
  Available = Terminal->ScrollBottom - Terminal->Row + 1;
  Count = MIN (Count, Available);
  Row = Terminal->ScrollBottom + 1;
  while (Row-- > Terminal->Row + Count) {
    CopyMem (
      CellAt (Terminal, Row, 0),
      CellAt (Terminal, Row - Count, 0),
      Terminal->Columns * sizeof (TERMINAL_CELL)
      );
  }
  Attribute = TerminalAttribute (Terminal);
  for (Row = Terminal->Row; Row < Terminal->Row + Count; Row++) {
    FillCells (Terminal, Row, 0, Terminal->Columns - 1, Attribute);
  }
  MarkDirtyRows (Terminal, Terminal->Row, Terminal->ScrollBottom);
}

STATIC
VOID
DeleteLines (
  IN OUT WOLFSSH_UEFI_TERMINAL  *Terminal,
  IN UINTN                      Count
  )
{
  UINTN Row;
  UINTN Available;
  UINTN Attribute;

  if ((Terminal->Row < Terminal->ScrollTop) ||
      (Terminal->Row > Terminal->ScrollBottom)) {
    return;
  }
  Available = Terminal->ScrollBottom - Terminal->Row + 1;
  Count = MIN (Count, Available);
  for (Row = Terminal->Row; Row + Count <= Terminal->ScrollBottom; Row++) {
    CopyMem (
      CellAt (Terminal, Row, 0),
      CellAt (Terminal, Row + Count, 0),
      Terminal->Columns * sizeof (TERMINAL_CELL)
      );
  }
  Attribute = TerminalAttribute (Terminal);
  for (; Row <= Terminal->ScrollBottom; Row++) {
    FillCells (Terminal, Row, 0, Terminal->Columns - 1, Attribute);
  }
  MarkDirtyRows (Terminal, Terminal->Row, Terminal->ScrollBottom);
}

STATIC
VOID
InsertCharacters (
  IN OUT WOLFSSH_UEFI_TERMINAL  *Terminal,
  IN UINTN                      Count
  )
{
  UINTN Column;
  UINTN Available;

  Available = Terminal->Columns - Terminal->Column;
  Count = MIN (Count, Available);
  Column = Terminal->Columns;
  while (Column-- > Terminal->Column + Count) {
    *CellAt (Terminal, Terminal->Row, Column) =
      *CellAt (Terminal, Terminal->Row, Column - Count);
  }
  FillCells (
    Terminal,
    Terminal->Row,
    Terminal->Column,
    Terminal->Column + Count - 1,
    TerminalAttribute (Terminal)
    );
  MarkDirty (
    Terminal,
    Terminal->Row,
    Terminal->Column,
    Terminal->Columns - 1
    );
}

STATIC
VOID
DeleteCharacters (
  IN OUT WOLFSSH_UEFI_TERMINAL  *Terminal,
  IN UINTN                      Count
  )
{
  UINTN Column;
  UINTN Available;

  Available = Terminal->Columns - Terminal->Column;
  Count = MIN (Count, Available);
  for (Column = Terminal->Column; Column + Count < Terminal->Columns; Column++) {
    *CellAt (Terminal, Terminal->Row, Column) =
      *CellAt (Terminal, Terminal->Row, Column + Count);
  }
  FillCells (
    Terminal,
    Terminal->Row,
    Terminal->Columns - Count,
    Terminal->Columns - 1,
    TerminalAttribute (Terminal)
    );
  MarkDirty (
    Terminal,
    Terminal->Row,
    Terminal->Column,
    Terminal->Columns - 1
    );
}

STATIC
VOID
HandleCsi (
  IN OUT WOLFSSH_UEFI_TERMINAL  *Terminal,
  IN UINT8                      FinalByte
  )
{
  UINTN Count;
  UINTN Row;
  UINTN Column;
  INT32 Mode;
  CHAR8 Reply[32];
  UINTN ReplyLength;

  Count = NonZeroParameter (Terminal, 0);
  Terminal->WrapPending = FALSE;
  switch (FinalByte) {
    case 'A':
      Terminal->Row = (Count > Terminal->Row) ? 0 : Terminal->Row - Count;
      break;
    case 'B':
      Terminal->Row = MIN (Terminal->Rows - 1, Terminal->Row + Count);
      break;
    case 'C':
      Terminal->Column = MIN (Terminal->Columns - 1, Terminal->Column + Count);
      break;
    case 'D':
      Terminal->Column = (Count > Terminal->Column) ? 0 : Terminal->Column - Count;
      break;
    case 'E':
      Terminal->Row = MIN (Terminal->Rows - 1, Terminal->Row + Count);
      Terminal->Column = 0;
      break;
    case 'F':
      Terminal->Row = (Count > Terminal->Row) ? 0 : Terminal->Row - Count;
      Terminal->Column = 0;
      break;
    case 'G':
      Terminal->Column = MIN (Terminal->Columns - 1, Count - 1);
      break;
    case 'H':
    case 'f':
      Row = NonZeroParameter (Terminal, 0) - 1;
      Column = NonZeroParameter (Terminal, 1) - 1;
      Terminal->Row = MIN (Terminal->Rows - 1, Row);
      Terminal->Column = MIN (Terminal->Columns - 1, Column);
      break;
    case 'd':
      Terminal->Row = MIN (Terminal->Rows - 1, Count - 1);
      break;
    case 'J':
      EraseDisplay (Terminal, Parameter (Terminal, 0, 0));
      break;
    case 'K':
      EraseLine (Terminal, Parameter (Terminal, 0, 0));
      break;
    case 'm':
      SelectGraphicRendition (Terminal);
      break;
    case 's':
      Terminal->SavedRow = Terminal->Row;
      Terminal->SavedColumn = Terminal->Column;
      break;
    case 'u':
      Terminal->Row = MIN (Terminal->SavedRow, Terminal->Rows - 1);
      Terminal->Column = MIN (Terminal->SavedColumn, Terminal->Columns - 1);
      break;
    case 'r':
      Row = NonZeroParameter (Terminal, 0) - 1;
      Column = (UINTN)Parameter (Terminal, 1, (INT32)Terminal->Rows) - 1;
      if ((Row < Column) && (Column < Terminal->Rows)) {
        Terminal->ScrollTop = Row;
        Terminal->ScrollBottom = Column;
        Terminal->Row = 0;
        Terminal->Column = 0;
      }
      break;
    case 'L':
      InsertLines (Terminal, Count);
      break;
    case 'M':
      DeleteLines (Terminal, Count);
      break;
    case '@':
      InsertCharacters (Terminal, Count);
      break;
    case 'P':
      DeleteCharacters (Terminal, Count);
      break;
    case 'X':
      FillCells (
        Terminal,
        Terminal->Row,
        Terminal->Column,
        MIN (Terminal->Columns - 1, Terminal->Column + Count - 1),
        TerminalAttribute (Terminal)
        );
      break;
    case 'S':
      ScrollUp (Terminal, Count);
      break;
    case 'T':
      ScrollDown (Terminal, Count);
      break;
    case 'h':
      SetModes (Terminal, TRUE);
      break;
    case 'l':
      SetModes (Terminal, FALSE);
      break;
    case 'n':
      Mode = Parameter (Terminal, 0, 0);
      if (Mode == 5) {
        QueueReply (Terminal, (CONST UINT8 *)"\x1b[0n", 4);
      } else if (Mode == 6) {
        ReplyLength = AsciiSPrint (
                        Reply,
                        sizeof (Reply),
                        "\x1b[%u;%uR",
                        (UINT32)(Terminal->Row + 1),
                        (UINT32)(Terminal->Column + 1)
                        );
        QueueReply (Terminal, (CONST UINT8 *)Reply, ReplyLength);
      }
      break;
    case 'c':
      QueueReply (Terminal, (CONST UINT8 *)"\x1b[?1;0c", 7);
      break;
    default:
      break;
  }
}

STATIC
VOID
ResetParser (
  IN OUT WOLFSSH_UEFI_TERMINAL  *Terminal
  )
{
  UINTN Index;

  Terminal->ParserState = ParserGround;
  Terminal->ParameterCount = 1;
  Terminal->PrivateCsi = FALSE;
  for (Index = 0; Index < TERMINAL_MAX_PARAMS; Index++) {
    Terminal->Parameters[Index] = -1;
  }
}

STATIC
VOID
EmitCodePoint (
  IN OUT WOLFSSH_UEFI_TERMINAL  *Terminal,
  IN UINT32                     CodePoint
  )
{
  if ((CodePoint == 0) || (CodePoint > 0xffff) ||
      ((CodePoint >= 0xd800) && (CodePoint <= 0xdfff))) {
    PutCharacter (Terminal, L'?');
  } else {
    PutCharacter (Terminal, (CHAR16)CodePoint);
  }
}

STATIC
VOID
HandleGroundByte (
  IN OUT WOLFSSH_UEFI_TERMINAL  *Terminal,
  IN UINT8                      Byte
  )
{
  if (Terminal->Utf8Remaining != 0) {
    if ((Byte & 0xc0) != 0x80) {
      Terminal->Utf8Remaining = 0;
      EmitCodePoint (Terminal, 0xfffd);
      HandleGroundByte (Terminal, Byte);
      return;
    }
    Terminal->Utf8CodePoint = (Terminal->Utf8CodePoint << 6) | (Byte & 0x3f);
    Terminal->Utf8Remaining--;
    if (Terminal->Utf8Remaining == 0) {
      EmitCodePoint (Terminal, Terminal->Utf8CodePoint);
    }
    return;
  }

  if (Byte == 0x1b) {
    Terminal->ParserState = ParserEscape;
  } else if (Byte == '\r') {
    Terminal->Column = 0;
    Terminal->WrapPending = FALSE;
  } else if (Byte == '\n') {
    IndexDown (Terminal);
  } else if (Byte == '\b') {
    if (Terminal->Column != 0) {
      Terminal->Column--;
    }
    Terminal->WrapPending = FALSE;
  } else if (Byte == '\t') {
    Terminal->Column = MIN (
                         Terminal->Columns - 1,
                         (Terminal->Column + 8) & ~(UINTN)7
                         );
    Terminal->WrapPending = FALSE;
  } else if (Byte >= 0x20 && Byte < 0x7f) {
    PutCharacter (Terminal, (CHAR16)Byte);
  } else if ((Byte & 0xe0) == 0xc0) {
    Terminal->Utf8Remaining = 1;
    Terminal->Utf8CodePoint = Byte & 0x1f;
  } else if ((Byte & 0xf0) == 0xe0) {
    Terminal->Utf8Remaining = 2;
    Terminal->Utf8CodePoint = Byte & 0x0f;
  } else if ((Byte & 0xf8) == 0xf0) {
    Terminal->Utf8Remaining = 3;
    Terminal->Utf8CodePoint = Byte & 0x07;
  }
}

VOID
WolfSshTerminalFeed (
  IN OUT WOLFSSH_UEFI_TERMINAL  *Terminal,
  IN CONST UINT8                *Data,
  IN UINTN                      Length
  )
{
  UINTN Index;
  UINT8 Byte;

  if ((Terminal == NULL) || (Data == NULL)) {
    return;
  }
  for (Index = 0; Index < Length; Index++) {
    Byte = Data[Index];
    if (Byte == 0x1b) {
      if (Terminal->ParserState == ParserOsc) {
        Terminal->ParserState = ParserOscEscape;
      } else if (Terminal->ParserState != ParserOscEscape) {
        Terminal->ParserState = ParserEscape;
        Terminal->Utf8Remaining = 0;
      }
      continue;
    }
    switch (Terminal->ParserState) {
      case ParserGround:
        HandleGroundByte (Terminal, Byte);
        break;
      case ParserEscape:
        if (Byte == '[') {
          ResetParser (Terminal);
          Terminal->ParserState = ParserCsi;
        } else if (Byte == ']') {
          Terminal->ParserState = ParserOsc;
        } else if (Byte == '7') {
          Terminal->SavedRow = Terminal->Row;
          Terminal->SavedColumn = Terminal->Column;
          Terminal->ParserState = ParserGround;
        } else if (Byte == '8') {
          Terminal->Row = MIN (Terminal->SavedRow, Terminal->Rows - 1);
          Terminal->Column = MIN (Terminal->SavedColumn, Terminal->Columns - 1);
          Terminal->ParserState = ParserGround;
        } else if (Byte == 'D') {
          IndexDown (Terminal);
          Terminal->ParserState = ParserGround;
        } else if (Byte == 'M') {
          IndexUp (Terminal);
          Terminal->ParserState = ParserGround;
        } else if (Byte == 'E') {
          Terminal->Column = 0;
          IndexDown (Terminal);
          Terminal->ParserState = ParserGround;
        } else if (Byte == 'c') {
          Terminal->Foreground = Terminal->DefaultForeground;
          Terminal->Background = Terminal->DefaultBackground;
          Terminal->Bold = FALSE;
          Terminal->Reverse = FALSE;
          Terminal->Row = 0;
          Terminal->Column = 0;
          Terminal->ScrollTop = 0;
          Terminal->ScrollBottom = Terminal->Rows - 1;
          ClearBuffer (Terminal, Terminal->Cells);
          gST->ConOut->ClearScreen (gST->ConOut);
          Terminal->ParserState = ParserGround;
        } else {
          Terminal->ParserState = ParserGround;
        }
        break;
      case ParserCsi:
        if ((Byte == '?') && (Terminal->ParameterCount == 1) &&
            (Terminal->Parameters[0] < 0)) {
          Terminal->PrivateCsi = TRUE;
        } else if ((Byte >= '0') && (Byte <= '9')) {
          INT32 *Value;
          Value = &Terminal->Parameters[Terminal->ParameterCount - 1];
          if (*Value < 0) {
            *Value = 0;
          }
          if (*Value < 1000000) {
            *Value = *Value * 10 + (Byte - '0');
          }
        } else if (Byte == ';') {
          if (Terminal->ParameterCount < TERMINAL_MAX_PARAMS) {
            Terminal->Parameters[Terminal->ParameterCount++] = -1;
          }
        } else if ((Byte >= 0x40) && (Byte <= 0x7e)) {
          HandleCsi (Terminal, Byte);
          Terminal->ParserState = ParserGround;
        }
        break;
      case ParserOsc:
        if (Byte == 0x07) {
          Terminal->ParserState = ParserGround;
        } else if (Byte == 0x1b) {
          Terminal->ParserState = ParserOscEscape;
        }
        break;
      case ParserOscEscape:
        Terminal->ParserState = (Byte == '\\') ? ParserGround : ParserOsc;
        break;
      default:
        Terminal->ParserState = ParserGround;
        break;
    }
  }
}

WOLFSSH_UEFI_TERMINAL *
WolfSshTerminalCreate (
  VOID
  )
{
  WOLFSSH_UEFI_TERMINAL *Terminal;
  EFI_STATUS            Status;
  UINTN                 Columns;
  UINTN                 Rows;
  UINTN                 Row;
  UINTN                 Attribute;

  if ((gST == NULL) || (gST->ConOut == NULL) ||
      (gST->ConOut->Mode == NULL)) {
    return NULL;
  }
  Columns = 80;
  Rows = 25;
  Status = gST->ConOut->QueryMode (
                         gST->ConOut,
                         gST->ConOut->Mode->Mode,
                         &Columns,
                         &Rows
                         );
  if (EFI_ERROR (Status) || (Columns == 0) || (Rows == 0) ||
      (Columns > MAX_UINT16) || (Rows > MAX_UINT16)) {
    Columns = 80;
    Rows = 25;
  }

  Terminal = AllocateZeroPool (sizeof (*Terminal));
  if (Terminal == NULL) {
    return NULL;
  }
  Terminal->Columns = Columns;
  Terminal->Rows = Rows;
  Terminal->Primary = AllocatePool (
                        Columns * Rows * sizeof (TERMINAL_CELL)
                        );
  Terminal->Alternate = AllocatePool (
                          Columns * Rows * sizeof (TERMINAL_CELL)
                          );
  Terminal->DirtyStart = AllocatePool (Rows * sizeof (UINT16));
  Terminal->DirtyEnd = AllocateZeroPool (Rows * sizeof (UINT16));
  Terminal->RenderBuffer = AllocatePool ((Columns + 1) * sizeof (CHAR16));
  if ((Terminal->Primary == NULL) || (Terminal->Alternate == NULL) ||
      (Terminal->DirtyStart == NULL) || (Terminal->DirtyEnd == NULL) ||
      (Terminal->RenderBuffer == NULL)) {
    WolfSshTerminalDestroy (Terminal);
    return NULL;
  }

  Attribute = (UINTN)gST->ConOut->Mode->Attribute;
  Terminal->DefaultForeground = Attribute & 0x0f;
  Terminal->DefaultBackground = (Attribute >> 4) & 0x07;
  Terminal->Foreground = Terminal->DefaultForeground;
  Terminal->Background = Terminal->DefaultBackground;
  Terminal->Cells = Terminal->Primary;
  Terminal->ScrollBottom = Rows - 1;
  Terminal->CursorVisible = TRUE;
  ClearBuffer (Terminal, Terminal->Primary);
  ClearBuffer (Terminal, Terminal->Alternate);
  for (Row = 0; Row < Rows; Row++) {
    Terminal->DirtyStart[Row] = (UINT16)Columns;
  }
  ResetParser (Terminal);
  return Terminal;
}

VOID
WolfSshTerminalFlush (
  IN OUT WOLFSSH_UEFI_TERMINAL  *Terminal
  )
{
  UINTN Row;
  UINTN Start;
  UINTN End;
  UINTN Position;
  UINTN RunEnd;
  UINTN Index;
  UINTN Attribute;

  if (Terminal == NULL) {
    return;
  }
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);
  for (Row = 0; Row < Terminal->Rows; Row++) {
    Start = Terminal->DirtyStart[Row];
    End = Terminal->DirtyEnd[Row];
    if ((Start >= Terminal->Columns) || (End < Start)) {
      continue;
    }
    Position = Start;
    while (Position <= End) {
      Attribute = CellAt (Terminal, Row, Position)->Attribute;
      RunEnd = Position;
      while ((RunEnd < End) &&
             (CellAt (Terminal, Row, RunEnd + 1)->Attribute == Attribute)) {
        RunEnd++;
      }
      for (Index = Position; Index <= RunEnd; Index++) {
        Terminal->RenderBuffer[Index - Position] =
          CellAt (Terminal, Row, Index)->Character;
      }
      Terminal->RenderBuffer[RunEnd - Position + 1] = L'\0';
      gST->ConOut->SetAttribute (gST->ConOut, Attribute);
      gST->ConOut->SetCursorPosition (gST->ConOut, Position, Row);
      gST->ConOut->OutputString (gST->ConOut, Terminal->RenderBuffer);
      Position = RunEnd + 1;
    }
    Terminal->DirtyStart[Row] = (UINT16)Terminal->Columns;
    Terminal->DirtyEnd[Row] = 0;
  }
  gST->ConOut->SetAttribute (gST->ConOut, TerminalAttribute (Terminal));
  gST->ConOut->SetCursorPosition (
                 gST->ConOut,
                 Terminal->Column,
                 Terminal->Row
                 );
  gST->ConOut->EnableCursor (gST->ConOut, Terminal->CursorVisible);
}

VOID
WolfSshTerminalDestroy (
  IN WOLFSSH_UEFI_TERMINAL  *Terminal
  )
{
  if (Terminal == NULL) {
    return;
  }
  if ((gST != NULL) && (gST->ConOut != NULL)) {
    gST->ConOut->SetAttribute (
                   gST->ConOut,
                   EFI_TEXT_ATTR (
                     Terminal->DefaultForeground,
                     Terminal->DefaultBackground
                     )
                   );
    gST->ConOut->EnableCursor (gST->ConOut, TRUE);
  }
  FreePool (Terminal->RenderBuffer);
  FreePool (Terminal->DirtyEnd);
  FreePool (Terminal->DirtyStart);
  FreePool (Terminal->Alternate);
  FreePool (Terminal->Primary);
  FreePool (Terminal);
}

UINTN
WolfSshTerminalTakeResponse (
  IN OUT WOLFSSH_UEFI_TERMINAL  *Terminal,
  OUT UINT8                     *Buffer,
  IN UINTN                      Capacity
  )
{
  UINTN Length;

  if ((Terminal == NULL) || (Buffer == NULL) || (Capacity == 0)) {
    return 0;
  }
  Length = MIN (Capacity, Terminal->ReplyLength);
  CopyMem (Buffer, Terminal->Replies, Length);
  Terminal->ReplyLength -= Length;
  if (Terminal->ReplyLength != 0) {
    CopyMem (
      Terminal->Replies,
      Terminal->Replies + Length,
      Terminal->ReplyLength
      );
  }
  return Length;
}

STATIC
EFI_STATUS
CopyKeySequence (
  OUT UINT8        *Buffer,
  IN OUT UINTN     *Length,
  IN CONST CHAR8   *Sequence
  )
{
  UINTN Required;

  Required = AsciiStrLen (Sequence);
  if (*Length < Required) {
    *Length = Required;
    return EFI_BUFFER_TOO_SMALL;
  }
  CopyMem (Buffer, Sequence, Required);
  *Length = Required;
  return EFI_SUCCESS;
}

EFI_STATUS
WolfSshTerminalReadKey (
  IN WOLFSSH_UEFI_TERMINAL  *Terminal,
  OUT UINT8                 *Buffer,
  IN OUT UINTN              *Length,
  OUT BOOLEAN               *LocalExit
  )
{
  EFI_INPUT_KEY Key;
  EFI_STATUS    Status;
  CHAR8         Sequence[4];
  CONST CHAR8   *Mapped;
  UINTN         Capacity;

  if ((Terminal == NULL) || (Buffer == NULL) || (Length == NULL) ||
      (LocalExit == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  Capacity = *Length;
  *Length = 0;
  *LocalExit = FALSE;
  Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Key.UnicodeChar == 0x1d) {
    *LocalExit = TRUE;
    return EFI_SUCCESS;
  }
  Mapped = NULL;
  switch (Key.ScanCode) {
    case SCAN_UP:
      Mapped = Terminal->ApplicationCursor ? "\x1bOA" : "\x1b[A";
      break;
    case SCAN_DOWN:
      Mapped = Terminal->ApplicationCursor ? "\x1bOB" : "\x1b[B";
      break;
    case SCAN_RIGHT:
      Mapped = Terminal->ApplicationCursor ? "\x1bOC" : "\x1b[C";
      break;
    case SCAN_LEFT:
      Mapped = Terminal->ApplicationCursor ? "\x1bOD" : "\x1b[D";
      break;
    case SCAN_HOME:
      Mapped = Terminal->ApplicationCursor ? "\x1bOH" : "\x1b[H";
      break;
    case SCAN_END:
      Mapped = Terminal->ApplicationCursor ? "\x1bOF" : "\x1b[F";
      break;
    case SCAN_INSERT:
      Mapped = "\x1b[2~";
      break;
    case SCAN_DELETE:
      Mapped = "\x1b[3~";
      break;
    case SCAN_PAGE_UP:
      Mapped = "\x1b[5~";
      break;
    case SCAN_PAGE_DOWN:
      Mapped = "\x1b[6~";
      break;
    case SCAN_F1:
      Mapped = "\x1bOP";
      break;
    case SCAN_F2:
      Mapped = "\x1bOQ";
      break;
    case SCAN_F3:
      Mapped = "\x1bOR";
      break;
    case SCAN_F4:
      Mapped = "\x1bOS";
      break;
    case SCAN_F5:
      Mapped = "\x1b[15~";
      break;
    case SCAN_F6:
      Mapped = "\x1b[17~";
      break;
    case SCAN_F7:
      Mapped = "\x1b[18~";
      break;
    case SCAN_F8:
      Mapped = "\x1b[19~";
      break;
    case SCAN_F9:
      Mapped = "\x1b[20~";
      break;
    case SCAN_F10:
      Mapped = "\x1b[21~";
      break;
    default:
      break;
  }
  if (Mapped != NULL) {
    *Length = Capacity;
    return CopyKeySequence (Buffer, Length, Mapped);
  }
  if (Key.UnicodeChar == 0) {
    return EFI_NOT_READY;
  }
  if (Capacity < 1) {
    *Length = 1;
    return EFI_BUFFER_TOO_SMALL;
  }
  if (Key.UnicodeChar == CHAR_BACKSPACE) {
    Buffer[0] = 0x7f;
    *Length = 1;
  } else if (Key.UnicodeChar < 0x80) {
    Buffer[0] = (UINT8)Key.UnicodeChar;
    *Length = 1;
  } else if (Key.UnicodeChar < 0x800) {
    if (Capacity < 2) {
      *Length = 2;
      return EFI_BUFFER_TOO_SMALL;
    }
    Buffer[0] = (UINT8)(0xc0 | (Key.UnicodeChar >> 6));
    Buffer[1] = (UINT8)(0x80 | (Key.UnicodeChar & 0x3f));
    *Length = 2;
  } else {
    if (Capacity < 3) {
      *Length = 3;
      return EFI_BUFFER_TOO_SMALL;
    }
    Sequence[0] = (CHAR8)(0xe0 | (Key.UnicodeChar >> 12));
    Sequence[1] = (CHAR8)(0x80 | ((Key.UnicodeChar >> 6) & 0x3f));
    Sequence[2] = (CHAR8)(0x80 | (Key.UnicodeChar & 0x3f));
    CopyMem (Buffer, Sequence, 3);
    *Length = 3;
  }
  return EFI_SUCCESS;
}

EFI_STATUS
WolfSshTerminalSelfTest (
  VOID
  )
{
  STATIC CONST UINT8 TestSequence[] =
    "\x1b[2J\x1b[H"
    "\x1b[1;36mwolfssh-uefi VT test\x1b[0m\r\n"
    "erase-me\x1b[2K\r"
    "\x1b[32mWOLFSSH_UEFI_TERMINAL_SELFTEST_OK\x1b[0m\r\n"
    "\x1b[?1049halternate-screen\x1b[?1049l";
  WOLFSSH_UEFI_TERMINAL *Terminal;

  Terminal = WolfSshTerminalCreate ();
  if (Terminal == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  WolfSshTerminalFeed (Terminal, TestSequence, sizeof (TestSequence) - 1);
  WolfSshTerminalFlush (Terminal);
  WolfSshTerminalDestroy (Terminal);
  Print (L"\r\nWOLFSSH_UEFI_TERMINAL_SELFTEST_PASS\r\n");
  return EFI_SUCCESS;
}

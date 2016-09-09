/********************************************************
*                                                       *
*   Copyright (C) Microsoft. All rights reserved.       *
*                                                       *
********************************************************/

#include "precomp.h"

#include "_output.h"
#include "output.h"

#include "cursor.h"
#include "dbcs.h"
#include "directio.h"
#include "misc.h"
#include "window.hpp"

#pragma hdrstop

// Attributes flags:
#define COMMON_LVB_GRID_SINGLEFLAG 0x2000   // DBCS: Grid attribute: use for ime cursor.

SHORT CalcWideCharToColumn(_In_ PCHAR_INFO Buffer, _In_ size_t NumberOfChars);

void ConsoleImeViewInfo(_In_ PCONVERSIONAREA_INFORMATION ConvAreaInfo, _In_ COORD coordConView);
void ConsoleImeWindowInfo(_In_ PCONVERSIONAREA_INFORMATION ConvAreaInfo, _In_ SMALL_RECT rcViewCaWindow);
NTSTATUS ConsoleImeResizeScreenBuffer(_In_ PSCREEN_INFORMATION ScreenInfo, _In_ COORD NewScreenSize, _In_ PCONVERSIONAREA_INFORMATION ConvAreaInfo);
NTSTATUS ConsoleImeWriteOutput(_In_ PCONVERSIONAREA_INFORMATION ConvAreaInfo, _In_ PCHAR_INFO Buffer, _In_ SMALL_RECT CharRegion, _In_ BOOL fUnicode);
bool InsertConvertedString(_In_ LPCWSTR lpStr);
void StreamWriteToScreenBufferIME(_In_reads_(StringLength) PWCHAR String,
                                  _In_ USHORT StringLength,
                                  _In_ PSCREEN_INFORMATION ScreenInfo,
                                  _In_reads_(StringLength) PCHAR StringA);

// Routine Description:
// - This method gives a rectangle to where the command edit line text is currently rendered
//   such that the IME suggestion window can pop up in a suitable location adjacent to the given rectangle.
// Arguments:
// - <none>
// Return Value:
// - Rectangle specifying current command line edit area.
RECT GetImeSuggestionWindowPos()
{
    TEXT_BUFFER_INFO* const ptbi = g_ciConsoleInformation.CurrentScreenBuffer->TextInfo;

    COORD const coordCursor = ptbi->GetCursor()->GetPosition();
    COORD const coordFont = ptbi->GetCurrentFont()->GetSize();

    // Map the point to be just under the current cursor position. Convert from coordinate to pixels using font.
    POINT ptSuggestion;
    ptSuggestion.x = (coordCursor.X + 1) * coordFont.X;
    ptSuggestion.y = (coordCursor.Y) * coordFont.Y;

    // Adjust client point to screen point via HWND.
    ClientToScreen(g_ciConsoleInformation.hWnd, &ptSuggestion);

    // Move into suggestion rectangle.
    RECT rcSuggestion = { 0 };
    rcSuggestion.top = rcSuggestion.bottom = ptSuggestion.y;
    rcSuggestion.left = rcSuggestion.right = ptSuggestion.x;

    // Add 1 line height and a few characters of width to represent the area where we're writing text.
    // This could be more exact by looking up the CONVAREA but it works well enough this way.
    // If there is a future issue with the pop-up window, tweak these metrics.
    rcSuggestion.bottom += coordFont.Y;
    rcSuggestion.right += (coordFont.X * 10);

    return rcSuggestion;
}

void LinkConversionArea(_In_ PCONVERSIONAREA_INFORMATION ConvAreaInfo)
{
    if (g_ciConsoleInformation.ConsoleIme.ConvAreaRoot == nullptr)
    {
        g_ciConsoleInformation.ConsoleIme.ConvAreaRoot = ConvAreaInfo;
    }
    else
    {
        PCONVERSIONAREA_INFORMATION PrevConvAreaInfo = g_ciConsoleInformation.ConsoleIme.ConvAreaRoot;
        while (PrevConvAreaInfo->ConvAreaNext)
        {
            PrevConvAreaInfo = PrevConvAreaInfo->ConvAreaNext;
        }

        PrevConvAreaInfo->ConvAreaNext = ConvAreaInfo;
    }
}

// Routine Description:
// - This routine frees the memory associated with a screen buffer.
// Arguments:
// - ScreenInfo - screen buffer data to free.
// Return Value:
// Note:
// - console handle table lock must be held when calling this routine
void FreeConvAreaScreenBuffer(_Inout_ PSCREEN_INFORMATION pScreenInfo)
{
    delete pScreenInfo;
}

NTSTATUS AllocateConversionArea(_In_ COORD dwScreenBufferSize, _Out_ PCONVERSIONAREA_INFORMATION * ConvAreaInfo)
{
    // allocate console data
    if (g_ciConsoleInformation.CurrentScreenBuffer == nullptr)
    {
        return STATUS_UNSUCCESSFUL;
    }

    PCONVERSIONAREA_INFORMATION ca = new CONVERSIONAREA_INFORMATION();
    if (ca == nullptr)
    {
        return STATUS_NO_MEMORY;
    }

    COORD dwWindowSize;
    dwWindowSize.X = g_ciConsoleInformation.CurrentScreenBuffer->GetScreenWindowSizeX();
    dwWindowSize.Y = g_ciConsoleInformation.CurrentScreenBuffer->GetScreenWindowSizeY();

    CHAR_INFO Fill;
    Fill.Attributes = g_ciConsoleInformation.CurrentScreenBuffer->GetAttributes()->GetLegacyAttributes();

    CHAR_INFO PopupFill;
    PopupFill.Attributes = g_ciConsoleInformation.CurrentScreenBuffer->GetPopupAttributes()->GetLegacyAttributes();

    const FontInfo* const pfiFont = g_ciConsoleInformation.CurrentScreenBuffer->TextInfo->GetCurrentFont();

    NTSTATUS Status = SCREEN_INFORMATION::CreateInstance(dwWindowSize,
                                                         pfiFont,
                                                         dwScreenBufferSize,
                                                         Fill,
                                                         PopupFill,
                                                         0, // cursor has no height because it won't be rendered for conversion areas.
                                                         &ca->ScreenBuffer);
    if (!NT_SUCCESS(Status))
    {
        delete ca->ScreenBuffer;
        delete ca;
        return Status;
    }

    // Suppress painting notifications for modifying a conversion area cursor as they're not actually rendered.
    ca->ScreenBuffer->TextInfo->GetCursor()->SetIsConversionArea(TRUE);

    *ConvAreaInfo = ca;

    return STATUS_SUCCESS;
}

NTSTATUS SetUpConversionArea(_In_ COORD coordCaBuffer,
                             _In_ SMALL_RECT rcViewCaWindow,
                             _In_ COORD coordConView,
                             _In_ DWORD dwOption,
                             _Out_ PCONVERSIONAREA_INFORMATION * ConvAreaInfo)
{
    PCONVERSIONAREA_INFORMATION ca;
    NTSTATUS Status = AllocateConversionArea(coordCaBuffer, &ca);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    ca->ConversionAreaMode = dwOption;
    ca->CaInfo.coordCaBuffer = coordCaBuffer;
    ca->CaInfo.rcViewCaWindow = rcViewCaWindow;
    ca->CaInfo.coordConView = coordConView;

    ca->ConvAreaNext = nullptr;

    ca->ScreenBuffer->ConvScreenInfo = ca;

    LinkConversionArea(ca);

    SetUndetermineAttribute();

    *ConvAreaInfo = ca;

    return STATUS_SUCCESS;
}

bool IsValidSmallRect(_In_ PSMALL_RECT const Rect)
{
    return (Rect->Right >= Rect->Left && Rect->Bottom >= Rect->Top);
}

void WriteConvRegionToScreen(_In_ const SCREEN_INFORMATION * const pScreenInfo,
                             _In_opt_ PCONVERSIONAREA_INFORMATION pConvAreaInfo,
                             _In_ const SMALL_RECT * const psrConvRegion)
{
    if (!pScreenInfo->IsActiveScreenBuffer())
    {
        return;
    }

    while (pConvAreaInfo)
    {
        if ((pConvAreaInfo->ConversionAreaMode & (CA_HIDDEN)) == 0)
        {
            // Do clipping region
            SMALL_RECT Region;
            Region.Left = pScreenInfo->BufferViewport.Left + pConvAreaInfo->CaInfo.rcViewCaWindow.Left + pConvAreaInfo->CaInfo.coordConView.X;
            Region.Right = Region.Left + (pConvAreaInfo->CaInfo.rcViewCaWindow.Right - pConvAreaInfo->CaInfo.rcViewCaWindow.Left);
            Region.Top = pScreenInfo->BufferViewport.Top + pConvAreaInfo->CaInfo.rcViewCaWindow.Top + pConvAreaInfo->CaInfo.coordConView.Y;
            Region.Bottom = Region.Top + (pConvAreaInfo->CaInfo.rcViewCaWindow.Bottom - pConvAreaInfo->CaInfo.rcViewCaWindow.Top);

            SMALL_RECT ClippedRegion;
            ClippedRegion.Left = max(Region.Left, pScreenInfo->BufferViewport.Left);
            ClippedRegion.Top = max(Region.Top, pScreenInfo->BufferViewport.Top);
            ClippedRegion.Right = min(Region.Right, pScreenInfo->BufferViewport.Right);
            ClippedRegion.Bottom = min(Region.Bottom, pScreenInfo->BufferViewport.Bottom);

            if (IsValidSmallRect(&ClippedRegion))
            {
                Region = ClippedRegion;
                ClippedRegion.Left = max(Region.Left, psrConvRegion->Left);
                ClippedRegion.Top = max(Region.Top, psrConvRegion->Top);
                ClippedRegion.Right = min(Region.Right, psrConvRegion->Right);
                ClippedRegion.Bottom = min(Region.Bottom, psrConvRegion->Bottom);
                if (IsValidSmallRect(&ClippedRegion))
                {
                    // if we have a renderer, we need to update. 
                    // we've already confirmed (above with an early return) that we're on conversion areas that are a part of the active (visible/rendered) screen 
                    // so send invalidates to those regions such that we're queried for data on the next frame and repainted.
                    if (g_pRender != nullptr)
                    {
                        // convert inclusive rectangle to exclusive rectangle
                        SMALL_RECT srExclusive = ClippedRegion;
                        srExclusive.Right++;
                        srExclusive.Bottom++;

                        g_pRender->TriggerRedraw(&srExclusive);
                    }
                }
            }
        }

        pConvAreaInfo = pConvAreaInfo->ConvAreaNext;
    }
}

NTSTATUS CreateConvAreaUndetermine()
{
    PCONSOLE_IME_INFORMATION const ConsoleIme = &g_ciConsoleInformation.ConsoleIme;
    
    COORD coordCaBuffer;
    coordCaBuffer = g_ciConsoleInformation.CurrentScreenBuffer->ScreenBufferSize;
    coordCaBuffer.Y = 1;

    SMALL_RECT rcViewCaWindow;
    rcViewCaWindow.Top = 0;
    rcViewCaWindow.Left = 0;
    rcViewCaWindow.Bottom = 0;
    rcViewCaWindow.Right = 0;
    
    COORD coordConView; 
    coordConView.X = 0;
    coordConView.Y = 0;

    PCONVERSIONAREA_INFORMATION ConvAreaInfo;
    RETURN_IF_NTSTATUS_FAILED(SetUpConversionArea(coordCaBuffer,
                                                  rcViewCaWindow,
                                                  coordConView,
                                                  CA_HIDDEN,
                                                  &ConvAreaInfo));

    try
    {
        ConsoleIme->ConvAreaCompStr.push_back(ConvAreaInfo);
    }
    catch (std::bad_alloc)
    {
        RETURN_NTSTATUS(STATUS_NO_MEMORY);
    }
    catch (...)
    {
        return wil::ResultFromCaughtException();
    }
    
    return STATUS_SUCCESS;
}

#define LOCAL_BUFFER_SIZE 100
NTSTATUS WriteUndetermineChars(_In_reads_(NumChars) LPWSTR lpString, _In_ PBYTE lpAtr, _In_reads_(CONIME_ATTRCOLOR_SIZE) PWORD lpAtrIdx, _In_ DWORD NumChars)
{
    PCONSOLE_IME_INFORMATION const ConsoleIme = &g_ciConsoleInformation.ConsoleIme;
    PSCREEN_INFORMATION const ScreenInfo = g_ciConsoleInformation.CurrentScreenBuffer;

    COORD Position = ScreenInfo->TextInfo->GetCursor()->GetPosition();
    COORD WindowOrigin;
    
    if ((ScreenInfo->BufferViewport.Left <= Position.X && Position.X <= ScreenInfo->BufferViewport.Right) &&
        (ScreenInfo->BufferViewport.Top <= Position.Y && Position.Y <= ScreenInfo->BufferViewport.Bottom))
    {
        Position.X = ScreenInfo->TextInfo->GetCursor()->GetPosition().X - ScreenInfo->BufferViewport.Left;
        Position.Y = ScreenInfo->TextInfo->GetCursor()->GetPosition().Y - ScreenInfo->BufferViewport.Top;
    }
    else
    {
        WindowOrigin.X = 0;
        WindowOrigin.Y = (SHORT)(Position.Y - ScreenInfo->BufferViewport.Bottom);
        ScreenInfo->SetViewportOrigin(FALSE, WindowOrigin);
    }

    SHORT PosY = Position.Y;
    ULONG NumStr;
    NumStr = WideCharToMultiByte(CP_ACP, 0, lpString, NumChars * sizeof(WCHAR), nullptr, 0, nullptr, nullptr);

    int const WholeLen = (int)Position.X + (int)NumStr;
    int const WholeRow = WholeLen / ScreenInfo->GetScreenWindowSizeX();

    if ((PosY + WholeRow) > (ScreenInfo->GetScreenWindowSizeY() - 1))
    {
        PosY = (SHORT)(ScreenInfo->GetScreenWindowSizeY() - 1 - WholeRow);
        if (PosY < 0)
        {
            PosY = ScreenInfo->BufferViewport.Top;
        }
    }

    BOOL UndetAreaUp = FALSE;
    if (PosY != Position.Y)
    {
        Position.Y = PosY;
        UndetAreaUp = TRUE;
    }

    DWORD ConvAreaIndex = 0;

    DWORD const BufferSize = NumChars;
    NumChars = 0;

    PCONVERSIONAREA_INFORMATION ConvAreaInfo;
    for (ConvAreaIndex = 0; NumChars < BufferSize; ConvAreaIndex++)
    {
        if (ConvAreaIndex + 1 > ConsoleIme->ConvAreaCompStr.size())
        {
            NTSTATUS Status;

            Status = CreateConvAreaUndetermine();
            if (!NT_SUCCESS(Status))
            {
                return Status;
            }
        }
        ConvAreaInfo = ConsoleIme->ConvAreaCompStr[ConvAreaIndex];
        PSCREEN_INFORMATION const ConvScreenInfo = ConvAreaInfo->ScreenBuffer;
        ConvScreenInfo->TextInfo->GetCursor()->SetXPosition(Position.X);

        if ((ConvAreaInfo->ConversionAreaMode & CA_HIDDEN) || (UndetAreaUp))
        {
            // This conversion area need positioning onto cursor position.
            COORD CursorPosition;
            CursorPosition.X = 0;
            CursorPosition.Y = (SHORT)(Position.Y + ConvAreaIndex);
            ConsoleImeViewInfo(ConvAreaInfo, CursorPosition);
        }

        SMALL_RECT Region;
        Region.Left = ConvScreenInfo->TextInfo->GetCursor()->GetPosition().X;
        Region.Top = 0;
        Region.Bottom = 0;

        while (NumChars < BufferSize)
        {
            ULONG i = 0;
            WCHAR LocalBuffer[LOCAL_BUFFER_SIZE];
            BYTE LocalBufferA[LOCAL_BUFFER_SIZE];

            PWCHAR LocalBufPtr = LocalBuffer;
            PBYTE LocalBufPtrA = LocalBufferA;

            WCHAR Char = 0;
            WORD Attr = 0;
            while (NumChars < BufferSize && i < LOCAL_BUFFER_SIZE && Position.X < ScreenInfo->GetScreenWindowSizeX())
            {
                Char = *lpString;
#pragma prefast(suppress:__WARNING_INCORRECT_ANNOTATION, "Precarious but this is internal-only code so we can live with it")
                Attr = *lpAtr;
                if (Char >= (WCHAR)' ')
                {
                    if (IsCharFullWidth(Char))
                    {
                        if (i < (LOCAL_BUFFER_SIZE - 1) && Position.X < ScreenInfo->GetScreenWindowSizeX() - 1)
                        {
                            *LocalBufPtr++ = Char;
                            *LocalBufPtrA++ = CHAR_ROW::ATTR_LEADING_BYTE;
                            *LocalBufPtr++ = Char;
                            *LocalBufPtrA++ = CHAR_ROW::ATTR_TRAILING_BYTE;
                            Position.X += 2;
                            i += 2;
                        }
                        else
                        {
                            Position.X++;
                            break;
                        }
                    }
                    else
                    {
                        *LocalBufPtr++ = Char;
                        *LocalBufPtrA++ = 0;
                        Position.X++;
                        i++;
                    }
                }
                lpString++;
                lpAtr++;
                NumChars++;

                if (NumChars < BufferSize && Attr != *lpAtr)
                {
                    break;
                }
            }

            if (i != 0)
            {
                WORD wLegacyAttr = lpAtrIdx[Attr & 0x07];
                if (Attr & 0x10)
                {
                    wLegacyAttr |= (COMMON_LVB_GRID_SINGLEFLAG | COMMON_LVB_GRID_RVERTICAL);
                }
                else if (Attr & 0x20)
                {
                    wLegacyAttr |= (COMMON_LVB_GRID_SINGLEFLAG | COMMON_LVB_GRID_LVERTICAL);
                }
                TextAttribute taAttribute = TextAttribute(wLegacyAttr);
                ConvScreenInfo->SetAttributes(&taAttribute);

                StreamWriteToScreenBufferIME(LocalBuffer, (USHORT) i, ConvScreenInfo, (PCHAR) LocalBufferA);

                ConvScreenInfo->TextInfo->GetCursor()->IncrementXPosition(i);

                if (NumChars == BufferSize ||
                    Position.X >= ScreenInfo->GetScreenWindowSizeX() ||
                    ((Char >= (WCHAR)' ' &&
                      IsCharFullWidth(Char) &&
                      Position.X >= ScreenInfo->GetScreenWindowSizeX() - 1)))
                {

                    Region.Right = (SHORT)(ConvScreenInfo->TextInfo->GetCursor()->GetPosition().X - 1);
                    ConsoleImeWindowInfo(ConvAreaInfo, Region);

                    ConvAreaInfo->ConversionAreaMode &= ~CA_HIDDEN;

                    ConsoleImePaint(ConvAreaInfo);

                    Position.X = 0;
                    break;
                }

                if (NumChars == BufferSize)
                {
                    return STATUS_SUCCESS;
                }
                continue;

            }
            else if (NumChars == BufferSize)
            {
                return STATUS_SUCCESS;
            }

            if (Position.X >= ScreenInfo->GetScreenWindowSizeX())
            {
                Position.X = 0;
                break;
            }
        }

    }

    for (; ConvAreaIndex < ConsoleIme->ConvAreaCompStr.size(); ConvAreaIndex++)
    {
        ConvAreaInfo = ConsoleIme->ConvAreaCompStr[ConvAreaIndex];
        if (!(ConvAreaInfo->ConversionAreaMode & CA_HIDDEN))
        {
            ConvAreaInfo->ConversionAreaMode |= CA_HIDDEN;
            ConsoleImePaint(ConvAreaInfo);
        }
    }

    return STATUS_SUCCESS;
}

NTSTATUS FillUndetermineChars(_In_ PCONVERSIONAREA_INFORMATION ConvAreaInfo)
{
    ConvAreaInfo->ConversionAreaMode |= CA_HIDDEN;
    
    COORD Coord = { 0 };
    DWORD CharsToWrite = ConvAreaInfo->ScreenBuffer->ScreenBufferSize.X;
    
    FillOutput(ConvAreaInfo->ScreenBuffer, (WCHAR)' ', Coord, CONSOLE_FALSE_UNICODE,    // faster than real unicode
               &CharsToWrite);

    CharsToWrite = ConvAreaInfo->ScreenBuffer->ScreenBufferSize.X;
    FillOutput(ConvAreaInfo->ScreenBuffer, g_ciConsoleInformation.CurrentScreenBuffer->GetAttributes()->GetLegacyAttributes(), Coord, CONSOLE_ATTRIBUTE, &CharsToWrite);
    ConsoleImePaint(ConvAreaInfo);
    return STATUS_SUCCESS;
}


NTSTATUS ConsoleImeCompStr(_In_ LPCONIME_UICOMPMESSAGE CompStr)
{
    CONSOLE_IME_INFORMATION* const pIme = &g_ciConsoleInformation.ConsoleIme;

    if (CompStr->dwCompStrLen == 0 || CompStr->dwResultStrLen != 0)
    {
        // Cursor turn ON.
        if (pIme->SavedCursorVisible)
        {
            pIme->SavedCursorVisible = FALSE;
            g_ciConsoleInformation.CurrentScreenBuffer->SetCursorInformation(g_ciConsoleInformation.CurrentScreenBuffer->TextInfo->GetCursor()->GetSize(), TRUE);
        }

        // Determine string.
        for (auto it = pIme->ConvAreaCompStr.begin(); it != pIme->ConvAreaCompStr.end(); ++it)
        {
            PCONVERSIONAREA_INFORMATION const ConvAreaInfo = *it;
            if (ConvAreaInfo && (!(ConvAreaInfo->ConversionAreaMode & CA_HIDDEN)))
            {
                FillUndetermineChars(ConvAreaInfo);
            }
        }

        if (CompStr->dwResultStrLen != 0)
        {
            #pragma prefast(suppress:26035, "CONIME_UICOMPMESSAGE structure impossible for PREfast to trace due to its structure.")
            if (!InsertConvertedString((LPCWSTR) ((PBYTE) CompStr + CompStr->dwResultStrOffset)))
            {
                return STATUS_INVALID_HANDLE;
            }
        }

        if (pIme->CompStrData)
        {
            delete[] pIme->CompStrData;
            pIme->CompStrData = nullptr;
        }
    }
    else
    {
        LPWSTR lpStr;
        PBYTE lpAtr;
        PWORD lpAtrIdx;

        // Cursor turn OFF.
        if (g_ciConsoleInformation.CurrentScreenBuffer->TextInfo->GetCursor()->IsVisible())
        {
            pIme->SavedCursorVisible = TRUE;
            g_ciConsoleInformation.CurrentScreenBuffer->SetCursorInformation(g_ciConsoleInformation.CurrentScreenBuffer->TextInfo->GetCursor()->GetSize(), FALSE);
        }

        // Composition string.
        for (auto it = pIme->ConvAreaCompStr.begin(); it != pIme->ConvAreaCompStr.end(); ++it)
        {
            PCONVERSIONAREA_INFORMATION const ConvAreaInfo = *it;
            if (ConvAreaInfo && (!(ConvAreaInfo->ConversionAreaMode & CA_HIDDEN)))
            {
                FillUndetermineChars(ConvAreaInfo);
            }
        }

        lpStr = (LPWSTR) ((PBYTE) CompStr + CompStr->dwCompStrOffset);
        lpAtr = (PBYTE) CompStr + CompStr->dwCompAttrOffset;
        lpAtrIdx = (PWORD) CompStr->CompAttrColor;
        #pragma prefast(suppress:26035, "CONIME_UICOMPMESSAGE structure impossible for PREfast to trace due to its structure.")
        WriteUndetermineChars(lpStr, lpAtr, lpAtrIdx, CompStr->dwCompStrLen / sizeof(WCHAR));
    }

    return STATUS_SUCCESS;
}

NTSTATUS ConsoleImeResizeCompStrView()
{
    CONSOLE_IME_INFORMATION* const pIme = &g_ciConsoleInformation.ConsoleIme;

    // Compositon string
    LPCONIME_UICOMPMESSAGE const CompStr = pIme->CompStrData;
    if (CompStr)
    {
        for (auto it = pIme->ConvAreaCompStr.begin(); it != pIme->ConvAreaCompStr.end(); ++it)
        {
            PCONVERSIONAREA_INFORMATION const ConvAreaInfo = *it;
            if (ConvAreaInfo && (!(ConvAreaInfo->ConversionAreaMode & CA_HIDDEN)))
            {
                FillUndetermineChars(ConvAreaInfo);
            }
        }

        LPWSTR lpStr = (LPWSTR) ((PBYTE) CompStr + CompStr->dwCompStrOffset);
        PBYTE lpAtr = (PBYTE) CompStr + CompStr->dwCompAttrOffset;
        PWORD lpAtrIdx = (PWORD) CompStr->CompAttrColor;

        WriteUndetermineChars(lpStr, lpAtr, lpAtrIdx, CompStr->dwCompStrLen / sizeof(WCHAR));
    }

    return STATUS_SUCCESS;
}

NTSTATUS ConsoleImeResizeCompStrScreenBuffer(_In_ COORD const coordNewScreenSize)
{
    CONSOLE_IME_INFORMATION* const pIme = &g_ciConsoleInformation.ConsoleIme;

    // Composition string
    for (auto it = pIme->ConvAreaCompStr.begin(); it != pIme->ConvAreaCompStr.end(); ++it)
    {
        PCONVERSIONAREA_INFORMATION const ConvAreaInfo = *it;

        if (ConvAreaInfo)
        {
            if (!(ConvAreaInfo->ConversionAreaMode & CA_HIDDEN))
            {
                ConvAreaInfo->ConversionAreaMode |= CA_HIDDEN;
                ConsoleImePaint(ConvAreaInfo);
            }

            NTSTATUS Status = ConsoleImeResizeScreenBuffer(ConvAreaInfo->ScreenBuffer, coordNewScreenSize, ConvAreaInfo);
            if (!NT_SUCCESS(Status))
            {
                return Status;
            }
        }

    }

    return STATUS_SUCCESS;
}

SHORT CalcWideCharToColumn(_In_reads_(NumberOfChars) PCHAR_INFO Buffer, _In_ size_t NumberOfChars)
{
    SHORT Column = 0;

    while (NumberOfChars--)
    {
        if (IsCharFullWidth(Buffer->Char.UnicodeChar))
        {
            Column += 2;
        }
        else
        {
            Column++;
        }

        Buffer++;
    }

    return Column;
}


void ConsoleImePaint(_In_ PCONVERSIONAREA_INFORMATION pConvAreaInfo)
{
    if (pConvAreaInfo == nullptr)
    {
        return;
    }

    PSCREEN_INFORMATION const ScreenInfo = g_ciConsoleInformation.CurrentScreenBuffer;
    if (ScreenInfo == nullptr)
    {
        return;
    }

    SMALL_RECT WriteRegion;
    WriteRegion.Left = ScreenInfo->BufferViewport.Left + pConvAreaInfo->CaInfo.coordConView.X + pConvAreaInfo->CaInfo.rcViewCaWindow.Left;
    WriteRegion.Right = WriteRegion.Left + (pConvAreaInfo->CaInfo.rcViewCaWindow.Right - pConvAreaInfo->CaInfo.rcViewCaWindow.Left);
    WriteRegion.Top = ScreenInfo->BufferViewport.Top + pConvAreaInfo->CaInfo.coordConView.Y + pConvAreaInfo->CaInfo.rcViewCaWindow.Top;
    WriteRegion.Bottom = WriteRegion.Top + (pConvAreaInfo->CaInfo.rcViewCaWindow.Bottom - pConvAreaInfo->CaInfo.rcViewCaWindow.Top);

    if (!(pConvAreaInfo->ConversionAreaMode & (CA_HIDDEN)))
    {
        WriteConvRegionToScreen(ScreenInfo, pConvAreaInfo, &WriteRegion);
    }
    else
    {
        WriteToScreen(ScreenInfo, &WriteRegion);
    }
}

void ConsoleImeViewInfo(_In_ PCONVERSIONAREA_INFORMATION ConvAreaInfo, _In_ COORD coordConView)
{

    if (ConvAreaInfo->ConversionAreaMode & CA_HIDDEN)
    {
        SMALL_RECT NewRegion;
        ConvAreaInfo->CaInfo.coordConView = coordConView;
        NewRegion = ConvAreaInfo->CaInfo.rcViewCaWindow;
        NewRegion.Left += ConvAreaInfo->CaInfo.coordConView.X;
        NewRegion.Right += ConvAreaInfo->CaInfo.coordConView.X;
        NewRegion.Top += ConvAreaInfo->CaInfo.coordConView.Y;
        NewRegion.Bottom += ConvAreaInfo->CaInfo.coordConView.Y;
    }
    else
    {
        SMALL_RECT OldRegion, NewRegion;
        OldRegion = ConvAreaInfo->CaInfo.rcViewCaWindow;
        OldRegion.Left += ConvAreaInfo->CaInfo.coordConView.X;
        OldRegion.Right += ConvAreaInfo->CaInfo.coordConView.X;
        OldRegion.Top += ConvAreaInfo->CaInfo.coordConView.Y;
        OldRegion.Bottom += ConvAreaInfo->CaInfo.coordConView.Y;
        ConvAreaInfo->CaInfo.coordConView = coordConView;

        WriteToScreen(g_ciConsoleInformation.CurrentScreenBuffer, &OldRegion);

        NewRegion = ConvAreaInfo->CaInfo.rcViewCaWindow;
        NewRegion.Left += ConvAreaInfo->CaInfo.coordConView.X;
        NewRegion.Right += ConvAreaInfo->CaInfo.coordConView.X;
        NewRegion.Top += ConvAreaInfo->CaInfo.coordConView.Y;
        NewRegion.Bottom += ConvAreaInfo->CaInfo.coordConView.Y;
        WriteToScreen(g_ciConsoleInformation.CurrentScreenBuffer, &NewRegion);
    }
}

void ConsoleImeWindowInfo(_In_ PCONVERSIONAREA_INFORMATION ConvAreaInfo, _In_ SMALL_RECT rcViewCaWindow)
{
    if (rcViewCaWindow.Left != ConvAreaInfo->CaInfo.rcViewCaWindow.Left ||
        rcViewCaWindow.Top != ConvAreaInfo->CaInfo.rcViewCaWindow.Top ||
        rcViewCaWindow.Right != ConvAreaInfo->CaInfo.rcViewCaWindow.Right || rcViewCaWindow.Bottom != ConvAreaInfo->CaInfo.rcViewCaWindow.Bottom)
    {
        if (!(ConvAreaInfo->ConversionAreaMode & CA_HIDDEN))
        {
            ConvAreaInfo->ConversionAreaMode |= CA_HIDDEN;
            ConsoleImePaint(ConvAreaInfo);

            ConvAreaInfo->CaInfo.rcViewCaWindow = rcViewCaWindow;
            ConvAreaInfo->ConversionAreaMode &= ~CA_HIDDEN;
            ConsoleImePaint(ConvAreaInfo);
        }
        else
        {
            ConvAreaInfo->CaInfo.rcViewCaWindow = rcViewCaWindow;
        }
    }
}

NTSTATUS ConsoleImeResizeScreenBuffer(_In_ PSCREEN_INFORMATION ScreenInfo, _In_ COORD NewScreenSize, _In_ PCONVERSIONAREA_INFORMATION ConvAreaInfo)
{
    NTSTATUS Status = ScreenInfo->ResizeScreenBuffer(NewScreenSize, FALSE);
    if (NT_SUCCESS(Status))
    {
        ConvAreaInfo->CaInfo.coordCaBuffer = NewScreenSize;
        if (ConvAreaInfo->CaInfo.rcViewCaWindow.Left > NewScreenSize.X - 1)
        {
            ConvAreaInfo->CaInfo.rcViewCaWindow.Left = NewScreenSize.X - 1;
        }

        if (ConvAreaInfo->CaInfo.rcViewCaWindow.Right > NewScreenSize.X - 1)
        {
            ConvAreaInfo->CaInfo.rcViewCaWindow.Right = NewScreenSize.X - 1;
        }

        if (ConvAreaInfo->CaInfo.rcViewCaWindow.Top > NewScreenSize.Y - 1)
        {
            ConvAreaInfo->CaInfo.rcViewCaWindow.Top = NewScreenSize.Y - 1;
        }

        if (ConvAreaInfo->CaInfo.rcViewCaWindow.Bottom > NewScreenSize.Y - 1)
        {
            ConvAreaInfo->CaInfo.rcViewCaWindow.Bottom = NewScreenSize.Y - 1;
        }
    }

    return Status;
}

NTSTATUS ConsoleImeWriteOutput(_In_ PCONVERSIONAREA_INFORMATION ConvAreaInfo, _In_ PCHAR_INFO Buffer, _In_ SMALL_RECT CharRegion, _In_ BOOL fUnicode)
{
    NTSTATUS Status;
 
    COORD BufferSize;
    BufferSize.X = (SHORT)(CharRegion.Right - CharRegion.Left + 1);
    BufferSize.Y = (SHORT)(CharRegion.Bottom - CharRegion.Top + 1);

    SMALL_RECT ConvRegion = CharRegion;

    PSCREEN_INFORMATION const ScreenInfo = ConvAreaInfo->ScreenBuffer;

    if (!fUnicode)
    {
        TranslateOutputToUnicode(Buffer, BufferSize);
        Status = WriteScreenBuffer(ScreenInfo, Buffer, &ConvRegion);
    }
    else
    {
        ULONG NumBytes;

        if (FAILED(ULongMult(BufferSize.Y, BufferSize.X, &NumBytes)) ||
            FAILED(ULongMult(NumBytes, 2, &NumBytes)) || 
            FAILED(ULongMult(NumBytes, sizeof(CHAR_INFO), &NumBytes)))
        {
            return STATUS_INVALID_PARAMETER;
        }

        PCHAR_INFO TransBuffer = (PCHAR_INFO) new BYTE[NumBytes];
        if (TransBuffer == nullptr)
        {
            return STATUS_NO_MEMORY;
        }

        TranslateOutputToPaddingUnicode(Buffer, BufferSize, &TransBuffer[0]);

        Status = WriteScreenBuffer(ScreenInfo, &TransBuffer[0], &ConvRegion);
        delete[] TransBuffer;
    }

    if (NT_SUCCESS(Status))
    {
        PSCREEN_INFORMATION ScreenInfo = g_ciConsoleInformation.CurrentScreenBuffer;

        // cause screen to be updated
        ConvRegion.Left += (ScreenInfo->BufferViewport.Left + ConvAreaInfo->CaInfo.coordConView.X);
        ConvRegion.Right += (ScreenInfo->BufferViewport.Left + ConvAreaInfo->CaInfo.coordConView.X);
        ConvRegion.Top += (ScreenInfo->BufferViewport.Top + ConvAreaInfo->CaInfo.coordConView.Y);
        ConvRegion.Bottom += (ScreenInfo->BufferViewport.Top + ConvAreaInfo->CaInfo.coordConView.Y);

        WriteConvRegionToScreen(ScreenInfo, ConvAreaInfo, &ConvRegion);
    }

    return Status;
}

// Routine Description:
// - This routine handle WM_COPYDATA message.
// Arguments:
// - Console - Pointer to console information structure.
// - wParam -
// - lParam -
// Return Value:
NTSTATUS ImeControl(_In_ PCOPYDATASTRUCT pCopyDataStruct)
{
    if (pCopyDataStruct == nullptr)
    {
        // fail safe.
        return STATUS_SUCCESS;
    }

    switch ((LONG) pCopyDataStruct->dwData)
    {
        case CI_CONIMECOMPOSITION:
            if (pCopyDataStruct->cbData >= sizeof(CONIME_UICOMPMESSAGE))
            {
                LPCONIME_UICOMPMESSAGE CompStr;

                CompStr = (LPCONIME_UICOMPMESSAGE) pCopyDataStruct->lpData;
                if (CompStr && CompStr->dwSize == pCopyDataStruct->cbData)
                {
                    if (g_ciConsoleInformation.ConsoleIme.CompStrData)
                    {
                        delete[] g_ciConsoleInformation.ConsoleIme.CompStrData;
                    }

                    g_ciConsoleInformation.ConsoleIme.CompStrData = (LPCONIME_UICOMPMESSAGE) new BYTE[CompStr->dwSize];
                    if (g_ciConsoleInformation.ConsoleIme.CompStrData == nullptr)
                    {
                        break;
                    }

                    memmove(g_ciConsoleInformation.ConsoleIme.CompStrData, CompStr, CompStr->dwSize);
                    ConsoleImeCompStr(g_ciConsoleInformation.ConsoleIme.CompStrData);
                }
            }
            break;
        case CI_ONSTARTCOMPOSITION:
            g_ciConsoleInformation.pInputBuffer->ImeMode.InComposition = TRUE;
            break;
        case CI_ONENDCOMPOSITION:
            g_ciConsoleInformation.pInputBuffer->ImeMode.InComposition = FALSE;
            break;
    }

    return STATUS_SUCCESS;
}

bool InsertConvertedString(_In_ LPCWSTR lpStr)
{
    bool fResult = false;

    if (g_ciConsoleInformation.CurrentScreenBuffer->TextInfo->GetCursor()->IsOn())
    {
        g_ciConsoleInformation.CurrentScreenBuffer->TextInfo->GetCursor()->TimerRoutine(g_ciConsoleInformation.CurrentScreenBuffer);
    }

    size_t cchLen = wcslen(lpStr) + 1;
    PINPUT_RECORD const InputEvent = new INPUT_RECORD[cchLen];
    if (InputEvent == nullptr)
    {
        return false;
    }

    PINPUT_RECORD TmpInputEvent = InputEvent;
    DWORD dwControlKeyState = GetControlKeyState(0);

    while (*lpStr)
    {
        TmpInputEvent->EventType = KEY_EVENT;
        TmpInputEvent->Event.KeyEvent.bKeyDown = TRUE;
        TmpInputEvent->Event.KeyEvent.wVirtualKeyCode = 0;
        TmpInputEvent->Event.KeyEvent.wVirtualScanCode = 0;
        TmpInputEvent->Event.KeyEvent.dwControlKeyState = dwControlKeyState;
        TmpInputEvent->Event.KeyEvent.uChar.UnicodeChar = *lpStr++;
        TmpInputEvent->Event.KeyEvent.wRepeatCount = 1;
        TmpInputEvent++;
    }

    WriteInputBuffer(g_ciConsoleInformation.pInputBuffer, InputEvent, (DWORD) (cchLen - 1));

    fResult = true;

    delete[] InputEvent;
    return fResult;
}

void SetUndetermineAttribute()
{
    PSCREEN_INFORMATION const ScreenInfo = g_ciConsoleInformation.CurrentScreenBuffer;
    PCONVERSIONAREA_INFORMATION ConvAreaInfo = g_ciConsoleInformation.ConsoleIme.ConvAreaRoot;
    if (ConvAreaInfo != nullptr)
    {
        do
        {
            ConvAreaInfo->ScreenBuffer->SetAttributes(ScreenInfo->GetAttributes());
            ConvAreaInfo = ConvAreaInfo->ConvAreaNext;
        }
        while (ConvAreaInfo != nullptr);
    }
}

void StreamWriteToScreenBufferIME(_In_reads_(StringLength) PWCHAR String,
                                  _In_ USHORT StringLength,
                                  _In_ PSCREEN_INFORMATION ScreenInfo,
                                  _In_reads_(StringLength) PCHAR StringA)
{
    DBGOUTPUT(("StreamWriteToScreenBuffer\n"));

    COORD TargetPoint = ScreenInfo->TextInfo->GetCursor()->GetPosition();

    ROW* const Row = ScreenInfo->TextInfo->GetRowByOffset(TargetPoint.Y);
    ASSERT(Row != nullptr);
    DBGOUTPUT(("Row = 0x%p, TargetPoint = (0x%x,0x%x)\n", Row, TargetPoint.X, TargetPoint.Y));

    // copy chars
    BisectWrite(StringLength, TargetPoint, ScreenInfo);

    USHORT ScreenEndOfString;
    if (SUCCEEDED(UShortSub(ScreenInfo->ScreenBufferSize.X, TargetPoint.X, &ScreenEndOfString)) && ScreenEndOfString && StringLength > ScreenEndOfString)
    {

        if (TargetPoint.Y == ScreenInfo->ScreenBufferSize.Y - 1 &&
            TargetPoint.X + StringLength >= ScreenInfo->ScreenBufferSize.X && *(StringA + ScreenEndOfString - 1) & CHAR_ROW::ATTR_LEADING_BYTE)
        {
            *(String + ScreenEndOfString - 1) = UNICODE_SPACE;
            *(StringA + ScreenEndOfString - 1) = 0;
            if (StringLength > ScreenEndOfString - 1)
            {
                *(String + ScreenEndOfString) = UNICODE_SPACE;
                *(StringA + ScreenEndOfString) = 0;
            }
        }
    }
    memmove(&Row->CharRow.Chars[TargetPoint.X], String, StringLength * sizeof(WCHAR));
    memmove(&Row->CharRow.KAttrs[TargetPoint.X], StringA, StringLength * sizeof(CHAR));

    // recalculate first and last non-space char
    if (TargetPoint.X < Row->CharRow.Left)
    {
        // CharRow.Left is leftmost bound of chars in Chars array (array will be full width) i.e. type is COORD
        PWCHAR LastChar = &Row->CharRow.Chars[ScreenInfo->ScreenBufferSize.X - 1];
        PWCHAR Char;

        for (Char = &Row->CharRow.Chars[TargetPoint.X]; Char < LastChar && *Char == (WCHAR)' '; Char++)
        {
            /* do nothing */ ;
        }
        Row->CharRow.Left = (SHORT)(Char - Row->CharRow.Chars);
    }

    if ((TargetPoint.X + StringLength) >= Row->CharRow.Right)
    {
        PWCHAR FirstChar = Row->CharRow.Chars;
        PWCHAR Char;

        for (Char = &Row->CharRow.Chars[TargetPoint.X + StringLength - 1]; *Char == (WCHAR)' ' && Char >= FirstChar; Char--)
        {
            /* do nothing */ ;
        }

        Row->CharRow.Right = (SHORT)(Char + 1 - FirstChar);
    }

    // see if attr string is different.  if so, allocate a new attr buffer and merge the two strings.
    if (Row->AttrRow.Length != 1 || !(Row->AttrRow.GetHead()->GetAttributes()->IsEqual(ScreenInfo->GetAttributes())))
    {        
        TextAttributeRun InsertedRun;

        const WORD wScreenAttributes = ScreenInfo->GetAttributes()->GetLegacyAttributes();
        const bool fRVerticalSet = IsFlagSet(wScreenAttributes, COMMON_LVB_GRID_SINGLEFLAG | COMMON_LVB_GRID_RVERTICAL);
        const bool fLVerticalSet = IsFlagSet(wScreenAttributes, COMMON_LVB_GRID_SINGLEFLAG | COMMON_LVB_GRID_LVERTICAL);
        
        if (fLVerticalSet || fRVerticalSet)
        {
            const byte LeadOrTrailByte = fRVerticalSet? CHAR_ROW::ATTR_LEADING_BYTE : CHAR_ROW::ATTR_TRAILING_BYTE;
            const int iFlag = fRVerticalSet? COMMON_LVB_GRID_RVERTICAL : COMMON_LVB_GRID_LVERTICAL;
            for (short i = 0; i < StringLength; i++)
            {
                InsertedRun.SetLength(1);
                if (*(StringA + i) & LeadOrTrailByte)
                {
                    InsertedRun.SetAttributesFromLegacy(wScreenAttributes & ~(COMMON_LVB_GRID_SINGLEFLAG | iFlag));
                }
                else
                {
                    InsertedRun.SetAttributesFromLegacy(wScreenAttributes & ~COMMON_LVB_GRID_SINGLEFLAG);
                }
            }

        }
        else
        {
            InsertedRun.SetLength(StringLength);
            InsertedRun.SetAttributesFromLegacy(wScreenAttributes);
        }

        Row->AttrRow.InsertAttrRuns(&InsertedRun, 1, TargetPoint.X, (SHORT)(TargetPoint.X + StringLength - 1), ScreenInfo->ScreenBufferSize.X);
        
    }

    ScreenInfo->ResetTextFlags(TargetPoint.X, TargetPoint.Y, TargetPoint.X + StringLength - 1, TargetPoint.Y);
}


// genprint.cpp - generalised printing

#include "stdafx.h"
#include <afxext.h>

#include "genprint.h"
#include "MUSHclient.h"
#include "doc.h"
#include "mainfrm.h"
  
#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char BASED_CODE THIS_FILE[] = __FILE__;
#endif
  
BOOL bAborted = FALSE;
static CMyPrintingDialog * pActivePrintingDialog = NULL;

class CPrintStartGuard
  {
  public:
    CPrintStartGuard (t_print_control_block & pcb)
      : m_pcb (pcb), m_bDocumentStarted (false),
        m_bFrameDisabled (false), m_bCommitted (false) { }

    ~CPrintStartGuard ()
      {
      if (m_bCommitted)
        return;

      if (m_bDocumentStarted && m_pcb.hDC)
        AbortDoc (m_pcb.hDC);

      if (m_pcb.hDC)
        {
        DeleteDC (m_pcb.hDC);
        m_pcb.hDC = NULL;
        }

      for (int i = 0; i < 8; i++)
        if (m_pcb.font [i])
          {
          DeleteObject (m_pcb.font [i]);
          m_pcb.font [i] = NULL;
          }

      pActivePrintingDialog = NULL;
      if (m_pcb.dlgPrintStatus)
        {
        if (m_pcb.dlgPrintStatus->GetSafeHwnd ())
          m_pcb.dlgPrintStatus->DestroyWindow ();
        delete m_pcb.dlgPrintStatus;
        m_pcb.dlgPrintStatus = NULL;
        }

      delete m_pcb.pd;
      m_pcb.pd = NULL;

      if (m_bFrameDisabled)
        {
        Frame.EnableWindow (TRUE);
        Frame.SetFocus ();
        }

      m_pcb.initialised = FALSE;
      m_pcb.ok = FALSE;
      }

    void DocumentStarted () { m_bDocumentStarted = true; }
    void FrameDisabled () { m_bFrameDisabled = true; }
    void Commit () { m_bCommitted = true; }

  private:
    t_print_control_block & m_pcb;
    bool m_bDocumentStarted;
    bool m_bFrameDisabled;
    bool m_bCommitted;
  };

/* +++++++++++++++++++++++++++++++++++++++++++++++++++ */
/*                                                     */
/*       print_start_document                          */
/*                                                     */
/* +++++++++++++++++++++++++++++++++++++++++++++++++++ */


// initiates a print job
   
BOOL print_start_document (t_print_control_block & pcb,
                            const CString docname,
                            const WORD first_page,
                            const WORD last_page,
                            const int left_margin,
                            const int top_margin,
                            const int lines_per_page,
                            const int point_size,
                            const int printer_spacing,
                            const CString & printer_font,
                            const BOOL bSelection)
                            
  {
int err;
DWORD flags;
int i;
CFont fontSpacing;

  ZeroMemory (&pcb, sizeof pcb);
  CPrintStartGuard startGuard (pcb);

  pcb.hwnd = Frame.GetSafeHwnd ();

  pcb.left_margin      = left_margin;
  pcb.top_margin       = top_margin; 
  pcb.lines_per_page   = lines_per_page;
  pcb.point_size       = point_size; 
  pcb.printer_spacing  = printer_spacing;
  strncpy (pcb.printer_font, printer_font, sizeof pcb.printer_font - 1);
  pcb.printer_font [sizeof pcb.printer_font - 1] = 0;
  
  //
  // Initialize a PRINTDLG struct and call PrintDlg to allow user to
  //   specify various printing options...
  //

  flags = PD_RETURNDC;
  if (last_page == 0)
    flags |= PD_NOPAGENUMS;
  if (!bSelection)
     flags |= PD_NOSELECTION;
  else
     flags |= PD_SELECTION;
											   
// create new print dialog

  pcb.pd = new CPrintDialog (FALSE, flags, &Frame);

  pcb.pd->m_pd.nMinPage = pcb.pd->m_pd.nFromPage = first_page;
  pcb.pd->m_pd.nMaxPage = pcb.pd->m_pd.nToPage 	 = last_page;

  if (AfxGetApp ()->DoPrintDialog (pcb.pd) != IDOK)
    return TRUE;

  if (last_page == 0)
    {
    pcb.pd->m_pd.nFromPage  = 1;
    pcb.pd->m_pd.nToPage = 0xFFFF;
    } // end of no page range specified

// set up the DOCINFO field
  
  pcb.di.lpszDocName = docname;
  pcb.di.lpszOutput = NULL;
  pcb.di.cbSize = sizeof (pcb.di);


// get a copy of the DC so we don't have to call GetPrinterDC all the time

  pcb.hDC = pcb.pd->GetPrinterDC ();

// set up an abort procedure so they can cancel the printing

  if (SetAbortProc (pcb.hDC, PrintingAbortProc) == SP_ERROR)
    return TRUE;

// start the document

  err = StartDoc  (pcb.hDC, &pcb.di);

  if (err <= 0)
    return TRUE;
  startGuard.DocumentStarted ();

// disable main window while printing & init printing status dialog

  Frame.EnableWindow (FALSE);
  startGuard.FrameDisabled ();

// set up a progress dialog

  pcb.dlgPrintStatus = new CMyPrintingDialog (&Frame);
  if (!pcb.dlgPrintStatus->GetSafeHwnd ())
    AfxThrowResourceException ();
  pActivePrintingDialog = pcb.dlgPrintStatus;

  pcb.dlgPrintStatus->SetDlgItemText(AFX_IDC_PRINT_DOCNAME, docname);

  pcb.dlgPrintStatus->SetDlgItemText(AFX_IDC_PRINT_PRINTERNAME, pcb.pd->GetDeviceName());

  CString strTemp;
  CString strPortName = pcb.pd->GetPortName();
  int nFormatID = AFX_IDS_PRINTONPORT;

  AfxFormatString1(strTemp, nFormatID, strPortName);
  pcb.dlgPrintStatus->SetDlgItemText(AFX_IDC_PRINT_PORTNAME, strTemp);

  pcb.dlgPrintStatus->ShowWindow(SW_SHOW);
  pcb.dlgPrintStatus->UpdateWindow();

  pcb.initialised = TRUE;
  pcb.current_page = 0;
  pcb.pages_printed = 0;
  pcb.lines_printed = 0;

  pcb.logpelsX = GetDeviceCaps (pcb.hDC, LOGPIXELSX);
  pcb.logpelsY = GetDeviceCaps (pcb.hDC, LOGPIXELSY);
  pcb.offsetX = GetDeviceCaps (pcb.hDC, PHYSICALOFFSETX);
  pcb.offsetY = GetDeviceCaps (pcb.hDC, PHYSICALOFFSETY);

// calculate initial top and left positions

double left = pcb.left_margin / 25.4 * pcb.logpelsX;       // 25.4 mm to an inch
double top = pcb.top_margin / 25.4 * pcb.logpelsY;

// adjust for non-printable area, and save as start of this page

  pcb.init_left = (long) left - pcb.offsetX; 
  pcb.init_top = (long) top - pcb.offsetY;     

// make sure not negative (i.e. off edge of printable area)

  if (pcb.init_left < 0)
    pcb.init_left = 0;

  if (pcb.init_top < 0)
    pcb.init_top = 0;

// calculate font height in device coordinates

double height = pcb.point_size / 72.0 * double (pcb.logpelsY);

LOGFONT lf;
  ZeroMemory (&lf, sizeof lf);
                                                                        
  lf.lfHeight         =  (long) height;	// logical height of font    
  lf.lfWidth          =  0;	// logical average character width         
  lf.lfEscapement     =  0;	// angle of escapement                     
  lf.lfOrientation    =  0;	// base-line orientation angle             
  lf.lfWeight         =  FW_NORMAL	;	// font weight                   
  lf.lfItalic         =  FALSE;	// italic attribute flag               
  lf.lfUnderline      =  FALSE;	// underline attribute flag            
  lf.lfStrikeOut      =  FALSE;	// strikeout attribute flag            
  lf.lfCharSet        =  MUSHCLIENT_FONT_CHARSET;	// character set identifier      
  lf.lfOutPrecision   =  OUT_DEVICE_PRECIS;	// output precision        
  lf.lfClipPrecision  =  CLIP_DEFAULT_PRECIS;	// clipping precision    
  lf.lfQuality        =  DEFAULT_QUALITY;	// output quality            
  lf.lfPitchAndFamily =  MUSHCLIENT_FONT_FAMILY;	// pitch and family              
  strncpy (lf.lfFaceName, pcb.printer_font, sizeof lf.lfFaceName - 1);
  lf.lfFaceName [sizeof lf.lfFaceName - 1] = 0;


// create 8 fonts (all possible combinations of bold, italic and underline)

  for (i = 0; i < 8; i++)
    {
    lf.lfItalic = (i & FONT_ITALIC) != 0;
    lf.lfUnderline = (i & FONT_UNDERLINE) != 0;
    if (i & FONT_BOLD)
      lf.lfWeight         =  FW_BOLD;	                 
    else      
      lf.lfWeight         =  FW_NORMAL;	                  
    
    pcb.font [i] =  CreateFontIndirect (&lf);
    if (!pcb.font [i])
      {
      ::TMessageBox ("Unable to create a font for printing");
      return TRUE;
      }
    }

	// Calc line spacing height
	lf.lfHeight = -MulDiv(pcb.printer_spacing, pcb.logpelsY, 72);
	lf.lfWeight = FW_NORMAL;
	lf.lfItalic = FALSE;
	lf.lfUnderline = FALSE;
  lf.lfPitchAndFamily =  MUSHCLIENT_FONT_FAMILY;	// pitch and family              
	if (!fontSpacing.CreateFontIndirect(&lf))
    return TRUE;
	HGDIOBJ hOldSpacingFont = ::SelectObject(pcb.hDC, fontSpacing.GetSafeHandle());
	if (!hOldSpacingFont)
    return TRUE;
	TEXTMETRIC	tmSpacing;
	if (!::GetTextMetrics(pcb.hDC, &tmSpacing))
    {
    ::SelectObject (pcb.hDC, hOldSpacingFont);
    return TRUE;
    }
	pcb.m_nLineSpacing = tmSpacing.tmHeight + tmSpacing.tmExternalLeading;
  if (!::SelectObject (pcb.hDC, hOldSpacingFont))
    return TRUE;

	// Select normal font     
	if (!SelectObject (pcb.hDC, pcb.font [FONT_NORMAL]))
      return TRUE;
  	pcb.current_font = &pcb.font [FONT_NORMAL];

  if (!GetTextMetrics (pcb.hDC, &pcb.tm))
    return TRUE;
  pcb.ok = TRUE;

  bAborted = FALSE;

  startGuard.Commit ();

  return FALSE; // OK exit
  }   // end of print_start_document


/* +++++++++++++++++++++++++++++++++++++++++++++++++++ */
/*                                                     */
/*       print_start_page                              */
/*                                                     */
/* +++++++++++++++++++++++++++++++++++++++++++++++++++ */


// initiates a print page
   
BOOL print_start_page (t_print_control_block & pcb)
  {
int err;

  if (!pcb.ok)
    return TRUE;
  
  if (!PrintingAbortProc(pcb.hDC, 0))
    {
    pcb.ok = FALSE;
    pcb.cancelled = TRUE;
    return TRUE;
    }

// count pages, if out of page range, don't print

  pcb.current_page++;
                                
  if (pcb.pd->PrintRange () && 
      (pcb.current_page < pcb.pd->GetFromPage () || pcb.current_page > pcb.pd->GetToPage ()))
    return FALSE;

// update page number in dialogue

  TCHAR szBuf[80];
  CString strTemp;

  VERIFY(strTemp.LoadString(AFX_IDS_PRINTPAGENUM));

  wsprintf(szBuf, strTemp, pcb.current_page);
  pcb.dlgPrintStatus->SetDlgItemText(AFX_IDC_PRINT_PAGENUM, szBuf);

  err = StartPage (pcb.hDC);

  if (err <= 0)
    {
    ::TMessageBox ("Error occurred starting a new page");
    pcb.ok = FALSE;
    return TRUE;
    }

// reset the font to the current one (some printer drivers seem to change it)

  SelectObject (pcb.hDC, *(pcb.current_font));

// initialise position on page

  pcb.left = pcb.init_left;
  pcb.top = pcb.init_top;

  pcb.pages_printed++;

  return FALSE;
  } // end of print_start_page

/* +++++++++++++++++++++++++++++++++++++++++++++++++++ */
/*                                                     */
/*       print_printline                               */
/*                                                     */
/* +++++++++++++++++++++++++++++++++++++++++++++++++++ */

static char printline_buff [MAX_LINE_WIDTH + 1];

BOOL print_printline (t_print_control_block & pcb, int skip, const char * theline, ...)
  {
va_list arglist;

  if (!pcb.ok)
    return TRUE;

// if out of page range, don't print

  if (pcb.pd->PrintRange () && pcb.current_page < pcb.pd->GetFromPage ())
    return FALSE;

// if past last page, return TRUE so we stop reading the file

  if (pcb.pd->PrintRange () && pcb.current_page > pcb.pd->GetToPage ())
    return TRUE;

/* print the message as if it was a PRINTF type message */

  va_start (arglist, theline);
  _vsnprintf (printline_buff, MAX_LINE_WIDTH, theline, arglist);
  va_end (arglist);
  printline_buff [MAX_LINE_WIDTH] = 0;

  TextOut (pcb.hDC, pcb.left, pcb.top, printline_buff, strlen (printline_buff));

// if this is a new line, count lines, and move down to the next one

  if (skip)
    {  
    pcb.left = pcb.init_left;   // back to left margin
  	pcb.top += pcb.m_nLineSpacing * skip;
    pcb.lines_printed++;
    }   // end of starting a new line
  else
    {

// same line, find width of this piece of text and add to the left pixel position

    SIZE size;

    GetTextExtentPoint32 (pcb.hDC, printline_buff, strlen (printline_buff), &size); 
    pcb.left += size.cx;
    }   // end of not starting a new line

  return FALSE;
  } // end of print_printline

/* +++++++++++++++++++++++++++++++++++++++++++++++++++ */
/*                                                     */
/*       print_end_page                                */
/*                                                     */
/* +++++++++++++++++++++++++++++++++++++++++++++++++++ */


// ends a print page
   
BOOL print_end_page (t_print_control_block & pcb)
  {
int err;

  if (!pcb.ok)
    return TRUE;

// if out of page range, don't print

  if (pcb.pd->PrintRange () && 
      (pcb.current_page < pcb.pd->GetFromPage () || pcb.current_page > pcb.pd->GetToPage ()))
    return FALSE;

  err = EndPage (pcb.hDC);

  if (err <= 0)
    {
    ::TMessageBox ("Error occurred starting a new page");
    pcb.ok = FALSE;
    return TRUE;
    }

  return FALSE;
  } // end of print_end_page

/* +++++++++++++++++++++++++++++++++++++++++++++++++++ */
/*                                                     */
/*       print_end_document                            */
/*                                                     */
/* +++++++++++++++++++++++++++++++++++++++++++++++++++ */


static void print_cleanup_document (t_print_control_block & pcb)
  {
int i;

// delete our device context before its selected fonts
  if (pcb.hDC)
    {
    DeleteDC (pcb.hDC);
    pcb.hDC = NULL;
    }

  for (i = 0; i < 8; i++)
    if (pcb.font [i])
      {
      DeleteObject (pcb.font [i]);
      pcb.font [i] = NULL;
      }

  pActivePrintingDialog = NULL;
  if (pcb.dlgPrintStatus)
    {
    if (pcb.dlgPrintStatus->GetSafeHwnd ())
      pcb.dlgPrintStatus->DestroyWindow ();
    delete pcb.dlgPrintStatus;
    pcb.dlgPrintStatus = NULL;
    }
  
  delete pcb.pd;
  pcb.pd = NULL;
  
  Frame.EnableWindow (TRUE);   
  Frame.SetFocus ();           // so keyboard input works

  pcb.initialised = FALSE;
  pcb.ok = FALSE;
  }

BOOL print_end_document (t_print_control_block & pcb)
  {
  if (pcb.initialised)
    {
    int err = EndDoc (pcb.hDC);

    if (err <= 0)
      ::TMessageBox ("Error occurred closing printer");

    } // end of having started the document

  print_cleanup_document (pcb);
  return FALSE; // OK exit
  } // end of print_end_document

void print_abort_document (t_print_control_block & pcb)
  {
  if (pcb.initialised && pcb.hDC)
    AbortDoc (pcb.hDC);
  print_cleanup_document (pcb);
  }


/* +++++++++++++++++++++++++++++++++++++++++++++++++++ */
/*                                                     */
/*       print_font                                    */
/*                                                     */
/* +++++++++++++++++++++++++++++++++++++++++++++++++++ */

// changes the printout to the specified font
/*
Possible combinations of font_type are:

    FONT_NORMAL  
    FONT_ITALIC    
    FONT_UNDERLINE
    FONT_BOLD      

*/

void print_font (t_print_control_block & pcb, const short font_type)
  {

  if (pcb.ok && font_type >= 0 && font_type <= 7)
    {
    SelectObject (pcb.hDC, pcb.font [font_type]);
    pcb.current_font = &pcb.font [font_type];
    }

  } // end of print_font


BOOL CALLBACK PrintingAbortProc(HDC, int)
{
	MSG msg;

	if (::PeekMessage(&msg, NULL, WM_QUIT, WM_QUIT, PM_REMOVE))
	{
		::PostQuitMessage((int) msg.wParam);
		return FALSE;
	}

	HWND hPrintDialog = pActivePrintingDialog == NULL ? NULL :
		pActivePrintingDialog->GetSafeHwnd ();

	while (!bAborted && hPrintDialog != NULL &&
		::PeekMessage(&msg, hPrintDialog, 0, 0, PM_REMOVE))
	{
		if (msg.message == WM_QUIT)
		{
			::PostQuitMessage((int) msg.wParam);
			return FALSE;
		}

		if (!pActivePrintingDialog->IsDialogMessage(&msg))
		{
			::TranslateMessage(&msg);
			::DispatchMessage(&msg);
		}
	}
	return !bAborted;
}

BOOL CMyPrintingDialog::OnInitDialog()
{
	SetWindowText(AfxGetAppName());
	CenterWindow();
	return CDialog::OnInitDialog();
}

void CMyPrintingDialog::OnCancel()
{
	bAborted = TRUE;  // flag that user aborted print
	CDialog::OnCancel();
}

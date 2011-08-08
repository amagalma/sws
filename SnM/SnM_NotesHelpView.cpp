/******************************************************************************
/ SnM_NotesHelpView.cpp
/
/ Copyright (c) 2010-2011 Tim Payne (SWS), Jeffos
/ http://www.standingwaterstudios.com/reaper
/
/ Permission is hereby granted, free of charge, to any person obtaining a copy
/ of this software and associated documentation files (the "Software"), to deal
/ in the Software without restriction, including without limitation the rights to
/ use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
/ of the Software, and to permit persons to whom the Software is furnished to
/ do so, subject to the following conditions:
/ 
/ The above copyright notice and this permission notice shall be included in all
/ copies or substantial portions of the Software.
/ 
/ THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
/ EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
/ OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
/ NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
/ HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
/ WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
/ FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
/ OTHER DEALINGS IN THE SOFTWARE.
/
******************************************************************************/

//JFB TODO?
// - atm, undo on *each* key stroke (!)
//   => it fills the undo history (+ can't restore caret pos.) BUT it fixes:
//   * SaveExtensionConfig() that is not called if no proj mods but notes may have been added..
//   * project switches
// - take changed => title not updated
// - action_help_t (even if not used yet ?)
// - drag'n'drop text

#include "stdafx.h"
#include "SnM_Actions.h"
#include "SNM_NotesHelpView.h"
#include "../../WDL/projectcontext.h"

#define MAX_HELP_LENGTH				4096 //JFB 4096 rather than MAX_INI_SECTION (too big)

// Msgs
#define SET_ACTION_HELP_FILE_MSG	0x110001

enum {
  BUTTONID_LOCK=1000,
  COMBOID_TYPE,
  TXTID_LABEL,
  BUTTONID_ALR
};

enum {
  NOTES_HELP_DISABLED=0,
  ITEM_NOTES,
  TRACK_NOTES,
  MARKER_NAME,
  ACTION_HELP // must remain the last item: no OSX support yet 
};

enum {
  REQUEST_REFRESH=0,
  REQUEST_REFRESH_EMPTY,
  NO_REFRESH
};

// Globals
SNM_NotesHelpWnd* g_pNotesHelpWnd = NULL;
SWSProjConfig<WDL_PtrList_DeleteOnDestroy<SNM_TrackNotes> > g_pTracksNotes;
SWSProjConfig<WDL_String> g_prjNotes;

int g_bDocked = -1, g_bLastDocked = 0; 
char g_locked = 1;
char g_lastText[MAX_HELP_LENGTH];

// Action help tracking
//JFB TODO: cleanup when we'll be able to access all sections & custom ids
int g_lastActionListSel = -1;
DWORD g_lastActionListCmd = 0;
char g_lastActionSection[SNM_MAX_SECTION_NAME_LEN] = "";
char g_lastActionId[SNM_MAX_ACTION_CUSTID_LEN] = "";
char g_lastActionDesc[128] = ""; 

double g_lastMarkerPos = -1.0;
int g_lastMarkerIdx = -1;
MediaItem* g_mediaItemNote = NULL;
MediaTrack* g_trNote = NULL;


///////////////////////////////////////////////////////////////////////////////
// SNM_NotesHelpWnd
///////////////////////////////////////////////////////////////////////////////

SNM_NotesHelpWnd::SNM_NotesHelpWnd()
:SWS_DockWnd(IDD_SNM_NOTES_HELP, "Notes/Help", "SnMNotesHelp", 30007, SWSGetCommandID(OpenNotesHelpView))
{
	m_type = m_previousType = NOTES_HELP_DISABLED;

	// Get the action help file
	readActionHelpFilenameIniFile();
	m_internalTLChange = false;

	// Must call SWS_DockWnd::Init() to restore parameters and open the window if necessary
	Init();
}

int SNM_NotesHelpWnd::GetType(){
	return m_type;
}

void SNM_NotesHelpWnd::SetType(int _type)
{
	int prev = m_previousType = m_type;
	m_type = _type;
	m_cbType.SetCurSel(_type);

	// force an initial refresh (when IDC_EDIT has the focus, re-enabling the timer 
	// isn't enough: Update() is skipped, see OnTimer() & IsActive()
	Update(); 

	if (prev == NOTES_HELP_DISABLED && prev != _type)
		SetTimer(m_hwnd, 1, 125, NULL);
}

void SNM_NotesHelpWnd::SetText(const char* _str) 
{
	if (_str)
	{
		char buf[MAX_HELP_LENGTH] = "";
		GetStringWithRN(_str, buf, MAX_HELP_LENGTH);
		lstrcpyn(g_lastText, buf, MAX_HELP_LENGTH);
		SetDlgItemText(GetHWND(), IDC_EDIT, buf);
	}
}

void SNM_NotesHelpWnd::RefreshGUI(bool _emtpyNotes) 
{
	if (_emtpyNotes)
		SetText("");

	bool bHide = true;
	switch(GetType())
	{
		case ACTION_HELP:
			if (g_lastActionId && *g_lastActionId)
				bHide = false; // for copy/paste even if "Custom: ..."
			break;
		case ITEM_NOTES:
			if (g_mediaItemNote)
				bHide = false;
			break;
		case TRACK_NOTES:
			if (g_trNote)
				bHide = false;
			break;
		case MARKER_NAME:
			if (g_lastMarkerIdx != -1)
				bHide = false;
			break;
		case NOTES_HELP_DISABLED:
			bHide = false;
			break;
		default:
			break;
	}
	ShowWindow(GetDlgItem(GetHWND(), IDC_EDIT), bHide || g_locked ? SW_HIDE : SW_SHOW);
	m_parentVwnd.RequestRedraw(NULL); // WDL refresh
}

void SNM_NotesHelpWnd::CSurfSetTrackTitle() {
	if (m_type == TRACK_NOTES)
		RefreshGUI();
}

//JFB TODO? replace "timer-ish track sel change tracking" with this notif..
void SNM_NotesHelpWnd::CSurfSetTrackListChange() 
{
	// This is our only notification of active project tab change, so update everything
	// (we use a ScheduledJob because of possible multi-notifs)
	if (!m_internalTLChange)
	{
		SNM_NoteHlp_TLChangeSchedJob* job = new SNM_NoteHlp_TLChangeSchedJob();
		AddOrReplaceScheduledJob(job);
	}
	else
		m_internalTLChange = false;
}

void SNM_NotesHelpWnd::Update(bool _force)
{
	static bool updateReentrance;
	if (!updateReentrance)
	{
		updateReentrance = true; 

		// force refresh if needed (dock state changed, ..)
		if (_force || g_bLastDocked != g_bDocked || m_type != m_previousType)
		{
			g_bLastDocked = g_bDocked;
			m_previousType = m_type;
			g_lastActionListSel = -1;
			*g_lastActionId = '\0';
			*g_lastActionDesc = '\0';
			g_lastActionListCmd = 0;
			*g_lastActionSection = '\0';
			g_mediaItemNote = NULL;
			g_trNote = NULL;
			g_lastMarkerPos = -1.0;
			g_lastMarkerIdx = -1;
		}

		// Update
		int refreshType = NO_REFRESH;
		switch(m_type)
		{
			case ITEM_NOTES:
				refreshType = updateItemNotes();
				if (refreshType == REQUEST_REFRESH_EMPTY)
					g_mediaItemNote = NULL;

				// Concurent item note update ?
/*JFB commented: kludge..
#ifdef _WIN32
				else if (refreshType == NO_REFRESH)
				{
					char name[BUFFER_SIZE] = "";
					MediaItem_Take* tk = GetActiveTake(g_mediaItemNote);
					char* tkName = tk ? (char*)GetSetMediaItemTakeInfo(tk, "P_NAME", NULL) : NULL;
					sprintf(name, "Notes for \"%s\"", tkName ? tkName : "Empty Item");
					HWND w = SearchWindow(name);
					if (w)
						g_mediaItemNote = NULL; //will force refresh
				}
#endif
*/
				break;
			case TRACK_NOTES:
				refreshType = updateTrackNotes();
				if (refreshType == REQUEST_REFRESH_EMPTY)
					g_trNote = NULL;
				break;
			case ACTION_HELP:
				refreshType = updateActionHelp();
				if (refreshType == REQUEST_REFRESH_EMPTY) {
					g_lastActionListSel = -1;
					*g_lastActionId = '\0';
					*g_lastActionDesc = '\0';
					g_lastActionListCmd = 0;
					*g_lastActionSection = '\0';
				}
				break;
			case MARKER_NAME:
				refreshType = updateMarkerName();
				if (refreshType == REQUEST_REFRESH_EMPTY) {
					g_lastMarkerPos = -1.0;
					g_lastMarkerIdx = -1;
				}
				break;
			case NOTES_HELP_DISABLED:
				KillTimer(m_hwnd, 1);
				SetText(g_prjNotes.Get()->Get());
				refreshType = REQUEST_REFRESH;
				break;
		}
		
		if (refreshType != NO_REFRESH)
			RefreshGUI(refreshType == REQUEST_REFRESH_EMPTY);
	}
	updateReentrance = false;
}

void SNM_NotesHelpWnd::saveCurrentText(int _type) 
{
	if (g_pNotesHelpWnd)
	{
		switch(_type) 
		{
			case ITEM_NOTES: 
				m_internalTLChange = true;	// item note updates lead to SetTrackListChange() CSurf notif (reentrance)
											//JFB TODO .. check if it can be used for concurent item note updates ?
				saveCurrentItemNotes(); 
				break;
			case TRACK_NOTES: saveCurrentTrackNotes(); break;
			case MARKER_NAME: saveCurrentMarkerName(); break;
			case ACTION_HELP: saveCurrentHelp(); break;
			case NOTES_HELP_DISABLED: saveCurrentPrjNotes(); break;
		}
	}
}

void SNM_NotesHelpWnd::saveCurrentPrjNotes()
{
	char buf[MAX_HELP_LENGTH] = "";
	memset(buf, 0, sizeof(buf));
	GetDlgItemText(GetHWND(), IDC_EDIT, buf, MAX_HELP_LENGTH);
	if (strncmp(g_lastText, buf, MAX_HELP_LENGTH)) {
		g_prjNotes.Get()->Set(buf);
		Undo_OnStateChangeEx("Edit project notes", UNDO_STATE_MISCCFG, -1);//JFB TODO? -1 to remplace?
	}
}

void SNM_NotesHelpWnd::saveCurrentHelp()
{
	// skip custom cmds
	if (*g_lastActionId && g_lastActionDesc && 
	    *g_lastActionDesc && _strnicmp(g_lastActionDesc, "Custom:", 7))
	{
		char buf[MAX_HELP_LENGTH] = "";
		memset(buf, 0, sizeof(buf));
		GetDlgItemText(GetHWND(), IDC_EDIT, buf, MAX_HELP_LENGTH);
		if (strncmp(g_lastText, buf, MAX_HELP_LENGTH))
			saveHelp(g_lastActionId, buf);
	}
}

void SNM_NotesHelpWnd::saveCurrentItemNotes()
{
	if (g_mediaItemNote && GetMediaItem_Track(g_mediaItemNote))
	{
		char buf[MAX_HELP_LENGTH] = "";
		memset(buf, 0, sizeof(buf));
		GetDlgItemText(GetHWND(), IDC_EDIT, buf, MAX_HELP_LENGTH);
		if (strncmp(g_lastText, buf, MAX_HELP_LENGTH))
		{
			bool update = false;
			WDL_String newNotes;
			if (!*buf || GetNotesChunkFromString(buf, &newNotes))
			{
				SNM_ChunkParserPatcher p(g_mediaItemNote);
				// Replaces notes (or adds them if failed), remarks:
				// - newNotes can be "", i.e. remove notes
				// - we use VOLPAN as it also exists for empty items
				update = p.ReplaceSubChunk("NOTES", 2, 0, newNotes.Get(), "VOLPAN"); 
				if (!update && *buf)
					update = p.InsertAfterBefore(1, newNotes.Get(), "ITEM", g_bv4 ? "IID" : "IGUID", 1, 0, "VOLPAN");
			}
			// the patch has occured
			if (update)
			{
				UpdateItemInProject(g_mediaItemNote);
				UpdateTimeline();
				Undo_OnStateChangeEx("Edit item notes", UNDO_STATE_ALL, -1); //JFB TODO? -1 to remplace?
			}
		}
	}
}

void SNM_NotesHelpWnd::saveCurrentTrackNotes()
{
	if (g_trNote && CSurf_TrackToID(g_trNote, false) >= 0)
	{
		char buf[MAX_HELP_LENGTH] = "";
		memset(buf, 0, sizeof(buf));
		GetDlgItemText(GetHWND(), IDC_EDIT, buf, MAX_HELP_LENGTH);
		if (strncmp(g_lastText, buf, MAX_HELP_LENGTH))
		{
			bool found = false;
			for (int i = 0; i < g_pTracksNotes.Get()->GetSize(); i++) 
			{
				if (g_pTracksNotes.Get()->Get(i)->m_tr == g_trNote) {
					g_pTracksNotes.Get()->Get(i)->m_notes.Set(buf);
					found = true;
					break;
				}
			}
			if (!found)
				g_pTracksNotes.Get()->Add(new SNM_TrackNotes(g_trNote, buf));
			Undo_OnStateChangeEx("Edit track notes", UNDO_STATE_MISCCFG, -1); //JFB TODO? -1 to remplace?
		}
	}
}

void SNM_NotesHelpWnd::saveCurrentMarkerName()
{
	if (g_lastMarkerIdx >= 0)
	{
		double pos; int markrgnindexnumber;
		if (EnumProjectMarkers2(NULL, g_lastMarkerIdx, NULL, &pos, NULL, NULL, &markrgnindexnumber))
		{
			char buf[SNM_MAX_MARKER_NAME_LEN] = "";
			memset(buf, 0, sizeof(buf));
			GetDlgItemText(GetHWND(), IDC_EDIT, buf, SNM_MAX_MARKER_NAME_LEN);
			ShortenStringToFirstRN(buf);
			if (strncmp(g_lastText, buf, SNM_MAX_MARKER_NAME_LEN) && SetProjectMarker2(NULL, markrgnindexnumber, false, pos, 0.0, buf))
				Undo_OnStateChangeEx("Edit marker name", UNDO_STATE_ALL, -1);
		}
	}
}

int SNM_NotesHelpWnd::updateActionHelp()
{
	int iSel, actionId;
	char idstr[SNM_MAX_ACTION_CUSTID_LEN] = "", desc[128] = "";
	iSel = GetSelectedActionId(g_lastActionSection, SNM_MAX_SECTION_NAME_LEN, &actionId, idstr, 64, desc, 128);
	if (iSel >= 0)
	{
		if (iSel != g_lastActionListSel)
		{
			g_lastActionListSel = iSel;
			g_lastActionListCmd = actionId; 
			lstrcpyn(g_lastActionDesc, desc, 128);
			lstrcpyn(g_lastActionId, idstr, SNM_MAX_ACTION_CUSTID_LEN);

			if (g_lastActionId && *g_lastActionId && g_lastActionDesc && *g_lastActionDesc)
			{
				// skip macros
				if (!_strnicmp(g_lastActionDesc, "Custom:", 7))
					SetText(g_lastActionId);
				else
				{
					char buf[MAX_HELP_LENGTH] = "";
					loadHelp(g_lastActionId, buf, MAX_HELP_LENGTH);
					SetText(buf);
				}
				return REQUEST_REFRESH;
			}
		}
		else
			return NO_REFRESH;
	}
	return REQUEST_REFRESH_EMPTY;
}

int SNM_NotesHelpWnd::updateItemNotes()
{
	int refreshType = REQUEST_REFRESH_EMPTY;
	if (CountSelectedMediaItems(NULL))
	{
		MediaItem* selItem = GetSelectedMediaItem(NULL, 0);
		if (selItem != g_mediaItemNote)
		{
			g_mediaItemNote = selItem;

			SNM_ChunkParserPatcher p(g_mediaItemNote);
			WDL_String notes;
			char buf[MAX_HELP_LENGTH] = "";
			if (p.GetSubChunk("NOTES", 2, 0, &notes, "VOLPAN") >= 0) // rmk: we use VOLPAN as it also exists for empty items
				GetStringFromNotesChunk(&notes, buf, MAX_HELP_LENGTH);
			SetText(buf);
			refreshType = REQUEST_REFRESH;
		} 
		else
			refreshType = NO_REFRESH;
	}
	return refreshType;
}

int SNM_NotesHelpWnd::updateTrackNotes()
{
	int refreshType = REQUEST_REFRESH_EMPTY;
	if (CountSelectedTracksWithMaster(NULL))
	{
		MediaTrack* selTr = GetSelectedTrackWithMaster(NULL, 0);
		if (selTr != g_trNote)
		{
			g_trNote = selTr;

			WDL_String* notes = NULL;
			for (int i = 0; i < g_pTracksNotes.Get()->GetSize(); i++) {
				if (g_pTracksNotes.Get()->Get(i)->m_tr == g_trNote) {
					notes = &(g_pTracksNotes.Get()->Get(i)->m_notes);
					break;
				}
			}
			if (!notes) {
				SNM_TrackNotes* tn = new SNM_TrackNotes(g_trNote, "");
				g_pTracksNotes.Get()->Add(tn);
				notes = &(tn->m_notes);
			}
			SetText(notes->Get());
			refreshType = REQUEST_REFRESH;
		} 
		else
			refreshType = NO_REFRESH;
	}
	return refreshType;
}

int SNM_NotesHelpWnd::updateMarkerName()
{
	int refreshType = REQUEST_REFRESH_EMPTY;

	double dPos;
	if (GetPlayState())
		dPos = GetPlayPosition();
	else 
		dPos = GetCursorPosition();

	if (g_lastMarkerPos < 0.0 || fabs(g_lastMarkerPos-dPos) > 0.1)
	{
		g_lastMarkerPos = dPos;
		int idx = FindMarker(dPos);
		if (idx >= 0)
		{
			if (idx != g_lastMarkerIdx)
			{
				char* name;
				EnumProjectMarkers2(NULL, idx, NULL, NULL, NULL, &name, NULL);
				SetText(name);
				refreshType = REQUEST_REFRESH;
				g_lastMarkerIdx = idx;
			}
			else
				refreshType = NO_REFRESH;
		}
		// else => empty
	}
	else
		refreshType = NO_REFRESH;
	return refreshType;
}

void SNM_NotesHelpWnd::OnInitDlg()
{
	m_resize.init_item(IDC_EDIT, 0.0, 0.0, 1.0, 1.0);
	SetWindowLongPtr(GetDlgItem(m_hwnd, IDC_EDIT), GWLP_USERDATA, 0xdeadf00b);

	// Load prefs 
	m_type = GetPrivateProfileInt("NOTES_HELP_VIEW", "TYPE", 0, g_SNMiniFilename.Get());
	g_locked = GetPrivateProfileInt("NOTES_HELP_VIEW", "LOCK", 1, g_SNMiniFilename.Get());

	// WDL GUI init
	m_vwnd_painter.SetGSC(WDL_STYLE_GetSysColor);
    m_parentVwnd.SetRealParent(m_hwnd);

	m_btnLock.SetID(BUTTONID_LOCK);
	m_btnLock.SetRealParent(m_hwnd);
	m_parentVwnd.AddChild(&m_btnLock);

	m_btnAlr.SetID(BUTTONID_ALR);
	m_btnAlr.SetRealParent(m_hwnd);
	m_parentVwnd.AddChild(&m_btnAlr);

	m_cbType.SetID(COMBOID_TYPE);
	m_cbType.SetRealParent(m_hwnd);
	m_cbType.AddItem("Project notes");
	m_cbType.AddItem("Item notes");
	m_cbType.AddItem("Track notes");
	m_cbType.AddItem("Marker names");
#ifdef _WIN32
	m_cbType.AddItem("Action help");
#endif
	m_cbType.SetCurSel(min(m_cbType.GetCount(), m_type)); // safety for SWS beta <-> SWS official
	m_parentVwnd.AddChild(&m_cbType);

	m_txtLabel.SetID(TXTID_LABEL);
	m_txtLabel.SetRealParent(m_hwnd);
	m_parentVwnd.AddChild(&m_txtLabel);
	
	m_previousType = -1; // will force refresh
	Update();

	if (m_type != NOTES_HELP_DISABLED)
		SetTimer(m_hwnd, 1, 125, NULL);
}

void SNM_NotesHelpWnd::OnCommand(WPARAM wParam, LPARAM lParam)
{
	if (wParam == (IDC_EDIT | (EN_CHANGE << 16)))
		saveCurrentText(m_type); // + undos
	else if (wParam == SET_ACTION_HELP_FILE_MSG)
		SetActionHelpFilename(NULL);
	else if (HIWORD(wParam)==0)	
	{
		switch(LOWORD(wParam))
		{
			case BUTTONID_LOCK:
			{
				g_locked = !g_locked;
				if (!g_locked)
					SetFocus(GetDlgItem(m_hwnd, IDC_EDIT));
				RefreshToolbar(NamedCommandLookup("_S&M_ACTIONHELPTGLOCK"));
				RefreshGUI();
			}
			break;
			case BUTTONID_ALR:
			{
				if (g_lastActionId && *g_lastActionId && g_lastActionDesc && 
					*g_lastActionDesc && _strnicmp(g_lastActionDesc, "Custom:", 7))
				{
					char cLink[256] = "";
					char sectionURL[SNM_MAX_SECTION_NAME_LEN] = "";
					if (GetSectionName(true, g_lastActionSection, sectionURL, SNM_MAX_SECTION_NAME_LEN))
					{					
						_snprintf(cLink, 256, "http://www.cockos.com/wiki/index.php/%s_%s", sectionURL, g_lastActionId);
						ShellExecute(m_hwnd, "open", cLink , NULL, NULL, SW_SHOWNORMAL);
					}
				}
				else
					ShellExecute(m_hwnd, "open", "http://wiki.cockos.com/wiki/index.php/Action_List_Reference" , NULL, NULL, SW_SHOWNORMAL);
			}
			break;
		}
	}
	else if (HIWORD(wParam)==CBN_SELCHANGE && LOWORD(wParam)==COMBOID_TYPE)	
	{
		SetType(m_cbType.GetCurSel());
		if (!g_locked)
			SetFocus(GetDlgItem(m_hwnd, IDC_EDIT));
	}
	else 
		Main_OnCommand((int)wParam, (int)lParam);
}

/*JFB r376
bool SNM_NotesHelpWnd::IsActive(bool bWantEdit) {
	return (bWantEdit || GetForegroundWindow() == m_hwnd || GetFocus() == GetDlgItem(m_hwnd, IDC_EDIT));
}
*/

HMENU SNM_NotesHelpWnd::OnContextMenu(int x, int y)
{
	HMENU hMenu = CreatePopupMenu();
	AddToMenu(hMenu, "Set action help file...", SET_ACTION_HELP_FILE_MSG);
	return hMenu;
}

void SNM_NotesHelpWnd::OnDestroy() 
{
	KillTimer(m_hwnd, 1);

	// save prefs
	char cType[2], cLock[2];
	sprintf(cType, "%d", m_type);
	sprintf(cLock, "%d", g_locked);
	WritePrivateProfileString("NOTES_HELP_VIEW", "TYPE", cType, g_SNMiniFilename.Get()); 
	WritePrivateProfileString("NOTES_HELP_VIEW", "LOCK", cLock, g_SNMiniFilename.Get()); 

	m_previousType = -1;
	m_cbType.Empty();
	m_parentVwnd.RemoveAllChildren(false);
	m_parentVwnd.SetRealParent(NULL);
}

// we don't check iKeyState in order to catch (almost) everything
// some key masks won't pass here though (e.g. Ctrl+Shift)
// returns:
// -1 -> catch and send to the control
//  0 -> pass-thru to main window (then -666 in SWS_DockWnd::keyHandler())
//  1 -> eat
int SNM_NotesHelpWnd::OnKey(MSG* msg, int iKeyState) 
{
	if (GetDlgItem(m_hwnd, IDC_EDIT) == msg->hwnd)
	{
		if (g_locked) {
			msg->hwnd = m_hwnd; // redirect to main window
			return 0; // pass-thru to main window
		}
		else if ((msg->message == WM_KEYDOWN || msg->message == WM_CHAR) && msg->wParam == VK_RETURN)
			return -1; // Catch the return and send to edit control for multi-line
	}
	return 0; 
}

void SNM_NotesHelpWnd::OnTimer(WPARAM wParam) {
	// no update when the user edits & when the view is hidden (e.g. inactive docker tab)
	// but when the view is active: update only for markers and if the view is locked 
    //(=> updates during playback, in other cases -e.g. item selection change- the main window 
	// *will* be the active one)
	if ((!IsActive() || (g_locked && m_type == MARKER_NAME)) && IsWindowVisible(m_hwnd))
		Update();
}

void SNM_NotesHelpWnd::DrawControls(LICE_IBitmap* _bm, RECT* _r)
{
	if (!_bm)
		return;
	
	LICE_CachedFont* font = SNM_GetThemeFont();
	IconTheme* it = (IconTheme*)GetIconThemeStruct(NULL);// returns the whole icon theme (icontheme.h) and the size
	int x0=_r->left+10, h=35;

	// Lock button
	WDL_VirtualIconButton_SkinConfig* img = it ? &(it->toolbar_lock[!g_locked]) : NULL;
	if (img)
		m_btnLock.SetIcon(img);
	else {
		m_btnLock.SetTextLabel(g_locked ? "Unlock" : "Lock", 0, font);
		m_btnLock.SetForceBorder(true);
	}
	if (!SetVWndAutoPosition(&m_btnLock, NULL, _r, &x0, _r->top, h))
		return;

	// view type
	m_cbType.SetFont(font);
	if (!SetVWndAutoPosition(&m_cbType, NULL, _r, &x0, _r->top, h))
		return;

	// online help
	m_btnAlr.SetVisible(m_type == ACTION_HELP);
	if (m_type == ACTION_HELP)
	{
		m_btnAlr.SetTextLabel("ALR Wiki", 0, font);
		m_btnAlr.SetForceBorder(true);
		if (!SetVWndAutoPosition(&m_btnAlr, NULL, _r, &x0, _r->top, h, 5))
			return;
	}

	// label
	char str[512] = "No selection!";
	switch(m_type)
	{
		case ACTION_HELP:
			if (g_lastActionDesc && *g_lastActionDesc && g_lastActionSection && *g_lastActionSection)
				_snprintf(str, 512, " [%s] %s", g_lastActionSection, g_lastActionDesc);
/*JFB API limitation: use smthg like that when we'll be able to access all sections
				lstrcpyn(str, kbd_getTextFromCmd(g_lastActionListCmd, NULL), 512);
*/
			break;
		case ITEM_NOTES:
			if (g_mediaItemNote)
			{
				MediaItem_Take* tk = GetActiveTake(g_mediaItemNote);
				char* tkName= tk ? (char*)GetSetMediaItemTakeInfo(tk, "P_NAME", NULL) : NULL;
				lstrcpyn(str, tkName ? tkName : "", 512);
			}
			break;
		case TRACK_NOTES:
			if (g_trNote)
			{
				int id = CSurf_TrackToID(g_trNote, false);
				if (id > 0)
					_snprintf(str, 512, " [%d] %s", id, (char*)GetSetMediaTrackInfo(g_trNote, "P_NAME", NULL));
				else if (id == 0)
					strcpy(str, "[MASTER]");
			}
			break;
		case MARKER_NAME:
			if (g_lastMarkerIdx >= 0)
			{
				double pos; int nb;
				if (EnumProjectMarkers2(NULL, g_lastMarkerIdx, NULL, &pos, NULL, NULL, &nb))
				{
					char timeStr[32] = "";
					format_timestr_pos(pos, timeStr, 32, -1);
					_snprintf(str, 512, " [%d] %s", nb, timeStr);
				}
				else
					*str = '\0';
			}
			else
				strcpy(str, "No marker found before play/edit cursor position!");
			break;
		case NOTES_HELP_DISABLED:
			EnumProjects(-1, str, 512);
			break;
	}
	m_txtLabel.SetFont(font);
	m_txtLabel.SetText(str);
	if (!SetVWndAutoPosition(&m_txtLabel, NULL, _r, &x0, _r->top, h))
		return;

	AddSnMLogo(_bm, _r, x0, h);

	// big notes (dynamic font size)
	if (g_locked)
	{
		char txt[MAX_HELP_LENGTH] = "";
		GetDlgItemText(GetHWND(), IDC_EDIT, txt, MAX_HELP_LENGTH);
		if (m_type == MARKER_NAME)
			ShortenStringToFirstRN(txt);

/*JFB light weight display, looks bad..
		ColorTheme* ct = (ColorTheme*)GetColorThemeStruct(NULL);
		int ww,hh;
		LICE_MeasureText(txt, &ww, &hh);
		LICE_MemBitmap* img_txt = new LICE_MemBitmap(ww,hh);
		LICE_Clear(img_txt,0);
		LICE_DrawText(img_txt,0,0,txt,ct ? LICE_RGBA_FROMNATIVE(ct->main_text,255) : LICE_RGBA(255,255,255,255),1.0,LICE_BLIT_MODE_COPY);
		LICE_ScaledBlit(_bm,img_txt,10,h+5,_bm->getWidth()-10,_bm->getHeight()-h,0,0,ww,hh,1.0,LICE_BLIT_MODE_ADD);
		delete img_txt;
*/

		char buf[MAX_HELP_LENGTH] = "";
		lstrcpyn(buf, txt, MAX_HELP_LENGTH);                    
		int numlines=1, maxlinelen=-1;
		char* p = strstr(buf, "\n");
		char* p2 = buf;
		while (p)
		{
		  maxlinelen = max(maxlinelen, (int)(p-p2));
		  p2 = p+1;
		  *p=0;
		  p = strstr(p+1, "\n");
		  ++numlines;
		}
		if (maxlinelen < 0)
			maxlinelen = strlen(buf);

		p=buf;

		static LICE_CachedFont dynFont;

		// creating fonts is ***super slow***
		// => use a text width estimation instead
		int fontHeight = (int)((_bm->getHeight()-h)/numlines + 0.5);
		while (fontHeight > 5 && (fontHeight*maxlinelen*0.6) > _bm->getWidth()) 
			fontHeight--;

		if (fontHeight <= 5)
			return;

		ColorTheme* ct = (ColorTheme*)GetColorThemeStruct(NULL);
		HFONT lf = CreateFont(fontHeight,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
			OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,SWSDLG_TYPEFACE);
		dynFont.SetFromHFont(lf,LICE_FONT_FLAG_OWNS_HFONT);
		dynFont.SetBkMode(TRANSPARENT);
		dynFont.SetTextColor(ct ? LICE_RGBA_FROMNATIVE(ct->main_text,255) : LICE_RGBA(255,255,255,255));

		int top = (int)(h + (_bm->getHeight()-h)/2 - (fontHeight*numlines)/2 + 0.5);
		for (int i = 0; i < numlines; ++i)
		{                   
			RECT tr = {0,0,0,0};
			dynFont.DrawText(NULL, p, -1, &tr, DT_CALCRECT);
			int txtw = tr.right - tr.left;
			tr.top = top + i*fontHeight;
			tr.bottom = tr.top+fontHeight;
			tr.left = (int)(_bm->getWidth()/2 - txtw/2 + 0.5);
			tr.right = tr.left + txtw;
			dynFont.DrawText(_bm, p, -1, &tr, 0);
			p += strlen(p)+1;
		}
		dynFont.SetFromHFont(NULL,LICE_FONT_FLAG_OWNS_HFONT);
		DeleteObject(lf); 
    }
	else
		LICE_FillRect(_bm,0,h,_bm->getWidth(),_bm->getHeight()-h,WDL_STYLE_GetSysColor(COLOR_WINDOW),0.5,LICE_BLIT_MODE_COPY);
}

void SNM_NotesHelpWnd::OnResize() {
  InvalidateRect(m_hwnd,NULL,FALSE);
}

int SNM_NotesHelpWnd::OnUnhandledMsg(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg)
	{
		case WM_PAINT:
		{
			RECT r;
			GetClientRect(m_hwnd,&r);		
			m_parentVwnd.SetPosition(&r);
			m_vwnd_painter.PaintBegin(m_hwnd, WDL_STYLE_GetSysColor(COLOR_WINDOW));
			int xo, yo;
			LICE_IBitmap* bm = m_vwnd_painter.GetBuffer(&xo, &yo);
			bm->resize(r.right-r.left,r.bottom-r.top);
			DrawControls(bm, &r);
			m_vwnd_painter.PaintVirtWnd(&m_parentVwnd);
			m_vwnd_painter.PaintEnd();
		}
		break;
		case WM_LBUTTONDOWN:
			SetFocus(m_hwnd);
			if (m_parentVwnd.OnMouseDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)) > 0)
				SetCapture(m_hwnd);
			break;
		case WM_LBUTTONUP:
			if (GetCapture() == m_hwnd) {
				m_parentVwnd.OnMouseUp(GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam));
				ReleaseCapture();
			}
			break;
		case WM_MOUSEMOVE:
			m_parentVwnd.OnMouseMove(GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam));
			break;
#ifdef _SNM_THEMABLE
		case WM_CTLCOLOREDIT:
		{
			if ((HWND)lParam == GetDlgItem(m_hwnd, IDC_EDIT))
			{
				SetBkColor((HDC)wParam, GSC_mainwnd(COLOR_WINDOW));
				SetTextColor((HDC)wParam, GSC_mainwnd(COLOR_BTNTEXT));
				return (INT_PTR)SNM_GetThemeBrush();
			}
		}
#endif
	}
	return 0;
}


///////////////////////////////////////////////////////////////////////////////

void SNM_NotesHelpWnd::loadHelp(const char* _cmdName, char* _buf, int _bufSize)
{
	if (_cmdName && *_cmdName && _buf)
	{
		memset(_buf, 0, sizeof(_buf));

		char buf[MAX_HELP_LENGTH] = "";
		int j=0, size = GetPrivateProfileSection(_cmdName,buf,_bufSize,m_actionHelpFilename.Get());
		for (int i=0; i < (size-1)  && i < _bufSize; i++) // size-1, double null terminated
		{
			if (!buf[i]) 
				_buf[j++] = '\n';
			else if (buf[i] != '|') // see saveHelp()
				_buf[j++] = buf[i];
		}
		buf[_bufSize-1] = 0; //just in case..
	}
}

void SNM_NotesHelpWnd::saveHelp(const char* _cmdName, const char* _help)
{
	if (_cmdName && *_cmdName && _help) 
	{
		char buf[MAX_HELP_LENGTH] = "";
		memset(buf, 0, sizeof(buf));

		int i=0, j=0;
		bool lastCR = false;
		while (_help[i] && i < MAX_HELP_LENGTH && j < MAX_HELP_LENGTH)
		{
			if (_help[i] == '[') {
				buf[j++] = '(';
				lastCR = false;
			}
			else if (_help[i] == ']') {
				buf[j++] = ')';
				lastCR = false;
			}
			else if (_help[i] != '\r' && _help[i] != '|')
			{
				if (_help[i] == '\n' && lastCR) {
					buf[j++] = '|'; // allows empty lines, scratched by GetPrivateProfileSection() otherwise!
					buf[j++] = '\n';
					// lastCR remains true
				}
				else {
					buf[j++] = _help[i];
					lastCR = (_help[i] == '\n');
				}
			}
//KO			lastCR = (_help[i] == '\n');
			i++;
		}
		buf[MAX_HELP_LENGTH-1] = 0; //just in case..

		WritePrivateProfileStruct(_cmdName, NULL, NULL, 0, m_actionHelpFilename.Get()); //flush section
		WritePrivateProfileSection(_cmdName, buf, m_actionHelpFilename.Get());
	}
}

void SNM_NotesHelpWnd::readActionHelpFilenameIniFile()
{
	char defaultHelpPath[BUFFER_SIZE], buf[BUFFER_SIZE];
	_snprintf(defaultHelpPath, BUFFER_SIZE, SNM_ACTION_HELP_INI_FILE, GetResourcePath());
	GetPrivateProfileString("NOTES_HELP_VIEW", "ACTION_HELP_FILE", defaultHelpPath, buf, BUFFER_SIZE, g_SNMiniFilename.Get());
	m_actionHelpFilename.Set(buf);
}

void SNM_NotesHelpWnd::saveActionHelpFilenameIniFile() {
	WDL_String escapedStr;
	makeEscapedConfigString(m_actionHelpFilename.Get(), &escapedStr);
	WritePrivateProfileString("NOTES_HELP_VIEW", "ACTION_HELP_FILE", escapedStr.Get(), g_SNMiniFilename.Get());
}

const char* SNM_NotesHelpWnd::getActionHelpFilename() {
	return m_actionHelpFilename.Get();
}

void SNM_NotesHelpWnd::setActionHelpFilename(const char* _filename) {
	m_actionHelpFilename.Set(_filename);
}

void SetActionHelpFilename(COMMAND_T*) 
{
	//JFB BrowseForSaveFile() always asks for overwrite: painful!
	char filename[BUFFER_SIZE] = "";
	if (g_pNotesHelpWnd && BrowseForSaveFile("Set action help file", g_pNotesHelpWnd->getActionHelpFilename(), g_pNotesHelpWnd->getActionHelpFilename(), SNM_INI_EXT_LIST, filename, BUFFER_SIZE))
		g_pNotesHelpWnd->setActionHelpFilename(filename);
}

///////////////////////////////////////////////////////////////////////////////

bool GetStringFromNotesChunk(WDL_String* _notes, char* _buf, int _bufMaxSize)
{
	if (!_buf || !_notes)
		return false;

	memset(_buf, 0, sizeof(_buf));

/* No! ProcessExtensionLine() provides partial chunks..
	// Test note chunk validity
	if (_strnicmp(_notes->Get(), "<NOTES", 6) || _strnicmp((char*)(_notes->Get()+_notes->GetLength()-3), "\n>\n", 3))
		return (_notes->GetLength() == 0); // empty _notes is a valid case
*/
	char* pNotes = _notes->Get();

	// find 1st '|'
	int i=0, j;
	while (*pNotes && pNotes[i] && pNotes[i] != '|') i++;

	if (*pNotes && pNotes[i]) j = i+1;
	else return true;

	int bufIdx = 0;
	while (pNotes[j] && j < _bufMaxSize && bufIdx < _bufMaxSize)
	{
		if (pNotes[j] != '|') 
			_buf[bufIdx++] = pNotes[j];
		j++;
	}
	_buf[bufIdx-3] = 0; // remove last chunk's "\n>\n"
	return true;
}

bool GetNotesChunkFromString(const char* _buf, WDL_String* _notes, const char* _startLine)
{
	if (_notes && _buf)
	{
		// Save current note
		if (!_startLine) _notes->Set("<NOTES\n|");
		else _notes->Set(_startLine);

		int j=0;
		while (_buf[j]) {
			if (_buf[j] == '\n') 
				_notes->Append("\n|");
			else if (_buf[j] != '\r') 
				_notes->AppendFormatted(2, "%c", _buf[j]); 
			j++;
		}
		_notes->Append("\n>\n");
	}
	else
		return false;
	return true;
}


///////////////////////////////////////////////////////////////////////////////

void SNM_NoteHlp_TLChangeSchedJob::Perform()
{
	if (g_pNotesHelpWnd)
		g_pNotesHelpWnd->Update(true);
}


///////////////////////////////////////////////////////////////////////////////

static bool ProcessExtensionLine(const char *line, ProjectStateContext *ctx, bool isUndo, struct project_config_extension_t *reg)
{
	LineParser lp(false);
	if (lp.parse(line) || lp.getnumtokens() < 1)
		return false;

	// Load project notes
	if (!strcmp(lp.gettoken_str(0), "<S&M_PROJNOTES"))
	{
		WDL_String notes;
		ExtensionConfigToString(&notes, ctx);

		char buf[MAX_HELP_LENGTH] = "";
		GetStringFromNotesChunk(&notes, buf, MAX_HELP_LENGTH);
		g_prjNotes.Get()->Set(buf);
		if (g_pNotesHelpWnd && g_pNotesHelpWnd->GetType() == NOTES_HELP_DISABLED)
			g_pNotesHelpWnd->Update();

		return true;
	}
	// Load track notes
	else if (!strcmp(lp.gettoken_str(0), "<S&M_TRACKNOTES"))
	{
		GUID g;		
		stringToGuid(lp.gettoken_str(1), &g);
		MediaTrack* tr = GuidToTrack(&g);
		if (tr)
		{
			WDL_String notes;
			ExtensionConfigToString(&notes, ctx);

			char buf[MAX_HELP_LENGTH] = "";
			if (GetStringFromNotesChunk(&notes, buf, MAX_HELP_LENGTH))
				g_pTracksNotes.Get()->Add(new SNM_TrackNotes(tr, buf));

			return true;
		}
	}
	return false;
}

static void SaveExtensionConfig(ProjectStateContext *ctx, bool isUndo, struct project_config_extension_t *reg)
{
	// first delete unused tracks
	for (int i = 0; i < g_pTracksNotes.Get()->GetSize(); i++)
		if (CSurf_TrackToID(g_pTracksNotes.Get()->Get(i)->m_tr, false) < 0)
			g_pTracksNotes.Get()->Delete(i--, true);

	char startLine[4096], strId[128];
	WDL_String formatedNotes;

	// Save project notes
	if (g_prjNotes.Get()->GetLength())
	{
		strcpy(startLine, "<S&M_PROJNOTES\n|");
		if (GetNotesChunkFromString(g_prjNotes.Get()->Get(), &formatedNotes, startLine))
			StringToExtensionConfig(formatedNotes.Get(), ctx);
	}

	// Save track notes
	for (int i = 0; i < g_pTracksNotes.Get()->GetSize(); i++)
	{
		if (g_pTracksNotes.Get()->Get(i)->m_notes.GetLength())
		{
			GUID g;
			if (CSurf_TrackToID(g_pTracksNotes.Get()->Get(i)->m_tr, false))
				g = *(GUID*)GetSetMediaTrackInfo(g_pTracksNotes.Get()->Get(i)->m_tr, "GUID", NULL);
			else
				g = GUID_NULL;
			guidToString(&g, strId);
			sprintf(startLine, "<S&M_TRACKNOTES %s\n|", strId);

			if (GetNotesChunkFromString(g_pTracksNotes.Get()->Get(i)->m_notes.Get(), &formatedNotes, startLine))
				StringToExtensionConfig(formatedNotes.Get(), ctx);
		}
	}
}

static void BeginLoadProjectState(bool isUndo, struct project_config_extension_t *reg)
{
	// Init S&M project notes with REAPER's ones
	g_prjNotes.Cleanup();
	g_prjNotes.Get()->Set("");

	// Load REAPER project notes from RPP file
	char cBuf[BUFFER_SIZE];
	EnumProjects(-1, cBuf, BUFFER_SIZE);
	ProjectStateContext* prjCtx = ProjectCreateFileRead(cBuf);
	if (prjCtx)
	{
		WDL_String rpp;
		while(!prjCtx->GetLine(cBuf, BUFFER_SIZE)) {
			rpp.Append(cBuf);
			rpp.Append("\n");
		}
		delete prjCtx;

		// "translate notes"
		SNM_ChunkParserPatcher p(&rpp);
		WDL_String notes;
		if (p.GetSubChunk("NOTES", 2, 0, &notes, "RIPPLE") >= 0)
		{
			char bufNotes[MAX_HELP_LENGTH] = "";
			if (GetStringFromNotesChunk(&notes, bufNotes, MAX_HELP_LENGTH))
				g_prjNotes.Get()->Set(bufNotes);
		}
	}

	// Init track notes
	g_pTracksNotes.Cleanup();
	g_pTracksNotes.Get()->Empty(true);
}

static project_config_extension_t g_projectconfig = {
	ProcessExtensionLine, SaveExtensionConfig, BeginLoadProjectState, NULL
};

int NotesHelpViewInit() {
	g_pNotesHelpWnd = new SNM_NotesHelpWnd();
	if (!g_pNotesHelpWnd || !plugin_register("projectconfig",&g_projectconfig))
		return 0;
	return 1;
}

void NotesHelpViewExit() {
	if (g_pNotesHelpWnd)
		g_pNotesHelpWnd->saveActionHelpFilenameIniFile();
	delete g_pNotesHelpWnd;
	g_pNotesHelpWnd = NULL;
}

void OpenNotesHelpView(COMMAND_T* _ct) {
	if (g_pNotesHelpWnd) {
		g_pNotesHelpWnd->Show(g_pNotesHelpWnd->GetType() == (int)_ct->user /* i.e toggle */, true);
		g_pNotesHelpWnd->SetType((int)_ct->user);
		if (!g_locked)
			SetFocus(GetDlgItem(g_pNotesHelpWnd->GetHWND(), IDC_EDIT));
	}
}

bool IsNotesHelpViewDisplayed(COMMAND_T* _ct) {
	return (g_pNotesHelpWnd && g_pNotesHelpWnd->IsValidWindow());
}

void ToggleNotesHelpLock(COMMAND_T*) {

	g_locked = !g_locked;
	if (g_pNotesHelpWnd)
	{
		g_pNotesHelpWnd->RefreshGUI();
		if (!g_locked)
			SetFocus(GetDlgItem(g_pNotesHelpWnd->GetHWND(), IDC_EDIT));
	}
}

bool IsNotesHelpLocked(COMMAND_T*) {
	return (g_locked == 1 ? true : false); // get rid of warning C4800
}
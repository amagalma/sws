/******************************************************************************
/ wol_Zoom.cpp
/
/ Copyright (c) 2014 wol
/ http://forum.cockos.com/member.php?u=70153
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

#include "stdafx.h"
#include "wol_Zoom.h"
#include "../sws_util.h"
#include "../SnM/SnM_Dlg.h"
#include "../Breeder/BR_Util.h"
#include "../Breeder/BR_EnvTools.h"

bool EnvelopesExtendedZoom = false;
int SavedEnvelopeOverlapSettings = 0;



// from SnM.cpp
// http://forum.cockos.com/project.php?issueid=4576
int AdjustRelative(int _adjmode, int _reladj)
{
	if (_adjmode == 1) { if (_reladj >= 0x40) _reladj |= ~0x3f; } // sign extend if 0x40 set
	else if (_adjmode == 2) { _reladj -= 0x40; } // offset by 0x40
	else if (_adjmode == 3) { if (_reladj & 0x40) _reladj = -(_reladj & 0x3f); } // 0x40 is sign bit
	else _reladj = 0;
	return _reladj;
}

int GetTcpEnvMinHeight()
{
	return SNM_GetIconTheme()->envcp_min_height;
}

int GetCurrentTcpMaxHeight()
{
	RECT r;
	GetClientRect(GetArrangeWnd(), &r);
	return r.bottom - r.top;
}

int CountVisibleTrackEnvelopesInTrackLane(MediaTrack* track)
{
	int count = 0;
	for (int i = 0; i < CountTrackEnvelopes(track); i++)
	{
		BR_Envelope env(GetTrackEnvelope(track, i));
		if (!env.IsInLane() && env.IsVisible())
			count++;
	}
	return count;
}

//Return true for multiple lanes; false for single lane (envelopes are overlapped or there is only one -visible-) or if envelope is not in track lane
bool IsTrackLaneEnvelopeInMultipleLanes(TrackEnvelope* envelope)
{
	BR_Envelope env(envelope);
	if (env.IsInLane())
		return false;

	int overlapMinHeight = *(int*)GetConfigVar("env_ol_minh");
	if (overlapMinHeight >= 0 && GetTrackEnvHeight(envelope, NULL, false) < overlapMinHeight)
		return false;

	return true;
}

// Stolen from BR_Util.cpp
void ScrollToTrackEnvIfNotInArrange(TrackEnvelope* envelope)
{
	int offsetY;
	int height = GetTrackEnvHeight(envelope, &offsetY, false, NULL);

	HWND hwnd = GetArrangeWnd();
	SCROLLINFO si = { sizeof(SCROLLINFO), };
	si.fMask = SIF_ALL;
	CoolSB_GetScrollInfo(hwnd, SB_VERT, &si);

	BR_Envelope env(envelope);
	int envEnd = offsetY + height;
	int pageEnd = si.nPos + (int)si.nPage + SCROLLBAR_W;

	if (offsetY < si.nPos || envEnd > pageEnd)
	{
		si.nPos = offsetY;
		CoolSB_SetScrollInfo(hwnd, SB_VERT, &si, true);
		SendMessage(hwnd, WM_VSCROLL, si.nPos << 16 | SB_THUMBPOSITION, NULL);
	}
}

void AdjustSelectedEnvelopeHeight(COMMAND_T* ct, int val, int valhw, int relmode, HWND hwnd)
{
	if (relmode > 0)
	{
		if (TrackEnvelope* env = GetSelectedTrackEnvelope(NULL))
		{
			BR_Envelope brEnv(env);
			int height = AdjustRelative(relmode, (valhw == -1) ? BOUNDED(val, 0, 127) : (int)BOUNDED(16384.0 - (valhw | val << 7), 0.0, 16383.0));

			if (brEnv.IsInLane())
			{
				height = SetToBounds(height + GetTrackEnvHeight(env, NULL, false), GetTcpEnvMinHeight(), GetCurrentTcpMaxHeight());
				brEnv.SetLaneHeight(height);
				brEnv.Commit();
			}
			else
			{
				int trackGapTop, trackGapBottom, mul, currentHeight;
				currentHeight = GetTrackHeight(brEnv.GetParent(), NULL, &trackGapTop, &trackGapBottom);
				mul = EnvelopesExtendedZoom ? (IsTrackLaneEnvelopeInMultipleLanes(env) ? CountVisibleTrackEnvelopesInTrackLane(brEnv.GetParent()) : 1) : 1;
				height = SetToBounds(height * mul + currentHeight, SNM_GetIconTheme()->tcp_small_height, GetCurrentTcpMaxHeight() * mul + (EnvelopesExtendedZoom ? trackGapTop + trackGapBottom : 0));
				GetSetMediaTrackInfo(brEnv.GetParent(), "I_HEIGHTOVERRIDE", &height);
				PreventUIRefresh(1);
				Main_OnCommand(41327, 0);
				Main_OnCommand(41328, 0);
				PreventUIRefresh(-1);
			}
			ScrollToTrackEnvIfNotInArrange(env);
		}
	}
}

void SetVerticalZoomSelectedEnvelope(COMMAND_T* ct)
{
	if (TrackEnvelope* env = GetSelectedTrackEnvelope(NULL))
	{
		BR_Envelope brEnv(env);
		if (brEnv.IsInLane())
		{
			brEnv.SetLaneHeight(((int)ct->user == 0) ? 0 : ((int)ct->user == 1) ? GetTcpEnvMinHeight() : GetCurrentTcpMaxHeight());
			brEnv.Commit();
		}
		else
		{
			int trackGapTop, trackGapBottom, mul, height;
			GetTrackHeight(brEnv.GetParent(), NULL, &trackGapTop, &trackGapBottom);
			mul = EnvelopesExtendedZoom ? (IsTrackLaneEnvelopeInMultipleLanes(env) ? CountVisibleTrackEnvelopesInTrackLane(brEnv.GetParent()) : 1) : 1;
			height = ((int)ct->user == 0) ? 0 : (((int)ct->user == 1) ? 0 : GetCurrentTcpMaxHeight()  * mul + (EnvelopesExtendedZoom ? trackGapTop + trackGapBottom : 0));
			GetSetMediaTrackInfo(brEnv.GetParent(), "I_HEIGHTOVERRIDE", &height);
			PreventUIRefresh(1);
			Main_OnCommand(41327, 0);
			Main_OnCommand(41328, 0);
			PreventUIRefresh(-1);
		}
		ScrollToTrackEnvIfNotInArrange(env);
	}
}

void ToggleEnableEnvelopesExtendedZoom(COMMAND_T*)
{
	EnvelopesExtendedZoom = !EnvelopesExtendedZoom;
	char str[32];
	sprintf(str, "%d", EnvelopesExtendedZoom);
	WritePrivateProfileString(SWS_INI, "WOLExtZoomEnvInTrLane", str, get_ini_file());
}

int IsEnvelopesExtendedZoomEnabled(COMMAND_T*)
{
	return EnvelopesExtendedZoom;
}

void ToggleEnableEnvelopeOverlap(COMMAND_T*)
{
	SetConfig("env_ol_minh", -((*(int*)GetConfigVar("env_ol_minh")) + 1));
	TrackList_AdjustWindows(false);
	UpdateTimeline();
}

int IsEnvelopeOverlapEnabled(COMMAND_T*)
{
	return (*(int*)GetConfigVar("env_ol_minh") >= 0);
}

void ForceEnvelopeOverlap(COMMAND_T* ct)
{
	if ((int)ct->user == 0)
	{
		if (TrackEnvelope* env = GetSelectedTrackEnvelope(NULL))
		{
			SavedEnvelopeOverlapSettings = *(int*)GetConfigVar("env_ol_minh");
			BR_Envelope envelope(env);
			int envCount = CountVisibleTrackEnvelopesInTrackLane(envelope.GetParent());
			if (envCount && !envelope.IsInLane())
			{
				int val = (int)(GetTrackHeight(envelope.GetParent(), NULL) / envCount);
				SetConfig("env_ol_minh", val);
				TrackList_AdjustWindows(false);
				UpdateTimeline();
			}
		}
	}
	else
	{
		SetConfig("env_ol_minh", SavedEnvelopeOverlapSettings);
		TrackList_AdjustWindows(false);
		UpdateTimeline();
	}
	RefreshToolbar(SWSGetCommandID(ToggleEnableEnvelopeOverlap));
}

void SetVerticalZoomCenter(COMMAND_T* ct)
{
	SetConfig("vzoommode", (int)ct->user);
}

void SetHorizontalZoomCenter(COMMAND_T* ct)
{
	SetConfig("zoommode", (int)ct->user);
}

void wol_ZoomInit()
{
	EnvelopesExtendedZoom = GetPrivateProfileInt(SWS_INI, "WOLExtZoomEnvInTrLane", 0, get_ini_file()) ? true : false;
	SavedEnvelopeOverlapSettings = *(int*)GetConfigVar("env_ol_minh");
}
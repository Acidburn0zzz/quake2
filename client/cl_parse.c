/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// cl_parse.c  -- parse a message received from the server

#include "client.h"

char *svc_strings[256] =
{
	"svc_bad",

	"svc_muzzleflash",
	"svc_muzzlflash2",
	"svc_temp_entity",
	"svc_layout",
	"svc_inventory",

	"svc_nop",
	"svc_disconnect",
	"svc_reconnect",
	"svc_sound",
	"svc_print",
	"svc_stufftext",
	"svc_serverdata",
	"svc_configstring",
	"svc_spawnbaseline",	
	"svc_centerprint",
	"svc_download",
	"svc_playerinfo",
	"svc_packetentities",
	"svc_deltapacketentities",
	"svc_frame"
};

typedef struct dlqueue_s
{
	struct dlqueue_s	*next;
	struct dlqueue_s	*prev;
	char				filename[MAX_QPATH];
} dlqueue_t;

dlqueue_t downloadqueue;

void CL_AddToDownloadQueue (char *path)
{
	dlqueue_t *dlq = &downloadqueue;

	if (!Cvar_VariableValue ("allow_download"))
		return;

	while (dlq->next) {
		dlq  = dlq->next;

		if (!Q_stricmp (path, dlq->filename))
			return;
	}

	dlq->next = Z_TagMalloc (sizeof(dlqueue_t), TAGMALLOC_CLIENT_DOWNLOAD);	
	dlq->next->prev = dlq;
	dlq = dlq->next;
	dlq->next = NULL;
	strncpy (dlq->filename, path, sizeof(dlq->filename)-1);

	Com_Printf ("DLQ: Added %s\n", dlq->filename);
}

void CL_RemoveFromDownloadQueue (char *path)
{
	dlqueue_t *dlq = &downloadqueue;

	while (dlq->next) {
		dlq  = dlq->next;

		if (!Q_stricmp (path, dlq->filename)) {
			if (dlq->next)
				dlq->next->prev = dlq->prev;
			dlq->prev->next = dlq->next;

			Z_Free (dlq);
			return;
		}
	}
}

void CL_FlushDownloadQueue (void)
{
	dlqueue_t *old = NULL, *dlq = &downloadqueue;

	while (dlq->next) {
		dlq  = dlq->next;

		if (old)
			Z_Free (old);

		old = dlq;
	}

	if (old)
		Z_Free (old);

	downloadqueue.next = NULL;
}

void CL_RunDownloadQueue (void)
{
	dlqueue_t *old, *dlq = &downloadqueue;

	if (cls.download || cls.downloadpending || cls.state < ca_active)
		return;

	while (dlq->next) {
		dlq  = dlq->next;

		if (CL_CheckOrDownloadFile (dlq->filename)) {
			Com_Printf ("DLQ: Removed %s\n", dlq->filename);
			dlq->prev->next = dlq->next;
			if (dlq->next)
				dlq->next->prev = dlq->prev;

			old = dlq;
			dlq = dlq->prev;
			Z_Free (old);
		} else {
			Com_Printf ("DLQ: Started %s\n", dlq->filename);
			return;
		}
	}
}

//=============================================================================

void CL_DownloadFileName(char *dest, int destlen, char *fn)
{
	//if (strncmp(fn, "players", 7) == 0)
	//	Com_sprintf (dest, destlen, "%s/%s", BASEDIRNAME, fn);
	//else
	Com_sprintf (dest, destlen, "%s/%s", FS_Gamedir(), fn);
}

void CL_FinishDownload (void)
{
	clientinfo_t *ci;

	int r;
	char	oldn[MAX_OSPATH];
	char	newn[MAX_OSPATH];

	fclose (cls.download);

	FS_FlushCache();

	// rename the temp file to it's final name
	CL_DownloadFileName(oldn, sizeof(oldn), cls.downloadtempname);
	CL_DownloadFileName(newn, sizeof(newn), cls.downloadname);

	r = rename (oldn, newn);
	if (r)
		Com_Printf ("failed to rename.\n");

	if (cls.serverProtocol == ENHANCED_PROTOCOL_VERSION && (strstr(newn, ".pcx") || strstr(newn, ".md2"))) {
		for (r = 0; r < MAX_CLIENTS; r++) {
			ci = &cl.clientinfo[r];
			if (ci->deferred)
				CL_ParseClientinfo (r);
		}
	}

	cls.downloadpending = false;
	cls.download = NULL;
	cls.downloadpercent = 0;
}
/*
unsigned int __stdcall ClientDownloadThread (void)
{
	int expected = 0, buffPos = 0;
	byte *zbuffer;
	byte buffRecv[1024];
	byte buffer[262144];
	int socket, ret, len;

	memset (buffer, 0, sizeof(buffer));

	socket = NET_Connect (&cls.netchan.remote_address, cls.dlserverport);
	if (socket == -1) {
		Com_Printf ("ClientDownloadThread: couldn't connect to download server on %s.\n", NET_AdrToString (cls.netchan.remote_address));
		fclose (cls.download);
		cls.download = NULL;
		_endthread ();
	}

	for (;;) {
		ret = NET_Select (socket, 15000);
		if (ret < 1) {
			Com_Printf ("ClientDownloadThread: NET_Select connection error.\n");
			NET_CloseSocket (socket);

			//could be caused by disconnect
			if (cls.download)
				fclose (cls.download);
			cls.download = NULL;
			_endthread ();
		}

		expected = 0;
		for (;;) {
			if (!buffPos || buffPos < expected) {
				ret = NET_RecvTCP (socket, buffRecv, sizeof(buffRecv));

				if (ret == -1) {
					Com_Printf ("ClientDownloadThread: NET_RecvTCP connection error.\n");
					NET_CloseSocket (socket);
					fclose (cls.download);
					cls.download = NULL;
					_endthread ();
				} else if (ret == 0) {
					NET_CloseSocket (socket);
					CL_FinishDownload ();
					_endthread ();
				}

				memcpy (buffer + buffPos, buffRecv, ret);
				buffPos += ret;
			}

			if (!expected) {
				expected = *(int *)buffer;
			}

			if (buffPos >= expected)
				break;
		}

		len = *(int *)(buffer + sizeof(int));
		if (len) {
			zbuffer = Z_TagMalloc (len, TAGMALLOC_CLIENT_DOWNLOAD);
			ret = ZLibDecompress (buffer+(sizeof(int)*2), expected-(sizeof(int)*2), zbuffer, len, -15);
			fwrite (zbuffer, ret, 1, cls.download);
			Z_Free (zbuffer);
		}

		cls.downloadpercent = ((float)ftell(cls.download)/(float)cls.downloadsize) * 100.0;

		memmove (buffer, buffer + expected, sizeof(buffer)-expected);

		buffPos -= expected;
	}

	//FIXME: should we ever get here?
	//NET_CloseSocket (socket);
}
*/

/*
===============
CL_CheckOrDownloadFile

Returns true if the file exists, otherwise it attempts
to start a download from the server.
===============
*/
qboolean	CL_CheckOrDownloadFile (char *filename)
{
	FILE *fp;
	char	*p;
	char	name[MAX_OSPATH];

	if (strstr (filename, ".."))
	{
		Com_Printf ("Refusing to download a path with .. (%s)\n", filename);
		return true;
	}

	if (FS_LoadFile (filename, NULL) != -1)
	{	// it exists, no need to download
		return true;
	}

	strncpy (cls.downloadname, filename, sizeof(cls.downloadname)-1);

	//r1: fix \ to /
	while ((p = strstr(cls.downloadname, "\\")))
		*p = '/';

	// download to a temp name, and only rename
	// to the real name when done, so if interrupted
	// a runt file wont be left
	COM_StripExtension (cls.downloadname, cls.downloadtempname);
	strcat (cls.downloadtempname, ".tmp");

//ZOID
	// check to see if we already have a tmp for this file, if so, try to resume
	// open the file if not opened yet
	CL_DownloadFileName(name, sizeof(name), cls.downloadtempname);

//	FS_CreatePath (name);

	fp = fopen (name, "r+b");
	if (fp) { // it exists
		int len;
		
		fseek(fp, 0, SEEK_END);
		len = ftell(fp);

		cls.download = fp;

		// give the server an offset to start the download
		Com_Printf ("Resuming %s\n", cls.downloadname);
		MSG_WriteByte (&cls.netchan.message, clc_stringcmd);
		if (cls.serverProtocol == ENHANCED_PROTOCOL_VERSION && cls.dlserverport) {
			MSG_WriteString (&cls.netchan.message, va("download %s %i DOWNLOAD_TCP", cls.downloadname, len));
		} else {
			MSG_WriteString (&cls.netchan.message, va("download %s %i", cls.downloadname, len));
		}
	} else {
		Com_Printf ("Downloading %s\n", cls.downloadname);
		MSG_WriteByte (&cls.netchan.message, clc_stringcmd);
		if (cls.serverProtocol == ENHANCED_PROTOCOL_VERSION && cls.dlserverport) {
			MSG_WriteString (&cls.netchan.message, va("download %s 0 DOWNLOAD_TCP", cls.downloadname));
		} else {
			MSG_WriteString (&cls.netchan.message, va("download %s", cls.downloadname));
		}
	}

	cls.downloadpending = true;

	return false;
}

/*
===============
CL_Download_f

Request a download from the server
===============
*/
void CL_Download_f (void)
{
	char	name[MAX_OSPATH];
	FILE	*fp;
	char	*p;
	char	filename[MAX_OSPATH];

	if (Cmd_Argc() != 2) {
		Com_Printf("Usage: download <filename>\n");
		return;
	}

	Com_sprintf(filename, sizeof(filename), "%s", Cmd_Argv(1));

	if (strstr (filename, ".."))
	{
		Com_Printf ("Refusing to download a path with .. (%s)\n", filename);
		return;
	}

	if (cls.state <= ca_connecting) {
		Com_Printf ("Not connected.\n");
		return;
	}

	if (FS_LoadFile (filename, NULL) != -1)
	{	// it exists, no need to download
		Com_Printf("File already exists.\n");
		return;
	}

	strncpy (cls.downloadname, filename, sizeof(cls.downloadname)-1);

	//r1: fix \ to /
	while ((p = strstr(cls.downloadname, "\\")))
		*p = '/';

	// download to a temp name, and only rename
	// to the real name when done, so if interrupted
	// a runt file wont be left
	COM_StripExtension (cls.downloadname, cls.downloadtempname);
	strcat (cls.downloadtempname, ".tmp");

//ZOID
	// check to see if we already have a tmp for this file, if so, try to resume
	// open the file if not opened yet
	CL_DownloadFileName(name, sizeof(name), cls.downloadtempname);

	fp = fopen (name, "r+b");
	if (fp) { // it exists
		int len;
		
		fseek(fp, 0, SEEK_END);
		len = ftell(fp);

		cls.download = fp;

		// give the server an offset to start the download
		Com_Printf ("Resuming %s\n", cls.downloadname);
		MSG_WriteByte (&cls.netchan.message, clc_stringcmd);
		if (cls.serverProtocol == ENHANCED_PROTOCOL_VERSION && cls.dlserverport) {
			MSG_WriteString (&cls.netchan.message, va("download %s %i DOWNLOAD_TCP", cls.downloadname, len));
		} else {
			MSG_WriteString (&cls.netchan.message, va("download %s %i", cls.downloadname, len));
		}
	} else {
		Com_Printf ("Downloading %s\n", cls.downloadname);
	
		MSG_WriteByte (&cls.netchan.message, clc_stringcmd);
		if (cls.serverProtocol == ENHANCED_PROTOCOL_VERSION) {
			MSG_WriteString (&cls.netchan.message, va("download %s 0 DOWNLOAD_TCP", cls.downloadname));
		} else {
			MSG_WriteString (&cls.netchan.message, va("download %s 0", cls.downloadname));
		}
	}

	//cls.downloadnumber++;
}

void CL_Passive_f (void)
{
	if (cls.state != ca_disconnected) {
		Com_Printf ("Passive mode can only be modified when you are disconnected.\n");
	} else {
		cls.passivemode = !cls.passivemode;

		if (cls.passivemode) {
			NET_Config (NET_CLIENT);
			Com_Printf ("Listening for passive connections on port %d\n", (int)Cvar_VariableValue ("ip_clientport"));
		} else {
			Com_Printf ("No longer listening for passive connections.\n");
		}
	}
}

/*
======================
CL_RegisterSounds
======================
*/
void CL_RegisterSounds (void)
{
	int		i;

	S_BeginRegistration ();
	CL_RegisterTEntSounds ();
	for (i=1 ; i<MAX_SOUNDS ; i++)
	{
		if (!cl.configstrings[CS_SOUNDS+i][0])
			break;
		cl.sound_precache[i] = S_RegisterSound (cl.configstrings[CS_SOUNDS+i]);
		Sys_SendKeyEvents ();	// pump message loop
	}
	S_EndRegistration ();
}

/*
=====================
CL_ParseDownload

A download message has been received from the server
=====================
*/

void CL_ParseDownload (void)
{
	int		size, percent;
	char	name[MAX_OSPATH];

	// read the data
	size = MSG_ReadShort (&net_message);
	percent = MSG_ReadByte (&net_message);

	if (size == -1)
	{
		Com_Printf ("Server does not have this file.\n");

		//r1: nuke the temp filename
		*cls.downloadtempname = 0;

		if (cls.download)
		{
			// if here, we tried to resume a file but the server said no
			fclose (cls.download);
			cls.download = NULL;
		}
		CL_RemoveFromDownloadQueue (cls.downloadname);
		cls.downloadpending = false;
		CL_RequestNextDownload ();
		return;
	}
		
	/*if (cls.dlserverport) {
		if (percent == 0xFF) {
			unsigned int threadID;
			cls.downloadsize = (size+1) * 1024;
			// open the file if not opened yet
			if (!cls.download)
			{
				CL_DownloadFileName(name, sizeof(name), cls.downloadtempname);

				FS_CreatePath (name);

				cls.download = fopen (name, "wb");
				if (!cls.download)
				{
					Com_Printf ("Failed to open %s\n", cls.downloadtempname);
					cls.downloadpending = false;
					CL_RequestNextDownload ();
					return;
				}
			}
			_beginthreadex (NULL, 0, (unsigned int (__stdcall *)(void *))ClientDownloadThread, NULL, 0, &threadID);
		} else if (percent == 100) {
			while (cls.download)
				Sys_Sleep (10);
			CL_RequestNextDownload ();
		}
		return;
	}*/

	// open the file if not opened yet
	if (!cls.download)
	{
		if (!*cls.downloadtempname)
		{
			Com_DPrintf ("Received download packet without request. Ignored.\n");
			return;
		}
		CL_DownloadFileName(name, sizeof(name), cls.downloadtempname);

		FS_CreatePath (name);

		cls.download = fopen (name, "wb");
		if (!cls.download)
		{
			if (!cls.dlserverport)
				net_message.readcount += size;
			Com_Printf ("Failed to open %s\n", cls.downloadtempname);
			cls.downloadpending = false;
			CL_RequestNextDownload ();
			return;
		}
	}

	if (cls.serverProtocol != ENHANCED_PROTOCOL_VERSION || !cls.dlserverport) {
		fwrite (net_message.data + net_message.readcount, 1, size, cls.download);
	}

	if (!cls.dlserverport) {
		net_message.readcount += size;
	}

	if (percent != 100)
	{
		cls.downloadpercent = percent;

		//r1: enhanced server only sends download messages as status updates.
		if (!cls.dlserverport) {
			MSG_WriteByte (&cls.netchan.message, clc_stringcmd);
			SZ_Print (&cls.netchan.message, "nextdl");
		}
	}
	else
	{
		CL_FinishDownload ();

		// get another file if needed
		CL_RequestNextDownload ();
	}
}


/*
=====================================================================

  SERVER CONNECTING MESSAGES

=====================================================================
*/

/*
==================
CL_ParseServerData
==================
*/
void CL_ParseServerData (void)
{
	extern cvar_t	*fs_gamedirvar;
	char	*str;
	int		i;
	
//
// wipe the client_state_t struct
//
	CL_ClearState ();
	cls.state = ca_connected;

// parse protocol version number
	i = MSG_ReadLong (&net_message);
	cls.serverProtocol = i;

	if (i != ORIGINAL_PROTOCOL_VERSION && i != ENHANCED_PROTOCOL_VERSION && i != 26)
		Com_Error (ERR_DROP,"You are running protocol version %i, server is running %i. These are incompatible, please update your client to protocol version %d", ENHANCED_PROTOCOL_VERSION, i, i);

	cl.servercount = MSG_ReadLong (&net_message);
	cl.attractloop = MSG_ReadByte (&net_message);

	// game directory
	str = MSG_ReadString (&net_message);
	strncpy (cl.gamedir, str, sizeof(cl.gamedir)-1);

	// set gamedir
	if ((*str && (!fs_gamedirvar->string || !*fs_gamedirvar->string || strcmp(fs_gamedirvar->string, str))) || (!*str && (fs_gamedirvar->string || *fs_gamedirvar->string)))
	{
		Cvar_Set("game", str);
		Cvar_ForceSet ("$game", str);
	}

	// parse player entity number
	cl.playernum = MSG_ReadShort (&net_message);

	// get the full level name
	str = MSG_ReadString (&net_message);

	// read in download server port (if any)
	if (cls.serverProtocol == ENHANCED_PROTOCOL_VERSION && !cl.attractloop)
		cls.dlserverport = MSG_ReadShort (&net_message);
	else
		cls.dlserverport = 0;

	Com_DPrintf ("Serverdata packet received. protocol=%d, servercount=%d, attractloop=%d, clnum=%d, game=%s, map=%s, dlserver=%d\n", cls.serverProtocol, cl.servercount, cl.attractloop, cl.playernum, cl.gamedir, str, cls.dlserverport);

#ifdef CINEMATICS
	if (cl.playernum == -1)
	{	// playing a cinematic or showing a pic, not a level
		SCR_PlayCinematic (str);
	}
	else
	{
#endif
		// seperate the printfs so the server message can have a color
		Com_Printf("\n\n\35\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\37\n\n");
		Com_Printf ("%c%s\n", 2, str);

		// need to prep refresh at next oportunity
		cl.refresh_prepped = false;
#ifdef CINEMATICS
	}
#endif
}
/*
==================
CL_ParseBaseline
==================
*/
void CL_ParseBaseline (void)
{
	entity_state_t	*es;
	unsigned int	bits;
	int				newnum;
	entity_state_t	nullstate;

	memset (&nullstate, 0, sizeof(nullstate));

	newnum = CL_ParseEntityBits (&bits);
	es = &cl_entities[newnum].baseline;
	CL_ParseDelta (&nullstate, es, newnum, bits);
}

void CL_ParseZPacket (void)
{
	byte *buff_in;
	byte *buff_out;

	sizebuf_t sb, old;

	short compressed_len = MSG_ReadShort (&net_message);
	short uncompressed_len = MSG_ReadShort (&net_message);
	
	if (uncompressed_len <= 0)
		Com_Error (ERR_DROP, "CL_ParseZPacket: uncompressed_len <= 0");

	if (compressed_len <= 0)
		Com_Error (ERR_DROP, "CL_ParseZPacket: compressed_len <= 0");

	buff_in = Z_Malloc (compressed_len);
	buff_out = Z_Malloc (uncompressed_len);

	MSG_ReadData (&net_message, buff_in, compressed_len);

	SZ_Init (&sb, buff_out, uncompressed_len);
	sb.cursize = ZLibDecompress (buff_in, compressed_len, buff_out, uncompressed_len, -15);

	old = net_message;
	net_message = sb;

	/*for (;;)
	{
		cmd = MSG_ReadByte (&net_message);

		if (cmd == -1)
			break;

		switch (cmd) {
			case svc_configstring:
				CL_ParseConfigString ();
				break;
			case svc_spawnbaseline:
				CL_ParseBaseline ();
				break;
			default:
				Com_Error (ERR_DROP, "CL_ParseZPacket: unhandled command 0x%x!", cmd);
				break;
		}
	}*/

	CL_ParseServerMessage ();

	net_message = old;

	Z_Free (buff_in);
	Z_Free (buff_out);

	Com_DPrintf ("Got a ZPacket, %d->%d\n", uncompressed_len + 4, compressed_len);
}


/*
================
CL_LoadClientinfo

================
*/
void CL_LoadClientinfo (clientinfo_t *ci, char *s)
{
	int i;
	char		*t;
	char		original_model_name[MAX_QPATH];
	char		original_skin_name[MAX_QPATH];

	char		model_name[MAX_QPATH];
	char		skin_name[MAX_QPATH];
	char		model_filename[MAX_QPATH];
	char		skin_filename[MAX_QPATH];
	char		weapon_filename[MAX_QPATH];

	strncpy(ci->cinfo, s, sizeof(ci->cinfo));
	ci->cinfo[sizeof(ci->cinfo)-1] = 0;

	ci->deferred = false;

	// isolate the player's name
	strncpy(ci->name, s, sizeof(ci->name));
	ci->name[sizeof(ci->name)-1] = 0;
	t = strstr (s, "\\");
	if (t)
	{
		ci->name[t-s] = 0;
		s = t+1;
	}

	if (*s == 0)
	{
		Com_sprintf (model_filename, sizeof(model_filename), "players/male/tris.md2");
		Com_sprintf (weapon_filename, sizeof(weapon_filename), "players/male/weapon.md2");
		Com_sprintf (skin_filename, sizeof(skin_filename), "players/male/grunt.pcx");
		Com_sprintf (ci->iconname, sizeof(ci->iconname), "/players/male/grunt_i.pcx");
		ci->model = re.RegisterModel (model_filename);
		memset(ci->weaponmodel, 0, sizeof(ci->weaponmodel));
		ci->weaponmodel[0] = re.RegisterModel (weapon_filename);
		ci->skin = re.RegisterSkin (skin_filename);
		ci->icon = re.RegisterPic (ci->iconname);
	}
	else
	{
		// isolate the model name
		strcpy (model_name, s);

		t = strstr(model_name, "/");
		if (!t)
			t = strstr(model_name, "\\");
		if (!t)
			t = model_name;
		*t = 0;

		strcpy (original_model_name, model_name);

		// isolate the skin name
		strcpy (skin_name, s + strlen(model_name) + 1);
		strcpy (original_skin_name, s + strlen(model_name) + 1);

		// model file
		Com_sprintf (model_filename, sizeof(model_filename), "players/%s/tris.md2", model_name);
		ci->model = re.RegisterModel (model_filename);
		if (!ci->model)
		{
			ci->deferred = true;
			//if (!CL_CheckOrDownloadFile (model_filename))
			//	return;
			CL_AddToDownloadQueue (model_filename);
			strcpy(model_name, "male");
			//Com_sprintf (model_filename, sizeof(model_filename), "players/male/tris.md2");
			strcpy (model_filename, "players/male/tris.md2");
			ci->model = re.RegisterModel (model_filename);
		}

		// skin file
		Com_sprintf (skin_filename, sizeof(skin_filename), "players/%s/%s.pcx", model_name, skin_name);
		ci->skin = re.RegisterSkin (skin_filename);

		if (!ci->skin)
		{
			Com_sprintf (skin_filename, sizeof(skin_filename), "players/%s/%s.pcx", original_model_name, original_skin_name);
			ci->deferred = true;
			//CL_CheckOrDownloadFile (skin_filename);
			CL_AddToDownloadQueue (skin_filename);
		}

		// if we don't have the skin and the model wasn't male,
		// see if the male has it (this is for CTF's skins)
 		if (!ci->skin && Q_stricmp(model_name, "male"))
		{
			// change model to male
			strcpy(model_name, "male");
			strcpy (model_filename, "players/male/tris.md2");
			ci->model = re.RegisterModel (model_filename);

			// see if the skin exists for the male model
			Com_sprintf (skin_filename, sizeof(skin_filename), "players/%s/%s.pcx", model_name, skin_name);
			ci->skin = re.RegisterSkin (skin_filename);
		}

		// if we still don't have a skin, it means that the male model didn't have
		// it, so default to grunt
		if (!ci->skin) {
			// see if the skin exists for the male model
			Com_sprintf (skin_filename, sizeof(skin_filename), "players/%s/grunt.pcx", model_name);
			ci->skin = re.RegisterSkin (skin_filename);
		}

		// weapon file
		for (i = 0; i < num_cl_weaponmodels; i++) {
			Com_sprintf (weapon_filename, sizeof(weapon_filename), "players/%s/%s", model_name, cl_weaponmodels[i]);
			ci->weaponmodel[i] = re.RegisterModel(weapon_filename);
			if (!ci->weaponmodel[i]) {
				Com_sprintf (skin_filename, sizeof(skin_filename), "players/%s/%s.pcx", original_model_name, cl_weaponmodels[i]);
				ci->deferred = true;
				CL_AddToDownloadQueue (weapon_filename);
			}
			if (!ci->weaponmodel[i] && strcmp(model_name, "cyborg") == 0) {
				// try male
				Com_sprintf (weapon_filename, sizeof(weapon_filename), "players/male/%s", cl_weaponmodels[i]);
				ci->weaponmodel[i] = re.RegisterModel(weapon_filename);
			}
			if (!cl_vwep->value)
				break; // only one when vwep is off
		}

		// icon file
		Com_sprintf (ci->iconname, sizeof(ci->iconname), "/players/%s/%s_i.pcx", model_name, skin_name);
		ci->icon = re.RegisterPic (ci->iconname);

		if (!ci->icon) {
			Com_sprintf (ci->iconname, sizeof(ci->iconname), "players/%s/%s_i.pcx", original_model_name, original_skin_name);
			ci->deferred = true;
			CL_AddToDownloadQueue (ci->iconname);
			//ci->icon = re.RegisterPic ("/players/male/grunt_i.pcx");
		}
	}

	// must have loaded all data types to be valud
	if (!ci->skin || !ci->icon || !ci->model || !ci->weaponmodel[0])
	{
		ci->skin = NULL;
		ci->icon = NULL;
		ci->model = NULL;
		ci->weaponmodel[0] = NULL;
		return;
	}
}

/*
================
CL_ParseClientinfo

Load the skin, icon, and model for a client
================
*/
void CL_ParseClientinfo (int player)
{
	char			*s;
	clientinfo_t	*ci;

	s = cl.configstrings[player+CS_PLAYERSKINS];

	ci = &cl.clientinfo[player];

	CL_LoadClientinfo (ci, s);
}


/*
================
CL_ParseConfigString
================
*/
void CL_ParseConfigString (void)
{
	int		i;
	char	*s;
	char	olds[MAX_QPATH];

	i = MSG_ReadShort (&net_message);
	if (i < 0 || i >= MAX_CONFIGSTRINGS)
		Com_Error (ERR_DROP, "CL_ParseConfigString: configstring > MAX_CONFIGSTRINGS");
	s = MSG_ReadString(&net_message);

	strncpy (olds, cl.configstrings[i], sizeof(olds));
	olds[sizeof(olds) - 1] = 0;

	strcpy (cl.configstrings[i], s);

	//Com_Printf ("configstring %i: %s\n", i, s);

	// do something apropriate 

	if (i >= CS_LIGHTS && i < CS_LIGHTS+MAX_LIGHTSTYLES)
		CL_SetLightstyle (i - CS_LIGHTS);
#ifdef CD_AUDIO
	else if (i == CS_CDTRACK)
	{
		if (cl.refresh_prepped)
			CDAudio_Play (atoi(cl.configstrings[CS_CDTRACK]), true);
	}
#endif
	else if (i >= CS_MODELS && i < CS_MODELS+MAX_MODELS)
	{
		if (cl.refresh_prepped)
		{
			cl.model_draw[i-CS_MODELS] = re.RegisterModel (cl.configstrings[i]);
			if (cl.configstrings[i][0] == '*')
				cl.model_clip[i-CS_MODELS] = CM_InlineModel (cl.configstrings[i]);
			else
				cl.model_clip[i-CS_MODELS] = NULL;
		}
	}
	else if (i >= CS_SOUNDS && i < CS_SOUNDS+MAX_MODELS)
	{
		if (cl.refresh_prepped)
			cl.sound_precache[i-CS_SOUNDS] = S_RegisterSound (cl.configstrings[i]);
	}
	else if (i >= CS_IMAGES && i < CS_IMAGES+MAX_MODELS)
	{
		if (cl.refresh_prepped)
			cl.image_precache[i-CS_IMAGES] = re.RegisterPic (cl.configstrings[i]);
	}
	else if (i >= CS_PLAYERSKINS && i < CS_PLAYERSKINS+MAX_CLIENTS)
	{
		if (cl.refresh_prepped && strcmp(olds, s))
			CL_ParseClientinfo (i-CS_PLAYERSKINS);
	}
}

/*
=====================================================================

ACTION MESSAGES

=====================================================================
*/

/*
==================
CL_ParseStartSoundPacket
==================
*/
void CL_ParseStartSoundPacket(void)
{
    vec3_t  pos_v;
	float	*pos;
    int 	channel, ent;
    int 	sound_num;
    float 	volume;
    float 	attenuation;  
	int		flags;
	float	ofs;

	flags = MSG_ReadByte (&net_message);
	sound_num = MSG_ReadByte (&net_message);

    if (flags & SND_VOLUME)
		volume = MSG_ReadByte (&net_message) / 255.0;
	else
		volume = DEFAULT_SOUND_PACKET_VOLUME;
	
    if (flags & SND_ATTENUATION)
		attenuation = MSG_ReadByte (&net_message) / 64.0;
	else
		attenuation = DEFAULT_SOUND_PACKET_ATTENUATION;	

    if (flags & SND_OFFSET)
		ofs = MSG_ReadByte (&net_message) / 1000.0;
	else
		ofs = 0;

	if (flags & SND_ENT)
	{	// entity reletive
		channel = MSG_ReadShort(&net_message); 
		ent = channel>>3;
		if (ent > MAX_EDICTS)
			Com_Error (ERR_DROP,"CL_ParseStartSoundPacket: ent = %i", ent);

		channel &= 7;
	}
	else
	{
		ent = 0;
		channel = 0;
	}

	if (flags & SND_POS)
	{	// positioned in space
		MSG_ReadPos (&net_message, pos_v);
 
		pos = pos_v;
	}
	else	// use entity number
		pos = NULL;

	if (!cl.sound_precache[sound_num])
		return;

	S_StartSound (pos, ent, channel, cl.sound_precache[sound_num], volume, attenuation, ofs);
}       


void SHOWNET(char *s)
{
	if (cl_shownet->value>=2)
		Com_Printf ("%3i:%s\n", net_message.readcount-1, s);
}

/*
=====================
CL_ParseServerMessage
=====================
*/
void CL_ParseServerMessage (void)
{
	int			cmd;
	char		*s;
	int			i;

//
// if recording demos, copy the message out
//
	if (cl_shownet->value == 1)
		Com_Printf ("%i ",net_message.cursize);
	else if (cl_shownet->value >= 2)
		Com_Printf ("------------------\n");


//
// parse the message
//
	for (;;)
	{
		if (net_message.readcount > net_message.cursize)
		{
			Com_Error (ERR_DROP,"CL_ParseServerMessage: Bad server message (%d>%d)", net_message.readcount, net_message.cursize);
			break;
		}

		cmd = MSG_ReadByte (&net_message);

		if (cmd == -1)
		{
			SHOWNET("END OF MESSAGE");
			break;
		}

		if (cl_shownet->value>=2)
		{
			if (!svc_strings[cmd])
				Com_Printf ("%3i:BAD CMD %i\n", net_message.readcount-1,cmd);
			else
				SHOWNET(svc_strings[cmd]);
		}
	
	// other commands
		switch (cmd)
		{
		default:
			if (developer->value)
				Com_Printf ("Unknown command char %d, ignoring!!\n", cmd);
			else
				Com_Error (ERR_DROP,"CL_ParseServerMessage: Illegible server message %d (0x%.2x)\n", cmd, cmd);
			break;
			
		case svc_nop:
//			Com_Printf ("svc_nop\n");
			break;
			
		case svc_disconnect:
			Com_Error (ERR_DISCONNECT,"Server disconnected\n");
			break;

		case svc_reconnect:
			Com_Printf ("Server disconnected, reconnecting\n");
			if (cls.download) {
				//ZOID, close download
				fclose (cls.download);
				cls.download = NULL;
			}
			cls.state = ca_connecting;
			cls.connect_time = -99999;	// CL_CheckForResend() will fire immediately
			break;

		case svc_print:
			i = MSG_ReadByte (&net_message);
			s = MSG_ReadString (&net_message);
			if (i == PRINT_CHAT)
			{
				S_StartLocalSound ("misc/talk.wav");
				if (cl_filterchat->value)
				{
					strcpy (s, StripHighBits(s, (int)cl_filterchat->value == 2));
					strcat (s, "\n");
				}
				con.ormask = 128;

				//r1: change !p_version to !version since p is for proxies
				if (strstr (s, ": !r1q2_version") ||
					strstr (s, ": !version") &&
					(cls.lastSpamTime == 0 || cls.realtime > cls.lastSpamTime + 300000))
					cls.spamTime = cls.realtime + random() * 1500; 
			}
			Com_Printf ("%s", s);
			con.ormask = 0;
			break;
			
		case svc_centerprint:
			SCR_CenterPrint (MSG_ReadString (&net_message));
			break;
			
		case svc_stufftext:
			s = MSG_ReadString (&net_message);
			Com_DPrintf ("stufftext: %s\n", s);
			Cbuf_AddText (s);

			//strcpy (s, StripHighBits (s, 2));
			//Com_Printf ("stuff: %s\n", s);
			break;
			
		case svc_serverdata:
			Cbuf_Execute ();		// make sure any stuffed commands are done
			CL_ParseServerData ();
			break;
			
		case svc_configstring:
			CL_ParseConfigString ();
			break;
			
		case svc_sound:
			CL_ParseStartSoundPacket();
			break;
			
		case svc_spawnbaseline:
			CL_ParseBaseline ();
			break;

		case svc_temp_entity:
			CL_ParseTEnt ();
			break;

		case svc_muzzleflash:
			CL_ParseMuzzleFlash ();
			break;

		case svc_muzzleflash2:
			CL_ParseMuzzleFlash2 ();
			break;

		case svc_download:
			CL_ParseDownload ();
			break;

		case svc_frame:
			CL_ParseFrame ();
			break;

		case svc_inventory:
			CL_ParseInventory ();
			break;

		case svc_layout:
			s = MSG_ReadString (&net_message);
			strncpy (cl.layout, s, sizeof(cl.layout)-1);
			break;

		//************** r1q2 specific BEGIN ****************
		case svc_zpacket:
			CL_ParseZPacket();
			break;
		//************** r1q2 specific END ******************

		case svc_playerinfo:
		case svc_packetentities:
		case svc_deltapacketentities:
			Com_Error (ERR_DROP, "Out of place frame data");
			break;
		}
	}
}
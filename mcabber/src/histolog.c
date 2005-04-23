/*
 * histolog.c     -- File history handling
 * 
 * Copyright (C) 2005 Mikael Berthe <bmikael@lists.lilotux.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
 * USA
 */

#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "histolog.h"
#include "screen.h"

static guint UseFileLogging;
static char *RootDir;


//  user_histo_file()
// Returns history filename for the given jid
// Note: the caller *must* free the filename after use (if not null).
char *user_histo_file(const char *jid)
{
  char *filename;
  if (!UseFileLogging)
    return NULL;

  filename = g_new(char, strlen(RootDir) + strlen(jid) + 1);
  strcpy(filename, RootDir);
  strcat(filename, jid);
  return filename;
}

//  write()
// Adds a history (multi-)line to the jid's history logfile
void write(const char *jid,
           time_t timestamp, guchar type, guchar info, char *data)
{
  guint len = 0;
  time_t ts;
  char *p;
  char *filename = user_histo_file(jid);

  if (!filename)
    return;

  // If timestamp is null, get current date
  if (timestamp)
    ts = timestamp;
  else
    time(&ts);

  if (!data)
    data = "";

  // Count number of extra lines
  for (p=data ; *p ; p++)
    if (*p == '\n') len++;

  /* Line format: "T I DDDDDDDDDD LLL [data]"
   * T=Type, I=Info, D=date, LLL=0-padded-len
   *
   * Types:
   * - M message    Info: S (send) R (receive)
   * - S status     Info: [oaifdcn]
   * We don't check them, we'll trust the caller.
   */

  scr_LogPrint("Log to [%s]:", filename);
  scr_LogPrint("%c %c %10d %03d %s", type, info, ts, len, data);
}

//  hlog_enable()
// Enable logging to files.  If root_dir is NULL, then $HOME/.mcabber is used.
void hlog_enable(guint enable, char *root_dir)
{
  UseFileLogging = enable;

  if (enable) {
    if (root_dir) {
      int l = strlen(root_dir);
      if (l < 1) {
        scr_LogPrint("root_dir too short");
        UseFileLogging = FALSE;
        return;
      }
      // RootDir must be slash-terminated
      if (root_dir[l-1] == '/')
        RootDir = g_strdup(root_dir);
      else {
        RootDir = g_new(char, l+2);
        strcpy(RootDir, root_dir);
        strcat(RootDir, "/");
      }
    } else {
      char *home = getenv("HOME");
      char *dir = "/.mcabber/";
      RootDir = g_new(char, strlen(home) + strlen(dir) + 1);
      strcpy(RootDir, home);
      strcat(RootDir, dir);
    }
    // FIXME
    // We should check the directory actually exists
  } else    // Disable history logging
    if (RootDir) {
    g_free(RootDir);
  }
}


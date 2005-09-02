/*
 * utils.c      -- Various utility functions
 *
 * Copyright (C) 2005 Mikael Berthe <bmikael@lists.lilotux.net>
 * ut_* functions are derived from Cabber debug/log code.
 * from_iso8601() comes from the Gaim project.
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <config.h>
#include "logprint.h"

static int DebugEnabled;
static char *FName;

void ut_InitDebug(unsigned int level, const char *filename)
{
  FILE *fp;

  if (level < 1) {
    DebugEnabled = 0;
    FName = NULL;
    return;
  }

  if (filename)
    FName = strdup(filename);
  else {
    FName = getenv("HOME");
    if (!FName)
      FName = "/tmp/mcabberlog";
    else {
      char *tmpname = malloc(strlen(FName) + 12);
      strcpy(tmpname, FName);
      strcat(tmpname, "/mcabberlog");
      FName = tmpname;
    }
  }

  DebugEnabled = level;

  fp = fopen(FName, "a");
  if (!fp) {
    fprintf(stderr, "ERROR: Cannot open tracelog file\n");
    return;
  }
  fprintf(fp, "New trace log started.\n"
	  "----------------------\n");
  fchmod(fileno(fp), S_IRUSR|S_IWUSR);
  fclose(fp);
}

void ut_WriteLog(unsigned int flag, const char *data)
{
  if (!DebugEnabled || !FName) return;

  if (((DebugEnabled == 2) && (flag & (LPRINT_LOG|LPRINT_DEBUG))) ||
      ((DebugEnabled == 1) && (flag & LPRINT_LOG))) {
    FILE *fp = fopen(FName, "a+");
    if (!fp) {
      scr_LogPrint(LPRINT_NORMAL, "ERROR: Cannot open tracelog file");
      return;
    }
    fputs(data, fp);
    fclose(fp);
  }
}

//  checkset_perm(name, setmode)
// Check the permissions of the "name" file/dir
// If setmode is true, correct the permissions if they are wrong
// Return values: -1 == bad file/dir, 0 == success, 1 == cannot correct
int checkset_perm(const char *name, unsigned int setmode)
{
  int fd;
  struct stat buf;

  fd = lstat(name, &buf);
  if (fd == -1) return -1;

  if (buf.st_uid != geteuid()) {
    scr_LogPrint(LPRINT_LOGNORM, "Wrong file owner [%s]", name);
    return 1;
  }

  if (buf.st_mode & (S_IRGRP | S_IWGRP | S_IXGRP) ||
      buf.st_mode & (S_IROTH | S_IWOTH | S_IXOTH)) {
    if (setmode) {
      mode_t newmode = 0;
      scr_LogPrint(LPRINT_LOGNORM, "Bad permissions [%s]", name);
      if (S_ISDIR(buf.st_mode))
        newmode |= S_IXUSR;
      newmode |= S_IRUSR | S_IWUSR;
      if (chmod(name, newmode)) {
        scr_LogPrint(LPRINT_LOGNORM, "WARNING: Failed to correct permissions!");
        return 1;
      }
      scr_LogPrint(LPRINT_LOGNORM, "Permissions have been corrected");
    } else {
      scr_LogPrint(LPRINT_LOGNORM, "WARNING: Bad permissions [%s]", name);
      return 1;
    }
  }

  return 0;
}

//  to_iso8601(dststr, timestamp)
// Convert timestamp to iso8601 format, and store it in dststr.
// NOTE: dststr should be at last 19 chars long.
// Return should be 0
int to_iso8601(char *dststr, time_t timestamp)
{
  struct tm *tm_time;
  int ret;

  tm_time = gmtime(&timestamp);

  ret = snprintf(dststr, 19, "%.4d%02d%02dT%02d:%02d:%02dZ",
        1900+tm_time->tm_year, tm_time->tm_mon+1, tm_time->tm_mday,
        tm_time->tm_hour, tm_time->tm_min, tm_time->tm_sec);

  return ((ret == -1) ? -1 : 0);
}

//  from_iso8601(timestamp, utc)
// This function comes from the Gaim project, gaim_str_to_time().
// (Actually date may not be pure iso-8601)
// Thanks, guys!
time_t from_iso8601(const char *timestamp, int utc)
{
  struct tm t;
  time_t retval = 0;
  char buf[32];
  char *c;
  int tzoff = 0;

  time(&retval);
  localtime_r(&retval, &t);

  snprintf(buf, sizeof(buf), "%s", timestamp);
  c = buf;

  /* 4 digit year */
  if (!sscanf(c, "%04d", &t.tm_year)) return 0;
  c+=4;
  if (*c == '-')
    c++;

  t.tm_year -= 1900;

  /* 2 digit month */
  if (!sscanf(c, "%02d", &t.tm_mon)) return 0;
  c+=2;
  if (*c == '-')
    c++;

  t.tm_mon -= 1;

  /* 2 digit day */
  if (!sscanf(c, "%02d", &t.tm_mday)) return 0;
  c+=2;
  if (*c == 'T' || *c == '.') { /* we have more than a date, keep going */
    c++; /* skip the "T" */

    /* 2 digit hour */
    if (sscanf(c, "%02d:%02d:%02d", &t.tm_hour, &t.tm_min, &t.tm_sec) == 3 ||
        sscanf(c, "%02d%02d%02d", &t.tm_hour, &t.tm_min, &t.tm_sec) == 3) {
      int tzhrs, tzmins;
      c+=8;
      if (*c == '.') /* dealing with precision we don't care about */
        c += 4;

      if ((*c == '+' || *c == '-') &&
          sscanf(c+1, "%02d:%02d", &tzhrs, &tzmins)) {
        tzoff = tzhrs*60*60 + tzmins*60;
        if (*c == '+')
          tzoff *= -1;
      }

      if (tzoff || utc) {

//#ifdef HAVE_TM_GMTOFF
        tzoff += t.tm_gmtoff;
//#else
//#   ifdef HAVE_TIMEZONE
//        tzset();    /* making sure */
//        tzoff -= timezone;
//#   endif
//#endif
      }
    }
  }

  t.tm_isdst = -1;

  retval = mktime(&t);

  retval += tzoff;

  return retval;
}

// Should only be used for delays < 1s
inline void safe_usleep(unsigned int usec)
{
  struct timespec req;
  req.tv_sec = 0;
  req.tv_nsec = (long)usec * 1000L;
  nanosleep(&req, NULL);
}

/* Copyright 1997 by the Massachusetts Institute of Technology.
 *
 * Permission to use, copy, modify, and distribute this
 * software and its documentation for any purpose and without
 * fee is hereby granted, provided that the above copyright
 * notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting
 * documentation, and that the name of M.I.T. not be used in
 * advertising or publicity pertaining to distribution of the
 * software without specific, written prior permission.
 * M.I.T. makes no representations about the suitability of
 * this software for any purpose.  It is provided "as is"
 * without express or implied warranty.
 */

/* This file is part of the Athena login library.  It implements the
 * function to check if a user is allowed to log in.
 */

static const char rcsid[] = "$Id: allowed.c,v 1.3 1997-10-30 23:58:53 ghudson Exp $";

#include <errno.h>
#include <hesiod.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "al.h"
#include "al_private.h"

/* The al_login_allowed() function determines whether a user is allowed
 * to log in.  The calling process provides an indication of whether the
 * login session is from a remote host.  The al_login_allowed() function
 * has no side-effects.
 * 
 * The al_login_allowed() function may return the following values:
 * 
 * 	AL_SUCCESS	The user may log in
 * 	AL_ENOUSER	Unknown user
 * 	AL_EBADHES	Illegal hesiod entry for user
 * 	AL_ENOLOGIN	Login denied because logins are disabled
 * 	AL_ENOREMOTE	Login denied because remote logins are disabled
 * 			for users not in the local passwd database
 * 	AL_ENOCREATE	Login denied because logins are disabled for
 * 			users not in the local passwd database
 * 	AL_ENOMEM	Ran out of memory
 * 
 * If al_login_allowed() returns AL_ENOLOGIN, AL_ENOREMOTE, or
 * AL_ENOCREATE and filetext is not NULL, then *filetext is set to a
 * malloc()'d string (which the caller must free) containing the text
 * of the file which caused the login to be denied.  Otherwise,
 * *filetext is set to NULL.
 */

int al_login_allowed(const char *username, int isremote,
		     char **filetext)
{
  struct passwd *local_pwd, *hes_pwd = NULL;
  int retval = AL_SUCCESS;
  char *retfname = NULL;
  FILE *retfile;
  void *hescontext = NULL;

  local_pwd = al__getpwnam(username);

  if (local_pwd)
    {
      if (local_pwd->pw_uid && !access(PATH_NOLOGIN, F_OK))
	{
	  retval = AL_ENOLOGIN;
	  retfname = PATH_NOLOGIN;
	  goto cleanup;
	}
    }
  else
    {
      if (!access(PATH_NOLOGIN, F_OK))
	{
	  retval = AL_ENOLOGIN;
	  retfname = PATH_NOLOGIN;
	  goto cleanup;
	}
      if (!access(PATH_NOCREATE, F_OK))
	{
	  retval = AL_ENOCREATE;
	  retfname = PATH_NOCREATE;
	  goto cleanup;
	}
      if (isremote && !access(PATH_NOREMOTE, F_OK))
	{
	  retval = AL_ENOREMOTE;
	  retfname = PATH_NOREMOTE;
	  goto cleanup;
	}

      errno = 0;
      if (hesiod_init(&hescontext) != 0)
	{
	  retval = (errno == ENOMEM) ? AL_ENOMEM : AL_ENOUSER;
	  goto cleanup;
	}
      hes_pwd = hesiod_getpwnam(hescontext, username);
      if (!hes_pwd)
	{
	  retval = AL_ENOUSER;
	  goto cleanup;
	}
      local_pwd = al__getpwuid(hes_pwd->pw_uid);
      if (local_pwd)
	{
	  retval = AL_EBADHES;
	  goto cleanup;
	}
    }

cleanup:
  if (local_pwd)
    al__free_passwd(local_pwd);
  if (hes_pwd)
    hesiod_free_passwd(hescontext, hes_pwd);
  if (hescontext)
    hesiod_end(hescontext);
  if (retfname && filetext)
    {
      *filetext = NULL;
      retfile = fopen(retfname, "r");
      if (retfile)
	{
	  struct stat st;

	  if (!fstat(fileno(retfile), &st))
	    {
	      *filetext = malloc(1 + st.st_size);
	      if (*filetext)
		{
		  /* Zero all in case fewer chars read than expected. */
		  memset(*filetext, 0, 1 + st.st_size);
		  fread(*filetext, sizeof(char), st.st_size, retfile);
		}
	    }
	  fclose(retfile);
	}
    }
  return retval;
}

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

/* This file is part of the Athena login library.  It implements
 * functions to add and remove a user from the system passwd database.
 */

static const char rcsid[] = "$Id: passwd.c,v 1.18 2003-10-03 18:36:35 ghudson Exp $";

#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <hesiod.h>
#ifdef HAVE_SHADOW
#include <shadow.h>
#endif
#include "al.h"
#include "al_private.h"

/* /etc/ptmp should be mode 600 on a master.passwd system, 644 otherwise. */
#ifdef HAVE_MASTER_PASSWD
#define PTMP_MODE (S_IWUSR|S_IRUSR)
#else
#define PTMP_MODE (S_IWUSR|S_IRUSR|S_IRGRP|S_IROTH)
#endif

/* To update the home directory field, we have to know which field it is. */
#ifdef HAVE_MASTER_PASSWD
#define HOMEDIR_FIELD 9
#else 
#define HOMEDIR_FIELD 6
#endif

#ifdef HAVE_LCKPWDF
static int safe_lckpwdf(void);
#endif

/* This is an internal function.  Its contract is to lock the passwd
 * database in a manner consistent with the operating system and return
 * the file handle of a temporary file (which may or may not also be
 * the lock file) into which to write the new contents of the passwd
 * file.
 */

static FILE *lock_passwd(void)
{
#ifdef HAVE_LCKPWDF
  FILE *fp;

  if (safe_lckpwdf() == -1)
    return NULL;
  fp = fopen(PATH_PASSWD_TMP, "w");
  if (fp)
    fchmod(fileno(fp), S_IWUSR|S_IRUSR|S_IRGRP|S_IROTH);
  else
    ulckpwdf();
  return fp;
#else
  int i, fd = -1;
  FILE *fp;

  for (i = 0; i < 10; i++)
    {
      fd = open(PATH_PASSWD_TMP, O_RDWR|O_CREAT|O_EXCL, PTMP_MODE);
      if (fd >= 0 || errno != EEXIST)
	break;
      sleep(1);
    }
  if (fd == -1)
    return NULL;
  fp = fdopen(fd, "w");
  if (fp == NULL)
    unlink(PATH_PASSWD_TMP);
  return fp;
#endif
}

/* This is an internal function.  Its contract is to replace the passwd
 * file with the temporary file.
 */

static int update_passwd(FILE *fp)
{
#ifdef HAVE_MASTER_PASSWD
  int pstat;
  pid_t pid, rpid;
#endif
  int status;

  fflush(fp);
  status = (fsync(fileno(fp)) == -1);
  status = ferror(fp) || status;
  if (fclose(fp) || status)
    {
      unlink(PATH_PASSWD_TMP);
#ifdef HAVE_LCKPWDF
      ulckpwdf();
#endif
    }

  /* Replace the passwd file with the lock file. */
#ifdef HAVE_MASTER_PASSWD
  pid = fork();
  if (pid == 0)
    {
      int fd;
      close(STDIN_FILENO);
      close(STDOUT_FILENO);
      close(STDERR_FILENO);
      fd = open("/dev/null", O_RDWR);
      dup2(fd, STDIN_FILENO);
      dup2(fd, STDOUT_FILENO);
      dup2(fd, STDERR_FILENO);
      execl(_PATH_PWD_MKDB, "pwd_mkdb", "-p", PATH_PASSWD_TMP, (char *) NULL);
      _exit(1);
    }
  while ((rpid = waitpid(pid, &pstat, 0)) < 0 && errno == EINTR)
    ;
  if (rpid == -1 || !WIFEXITED(pstat) || WEXITSTATUS(pstat) != 0)
    {
      unlink(PATH_PASSWD_TMP);
      return AL_EPASSWD;
    }
#else /* HAVE_MASTER_PASSWD */
  if (rename(PATH_PASSWD_TMP, PATH_PASSWD))
    {
      unlink(PATH_PASSWD_TMP);
#ifdef HAVE_LCKPWDF
      ulckpwdf();
#endif
      return AL_EPASSWD;
    }
#endif /* HAVE_MASTER_PASSWD */

#ifdef sgi
  /* Kludge: nsd has a one-second granularity in checking the mod time
   * of a file, so make sure we don't modify it twice within a second.
   */
  sleep(1);
#endif

  /* Unlock the passwd file if we're using System V style locking. */
#ifdef HAVE_LCKPWDF
  ulckpwdf();
#endif

  return AL_SUCCESS;
}

/* This is an internal function.  Its contract is to clean up after a
 * failed attempt to write the new passwd file.
 */

static void discard_passwd_lockfile(FILE *fp)
{
  fclose(fp);
  unlink(PATH_PASSWD_TMP);
#ifdef HAVE_LCKPWDF
  ulckpwdf();
#endif
  return;
}

/* This is an internal function.  Its contract is to add the user to the
 * local passwd database if appropriate, and set record->passwd_added to
 * 1 if it adds a passwd line.
 *
 * There are three implementations of the passwd database which concern
 * us:
 * 	* An /etc/passwd file
 * 	* An /etc/passwd and /etc/shadow file
 * 	* An /etc/master.passwd file and associated databases
 * For simplicity, PATH_PASSWD is defined to be "/etc/passwd" on the
 * first two types of systems and to "/etc/master.passwd" on the third
 * type.
 */

int al__add_to_passwd(const char *username, struct al_record *record)
{
  FILE *in = NULL, *out = NULL;
#ifdef HAVE_SHADOW
  FILE *shadow_in = NULL, *shadow_out = NULL;
#endif
  struct passwd *pwd, *tmppwd;
  char buf[BUFSIZ], *line = NULL;
  int linesize, len, found, nbytes, retval, fd;
  void *hescontext = NULL;

  tmppwd = al__getpwnam(username);
  if (tmppwd)
    {
      al__free_passwd(tmppwd);
      return AL_SUCCESS;
    }

  errno = 0;
  if (hesiod_init(&hescontext) != 0)
    return (errno == ENOMEM) ? AL_ENOMEM : AL_ENOUSER;

  pwd = hesiod_getpwnam(hescontext, username);

  if (!pwd)
    {
      hesiod_end(hescontext);
      return (errno == ENOMEM) ? AL_ENOMEM : AL_ENOUSER;
    }

  /* uid must not conflict with one already in passwd file.  gid
   * must not be in the range of reserved gids.
   */
  tmppwd = al__getpwuid(pwd->pw_uid);
  if (tmppwd || pwd->pw_gid < MIN_HES_GROUP)
    {
      al__free_passwd(tmppwd);
      hesiod_free_passwd(hescontext, pwd);
      hesiod_end(hescontext);
      return AL_EBADHES;
    }

  out = lock_passwd();
  in = fopen(PATH_PASSWD, "r");
  if (!out || !in)
    goto cleanup;
  do
    {
      nbytes = fread(buf, sizeof(char), BUFSIZ, in);
      if (nbytes)
	{
	  if (!fwrite(buf, sizeof(char), nbytes, out))
	    goto cleanup;
	}
    }
  while (nbytes == BUFSIZ);

  fprintf(out, "%s:%s:%lu:%lu%s:%s:%s:%s\n",
	  pwd->pw_name,
#ifdef HAVE_SHADOW
	  "x",
#else
	  pwd->pw_passwd,
#endif
	  (unsigned long) pwd->pw_uid, (unsigned long) pwd->pw_gid,
#ifdef HAVE_MASTER_PASSWD
	  "::0:0",
#else
	  "",
#endif
	  pwd->pw_gecos, pwd->pw_dir, pwd->pw_shell);

#ifdef HAVE_SHADOW
  /* Prepare to copy shadow file to a temporary file. */
  fd = open(PATH_SHADOW_TMP, O_RDWR|O_CREAT, S_IWUSR|S_IRUSR);
  if (fd < 0)
    goto cleanup;
  shadow_out = fdopen(fd, "w");
  if (!shadow_out)
    {
      close(fd);
      unlink(PATH_SHADOW_TMP);
      goto cleanup;
    }
  shadow_in = fopen(PATH_SHADOW, "r");
  if (!shadow_in)
    goto cleanup;

  /* Copy shadow file, noting if there is an entry for username. */
  found = 0;
  len = strlen(username);
  while ((retval = al__read_line(shadow_in, &line, &linesize)) == 0)
    {
      if (strncmp(username, line, len) == 0 && line[len] == ':')
	found = 1;
      fprintf(shadow_out, "%s\n", line);
    }
  free(line);
  if (retval == -1)
    goto cleanup;

  /* Add an entry for username if one was not already present. */
  if (!found)
    {
      fprintf(shadow_out, "%s:%s:%lu::::::\n", pwd->pw_name, pwd->pw_passwd,
	      (unsigned long) (time(NULL) / (60 * 60 * 24)));
    }

  fflush(shadow_out);
  retval = (fsync(fileno(shadow_out)) == -1);
  retval = ferror(shadow_out) || retval;
  retval = fclose(shadow_out) || retval;
  shadow_out = NULL;
  if (retval)
    goto cleanup;
  retval = fclose(shadow_in);
  shadow_in = NULL;
  if (retval)
    goto cleanup;
  if (rename(PATH_SHADOW_TMP, PATH_SHADOW))
    goto cleanup;
#endif

  retval = fclose(in);
  in = NULL;
  if (retval)
    goto cleanup;
  hesiod_free_passwd(hescontext, pwd);
  hesiod_end(hescontext);
  retval = update_passwd(out);
  if (retval == AL_SUCCESS)
    record->passwd_added = 1;
  return retval;

cleanup:
  if (hescontext)
    {
      if (pwd)
	hesiod_free_passwd(hescontext, pwd);
      hesiod_end(hescontext);
    }
  if (in)
    fclose(in);
#ifdef HAVE_SHADOW
  if (shadow_in)
    fclose(shadow_in);
  if (shadow_out)
    {
      fclose(shadow_out);
      unlink(PATH_SHADOW_TMP);
    }
#endif
  if (out)
    discard_passwd_lockfile(out);
  return AL_EPASSWD;
}

/* This is an internal function.  Its contract is to remove username from
 * the passwd file and shadow file if record->passwd_added is true.
 */

int al__remove_from_passwd(const char *username, struct al_record *record)
{
  FILE *in = NULL, *out = NULL;
#ifdef HAVE_SHADOW
  FILE *shadow_in = NULL, *shadow_out = NULL;
#endif
  char *buf = NULL;
  int bufsize, retval, len, fd;

  if (!record->passwd_added)
    return AL_SUCCESS;
  
  out = lock_passwd();
  in = fopen(PATH_PASSWD, "r");
  if (!out || !in)
    goto cleanup;
  len = strlen(username);

  while ((retval = al__read_line(in, &buf, &bufsize)) == 0)
    {
      if (strncmp(username, buf, len) != 0 || buf[len] != ':')
	{
	  fputs(buf, out);
	  fputs("\n", out);
	}
    }
  if (retval == -1)
    goto cleanup;
  retval = fclose(in);
  in = NULL;
  if (retval)
    goto cleanup;

#ifdef HAVE_SHADOW
  fd = open(PATH_SHADOW_TMP, O_RDWR|O_CREAT, S_IWUSR|S_IRUSR);
  if (fd < 0)
    goto cleanup;
  shadow_out = fdopen(fd, "w");
  if (!shadow_out)
    {
      close(fd);
      unlink(PATH_SHADOW_TMP);
      goto cleanup;
    }
  shadow_in = fopen(PATH_SHADOW, "r");
  if (!shadow_in)
    goto cleanup;

  while ((retval = al__read_line(shadow_in, &buf, &bufsize)) == 0)
    {
      if (strncmp(username, buf, len) != 0 || buf[len] != ':')
	{
	  fputs(buf, shadow_out);
	  fputs("\n", shadow_out);
	}
    }
  if (retval == -1)
    goto cleanup;
  fflush(shadow_out);
  retval = (fsync(fileno(shadow_out)) == -1);
  retval = ferror(shadow_out) || retval;
  retval = fclose(shadow_out) || retval;
  shadow_out = NULL;
  if (retval)
    goto cleanup;
  retval = fclose(shadow_in);
  shadow_in = NULL;
  if (retval)
    goto cleanup;
  if (rename(PATH_SHADOW_TMP, PATH_SHADOW))
    goto cleanup;
#endif

  free(buf);
  return update_passwd(out);

cleanup:
  free(buf);
#ifdef HAVE_SHADOW
  if (shadow_in)
    fclose(shadow_in);
  if (shadow_out)
    {
      fclose(shadow_out);
      unlink(PATH_SHADOW_TMP);
    }
#endif
  if (in)
    fclose(in);
  if (out)
    discard_passwd_lockfile(out);
  return AL_EPASSWD;
}

/* This is an internal function.  Its contract is to edit the passwd
 * file, changing the home directory field to homedir.
 */

int al__change_passwd_homedir(const char *username, const char *homedir)
{
  FILE *in = NULL, *out = NULL;
  int bufsize, retval, len, i;
  char *ptr1, *buf = NULL;

  out = lock_passwd();
  if (!out)
    return AL_EPASSWD;
  in = fopen(PATH_PASSWD, "r");
  if (!in)
    {
      discard_passwd_lockfile(out);
      return AL_EPASSWD;
    }
  len = strlen(username);

  while ((retval = al__read_line(in, &buf, &bufsize)) == 0)
    {
      if (strncmp(username, buf, len) == 0 && buf[len] == ':')
	{
	  /* Skip to colon before homedir field.  We start on
	   * the first colon and skip HOMEDIR_FIELD-2 more). */
	  for (ptr1 = buf + len, i = 0; ptr1 && i < HOMEDIR_FIELD - 2;
	       ptr1 = strchr(ptr1 + 1, ':'), i++)
	      ;
	  if (!ptr1)
	    continue;
	  fwrite(buf, sizeof(char), ptr1 + 1 - buf, out);
	  fputs(homedir, out);
	  ptr1 = strchr(ptr1 + 1, ':');
	  if (ptr1)
	    fputs(ptr1, out);
	  fputs("\n", out);
	}
      else
	{
	  fputs(buf, out);
	  fputs("\n", out);
	}
    }
  free(buf);
  fclose(in);
  if (retval == -1)
    {
      discard_passwd_lockfile(out);
      return AL_EPASSWD;
    }

  return update_passwd(out);
}

#ifdef HAVE_LCKPWDF
/* lckpwdf() is "for internal use only" and does not play nice with
 * alarms and the SIGALRM handler.  So we need to wrap it in a
 * function which restores the SIGALRM handler and alarm timer.
 */
static int safe_lckpwdf(void)
{
  struct sigaction act;
  unsigned int sec;
  int result;

  sec = alarm(0);
  sigaction(SIGALRM, NULL, &act);
  result = lckpwdf();
  sigaction(SIGALRM, &act, NULL);
  alarm(sec);
  return result;
}
#endif

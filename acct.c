#include <stdlib.h>
#include <sys/types.h>
#include <signal.h>
#include <string.h>
#include "al.h"
#include "al_private.h"

/* The al_acct_create() function has the following side-effects:
 *
 * 	* If a login record is not already present for this user:
 * 		- The user is added to the system passwd database if
 * 		  not already present there.  If /etc/nocrack is not
 * 		  present in the filesystem and the value of cryptpw
 * 		  is not NULL, it is substituted for the password
 * 		  field of the Hesiod passwd line.
 * 		- The user is added to zero or more groups in the
 * 		  system group database according to the user's hesiod
 * 		  group list.
 * 		- A login session record is created containing the
 * 		  requisite information for reversal of the above
 * 		  steps by al_acct_revert().
 *
 * 	* Unless a login record was already present and indicated that
 * 	  a temporary directory has been created for the user:
 * 		- The user's home directory is attached (by
 * 		  an invocation of "attach").
 * 	  If the value of havecred is true, the user's home directory
 * 	  is attached with authentication; otherwise the "-n" flag is
 * 	  passed to attach to suppress authentication.
 *
 * 	* If the user's home directory is remote and "attach"
 * 	  fails and tmphomedir is true:
 * 		- A temporary home directory is created for the user.
 * 		- The user's passwd entry is modified to point at the
 * 		  temporary home directory.
 * 		- The login session record is modified to indicate
 * 		  that a temporary directory has been created.  The
 * 		  old home directory field of the user's passwd entry
 * 		  is stored in the record for restoration of the
 * 		  passwd entry when the user is no longer logged in.
 *
 * 	* If sessionpid is not already in the login session record's
 *	  list of pids of active login sessions, it is added.
 *
 * The al_acct_create() function may return the following values:
 *
 * 	AL_SUCCESS	Successful completion
 * 	AL_ENOUSER	Unknown user
 * 	AL_EPASSWD	Can't add to passswd file
 * 	AL_ESESSION	Could not lock or modify login session record
 * 	AL_ENOMEM	Ran out of memory
 * 	AL_WARNINGS	Successful completion with some non-optimal
 * 			behavior
 *
 * If al_acct_create() returns AL_WARNINGS, then *warnings is set to a
 * malloc()'d array (which the caller must free) containing a list of
 * warning codes terminated by AL_SUCCESS.
 */


int al_acct_create(const char *username, const char *cryptpw,
		   pid_t sessionpid, int havecred, int tmphomedir,
		   int **warnings)
{
  int retval = AL_SUCCESS, nwarns = 0, warns[6], i;
  struct al_record record;
  pid_t *newpids;

  /* Get and lock the session record. */
  retval = al__get_session_record(username, &record);
  if (AL_ISWARNING(retval))
    warns[nwarns++] = retval;
  else
    {
      if (retval != AL_SUCCESS)
	return retval;
    }

  if (!record.exists)		/* We're first interested in this user. */
    {
      /* Add the user to the passwd file if necessary. */
      retval = al__add_to_passwd(username, &record, cryptpw);
      if (AL_ISWARNING(retval))
	warns[nwarns++] = retval;
      else
	{
	  if (retval != AL_SUCCESS)
	    goto cleanup;
	}

      /* Make sure user added to passwd file can be cleaned up later. */
      record.exists = 1;

      /* Add the user's groups to the group file if not already there. */
      retval = al__add_to_group(username, &record);
      if (AL_ISWARNING(retval))
	warns[nwarns++] = retval;
      else
	{
	  if (retval != AL_SUCCESS)
	    goto cleanup;
	}

      record.pids = malloc(sizeof(pid_t));
      if (!record.pids)
	{
	  retval = AL_ENOMEM;
	  goto cleanup;
	}
      record.npids = 1;
      record.pids[0] = sessionpid;
    }
  else				/* Other processes also interested in user. */
    {
      /* Update the encrypted password entry if we added a passwd line. */
      al__update_cryptpw(username, &record, cryptpw);

      /* Add pid to record if not already there. */
      for (i = 0; i < record.npids; i++)
	{
	  if (record.pids[i] == sessionpid)
	    break;
	}

      if (i == record.npids)
	{
	  newpids = malloc(++record.npids * sizeof(pid_t));
	  if (!newpids)
	    {
	      retval = AL_ENOMEM;
	      goto cleanup;
	    }
	  memcpy(newpids, record.pids, record.npids * sizeof(pid_t));
	  free(record.pids);
	  record.pids = newpids;
	  record.pids[record.npids - 1] = sessionpid;
	}
    }

  retval = al__setup_homedir(username, &record, havecred, tmphomedir);
  if (AL_ISWARNING(retval))
    warns[nwarns++] = retval;
  else
    {
      if (retval != AL_SUCCESS)
	goto cleanup;
    }

  /* Set warnings. */
  if (nwarns > 0)
    {
      warns[nwarns++] = AL_SUCCESS;
      *warnings = malloc(nwarns * sizeof(int));
      if (!*warnings)
	retval = AL_ENOMEM;
      else
	{
	  retval = AL_WARNINGS;
	  memcpy(*warnings, warns, nwarns * sizeof(int));
	}
    }

cleanup:
  al__put_session_record(&record);
  return retval;
}

static int revert(const char *username, struct al_record *record)
{
  int retval, reterr = AL_SUCCESS;

  retval = al__revert_homedir(username, record);
  if (AL_SUCCESS != retval)
    reterr = retval;
  retval = al__remove_from_group(username, record);
  if (AL_SUCCESS != retval)
    reterr = retval;
  retval = al__remove_from_passwd(username, record);
  if (AL_SUCCESS != retval)
    reterr = retval;

  record->exists = 0;
  return reterr;
}

/* The al_acct_revert() function has the following side effects:
 *
 * 	* If a login record for the user exists and sessionpid is in
 * 	  the record's list of pids of active login sessions:
 * 	  - sessionpid is removed from the user's login record's list
 * 	    of pids of active login sessions.
 *
 * 	* If sessionpid was successfully removed from the list, and
 * 	  the list of pids is now empty:
 * 	  - All modifications to the passwd and group database
 * 	    effected by calls to al_acct_create() are reverted.
 * 	  - The user's home directory is detached.
 */

int al_acct_revert(const char *username, pid_t sessionpid)
{
  int retval, found = 0, i;
  struct al_record record;

  retval = al__get_session_record(username, &record);
  if (AL_SUCCESS != retval)
    return retval;

  if (record.exists)
    {
      for (i = 0; i < record.npids; i++)
	{
	  if (record.pids[i] == sessionpid)
	    found = 1;
	  if (found && i + 1 < record.npids)
	    record.pids[i] = record.pids[i + 1];
	}
      if (found)
	record.npids--;
      if (record.npids <= 0)
	revert(username, &record);
    }

  return al__put_session_record(&record);
}

/* The al_acct_cleanup() function has the following side effects:
 *
 * 	* If a login record for the user exists:
 * 	  - All pids in the login record are checked for existence and
 * 	    the ones which don't exist are removed from the list.
 *
 * 	* If the list of pids was emptied by the above operation:
 * 	  - All modifications to the passwd and group database
 * 	    effected by calls to al_acct_create() are reverted.
 * 	  - The user's home directory is detached.
 */

int al_acct_cleanup(const char *username)
{
  int retval, i;
  struct al_record record;

  retval = al__get_session_record(username, &record);
  if (AL_SUCCESS != retval)
    return retval;

  if (record.exists)
    {
      /* Remove nonexistent processes from pid list. */
      i = 0;
      while (i < record.npids)
	{
	  if (kill(record.pids[i], 0))
	    {
	      record.npids--;
	      if (i < record.npids)
		{
		  memmove(&record.pids[i], &record.pids[i + 1],
			  (record.npids - i) * sizeof(pid_t));
		}
	    }
	  else
	    i++;
	}
      if (record.npids <= 0)
	revert(username, &record);
    }

  return al__put_session_record(&record);
}
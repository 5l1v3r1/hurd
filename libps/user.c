/* The type ps_user_t, for per-user info.

   Copyright (C) 1995 Free Software Foundation, Inc.

   Written by Miles Bader <miles@gnu.ai.mit.edu>

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2, or (at
   your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA. */

#include <hurd.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include "ps.h"
#include "common.h"

/* ---------------------------------------------------------------- */

/* Create a ps_user_t for the user referred to by UID, returning it in U.
   If a memory allocation error occurs, ENOMEM is returned, otherwise 0.  */
error_t
ps_user_create(int uid, ps_user_t *u)
{
  *u = NEW(struct ps_user);
  if (*u == NULL)
    return ENOMEM;

  (*u)->uid = uid;
  (*u)->passwd_state = PS_USER_PASSWD_PENDING;

  return 0;
}

/* Free U and any resources it consumes.  */
void
ps_user_free(ps_user_t u)
{
  if (u->passwd_state == PS_USER_PASSWD_OK)
    free(u->storage);
  free(u);
}

/* ---------------------------------------------------------------- */

/* Returns the password file entry (struct passwd, from <pwd.h>) for the user
   referred to by U, or NULL if it can't be gotten.  */
struct passwd *ps_user_passwd(ps_user_t u)
{
  if (u->passwd_state == PS_USER_PASSWD_OK)
    return &u->passwd;
  else if (u->passwd_state == PS_USER_PASSWD_ERROR)
    return NULL;
  else
    {
      struct passwd *pw = getpwuid(u->uid);
      if (pw != NULL)
	{
	  int needed = 0;

#define COUNT(field) if (pw->field != NULL) (needed += strlen(pw->field) + 1)
	  COUNT(pw_name);
	  COUNT(pw_passwd);
	  COUNT(pw_gecos);
	  COUNT(pw_dir);
	  COUNT(pw_shell);

	  u->storage = malloc(needed);
	  if (u->storage != NULL)
	    {
	      char *p = u->storage;

	      /* Copy each string field into storage allocated in the u
		 structure and point the fields at that instead of the static
		 storage that pw currently points to.  */
#define COPY(field) \
  if (pw->field != NULL) \
   strcpy(p, pw->field), (pw->field = p), (p += strlen(p) + 1)
	      COPY(pw_name);
	      COPY(pw_passwd);
	      COPY(pw_gecos);
	      COPY(pw_dir);
	      COPY(pw_shell);

	      u->passwd = *pw;
	      u->passwd_state = PS_USER_PASSWD_OK;

	      return &u->passwd;
	    }
	}
    }

  u->passwd_state = PS_USER_PASSWD_ERROR;
  return NULL;
}

/* Returns the user name for the user referred to by U, or NULL if it can't
   be gotten.  */
char *ps_user_name(ps_user_t u)
{
  struct passwd *pw = ps_user_passwd(u);
  if (pw)
    return pw->pw_name;
  else
    return NULL;
}

/* Implements the type ps_fmt_t, which describes how to output a user-readable
   version of a proc_stat_t.

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

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <ctype.h>

#include "ps.h"
#include "common.h"

/* ---------------------------------------------------------------- */

/* Make a PS_FMT_T by parsing the string SRC, searching for any named
   field specs in FMT_SPECS, and returning the result in FMT.  If a memory
   allocation error occurs, ENOMEM is returned.  If SRC contains an unknown
   field name, EINVAL is returned.  Otherwise 0 is returned.  See ps.h for an
   explanation of how FMT is derived from SRC.  */
error_t
ps_fmt_create(char *src, ps_fmt_specs_t fmt_specs, ps_fmt_t *fmt)
{
  ps_fmt_t new_fmt;
  int needs = 0;
  int fields_alloced = 10;
  ps_fmt_field_t fields = NEWVEC(struct ps_fmt_field, fields_alloced);
  ps_fmt_field_t field = fields; /* current last field */

  if (fields == NULL)
    return ENOMEM;

  new_fmt = NEW(struct ps_fmt);
  if (fmt == NULL)
    {
      FREE(fields);
      return ENOMEM;
    }

  /* Make a private copy of SRC so we can mutate it.  */
  new_fmt->src = NEWVEC(char, strlen(src) + 1);
  if (new_fmt->src == NULL)
    {
      FREE(fields);
      FREE(new_fmt);
      return ENOMEM;
    }
  strcpy(new_fmt->src, src);
  src = new_fmt->src;

  while (*src != '\0')
    {
      if (field - fields == fields_alloced)
	/* Time to grow FIELDS to make room for more.  */
	{
	  int offs = field - fields;

	  fields_alloced += 10;
	  fields = GROWVEC(fields, struct ps_fmt_field, fields_alloced);

	  if (fields == NULL)
	    {
	      FREE(new_fmt);
	      FREE(new_fmt->src);
	      return ENOMEM;
	    }

	  field = fields + offs;
	}

      /* Find the text to be reproduced verbatim between the last field and
	 the next one; we'll add this a prefix to FIELD.  */
      field->pfx = src;
      while (*src != '\0' && *src != '~')
	src++;
      field->pfx_len = src - field->pfx;

      field->spec = NULL;
      field->title = NULL;
      field->width = 0;

      if (*src != '\0')
	/* Another format-spec.  */
	{
	  char *name;
	  int sign = 1;
	  bool explicit_width = FALSE; /* True if the width set from SRC.  */
	  char *spec_start = src++;

	  /* Read an explicit field width.  */
	  field->width = 0;
	  if (*src == '-')
	    sign = -1, src++;
	  while (isdigit(*src))
	    {
	      field->width = field->width * 10 + (*src++ - '0');
	      explicit_width = TRUE;
	    }

	  /* Skip `/' between optional width and spec name.  */
	  if (*src == '/')
	    src++;

	  /* The name of the spec, or `TITLE=NAME'.  */
	  name = src;
	  while (*src != '\0' && !isspace(*src) && *src != '/' && *src != '=')
	    src++;

	  if (*src == '=')
	    /* A title different from the spec name; the actual name follows
	       the `='.  */
	    {
	      field->title = name;
	      *src++ = '\0';

	      /* Now read the real name.  */
	      name = src;
	      while (*src != '\0' && !isspace(*src) && *src != '/')
		src++;
	    }

	  /* Now that we've parsed everything in this spec, move the whole
	     thing down one byte (trashing the leading `~') so that we have
	     room to NUL-terminate the name for which we're searching.  We
	     also adjust any pointers into this spec-string accordingly.  */
	  bcopy (spec_start + 1, spec_start, src - spec_start - 1);
	  name--;
	  if (field->title)
	    field->title--;

	  /* Now that we've made room, do the termination of NAME.  */
	  src[-1] = '\0';

	  field->spec = ps_fmt_specs_find (fmt_specs, name);
	  if (!field->spec)
	    /* Failed to find any named spec called NAME.  */
	    {
	      FREE(new_fmt->src);
	      FREE(fields);
	      FREE(new_fmt);
	      return EINVAL;
	    }

	  if (! field->title)
	    /* No explicit title specified in the fmt string.  */
	    if (field->spec->title)
	      field->title = field->spec->title; /* But the spec has one.  */
	    else
	      field->title = name; /* Just use the field name.  */

	  /* Add FIELD's required pstat_flags to FMT's set */
	  needs |= ps_getter_needs(ps_fmt_spec_getter(field->spec));

	  if (!explicit_width)
	    field->width = ps_fmt_spec_width(field->spec);

	  /* Skip optional trailing `/' after the spec name.  */
	  if (*src == '/')
	    src++;

	  /* Remember the width's sign (we put it here after possibly using a
	     default width so that the user may include a `-' with no width
	     to flip the justification of the default width).  */
	  field->width *= sign;

	  {
	    /* Force the field to be wide enough to hold the title.  */
	    int width = field->width;
	    int tlen = strlen(field->title);
	    if (width != 0 && tlen > ABS(width))
	      field->width = (width > 0 ? tlen : -tlen);
	  }
	}

      field++;
    }

  new_fmt->fields = fields;
  new_fmt->num_fields = field - fields;
  new_fmt->needs = needs;
  new_fmt->inval = 0;

  *fmt = new_fmt;

  return 0;
}

/* Free FMT, and any resources it consumes.  */
void 
ps_fmt_free(ps_fmt_t fmt)
{
  FREE(fmt->src);
  FREE(fmt->fields);
  FREE(fmt);
}

/* ---------------------------------------------------------------- */

/* Write an appropiate header line for FMT, containing the titles of all its
   fields appropiately aligned with where the values would be printed, to
   STREAM (without a trailing newline).  If count is non-NULL, the total
   number number of characters output is added to the integer it points to.
   If any fatal error occurs, the error code is returned, otherwise 0.  */
error_t
ps_fmt_write_titles (ps_fmt_t fmt, ps_stream_t stream)
{
  error_t err = 0;
  ps_fmt_field_t field = ps_fmt_fields(fmt);
  int left = ps_fmt_num_fields(fmt);

  while (left-- > 0 && !err)
    {
      char *pfx = ps_fmt_field_prefix(field);
      int pfx_len = ps_fmt_field_prefix_length(field);

      if (pfx_len > 0)
	err = ps_stream_write (stream, pfx, pfx_len);

      if (ps_fmt_field_fmt_spec(field) != NULL && !err)
	{
	  char *title = ps_fmt_field_title(field);
	  int width = ps_fmt_field_width(field);

	  if (title == NULL)
	    title = "??";

	  err = ps_stream_write_field (stream, title, width);
	}

      field++;
    }

  return err;
}

/* Format a description as instructed by FMT, of the process described by PS
   to STREAM (without a trailing newline).  If count is non-NULL, the total
   number number of characters output is added to the integer it points to.
   If any fatal error occurs, the error code is returned, otherwise 0.  */
error_t
ps_fmt_write_proc_stat (ps_fmt_t fmt, proc_stat_t ps, ps_stream_t stream)
{
  error_t err = 0;
  ps_fmt_field_t field = ps_fmt_fields(fmt);
  int nfields = ps_fmt_num_fields(fmt);
  int have = proc_stat_flags(ps);

  while (nfields-- > 0 && !err)
    {
      ps_fmt_spec_t spec = ps_fmt_field_fmt_spec(field);
      char *pfx = ps_fmt_field_prefix(field);
      int pfx_len = ps_fmt_field_prefix_length(field);

      if (pfx_len > 0)
	err = ps_stream_write (stream, pfx, pfx_len);

      if (spec != NULL && !err)
	{
	  int need = ps_getter_needs(ps_fmt_spec_getter(spec));
	  int width = ps_fmt_field_width(field);

	  /* do we have the resources to print this field? */
	  if ((need & have) == need)
	    /* Yup */
	    {
	      int (*output_fn)() = (int (*)())ps_fmt_spec_output_fn(spec);
	      ps_getter_t getter = ps_fmt_spec_getter(spec);
	      err = output_fn(ps, getter, width, stream);
	    }
	  else
	    /* Something to display in invalid fields.  */
	    err = ps_stream_write_field (stream, fmt->inval ?: "", width);
	}

      field++;
    }

  return err;
}

/* ---------------------------------------------------------------- */

/* Remove those fields from FMT for which the function FN, when called on the
   field's format spec, returns true.  Appropiate inter-field characters are
   also removed: those *following* deleted fields at the beginning of the
   fmt, and those *preceeding* deleted fields *not* at the beginning. */
void
ps_fmt_squash (ps_fmt_t fmt, bool (*fn)(ps_fmt_spec_t spec))
{
  int nfields = fmt->num_fields;
  ps_fmt_field_t fields = fmt->fields, field = fields;
  /* As we're removing some fields, we must recalculate the set of ps flags
     needed by all fields.  */
  ps_flags_t need = 0;

  while ((field - fields) < nfields)
    {
      ps_fmt_spec_t spec = field->spec;

      if (spec != NULL && (*fn)(spec))
	/* Squash this field! */
	{
	  /* Save the old prefix, in case we're deleting the first field,
	     and need to prepend it to the next field.  */
	  char *beg_pfx = field->pfx; 
	  int beg_pfx_len = field->pfx_len;

	  nfields--;

	  /* Shift down all following fields over this one.  */
	  if (nfields > 0)
	    bcopy(field + 1, field,
		  (nfields - (field - fields)) * sizeof *field);

	  if (field == fields)
	    /* This is the first field, so move its prefix to the
	       following field (overwriting that field's prefix).  This
	       is to ensure that the beginning of the format string is
	       preserved in preference to the middle, as it is more
	       likely to be significant.  */
	    {
	      if (nfields == 0)
		/* no following fields, so just make a new end field (we're
		   sure to have room, as we just vacated a space).  */
		{
		  nfields++;
		  field->pfx = beg_pfx;
		  field->pfx_len = beg_pfx_len;
		  field->spec = NULL;
		}
	      else if (field->spec == NULL)
		/* One following field with only a prefix -- the suffix
		   of the format string.  Tack the prefix on before the
		   suffix so we preserve both the beginning and the end
		   of the format string.  We know there's space in our
		   copy of the source string, because we've just squashed
		   a field which took at least that much room (as it
		   previously contained the same prefix).  */
		{
		  field->pfx -= beg_pfx_len;
		  field->pfx_len += beg_pfx_len;
		  bcopy(beg_pfx, field->pfx, beg_pfx_len);
		}
	      else
		/* otherwise just replace the next field's prefix with
		   the beginning one */
		{
		  field->pfx = beg_pfx;
		  field->pfx_len = beg_pfx_len;
		}
	    }
	}
      else
	/* don't squash this field, just move to the next one */
	{
	  need |= ps_getter_needs (ps_fmt_spec_getter (spec));
	  field++;
	}
    }

  fmt->num_fields = nfields;
  fmt->needs = need;
}

/* ---------------------------------------------------------------- */

/* Remove those fields from FMT which would need the proc_stat flags FLAGS.
   Appropiate inter-field characters are also removed: those *following*
   deleted fields at the beginning of the fmt, and those *preceeding* deleted
   fields *not* at the beginning.  */
void
ps_fmt_squash_flags(ps_fmt_t fmt, ps_flags_t flags)
{
  bool squashable_spec (ps_fmt_spec_t spec)
    {
      return ps_getter_needs (ps_fmt_spec_getter (spec)) & flags;
    }

  ps_fmt_squash (fmt, squashable_spec);
}

/* ---------------------------------------------------------------- */

/* Try and restrict the number of output columns in FMT to WIDTH.  */
void
ps_fmt_set_output_width (ps_fmt_t fmt, int width)
{
  struct ps_fmt_field *field = ps_fmt_fields (fmt);
  int nfields = ps_fmt_num_fields (fmt);

  /* We're not very clever about this -- just add up the width of all the
     fields but the last, and if the last has no existing width (as is
     the case in most output formats), give it whatever is left over.  */
  while (--nfields > 0)
    {
      int fw = field->width;
      width -= field->pfx_len + (fw < 0 ? -fw : fw);
      field++;
    }
  if (nfields == 0 && field->width == 0 && width > 0)
    field->width = width - field->pfx_len - 1; /* 1 for the CR. */
}

/* Access, formatting, & comparison routines for printing process info.

   Copyright (C) 1995, 1996 Free Software Foundation, Inc.

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
#include <pwd.h>
#include <hurd/resource.h>
#include <unistd.h>
#include <string.h>
#include <timefmt.h>
#include <sys/time.h>

#include "ps.h"
#include "common.h"

/* XXX */
static char *get_syscall_name (int num) { return 0; }
static char *get_rpc_name (mach_msg_id_t it) { return 0; }

/* ---------------------------------------------------------------- */
/* Getter definitions */

typedef void (*vf)();

static int 
ps_get_pid(proc_stat_t ps)
{
  return proc_stat_pid(ps);
}
const struct ps_getter ps_pid_getter =
{"pid", PSTAT_PID, (vf) ps_get_pid};

static int 
ps_get_thread_index(proc_stat_t ps)
{
  return proc_stat_thread_index(ps);
}
const struct ps_getter ps_thread_index_getter =
{"thread_index", PSTAT_THREAD, (vf) ps_get_thread_index};

static ps_user_t
ps_get_owner(proc_stat_t ps)
{
  return proc_stat_owner (ps);
}
const struct ps_getter ps_owner_getter =
{"owner", PSTAT_OWNER, (vf) ps_get_owner};

static int
ps_get_owner_uid (proc_stat_t ps)
{
  return proc_stat_owner_uid (ps);
}
const struct ps_getter ps_owner_uid_getter =
{"uid", PSTAT_OWNER_UID, (vf) ps_get_owner_uid};

static int 
ps_get_ppid(proc_stat_t ps)
{
  return proc_stat_proc_info(ps)->ppid;
}
const struct ps_getter ps_ppid_getter =
{"ppid", PSTAT_PROC_INFO, (vf) ps_get_ppid};

static int 
ps_get_pgrp(proc_stat_t ps)
{
  return proc_stat_proc_info(ps)->pgrp;
}
const struct ps_getter ps_pgrp_getter =
{"pgrp", PSTAT_PROC_INFO, (vf) ps_get_pgrp};

static int 
ps_get_session(proc_stat_t ps)
{
  return proc_stat_proc_info(ps)->session;
}
const struct ps_getter ps_session_getter =
{"session", PSTAT_PROC_INFO, (vf) ps_get_session};

static int 
ps_get_login_col(proc_stat_t ps)
{
  return proc_stat_proc_info(ps)->logincollection;
}
const struct ps_getter ps_login_col_getter =
{"login_col", PSTAT_PROC_INFO, (vf) ps_get_login_col};

static int 
ps_get_num_threads(proc_stat_t ps)
{
  return proc_stat_num_threads(ps);
}
const struct ps_getter ps_num_threads_getter =
{"num_threads", PSTAT_NUM_THREADS, (vf)ps_get_num_threads};

static void 
ps_get_args(proc_stat_t ps, char **args_p, int *args_len_p)
{
  *args_p = proc_stat_args(ps);
  *args_len_p = proc_stat_args_len(ps);
}
const struct ps_getter ps_args_getter =
{"args", PSTAT_ARGS, ps_get_args};

static int 
ps_get_state(proc_stat_t ps)
{
  return proc_stat_state(ps);
}
const struct ps_getter ps_state_getter =
{"state", PSTAT_STATE, (vf) ps_get_state};

static void
ps_get_wait (proc_stat_t ps, char **wait, int *rpc)
{
  *wait = ps->thread_wait;
  *rpc = ps->thread_rpc;
}
const struct ps_getter ps_wait_getter =
{"wait", PSTAT_THREAD_WAIT, ps_get_wait};

static int 
ps_get_vsize(proc_stat_t ps)
{
  return proc_stat_task_basic_info(ps)->virtual_size;
}
const struct ps_getter ps_vsize_getter =
{"vsize", PSTAT_TASK_BASIC, (vf) ps_get_vsize};

static int 
ps_get_rsize(proc_stat_t ps)
{
  return proc_stat_task_basic_info(ps)->resident_size;
}
const struct ps_getter ps_rsize_getter =
{"rsize", PSTAT_TASK_BASIC, (vf) ps_get_rsize};

static int 
ps_get_cur_priority(proc_stat_t ps)
{
  return proc_stat_thread_basic_info(ps)->cur_priority;
}
const struct ps_getter ps_cur_priority_getter =
{"cur_priority", PSTAT_THREAD_BASIC, (vf) ps_get_cur_priority};

static int 
ps_get_base_priority(proc_stat_t ps)
{
  return proc_stat_thread_basic_info(ps)->base_priority;
}
const struct ps_getter ps_base_priority_getter =
{"base_priority", PSTAT_THREAD_BASIC, (vf) ps_get_base_priority};

static int 
ps_get_max_priority(proc_stat_t ps)
{
  return proc_stat_thread_sched_info(ps)->max_priority;
}
const struct ps_getter ps_max_priority_getter =
{"max_priority", PSTAT_THREAD_SCHED, (vf) ps_get_max_priority};

static void 
ps_get_usr_time (proc_stat_t ps, struct timeval *tv)
{
  time_value_t tvt = proc_stat_thread_basic_info (ps)->user_time;
  tv->tv_sec = tvt.seconds;
  tv->tv_usec = tvt.microseconds;
}
const struct ps_getter ps_usr_time_getter =
{"usr_time", PSTAT_THREAD_BASIC, ps_get_usr_time};

static void 
ps_get_sys_time (proc_stat_t ps, struct timeval *tv)
{
  time_value_t tvt = proc_stat_thread_basic_info (ps)->system_time;
  tv->tv_sec = tvt.seconds;
  tv->tv_usec = tvt.microseconds;
}
const struct ps_getter ps_sys_time_getter =
{"sys_time", PSTAT_THREAD_BASIC, ps_get_sys_time};

static void 
ps_get_tot_time (proc_stat_t ps, struct timeval *tv)
{
  time_value_t tvt = proc_stat_thread_basic_info (ps)->user_time;
  time_value_add (&tvt, &proc_stat_thread_basic_info (ps)->system_time);
  tv->tv_sec = tvt.seconds;
  tv->tv_usec = tvt.microseconds;
}
const struct ps_getter ps_tot_time_getter =
{"tot_time", PSTAT_THREAD_BASIC, ps_get_tot_time};

static float 
ps_get_rmem_frac(proc_stat_t ps)
{
  static int mem_size = 0;

  if (mem_size == 0)
    {
      host_basic_info_t info;
      error_t err = ps_host_basic_info(&info);
      if (err == 0)
	mem_size = info->memory_size;
    }
  
  if (mem_size > 0)
    return
      (float)proc_stat_task_basic_info(ps)->resident_size
	/ (float)mem_size;
  else
    return 0.0;
}
const struct ps_getter ps_rmem_frac_getter =
{"rmem_frac", PSTAT_TASK_BASIC, (vf) ps_get_rmem_frac};

static float 
ps_get_cpu_frac(proc_stat_t ps)
{
  return (float) proc_stat_thread_basic_info(ps)->cpu_usage
    / (float) TH_USAGE_SCALE;
}
const struct ps_getter ps_cpu_frac_getter =
{"cpu_frac", PSTAT_THREAD_BASIC, (vf) ps_get_cpu_frac};

static int 
ps_get_sleep(proc_stat_t ps)
{
  return proc_stat_thread_basic_info(ps)->sleep_time;
}
const struct ps_getter ps_sleep_getter =
{"sleep", PSTAT_THREAD_BASIC, (vf) ps_get_sleep};

static int 
ps_get_susp_count(proc_stat_t ps)
{
  return proc_stat_suspend_count(ps);
}
const struct ps_getter ps_susp_count_getter =
{"susp_count", PSTAT_SUSPEND_COUNT, (vf) ps_get_susp_count};

static int 
ps_get_proc_susp_count(proc_stat_t ps)
{
  return proc_stat_task_basic_info(ps)->suspend_count;
}
const struct ps_getter ps_proc_susp_count_getter =
{"proc_susp_count", PSTAT_TASK_BASIC, (vf) ps_get_proc_susp_count};

static int 
ps_get_thread_susp_count(proc_stat_t ps)
{
  return proc_stat_thread_basic_info(ps)->suspend_count;
}
const struct ps_getter ps_thread_susp_count_getter =
{"thread_susp_count", PSTAT_SUSPEND_COUNT, (vf) ps_get_thread_susp_count};

static ps_tty_t
ps_get_tty(proc_stat_t ps)
{
  return proc_stat_tty(ps);
}
const struct ps_getter ps_tty_getter =
{"tty", PSTAT_TTY, (vf)ps_get_tty};

static int 
ps_get_page_faults(proc_stat_t ps)
{
  return proc_stat_task_events_info(ps)->faults;
}
const struct ps_getter ps_page_faults_getter =
{"page_faults", PSTAT_TASK_EVENTS, (vf) ps_get_page_faults};

static int 
ps_get_cow_faults(proc_stat_t ps)
{
  return proc_stat_task_events_info(ps)->cow_faults;
}
const struct ps_getter ps_cow_faults_getter =
{"cow_faults", PSTAT_TASK_EVENTS, (vf) ps_get_cow_faults};

static int 
ps_get_pageins(proc_stat_t ps)
{
  return proc_stat_task_events_info(ps)->pageins;
}
const struct ps_getter ps_pageins_getter =
{"pageins", PSTAT_TASK_EVENTS, (vf) ps_get_pageins};

static int 
ps_get_msgs_sent(proc_stat_t ps)
{
  return proc_stat_task_events_info(ps)->messages_sent;
}
const struct ps_getter ps_msgs_sent_getter =
{"msgs_sent", PSTAT_TASK_EVENTS, (vf) ps_get_msgs_sent};

static int 
ps_get_msgs_rcvd(proc_stat_t ps)
{
  return proc_stat_task_events_info(ps)->messages_received;
}
const struct ps_getter ps_msgs_rcvd_getter =
{"msgs_rcvd", PSTAT_TASK_EVENTS, (vf) ps_get_msgs_rcvd};

static int 
ps_get_zero_fills(proc_stat_t ps)
{
  return proc_stat_task_events_info(ps)->zero_fills;
}
const struct ps_getter ps_zero_fills_getter =
{"zero_fills", PSTAT_TASK_EVENTS, (vf) ps_get_zero_fills};

/* ---------------------------------------------------------------- */
/* some printing functions */

/* G() is a helpful macro that just returns the getter G's access function
   cast into a function pointer returning TYPE, as how the function should be
   called varies depending on the getter */
#define G(g,type)((type (*)())ps_getter_function(g))

error_t
ps_emit_int(proc_stat_t ps, ps_getter_t getter, int width, ps_stream_t stream)
{
  return ps_stream_write_int_field (stream, G(getter, int)(ps), width);
}

error_t
ps_emit_nz_int (proc_stat_t ps, ps_getter_t getter, int width,
		ps_stream_t stream)
{
  int value = G(getter, int)(ps);
  if (value)
    return ps_stream_write_int_field  (stream, value, width);
  else
    return ps_stream_write_field (stream, "-", width);
}

error_t
ps_emit_priority (proc_stat_t ps, ps_getter_t getter, int width,
		  ps_stream_t stream)
{
  return
    ps_stream_write_int_field (stream,
			       MACH_PRIORITY_TO_NICE (G(getter, int)(ps)),
			       width);
}

error_t
ps_emit_num_blocks (proc_stat_t ps, ps_getter_t getter, int width,
		    ps_stream_t stream)
{
  char buf[20];
  sprintf(buf, "%d", G(getter, int)(ps) / 1024);
  return ps_stream_write_field (stream, buf, width);
}

int 
sprint_frac_value(char *buf,
		  int value, int min_value_len,
		  int frac, int frac_scale,
		  int width)
{
  int value_len;
  int frac_len;

  if (value >= 100)		/* the integer part */
    value_len = 3;
  else if (value >= 10)
    value_len = 2;
  else
    value_len = 1;

  while (value_len < min_value_len--)
    *buf++ = '0';

  for (frac_len = frac_scale
       ; frac_len > 0 && (width < value_len + 1 + frac_len || frac % 10 == 0)
       ; frac_len--)
    frac /= 10;

  if (frac_len > 0)
    sprintf(buf, "%d.%0*d", value, frac_len, frac);
  else
    sprintf(buf, "%d", value);

  return strlen(buf);
}

error_t
ps_emit_percent (proc_stat_t ps, ps_getter_t getter, int width,
		 ps_stream_t stream)
{
  char buf[20];
  float perc = G(getter, float)(ps) * 100;

  if (width == 0)
    sprintf(buf, "%g", perc);
  else if (ABS(width) > 3)
    sprintf(buf, "%.*f", ABS(width) - 3, perc);
  else
    sprintf(buf, "%d", (int) perc);

  return ps_stream_write_field (stream, buf, width);
}

/* prints its value nicely */
error_t
ps_emit_nice_int (proc_stat_t ps, ps_getter_t getter, int width,
		  ps_stream_t stream)
{
  char buf[20];
  int value = G(getter, int)(ps);
  char *sfx = " KMG";
  int frac = 0;

  while (value >= 1024)
    {
      frac = ((value & 0x3FF) * 1000) >> 10;
      value >>= 10;
      sfx++;
    }

  sprintf(buf + sprint_frac_value (buf, value, 1, frac, 3, ABS(width) - 1),
	  "%c", *sfx);

  return ps_stream_write_field (stream, buf, width);
}

error_t
ps_emit_seconds (proc_stat_t ps, ps_getter_t getter, int width,
		 ps_stream_t stream)
{
  char buf[20];
  struct timeval tv;

  G(getter, void)(ps, &tv);

  fmt_seconds (&tv, ABS (width), buf, sizeof (buf));

  return ps_stream_write_field (stream, buf, width);
}

error_t
ps_emit_minutes (proc_stat_t ps, ps_getter_t getter,
		 int width, ps_stream_t stream)
{
  char buf[20];
  struct timeval tv;

  G(getter, int)(ps, &tv);

  fmt_minutes (&tv, ABS (width), buf, sizeof (buf));

  return ps_stream_write_field (stream, buf, width);
}

error_t
ps_emit_past_time (proc_stat_t ps, ps_getter_t getter,
		   int width, ps_stream_t stream)
{
  
}

error_t
ps_emit_uid (proc_stat_t ps, ps_getter_t getter, int width, ps_stream_t stream)
{
  int uid = G(getter, int)(ps);
  if (uid < 0)
    return ps_stream_write_field (stream, "-", width);
  else
    return ps_stream_write_int_field (stream, uid, width);
}

error_t
ps_emit_uname (proc_stat_t ps, ps_getter_t getter, int width,
	       ps_stream_t stream)
{
  ps_user_t u = G(getter, ps_user_t)(ps);
  if (u)
    {
      struct passwd *pw = ps_user_passwd (u);
      if (pw == NULL)
	return ps_stream_write_int_field (stream, ps_user_uid(u), width);
      else
	return ps_stream_write_field (stream, pw->pw_name, width);
    }
  else
    return ps_stream_write_field (stream, "-", width);
}

/* prints a string with embedded nuls as spaces */
error_t
ps_emit_string0 (proc_stat_t ps, ps_getter_t getter, int width,
		 ps_stream_t stream)
{
  char *s0, *p, *q;
  int s0len;
  int fwidth = ABS(width);
  char static_buf[200];
  char *buf = static_buf;

  G(getter, void)(ps, &s0, &s0len);

  if (s0 == NULL)
    *buf = '\0';
  else
    {
      if (s0len > sizeof static_buf)
	{
	  buf = malloc (s0len + 1);
	  if (buf == NULL)
	    return ENOMEM;
	}

      if (fwidth == 0 || fwidth > s0len)
	fwidth = s0len;

      for (p = buf, q = s0; fwidth-- > 0; p++, q++)
	{
	  int ch = *q;
	  *p = (ch == '\0' ? ' ' : ch);
	}
      if (q > s0 && *(q - 1) == '\0')
	*--p = '\0';
      else
	*p = '\0';
    }

  {
    error_t err = ps_stream_write_trunc_field (stream, buf, width);
    if (buf != static_buf)
      free(buf);
    return err;
  }
}

error_t
ps_emit_string (proc_stat_t ps, ps_getter_t getter, int width,
		ps_stream_t stream)
{
  char *str;
  int len;

  G(getter, void)(ps, &str, &len);

  if (str == NULL)
    str = "";

  return ps_stream_write_trunc_field (stream, str, width);
}

error_t
ps_emit_tty_name (proc_stat_t ps, ps_getter_t getter, int width,
		  ps_stream_t stream)
{
  char *name = "-";
  ps_tty_t tty = G(getter, ps_tty_t)(ps);

  if (tty)
    {
      name = ps_tty_short_name(tty);
      if (name == NULL || *name == '\0')
	name = "?";
    }

  return ps_stream_write_field (stream, name, width);
}

struct state_shadow
{
  /* If any states in STATES are set, the states in shadow are suppressed.  */
  int states;
  int shadow;
};

static const struct state_shadow
state_shadows[] = {
  /* Don't show sleeping thread if one is running, or the process is stopped.*/
  { PSTAT_STATE_T_RUN | PSTAT_STATE_P_STOP,
    PSTAT_STATE_T_SLEEP | PSTAT_STATE_T_IDLE | PSTAT_STATE_T_WAIT },
  /* Only show the longest sleep.  */
  { PSTAT_STATE_T_IDLE,		PSTAT_STATE_T_SLEEP | PSTAT_STATE_T_WAIT },
  { PSTAT_STATE_T_SLEEP,	PSTAT_STATE_T_WAIT },
  /* Turn off the thread stop bits if any thread is not stopped.  This is
     generally reasonable, as threads are often suspended to be frobed; if
     they're all suspended, then something's odd (probably in the debugger,
     or crashed).  */
  { PSTAT_STATE_T_STATES & ~PSTAT_STATE_T_HALT,
    PSTAT_STATE_T_HALT | PSTAT_STATE_T_UNCLEAN },
  { 0 }
};

error_t
ps_emit_state (proc_stat_t ps, ps_getter_t getter, int width,
	       ps_stream_t stream)
{
  char *tags;
  int raw_state = G(getter, int)(ps);
  int state = raw_state;
  char buf[20], *p = buf;
  const struct state_shadow *shadow = state_shadows;

  while (shadow->states)
    {
      if (raw_state & shadow->states)
	state &= ~shadow->shadow;
      shadow++;
    }

  for (tags = proc_stat_state_tags
       ; state != 0 && *tags != '\0'
       ; state >>= 1, tags++)
    if (state & 1)
      *p++ = *tags;

  *p = '\0';

  return ps_stream_write_field (stream, buf, width);
}

error_t
ps_emit_wait (proc_stat_t ps, ps_getter_t getter, int width,
	      ps_stream_t stream)
{
  int rpc;
  char *wait;
  char buf[80];

  G(getter, void)(ps, &wait, &rpc);

  if (wait == 0)
    return ps_stream_write_field (stream, "?", width);
  else if (*wait == 0)
    return ps_stream_write_field (stream, "-", width);
  else if (strcmp (wait, "kernel") == 0)
    /* A syscall.  RPC is actually the syscall number.  */
    {
      extern char *get_syscall_name (int num);
      char *name = get_syscall_name (rpc);
      if (! name)
	{
	  sprintf (buf, "syscall:%d", -rpc);
	  name = buf;
	}
      return ps_stream_write_trunc_field (stream, name, width);
    }
  else if (rpc)
    /* An rpc (with msg id RPC); WAIT describes the dest port.  */
    {
      char port_name_buf[20];
      extern char *get_rpc_name (mach_msg_id_t num);
      char *name = get_rpc_name (rpc);

      /* See if we should give a more useful name for the port.  */
      if (strcmp (wait, "init#0") == 0)
	wait = "cwdir";		/* Current directory */
      else if (strcmp (wait, "init#1") == 0)
	wait = "crdir";		/* Root directory */
      else if (strcmp (wait, "init#2") == 0)
	wait = "auth";		/* Auth port */
      else if (strcmp (wait, "init#3") == 0)
	wait = "proc";		/* Proc port */
      else if (strcmp (wait, "init#4") == 0)
	wait = "bootstrap";	/* Bootstrap port */
      else
	/* See if we can shorten the name to fit better.  We happen know that
	   all currently returned keys are unique in the first character. */
	{
	  char *sep = index (wait, '#');
	  if (sep && sep > wait)
	    {
	      snprintf (port_name_buf, sizeof port_name_buf,
			"%c%s", wait[0], sep);
	      wait = port_name_buf;
	    }
	}

      if (name)
	snprintf (buf, sizeof buf, "%s:%s", wait, name);
      else
	snprintf (buf, sizeof buf, "%s:%d", wait, rpc);

      return ps_stream_write_trunc_field (stream, buf, width);
    }
  else
    return ps_stream_write_field (stream, wait, width);
}
/* ---------------------------------------------------------------- */
/* comparison functions */

/* Evaluates CALL if both s1 & s2 are non-NULL, and otherwise returns -1, 0,
   or 1 ala strcmp, considering NULL to be less than non-NULL.  */
#define GUARDED_CMP(s1, s2, call) \
  ((s1) == NULL ? (((s2) == NULL) ? 0 : -1) : ((s2) == NULL ? 1 : (call)))

int 
ps_cmp_ints(proc_stat_t ps1, proc_stat_t ps2, ps_getter_t getter)
{
  int (*gf)() = G(getter, int);
  int v1 = gf(ps1), v2 = gf(ps2);
  return v1 == v2 ? 0 : v1 < v2 ? -1 : 1;
}

int 
ps_cmp_floats(proc_stat_t ps1, proc_stat_t ps2, ps_getter_t getter)
{
  float (*gf)() = G(getter, float);
  float v1 = gf(ps1), v2 = gf(ps2);
  return v1 == v2 ? 0 : v1 < v2 ? -1 : 1;
}

int 
ps_cmp_uids(proc_stat_t ps1, proc_stat_t ps2, ps_getter_t getter)
{
  ps_user_t (*gf)() = G(getter, ps_user_t);
  ps_user_t u1 = gf(ps1), u2 = gf(ps2);
  return (u1 ? ps_user_uid (u1) : -1) - (u2 ? ps_user_uid (u2) : -1);
}

int 
ps_cmp_unames(proc_stat_t ps1, proc_stat_t ps2, ps_getter_t getter)
{
  ps_user_t (*gf)() = G(getter, ps_user_t);
  ps_user_t u1 = gf(ps1), u2 = gf(ps2);
  struct passwd *pw1 = u1 ? ps_user_passwd (u1) : 0;
  struct passwd *pw2 = u2 ? ps_user_passwd (u2) : 0;
  return GUARDED_CMP (pw1, pw2, strcmp (pw1->pw_name, pw2->pw_name));
}

int 
ps_cmp_strings(proc_stat_t ps1, proc_stat_t ps2, ps_getter_t getter)
{
  void (*gf)() = G(getter, void);
  char *s1, *s2;
  int s1len, s2len;

  /* Get both strings */
  gf(ps1, &s1, &s1len);
  gf(ps2, &s2, &s2len);

  return GUARDED_CMP(s1, s2, strncmp(s1, s2, MIN(s1len, s2len)));
}

int
ps_cmp_times (proc_stat_t ps1, proc_stat_t ps2, ps_getter_t getter)
{
  void (*g)() = G(getter, void);
  struct timeval tv1, tv2;

  g (ps1, &tv1);
  g (ps2, &tv2);

  return
    tv1.tv_sec > tv2.tv_sec ? 1
      : tv1.tv_sec < tv2.tv_sec ? -1
	: tv1.tv_usec > tv2.tv_usec ? 1
	  : tv2.tv_usec < tv2.tv_usec ? -1
	    : 0;
}

/* ---------------------------------------------------------------- */
/* `Nominal' functions -- return true for `unexciting' values.  */

/* For many things, zero is not so interesting.  */
bool
ps_nominal_zint (proc_stat_t ps, ps_getter_t getter)
{
  return G(getter, int)(ps) == 0;
}

/* Priorities are similar, but have to be converted to the unix nice scale
   first.  */
bool
ps_nominal_pri (proc_stat_t ps, ps_getter_t getter)
{
  return MACH_PRIORITY_TO_NICE(G(getter, int)(ps)) == 0;
}

/* Hurd processes usually have 2 threads;  XXX is there someplace we get get
   this number from?  */
bool
ps_nominal_nth (proc_stat_t ps, ps_getter_t getter)
{
  return G(getter, int)(ps) == 2;
}

static int own_uid = -2;	/* -1 means no uid at all.  */

/* A user is nominal if it's the current user.  */
bool 
ps_nominal_user (proc_stat_t ps, ps_getter_t getter)
{
  ps_user_t u = G(getter, ps_user_t)(ps);
  if (own_uid == -2)
    own_uid = getuid();
  return own_uid >= 0 && u && u->uid == own_uid;
}

/* A uid is nominal if it's that of the current user.  */
bool 
ps_nominal_uid (proc_stat_t ps, ps_getter_t getter)
{
  uid_t uid = G(getter, uid_t)(ps);
  if (own_uid == -2)
    own_uid = getuid ();
  return own_uid >= 0 && uid == own_uid;
}

/* ---------------------------------------------------------------- */

ps_fmt_spec_t 
ps_fmt_specs_find (ps_fmt_specs_t specs, char *name)
{
  if (specs)			/* Allow NULL to make recursion more handy. */
    {
      ps_fmt_spec_t s = specs->specs;

      while (! ps_fmt_spec_is_end (s))
	{
	  char *alias = index (s->name, '=');
	  if (alias)
	    {
	      unsigned name_len = strlen (name);

	      if (name_len == alias - s->name
		  && strncasecmp (name, s->name, name_len) == 0)
		/* S is an alias, lookup what it refs to. */
		{
		  ps_fmt_spec_t src; /* What S is an alias to.  */

		  ++alias;	/* Point at the alias name.  */

		  if (strcasecmp (name, alias) == 0)
		    /* An alias to the same name (useful to just change some
		       property) -- start looking up in the parent.  */
		    src = ps_fmt_specs_find (specs->parent, alias);
		  else
		    src = ps_fmt_specs_find (specs, alias);

		  if (! src)
		    return 0;

		  /* Copy fields into the alias entry.  */
		  if (! s->title && src->title)
		    s->title = src->title;
		  if (! s->width && src->width)
		    s->width = src->width;
		  if (! s->getter && src->getter)
		    s->getter = src->getter;
		  if (! s->output_fn && src->output_fn)
		    s->output_fn = src->output_fn;
		  if (! s->cmp_fn && src->cmp_fn)
		    s->cmp_fn = src->cmp_fn;
		  if (! s->nominal_fn && src->nominal_fn)
		    s->nominal_fn = src->nominal_fn;

		  /* Now make this not an alias.  */
		  *--alias = '\0';

		  return s;
		}
	    }
	  else
	    if (strcasecmp (s->name, name) == 0)
	      return s;
	  s++;
	}

      /* Try again with our parent.  */
      return ps_fmt_specs_find (specs->parent, name);
    }
  else
    return 0;
}

/* ---------------------------------------------------------------- */

static const struct ps_fmt_spec specs[] =
{
  {"PID",	0,	-5,
   &ps_pid_getter,	   ps_emit_int,	    ps_cmp_ints,   0},
  {"TH#",	0,	-2,
   &ps_thread_index_getter,ps_emit_int,	    ps_cmp_ints,   0},
  {"PPID",	0,	-5,
   &ps_ppid_getter,	   ps_emit_int,     ps_cmp_ints,   0},
  {"UID",	0,	-4,
   &ps_owner_uid_getter,   ps_emit_uid,	    ps_cmp_ints,   ps_nominal_uid},
  {"User",	0,	8,
   &ps_owner_getter,	   ps_emit_uname,   ps_cmp_unames, ps_nominal_user},
  {"NTh",	0,	-2,
   &ps_num_threads_getter, ps_emit_int,	    ps_cmp_ints,   ps_nominal_nth},
  {"PGrp",	0,	-5,
   &ps_pgrp_getter,	   ps_emit_int,	    ps_cmp_ints,   0},
  {"Sess",	0,	-5,
   &ps_session_getter,     ps_emit_int,     ps_cmp_ints,   0},
  {"LColl",	0,	-5,
   &ps_login_col_getter,   ps_emit_int,     ps_cmp_ints,   0},
  {"Args",	0,	0,
   &ps_args_getter,	   ps_emit_string0, ps_cmp_strings,0},
  {"Arg0",	0,	0,
   &ps_args_getter,	   ps_emit_string,  ps_cmp_strings,0},
  {"Time",	0,	-8,
   &ps_tot_time_getter,    ps_emit_seconds, ps_cmp_times,  0},
  {"UTime",	0,	-8,
   &ps_usr_time_getter,    ps_emit_seconds, ps_cmp_times,  0},
  {"STime",	0,	-8,
   &ps_sys_time_getter,    ps_emit_seconds, ps_cmp_times,  0},
  {"VSize",	0,	-5,
   &ps_vsize_getter,	   ps_emit_nice_int,ps_cmp_ints,   0},
  {"RSize",	0,	-5,
   &ps_rsize_getter,	   ps_emit_nice_int,ps_cmp_ints,   0},
  {"Pri",	0,	-3,
   &ps_cur_priority_getter,ps_emit_priority,ps_cmp_ints,   ps_nominal_pri},
  {"BPri",	0,	-3,
   &ps_base_priority_getter,ps_emit_priority,ps_cmp_ints,  ps_nominal_pri},
  {"MPri",	0,	-3,
   &ps_max_priority_getter,ps_emit_priority,ps_cmp_ints,   ps_nominal_pri},
  {"%Mem",	0,	-4,
   &ps_rmem_frac_getter,   ps_emit_percent, ps_cmp_floats, 0},
  {"%CPU",	0,	-4,
   &ps_cpu_frac_getter,    ps_emit_percent, ps_cmp_floats, 0},
  {"State",	0,	4,
   &ps_state_getter,	   ps_emit_state,   0,   	   0},
  {"Wait",	0,	10,
   &ps_wait_getter,        ps_emit_wait,    0,		   0},
  {"Sleep",	0,	-2,
   &ps_sleep_getter,	   ps_emit_int,	    ps_cmp_ints,   ps_nominal_zint},
  {"Susp",	0,	-2,
   &ps_susp_count_getter,  ps_emit_int,	    ps_cmp_ints,   ps_nominal_zint},
  {"PSusp",	0,	-2,
   &ps_proc_susp_count_getter, ps_emit_int, ps_cmp_ints,   ps_nominal_zint},
  {"TSusp",	0,	-2,
   &ps_thread_susp_count_getter, ps_emit_int,ps_cmp_ints,  ps_nominal_zint},
  {"TTY",	0,	-2,
   &ps_tty_getter,	   ps_emit_tty_name,ps_cmp_strings,0},
  {"PgFlts",	0,	-5,
   &ps_page_faults_getter, ps_emit_int,	    ps_cmp_ints,   ps_nominal_zint},
  {"COWFlts",	0,	-5,
   &ps_cow_faults_getter,  ps_emit_int,	    ps_cmp_ints,   ps_nominal_zint},
  {"PgIns",	0,	-5,
   &ps_pageins_getter,     ps_emit_int,	    ps_cmp_ints,   ps_nominal_zint},
  {"MsgIn",	0,	-5,
   &ps_msgs_rcvd_getter,   ps_emit_int,	    ps_cmp_ints,   ps_nominal_zint},
  {"MsgOut",	0,	-5,
   &ps_msgs_sent_getter,   ps_emit_int,	    ps_cmp_ints,   ps_nominal_zint},
  {"ZFills",	0,	-5,
   &ps_zero_fills_getter,  ps_emit_int,	    ps_cmp_ints,   ps_nominal_zint},
  {0}
};

const struct ps_fmt_specs ps_std_fmt_specs = { (ps_fmt_spec_t)specs, 0 };

/* GNU Hurd standard exec server, main program and server mechanics.
   Copyright (C) 1992, 1993, 1994, 1995 Free Software Foundation, Inc.
   Written by Roland McGrath.

This file is part of the GNU Hurd.

The GNU Hurd is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2, or (at your option)
any later version.

The GNU Hurd is distributed in the hope that it will be useful, 
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with the GNU Hurd; see the file COPYING.  If not, write to
the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.  */

#include "priv.h"
#include <error.h>
#include <hurd/paths.h>
#include <hurd/startup.h>


bfd_arch_info_type host_bfd_arch_info;
bfd host_bfd = { arch_info: &host_bfd_arch_info };
Elf32_Half elf_machine;	/* ELF e_machine for the host.  */

extern error_t bfd_mach_host_arch_mach (host_t host,
					enum bfd_architecture *bfd_arch,
					long int *bfd_machine,
					Elf32_Half *elf_machine);


/* Trivfs hooks.  */
int trivfs_fstype = FSTYPE_MISC;
int trivfs_fsid = 0;
int trivfs_support_read = 1;
int trivfs_support_write = 1;
int trivfs_support_exec = 1;
int trivfs_allow_open = O_READ|O_WRITE|O_EXEC;

struct port_class *trivfs_protid_portclasses[1];
struct port_class *trivfs_cntl_portclasses[1];
int trivfs_protid_nportclasses = 1;
int trivfs_cntl_nportclasses = 1;

struct trivfs_control *fsys;

char *exec_version = "0.0 pre-alpha";
char **save_argv;


static int
exec_demuxer (mach_msg_header_t *inp, mach_msg_header_t *outp)
{
  extern int exec_server (mach_msg_header_t *inp, mach_msg_header_t *outp);
  extern int exec_startup_server (mach_msg_header_t *, mach_msg_header_t *);
  return (exec_startup_server (inp, outp) ||
	  exec_server (inp, outp) ||
	  trivfs_demuxer (inp, outp));
}


static int going_down;

/* Clean up the storage in BOOT, which was never used.  */

void
deadboot (void *p)
{
  struct bootinfo *boot = p;
  size_t i;

  vm_deallocate (mach_task_self (),
		 (vm_address_t) boot->argv, boot->argvlen);
  vm_deallocate (mach_task_self (),
		 (vm_address_t) boot->envp, boot->envplen);

  for (i = 0; i < boot->dtablesize; ++i)
    mach_port_deallocate (mach_task_self (), boot->dtable[i]);
  for (i = 0; i < boot->nports; ++i)
    mach_port_deallocate (mach_task_self (), boot->portarray[i]);
  vm_deallocate (mach_task_self (),
		 (vm_address_t) boot->portarray,
		 boot->nports * sizeof (mach_port_t));
  vm_deallocate (mach_task_self (),
		 (vm_address_t) boot->intarray,
		 boot->nints * sizeof (int));

  if (going_down)
    {
      /* We are not accepting new requests, only listening
	 for exec_startup RPCs from tasks we already started.
	 See if there are any more to be answered.  */
      int count = ports_count_class (execboot_portclass);
      if (count == 0)
	/* No more tasks starting up.  No reason to live.  */
	exit (0);
      ports_enable_class (execboot_portclass);
    }
}


int
main (int argc, char **argv)
{
  error_t err;
  mach_port_t bootstrap;

  save_argv = argv;

  /* Put the Mach kernel's idea of what flavor of machine this is into the
     fake BFD against which architecture compatibility checks are made.  */
  err = bfd_mach_host_arch_mach (mach_host_self (),
				 &host_bfd.arch_info->arch,
				 &host_bfd.arch_info->mach,
				 &elf_machine);
  if (err)
    error (1, err, "Getting host architecture from Mach");

  task_get_bootstrap_port (mach_task_self (), &bootstrap);
  if (bootstrap == MACH_PORT_NULL)
    error (2, 0, "Must be started as a translator");

  /* Fetch our proc server port for easy use.  If we are booting, it is not
     set yet and `getproc' returns MACH_PORT_NULL; we reset PROCSERVER in
     S_exec_init (below).  */
  procserver = getproc ();

  port_bucket = ports_create_bucket ();
  trivfs_cntl_portclasses[0] = ports_create_class (trivfs_clean_cntl, 0);
  trivfs_protid_portclasses[0] = ports_create_class (trivfs_clean_protid, 0);
  execboot_portclass = ports_create_class (deadboot, NULL);

  /* Reply to our parent.  */
  err = trivfs_startup (bootstrap, 0,
			trivfs_cntl_portclasses[0], port_bucket,
			trivfs_protid_portclasses[0], port_bucket,
			&fsys);
  mach_port_deallocate (mach_task_self (), bootstrap);
  if (err)
    error (3, err, "Contacting parent");

  /* Launch.  */
  ports_manage_port_operations_multithread (port_bucket, exec_demuxer,
					    2 * 60 * 1000, 0,
					    0, MACH_PORT_NULL);

  return 0;
}


void
trivfs_modify_stat (struct trivfs_protid *cred, struct stat *st)
{
  st->st_fstype = FSTYPE_MISC;
}

error_t
trivfs_goaway (struct trivfs_control *fsys, int flags)
{
  int count;

  /* Stop new requests.  */
  ports_inhibit_class_rpcs (trivfs_cntl_portclasses[0]);
  ports_inhibit_class_rpcs (trivfs_protid_portclasses[0]);

  /* Are there any extant user ports for the /servers/exec file?  */
  count = ports_count_class (trivfs_protid_portclasses[0]);
  if (count == 0 || (flags & FSYS_GOAWAY_FORCE))
    {
      /* No users.  Disconnect from the filesystem.  */
      mach_port_deallocate (mach_task_self (), fsys->underlying);

      /* Are there remaining exec_startup RPCs to answer?  */
      count = ports_count_class (execboot_portclass);
      if (count == 0)
	/* Nope.  We got no reason to live.  */
	exit (0);

      /* Continue servicing tasks starting up.  */
      ports_enable_class (execboot_portclass);

      /* No more communication with the parent filesystem.  */
      ports_destroy_right (fsys);
      going_down = 1;

      return 0;
    }
  else
    {
      /* We won't go away, so start things going again...  */
      ports_enable_class (trivfs_protid_portclasses[0]);
      ports_resume_class_rpcs (trivfs_cntl_portclasses[0]);
      ports_resume_class_rpcs (trivfs_protid_portclasses[0]);

      return EBUSY;
    }
}

/* Attempt to set the active translator for the exec server so that
   filesystems other than the bootstrap can find it. */
void
set_active_trans ()
{
  file_t execnode;

  execnode = file_name_lookup (_SERVERS_EXEC, O_NOTRANS | O_CREAT, 0666);
  if (execnode == MACH_PORT_NULL)
    return;

  file_set_translator (execnode, 0, FS_TRANS_SET, 0, 0, 0,
		       ports_get_right (fsys), MACH_MSG_TYPE_MAKE_SEND);
  /* Don't deallocate EXECNODE here.  If we drop the last reference,
     a bug in ufs might throw away the active translator. XXX */
}


/* Sent by the bootstrap filesystem after the other essential
   servers have been started up.  */

kern_return_t
S_exec_init (struct trivfs_protid *protid,
	     auth_t auth, process_t proc)
{
  mach_port_t host_priv, dev_master, startup;
  error_t err;

  if (! protid || ! protid->isroot)
    return EPERM;

  _hurd_port_set (&_hurd_ports[INIT_PORT_PROC], proc); /* Consume.  */
  _hurd_port_set (&_hurd_ports[INIT_PORT_AUTH], auth); /* Consume.  */

  /* Do initial setup with the proc server.  */
  _hurd_proc_init (save_argv);

  /* Set the active translator on /servers/exec. */
  set_active_trans ();

  procserver = getproc ();
  
  err = get_privileged_ports (&host_priv, &dev_master);
  if (!err)
    {
      proc_register_version (procserver, host_priv, "exec", HURD_RELEASE,
			     exec_version);
      mach_port_deallocate (mach_task_self (), dev_master);
      err = proc_getmsgport (procserver, 1, &startup);
      if (err)
	{
	  mach_port_deallocate (mach_task_self (), host_priv);
	  host_priv = MACH_PORT_NULL;
	}
    }
  else
    host_priv = MACH_PORT_NULL;
      
  {
    /* Have the proc server notify us when the canonical ints and ports
       change.  */

    struct trivfs_protid *cred;
    err = trivfs_open (fsys, 0, 0, 0, 0, 0, MACH_PORT_NULL, &cred);
    assert_perror (err);

    proc_execdata_notify (procserver, ports_get_right (cred),
			  MACH_MSG_TYPE_MAKE_SEND);
  }

  /* Call startup_essential task last; init assumes we are ready to
     run once we call it. */
  if (host_priv != MACH_PORT_NULL)
    {
      startup_essential_task (startup, mach_task_self (), MACH_PORT_NULL,
			      "exec", host_priv);
      mach_port_deallocate (mach_task_self (), startup);
      mach_port_deallocate (mach_task_self (), host_priv);
    }

  return 0;
}

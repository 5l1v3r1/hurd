/* 
   Copyright (C) 1994, 1999 Free Software Foundation

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

#include "priv.h"

error_t
trivfs_set_atime (struct trivfs_control *cntl)
{
  struct stat st;
  time_value_t atime;
  time_value_t mtime;
  
  io_stat (cntl->underlying, &st);
  mtime.seconds = st.st_mtime;
  mtime.microseconds = st.st_mtime_usec;
  atime.microseconds = -1;
  file_utimes (cntl->underlying, atime, mtime);
  return 0;
}

error_t
trivfs_set_mtime (struct trivfs_control *cntl)
{
  struct stat st;
  time_value_t atime;
  time_value_t mtime;

  io_stat (cntl->underlying, &st);
  atime.seconds = st.st_atime;
  atime.microseconds = st.st_atime_usec;
  mtime.microseconds = -1;
  file_utimes (cntl->underlying, atime, mtime);
  return 0;
}



  

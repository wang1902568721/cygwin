/* dtable.cc: file descriptor support.

   Copyright 1996, 1997, 1998, 1999, 2000, 2001, 2002 Red Hat, Inc.

This file is part of Cygwin.

This software is a copyrighted work licensed under the terms of the
Cygwin license.  Please consult the file "CYGWIN_LICENSE" for
details. */

#define  __INSIDE_CYGWIN_NET__

#include "winsup.h"
#include <errno.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/cygwin.h>
#include <assert.h>
#include <ntdef.h>
#include <winnls.h>

#define USE_SYS_TYPES_FD_SET
#include <winsock.h>
#include "pinfo.h"
#include "cygerrno.h"
#include "perprocess.h"
#include "security.h"
#include "fhandler.h"
#include "path.h"
#include "dtable.h"
#include "cygheap.h"
#include "ntdll.h"

static const NO_COPY DWORD std_consts[] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE,
					   STD_ERROR_HANDLE};

static const char *handle_to_fn (HANDLE, char *);

static const char NO_COPY unknown_file[] = "some disk file";

/* Set aside space for the table of fds */
void
dtable_init (void)
{
  if (!cygheap->fdtab.size)
    cygheap->fdtab.extend (NOFILE_INCR);
}

void __stdcall
set_std_handle (int fd)
{
  if (fd == 0)
    SetStdHandle (std_consts[fd], cygheap->fdtab[fd]->get_handle ());
  else if (fd <= 2)
    SetStdHandle (std_consts[fd], cygheap->fdtab[fd]->get_output_handle ());
}

void
dtable::dec_console_fds ()
{
  if (console_fds > 0 && !--console_fds &&
      myself->ctty != TTY_CONSOLE && !check_pty_fds())
    FreeConsole ();
}

int
dtable::extend (int howmuch)
{
  int new_size = size + howmuch;
  fhandler_base **newfds;

  if (howmuch <= 0)
    return 0;

  /* Try to allocate more space for fd table. We can't call realloc ()
     here to preserve old table if memory allocation fails */

  if (!(newfds = (fhandler_base **) ccalloc (HEAP_ARGV, new_size, sizeof newfds[0])))
    {
      debug_printf ("calloc failed");
      return 0;
    }
  if (fds)
    {
      memcpy (newfds, fds, size * sizeof (fds[0]));
      cfree (fds);
    }

  size = new_size;
  fds = newfds;
  debug_printf ("size %d, fds %p", size, fds);
  return 1;
}

void
dtable::get_debugger_info ()
{
  if (being_debugged ())
    {
      char std[3][sizeof ("/dev/ttyNNNN")];
      std[0][0] = std[1][0] = std [2][0] = '\0';
      char buf[sizeof ("cYgstd %x") + 32];
      sprintf (buf, "cYgstd %x %x %x", (unsigned) &std, sizeof (std[0]), 3);
      OutputDebugString (buf);
      for (int i = 0; i < 3; i++)
	if (std[i][0])
	  {
	    path_conv pc;
	    HANDLE h = GetStdHandle (std_consts[i]);
	    fhandler_base *fh = build_fhandler_from_name (i, std[i], NULL, pc);
	    if (!fh)
	      continue;
	    if (!fh->open (&pc, (i ? O_WRONLY : O_RDONLY) | O_BINARY, 0777))
	      release (i);
	    else
	      CloseHandle (h);
	  }
    }
}

/* Initialize the file descriptor/handle mapping table.
   This function should only be called when a cygwin function is invoked
   by a non-cygwin function, i.e., it should only happen very rarely. */

void
dtable::stdio_init ()
{
  extern void set_console_ctty ();
  /* Set these before trying to output anything from strace.
     Also, always set them even if we're to pick up our parent's fds
     in case they're missed.  */

  if (myself->ppid_handle || ISSTATE (myself, PID_CYGPARENT))
    return;

  HANDLE in = GetStdHandle (STD_INPUT_HANDLE);
  HANDLE out = GetStdHandle (STD_OUTPUT_HANDLE);
  HANDLE err = GetStdHandle (STD_ERROR_HANDLE);

  init_std_file_from_handle (0, in);

  /* STD_ERROR_HANDLE has been observed to be the same as
     STD_OUTPUT_HANDLE.  We need separate handles (e.g. using pipes
     to pass data from child to parent).  */
  if (out == err)
    {
      /* Since this code is not invoked for forked tasks, we don't have
	 to worry about the close-on-exec flag here.  */
      if (!DuplicateHandle (hMainProc, out, hMainProc, &err, 0,
			     1, DUPLICATE_SAME_ACCESS))
	{
	  /* If that fails, do this as a fall back.  */
	  err = out;
	  system_printf ("couldn't make stderr distinct from stdout");
	}
    }

  init_std_file_from_handle (1, out);
  init_std_file_from_handle (2, err);
  /* Assign the console as the controlling tty for this process if we actually
     have a console and no other controlling tty has been assigned. */
  if (myself->ctty < 0 && GetConsoleCP () > 0)
    set_console_ctty ();
}

int
dtable::find_unused_handle (int start)
{
  AssertResourceOwner (LOCK_FD_LIST, READ_LOCK);
  do
    {
      for (size_t i = start; i < size; i++)
	/* See if open -- no need for overhead of not_open */
	if (fds[i] == NULL)
	  return i;
    }
  while (extend (NOFILE_INCR));
  return -1;
}

void
dtable::release (int fd)
{
  if (!not_open (fd))
    {
      switch (fds[fd]->get_device ())
	{
	case FH_SOCKET:
	  dec_need_fixup_before ();
	  break;
	case FH_CONSOLE:
	  dec_console_fds ();
	  break;
	}
      delete fds[fd];
      fds[fd] = NULL;
    }
}

extern "C" int
cygwin_attach_handle_to_fd (char *name, int fd, HANDLE handle, mode_t bin,
			    DWORD myaccess)
{
  if (fd == -1)
    fd = cygheap->fdtab.find_unused_handle ();
  path_conv pc;
  fhandler_base *res = cygheap->fdtab.build_fhandler_from_name (fd, name, handle,
								pc);
  res->init (handle, myaccess, bin ?: pc.binmode ());
  return fd;
}

void
dtable::init_std_file_from_handle (int fd, HANDLE handle)
{
  const char *name = NULL;
  CONSOLE_SCREEN_BUFFER_INFO buf;
  struct sockaddr sa;
  int sal = sizeof (sa);
  DCB dcb;
  unsigned bin = O_BINARY;
  device dev;

  dev.devn = 0;		/* FIXME: device */
  first_fd_for_open = 0;

  if (!not_open (fd))
    return;

  SetLastError (0);
  DWORD ft = GetFileType (handle);
  if (ft != FILE_TYPE_UNKNOWN || GetLastError () != ERROR_INVALID_HANDLE)
    {
      /* See if we can consoleify it */
      if (GetConsoleScreenBufferInfo (handle, &buf))
	{
	  if (ISSTATE (myself, PID_USETTY))
	    dev.parse ("/dev/tty");
	  else
	    dev = *console_dev;
	}
      else if (GetNumberOfConsoleInputEvents (handle, (DWORD *) &buf))
	{
	  if (ISSTATE (myself, PID_USETTY))
	    dev.parse ("/dev/tty");
	  else
	    dev = *console_dev;
	}
      else if (ft == FILE_TYPE_PIPE)
	{
	  if (fd == 0)
	    dev = *piper_dev;
	  else
	    dev = *pipew_dev;
	}
      else if (wsock_started && getpeername ((SOCKET) handle, &sa, &sal) == 0)
	dev = *socket_dev;
      else if (GetCommState (handle, &dcb))
	dev.parse ("/dev/ttyS0");
      else
	{
	  name = handle_to_fn (handle, (char *) alloca (MAX_PATH + 100));
	  bin = 0;
	}
    }

  if (!name && !dev)
    fds[fd] = NULL;
  else
    {
      path_conv pc;
      fhandler_base *fh;

      if (dev)
	fh = build_fhandler (fd, dev);
      else
	fh = build_fhandler_from_name (fd, name, handle, pc);

      if (!bin)
	{
	  bin = fh->get_default_fmode (O_RDWR);
	  if (bin)
	    /* nothing */;
	  else if (dev)
	    bin = O_BINARY;
	  else if (name != unknown_file)
	    bin = pc.binmode ();
	}

      fh->init (handle, GENERIC_READ | GENERIC_WRITE, bin);
      set_std_handle (fd);
      paranoid_printf ("fd %d, handle %p", fd, handle);
    }
}

fhandler_base *
dtable::build_fhandler_from_name (int fd, const char *name, HANDLE handle,
				  path_conv& pc, unsigned opt, suffix_info *si)
{
  pc.check (name, opt | PC_NULLEMPTY | PC_FULL | PC_POSIX, si);
  if (pc.error)
    {
      set_errno (pc.error);
      return NULL;
    }

  if (!pc.exists () && handle)
    pc.fillin (handle);

  fhandler_base *fh = build_fhandler (fd, pc.dev,
				      pc.return_and_clear_normalized_path (),
				      pc);
  return fh;
}

fhandler_base *
dtable::build_fhandler (int fd, const device& dev, const char *unix_name,
			const char *win32_name)
{
  return build_fhandler (fd, dev, cstrdup (unix_name), win32_name);
}

#define cnew(name) new ((void *) ccalloc (HEAP_FHANDLER, 1, sizeof (name))) name
fhandler_base *
dtable::build_fhandler (int fd, const device& dev, char *unix_name,
			const char *win32_name)
{
  fhandler_base *fh = NULL;

  if (dev.upper)
    switch (dev.major)
      {
      case DEV_TTYS_MAJOR:
	fh = cnew (fhandler_tty_slave) ();
	break;
      case DEV_TTYM_MAJOR:
	fh = cnew (fhandler_tty_master) ();
	break;
      case DEV_CYGDRIVE_MAJOR:
	fh = cnew (fhandler_cygdrive) ();
	break;
      case DEV_FLOPPY_MAJOR:
      case DEV_CDROM_MAJOR:
      case DEV_SD_MAJOR:
      case DEV_RAWDRIVE_MAJOR:
	fh = cnew (fhandler_dev_floppy) ();
	break;
      case DEV_TAPE_MAJOR:
	fh = cnew (fhandler_dev_tape) ();
	break;
      }
  else
    switch (dev)
      {
      case FH_CONSOLE:
      case FH_CONIN:
      case FH_CONOUT:
	if ((fh = cnew (fhandler_console) ()))
	  inc_console_fds ();
	break;
      case FH_PTYM:
	fh = cnew (fhandler_pty_master) ();
	break;
      case FH_WINDOWS:
	fh = cnew (fhandler_windows) ();
	break;
      case FH_SERIAL:
	fh = cnew (fhandler_serial) ();
	break;
      case FH_PIPE:
      case FH_PIPER:
      case FH_PIPEW:
	fh = cnew (fhandler_pipe) ();
	break;
      case FH_SOCKET:
	if ((fh = cnew (fhandler_socket) ()))
	  inc_need_fixup_before ();
	break;
      case FH_FS:
	fh = cnew (fhandler_disk_file) ();
	break;
      case FH_NULL:
	fh = cnew (fhandler_dev_null) ();
	break;
      case FH_ZERO:
	fh = cnew (fhandler_dev_zero) ();
	break;
      case FH_RANDOM:
      case FH_URANDOM:
	fh = cnew (fhandler_dev_random) ();
	break;
      case FH_MEM:
      case FH_PORT:
	fh = cnew (fhandler_dev_mem) ();
	break;
      case FH_CLIPBOARD:
	fh = cnew (fhandler_dev_clipboard) ();
	break;
      case FH_OSS_DSP:
	fh = cnew (fhandler_dev_dsp) ();
	break;
      case FH_PROC:
	fh = cnew (fhandler_proc) ();
	break;
      case FH_REGISTRY:
	fh = cnew (fhandler_registry) ();
	break;
      case FH_PROCESS:
	fh = cnew (fhandler_process) ();
	break;
    }

  if (!fh)
    api_fatal ("internal error -- unknown device - %p, '%s'", (int) dev, dev.name);

  char w32buf[MAX_PATH + 1];
  if (!unix_name)
    {
      if (!win32_name && dev.fmt && *dev.fmt)
	{
	  sprintf (w32buf, dev.fmt, dev.minor);
	  win32_name = w32buf;
	}
      if (win32_name)
	{
	  unix_name = cstrdup (w32buf);
	  for (char *p = strchr (unix_name, '\\'); p; p = strchr (p + 1, '\\'))
	    *p = '/';
	}
    }

  fh->dev = dev;
  if (unix_name)
    {
      if (!win32_name)
	{
	  /* FIXME: ? Should we call win32_device_name here?
	     It seems like overkill, but... */
	  win32_name = strcpy (w32buf, unix_name);
	  for (char *p = w32buf; (p = strchr (p, '/')); p++)
	    *p = '\\';
	}
      fh->set_name (unix_name, win32_name);
    }
  debug_printf ("fd %d, fh %p", fd, fh);
  return fd >= 0 ? (fds[fd] = fh) : fh;
}

fhandler_base *
dtable::dup_worker (fhandler_base *oldfh)
{
  fhandler_base *newfh = build_fhandler (-1, oldfh->dev);
  *newfh = *oldfh;
  newfh->set_io_handle (NULL);
  if (oldfh->dup (newfh))
    {
      cfree (newfh);
      newfh = NULL;
      return NULL;
    }

  newfh->set_close_on_exec_flag (0);
  MALLOC_CHECK;
  debug_printf ("duped '%s' old %p, new %p", oldfh->get_name (), oldfh->get_io_handle (), newfh->get_io_handle ());
  return newfh;
}

int
dtable::dup2 (int oldfd, int newfd)
{
  int res = -1;
  fhandler_base *newfh = NULL;	// = NULL to avoid an incorrect warning

  MALLOC_CHECK;
  debug_printf ("dup2 (%d, %d)", oldfd, newfd);
  SetResourceLock (LOCK_FD_LIST, WRITE_LOCK | READ_LOCK, "dup");

  if (not_open (oldfd))
    {
      syscall_printf ("fd %d not open", oldfd);
      set_errno (EBADF);
      goto done;
    }

  if (newfd < 0)
    {
      syscall_printf ("new fd out of bounds: %d", newfd);
      set_errno (EBADF);
      goto done;
    }

  if (newfd == oldfd)
    {
      res = 0;
      goto done;
    }

  if ((newfh = dup_worker (fds[oldfd])) == NULL)
    {
      res = -1;
      goto done;
    }

  debug_printf ("newfh->io_handle %p, oldfh->io_handle %p",
		newfh->get_io_handle (), fds[oldfd]->get_io_handle ());

  if (!not_open (newfd))
    close (newfd);
  else if ((size_t) newfd < size)
    /* nothing to do */;
  else if (find_unused_handle (newfd) < 0)
    {
      newfh->close ();
      res = -1;
      goto done;
    }

  fds[newfd] = newfh;

  if ((res = newfd) <= 2)
    set_std_handle (res);

done:
  MALLOC_CHECK;
  ReleaseResourceLock (LOCK_FD_LIST, WRITE_LOCK | READ_LOCK, "dup");
  syscall_printf ("%d = dup2 (%d, %d)", res, oldfd, newfd);

  return res;
}

select_record *
dtable::select_read (int fd, select_record *s)
{
  if (not_open (fd))
    {
      set_errno (EBADF);
      return NULL;
    }
  fhandler_base *fh = fds[fd];
  s = fh->select_read (s);
  s->fd = fd;
  s->fh = fh;
  s->saw_error = 0;
  debug_printf ("%s fd %d", fh->get_name (), fd);
  return s;
}

select_record *
dtable::select_write (int fd, select_record *s)
{
  if (not_open (fd))
    {
      set_errno (EBADF);
      return NULL;
    }
  fhandler_base *fh = fds[fd];
  s = fh->select_write (s);
  s->fd = fd;
  s->fh = fh;
  s->saw_error = 0;
  debug_printf ("%s fd %d", fh->get_name (), fd);
  return s;
}

select_record *
dtable::select_except (int fd, select_record *s)
{
  if (not_open (fd))
    {
      set_errno (EBADF);
      return NULL;
    }
  fhandler_base *fh = fds[fd];
  s = fh->select_except (s);
  s->fd = fd;
  s->fh = fh;
  s->saw_error = 0;
  debug_printf ("%s fd %d", fh->get_name (), fd);
  return s;
}

/* Function to walk the fd table after an exec and perform
   per-fhandler type fixups. */
void
dtable::fixup_before_fork (DWORD target_proc_id)
{
  SetResourceLock (LOCK_FD_LIST, WRITE_LOCK | READ_LOCK, "fixup_before_fork");
  fhandler_base *fh;
  for (size_t i = 0; i < size; i++)
    if ((fh = fds[i]) != NULL)
      {
	debug_printf ("fd %d (%s)", i, fh->get_name ());
	fh->fixup_before_fork_exec (target_proc_id);
      }
  ReleaseResourceLock (LOCK_FD_LIST, WRITE_LOCK | READ_LOCK, "fixup_before_fork");
}

void
dtable::fixup_before_exec (DWORD target_proc_id)
{
  SetResourceLock (LOCK_FD_LIST, WRITE_LOCK | READ_LOCK, "fixup_before_exec");
  fhandler_base *fh;
  for (size_t i = 0; i < size; i++)
    if ((fh = fds[i]) != NULL && !fh->get_close_on_exec ())
      {
	debug_printf ("fd %d (%s)", i, fh->get_name ());
	fh->fixup_before_fork_exec (target_proc_id);
      }
  ReleaseResourceLock (LOCK_FD_LIST, WRITE_LOCK | READ_LOCK, "fixup_before_exec");
}

void
dtable::set_file_pointers_for_exec ()
{
  SetResourceLock (LOCK_FD_LIST, WRITE_LOCK | READ_LOCK, "set_file_pointers_for_exec");
  fhandler_base *fh;
  for (size_t i = 0; i < size; i++)
    if ((fh = fds[i]) != NULL && fh->get_flags () & O_APPEND)
      SetFilePointer (fh->get_handle (), 0, 0, FILE_END);
  ReleaseResourceLock (LOCK_FD_LIST, WRITE_LOCK | READ_LOCK, "fixup_before_exec");
}

void
dtable::fixup_after_exec (HANDLE parent)
{
  first_fd_for_open = 0;
  fhandler_base *fh;
  for (size_t i = 0; i < size; i++)
    if ((fh = fds[i]) != NULL)
      {
	fh->clear_readahead ();
	if (fh->get_close_on_exec ())
	  release (i);
	else
	  {
	    fh->fixup_after_exec (parent);
	    if (i == 0)
	      SetStdHandle (std_consts[i], fh->get_io_handle ());
	    else if (i <= 2)
	      SetStdHandle (std_consts[i], fh->get_output_handle ());
	  }
      }
}

void
dtable::fixup_after_fork (HANDLE parent)
{
  fhandler_base *fh;
  for (size_t i = 0; i < size; i++)
    if ((fh = fds[i]) != NULL)
      {
	if (fh->get_close_on_exec () || fh->get_need_fork_fixup ())
	  {
	    debug_printf ("fd %d (%s)", i, fh->get_name ());
	    fh->fixup_after_fork (parent);
	  }
	if (i == 0)
	  SetStdHandle (std_consts[i], fh->get_io_handle ());
	else if (i <= 2)
	  SetStdHandle (std_consts[i], fh->get_output_handle ());
      }
}

int
dtable::vfork_child_dup ()
{
  fhandler_base **newtable;
  SetResourceLock (LOCK_FD_LIST, WRITE_LOCK | READ_LOCK, "dup");
  newtable = (fhandler_base **) ccalloc (HEAP_ARGV, size, sizeof (fds[0]));
  int res = 1;

  /* Remove impersonation */
  if (cygheap->user.issetuid ())
    RevertToSelf ();

  for (size_t i = 0; i < size; i++)
    if (not_open (i))
      continue;
    else if ((newtable[i] = dup_worker (fds[i])) != NULL)
      newtable[i]->set_close_on_exec (fds[i]->get_close_on_exec ());
    else
      {
	res = 0;
	set_errno (EBADF);
	goto out;
      }

  fds_on_hold = fds;
  fds = newtable;

out:
  /* Restore impersonation */
  if (cygheap->user.issetuid ())
    ImpersonateLoggedOnUser (cygheap->user.token);

  ReleaseResourceLock (LOCK_FD_LIST, WRITE_LOCK | READ_LOCK, "dup");
  return 1;
}

void
dtable::vfork_parent_restore ()
{
  SetResourceLock (LOCK_FD_LIST, WRITE_LOCK | READ_LOCK, "restore");

  close_all_files ();
  fhandler_base **deleteme = fds;
  assert (fds_on_hold != NULL);
  fds = fds_on_hold;
  fds_on_hold = NULL;
  cfree (deleteme);

  ReleaseResourceLock (LOCK_FD_LIST, WRITE_LOCK | READ_LOCK, "restore");
  return;
}

void
dtable::vfork_child_fixup ()
{
  if (!fds_on_hold)
    return;
  debug_printf ("here");
  fhandler_base **saveme = fds;
  fds = fds_on_hold;

  fhandler_base *fh;
  for (int i = 0; i < (int) cygheap->fdtab.size; i++)
    if ((fh = cygheap->fdtab[i]) != NULL)
      {
	fh->clear_readahead ();
	if (fh->get_close_on_exec ())
	  release (i);
	else
	  {
	    fh->close ();
	    cygheap->fdtab.release (i);
	  }
      }

  fds = saveme;
  cfree (fds_on_hold);
  fds_on_hold = NULL;

  return;
}

#define DEVICE_PREFIX "\\device\\"
#define DEVICE_PREFIX_LEN sizeof (DEVICE_PREFIX) - 1
#define REMOTE "\\Device\\LanmanRedirector\\"
#define REMOTE_LEN sizeof (REMOTE) - 1

static const char *
handle_to_fn (HANDLE h, char *posix_fn)
{
  OBJECT_NAME_INFORMATION *ntfn;
  char fnbuf[32768];

  memset (fnbuf, 0, sizeof (fnbuf));
  ntfn = (OBJECT_NAME_INFORMATION *) fnbuf;
  ntfn->Name.MaximumLength = sizeof (fnbuf) - sizeof (*ntfn);
  ntfn->Name.Buffer = (WCHAR *) (ntfn + 1);

  DWORD res = NtQueryObject (h, ObjectNameInformation, ntfn, sizeof (fnbuf), NULL);

  if (res)
    {
      strcpy (posix_fn, unknown_file);
      debug_printf ("NtQueryObject failed");
      return unknown_file;
    }

  // NT seems to do this on an unopened file
  if (!ntfn->Name.Buffer)
    {
      debug_printf ("nt->Name.Buffer == NULL");
      return NULL;
    }

  ntfn->Name.Buffer[ntfn->Name.Length / sizeof (WCHAR)] = 0;

  char win32_fn[MAX_PATH + 100];
  sys_wcstombs (win32_fn, ntfn->Name.Buffer, ntfn->Name.Length);
  debug_printf ("nt name '%s'", win32_fn);
  if (!strncasematch (win32_fn, DEVICE_PREFIX, DEVICE_PREFIX_LEN)
      || !QueryDosDevice (NULL, fnbuf, sizeof (fnbuf)))
    return strcpy (posix_fn, win32_fn);

  char *p = strchr (win32_fn + DEVICE_PREFIX_LEN, '\\');
  if (!p)
    p = strchr (win32_fn + DEVICE_PREFIX_LEN, '\0');

  int n = p - win32_fn;
  int maxmatchlen = 0;
  char *maxmatchdos = NULL;
  for (char *s = fnbuf; *s; s = strchr (s, '\0') + 1)
    {
      char device[MAX_PATH + 10];
      device[MAX_PATH + 9] = '\0';
      if (strchr (s, ':') == NULL)
	continue;
      if (!QueryDosDevice (s, device, sizeof (device) - 1))
	continue;
      char *q = strrchr (device, ';');
      if (q)
	{
	  char *r = strchr (q, '\\');
	  if (r)
	    strcpy (q, r + 1);
	}
      int devlen = strlen (device);
      if (device[devlen - 1] == '\\')
	device[--devlen] = '\0';
      if (devlen < maxmatchlen)
	continue;
      if (!strncasematch (device, win32_fn, devlen) ||
	  (win32_fn[devlen] != '\0' && win32_fn[devlen] != '\\'))
	continue;
      maxmatchlen = devlen;
      maxmatchdos = s;
      debug_printf ("current match '%s'", device);
    }

  char *w32 = win32_fn;
  if (maxmatchlen)
    {
      n = strlen (maxmatchdos);
      if (maxmatchdos[n - 1] == '\\')
	n--;
      w32 += maxmatchlen - n;
      memcpy (w32, maxmatchdos, n);
      w32[n] = '\\';
    }
  else if (strncasematch (w32, REMOTE, REMOTE_LEN))
    {
      w32 += REMOTE_LEN - 2;
      *w32 = '\\';
      debug_printf ("remote drive");
    }


  debug_printf ("derived path '%s'", w32);
  cygwin_conv_to_full_posix_path (w32, posix_fn);
  return posix_fn;
}


#include <unistd.h>
#include "getopt.h"

/* configure + libiberty + glibc outsmarts itself and removes 'getopt' declaration :-( */
int getopt();

static char ** remote_file_list(void);
static struct section_addr_info *remote_section_list(char *);
static void    init_rtems_ops(void);
static void    discard_remote_objfile(struct objfile *of);
static void    add_remote_objfiles(char **fl, int);
static char ** split_cmdline (char *args, int *pnargs);

static struct target_ops rtems_ops;

/* call either with 'os' or with 'os==NULL' but a valid
 * bfd/section pair...
 */
static int
compare_section (struct obj_section *os, bfd *abfd, asection *s)
{
  struct remote_state *rs = get_remote_state ();
  unsigned long host_crc, target_crc;
  char *tmp;
  char *sectdata;
  bfd_size_type size;
  bfd_vma vma;

  if ( os )
	{
	  s = os->the_bfd_section;
	  abfd = os->objfile->obfd;
	}

  if (!(s->flags & SEC_LOAD))
	internal_error(__FILE__, __LINE__, "section '%s' not loadable", bfd_get_section_name( abfd, s));

  size = bfd_get_section_size (s);
  if (size == 0)
	return 0;		/* skip zero-length section */

  vma  = bfd_get_section_vma (abfd, s);
  if (os)
	vma += os->addr;
  gdb_assert ( 30 < rs->buf_size );
  /* FIXME: assumes lma can fit into long */
  sprintf (rs->buf, "qCRC:%lx,%lx", (long) vma, (long) size);
  putpkt (rs->buf);

  /* be clever; compute the host_crc before waiting for target reply */
  sectdata = xmalloc (size);
  bfd_get_section_contents (abfd, s, sectdata, 0, size);
  host_crc = crc32 ((unsigned char *) sectdata, size, 0xffffffff);
  xfree (sectdata);

  getpkt (&rs->buf, &rs->buf_size, 0);
  if (rs->buf[0] == 'E')
	error ("target memory fault, section %s, range 0x%s -- 0x%s",
	       bfd_get_section_name(abfd, s), paddr (vma), paddr (vma + size));
  if (rs->buf[0] != 'C')
	{
	  warning ("remote target does not support CRC operation - skipping...");
	  return 0;
	}

  for (target_crc = 0, tmp = &rs->buf[1]; *tmp; tmp++)
	target_crc = target_crc * 16 + fromhex (*tmp);

  return target_crc - host_crc;
}


struct saa_ {
	bfd                     *sbfd;
	int                      ftty;
	struct section_addr_info *sa;
	struct objfile			 *of;
};

static int
sfa(void *arg)
{
struct saa_    *sfaa = arg;

  sfaa->of = symbol_file_add_from_bfd (sfaa->sbfd, sfaa->ftty, sfaa->sa, 0, 0);
#if 0
/* This doesn't work -- on the target, the section contents
 * are relocated and hence differing from the ones in the
 * file :-(
 */
  if ( compare_section ( &of->sections[of->sect_index_text], 0, 0 ) )
	error("%s: '.text' section of object file doesn't match the target's -- do GDB and the target use the same file?", bfd_get_filename(of->obfd));
#endif
  return -1;
}

static struct objfile *
caught_symbol_file_add_from_bfd( bfd *sbfd, int from_tty, struct section_addr_info *sa)
{
  struct saa_ sfaa;
  sfaa.sbfd = sbfd;
  sfaa.ftty = from_tty;
  sfaa.sa   = sa;
  sfaa.of   = 0;
  catch_errors( sfa, &sfaa, "Omitting -- ", RETURN_MASK_ALL );
  return sfaa.of;
}

struct sbd_ {
	char	*name;
	bfd		*sbfd;
};

static int
sbdo(void *arg)
{
struct sbd_ *sbda = arg;
	sbda->sbfd = symfile_bfd_open (sbda->name);
	return -1;
}

static bfd *
caught_symfile_bfd_open(char *name)
{
	struct sbd_ sbda;
	sbda.name = name;
	sbda.sbfd = 0;
	catch_errors( sbdo, &sbda, "Omitting -- ", RETURN_MASK_ALL );
	return sbda.sbfd;
}

static char*
rtems_pid_to_str (ptid_t ptid)
{
  static char buf[30];

  sprintf(buf, "Thread %8x", PIDGET (ptid));
  return buf;
}

static void rtems_remote_open(char *name, int from_tty)
{
extern bfd *exec_bfd;

  discard_remote_objfile(0);
  remote_open_1 (name, from_tty, &rtems_ops, 0, 0);
  if ( exec_bfd )
	{
	  asection *s;
	  if ( !(s=bfd_get_section_by_name (exec_bfd, ".text")) ) {
		error("No '.text' section found in executable");
	  }
	  if ( compare_section ( 0, exec_bfd, s ) )
		warning("%s: '.text' section of executable file doesn't match the target's -- do GDB and the target use the same file?", bfd_get_filename(exec_bfd));
	}
  add_remote_objfiles(0,from_tty);
}

static void rtems_files_info(struct target_ops *t)
{
struct objfile *of;

  ALL_OBJFILES(of)
	{
	  if ( of->obfd )
		print_section_info(t, of->obfd);
	  else
		{
		  puts_filtered(of->name); puts_filtered("\n");
		}
	}
}

static char **
remote_file_list(void)
{
  struct remote_state      *rs = get_remote_state ();
  char                     *bufp, *n;
  int                      i, num_entries, chunks, addr;
  char                     **file_list = 0;

  if (remote_desc == 0)		/* paranoia */
    error ("need connection to the remote target for retrieving a file list.");

  putpkt ("qfCexpFileList");
  getpkt (&rs->buf, &rs->buf_size, 0);
  bufp = rs->buf;
  if (bufp[0] != '\0') {
    /* packet recognized */
	{
	  i = 0;
	  chunks = 0;
	  while (*bufp++ == 'm')	/* reply contains one 'text_addr ','file_name' pair */
		{
		  if ( i <= chunks )
			{
			  chunks += 15;
			  file_list = xrealloc (file_list, chunks * sizeof (*file_list));
		    }
		  addr = strtol (bufp, &bufp, 16); /* currently unused */
		  if ( bufp == rs->buf+1 || ',' != *bufp++ )
			{
			  xfree (file_list); file_list = 0; i = chunks = 0;  /* paranoia -- error shouldn't return */
			  error ("ill formated file list packet");
		 	  continue;
			}
		  /* buf is guaranteed to be null terminated */
		  n = file_list[i] = strdup (bufp);
		  i++;
		  make_cleanup (xfree, n);
	      putpkt ("qsCexpFileList");
	      getpkt (&rs->buf, &rs->buf_size, 0);
	      bufp = rs->buf;
	    }
	  if ( i<= chunks )
		file_list = xrealloc (file_list, (i + 1) * sizeof (*file_list));
	  file_list[i++] = 0;
	  make_cleanup (xfree, file_list);
	  return file_list;	/* done */
	}
  }
  return 0;
}

/* allocates and populates a section_addr_info structure
 * with name/addr info obtained from the target.
 * returned array and 'name' fields must be cleaned up
 * by the caller
 */

static struct section_addr_info *
remote_section_list(char *filename)
{
  struct remote_state      *rs = get_remote_state ();
  char                     *bufp, *n;
  int                      i, num_entries, chunks;
  struct section_addr_info *section_addrs = NULL;
  struct sal_ {
    int  addr;
	char *name;
  } *sal = 0;

  if (remote_desc == 0)		/* paranoia */
    error ("need connection to the remote target for retrieving a section list.");

  gdb_assert (strlen(filename) < rs->buf_size + 20);
  sprintf (rs->buf,"qfCexpSectionList,%s",filename);
  putpkt (rs->buf);
  getpkt (&rs->buf, &rs->buf_size, 0);
  bufp = rs->buf;
  if (bufp[0] != '\0') {
    /* packet recognized */
	{
	  i = 0;
	  chunks = 0;
	  while (*bufp++ == 'm')	/* reply contains one section 'addr ',' name' pair */
		{
		  if ( i <= chunks )
			{
			  chunks += 15;
			  sal = xrealloc (sal, chunks * sizeof (*sal));
		    }
		  sal[i].addr = strtol (bufp, &bufp, 16);
		  if ( bufp == rs->buf+1 || ',' != *bufp++ )
			{
			  xfree (sal); sal = 0; i = chunks = 0;  /* paranoia -- error shouldn't return */
			  error ("ill formated section list packet");
		 	  continue;
			}
		  /* buf is guaranteed to be null terminated */
		  n = sal[i].name = strdup (bufp);
		  i++;
		  make_cleanup (xfree, n);
	      putpkt ("qsCexpSectionList");
	      getpkt (&rs->buf, &rs->buf_size, 0);
	      bufp = rs->buf;
	    }
	  num_entries = i;
	  section_addrs = alloc_section_addr_info (num_entries);
	  make_cleanup (xfree, section_addrs);
	  for (i = 0; i<num_entries; i++)
		{
		  section_addrs->other[i].addr = sal[i].addr;
		  section_addrs->other[i].name = sal[i].name;
		}
	  xfree (sal);
	  if ( 'E' == rs->buf[0] )
		{
			int errn;
			unpack_byte(rs->buf+1,&errn);
			error ("Target Error: %s\n", strerror (errn));
		}
	  return section_addrs;	/* done */
	}
  }
  return 0;
}


static void
discard_remote_objfile (struct objfile *objfile)
/* throw away old symbols */
{
  struct objfile *temp;

  if ( objfile )
	{
 	  if ( symfile_objfile != objfile )
		free_objfile (objfile);
	}
  else
	{
	ALL_OBJFILES_SAFE (objfile, temp)
	{
	  if ( symfile_objfile != objfile )
		free_objfile (objfile);
	}
	}

  clear_symtab_users ();

  if (remote_debug)
 	{
	  ALL_OBJFILES(objfile)
	  printf_filtered("still have: '%s'\n", objfile->name);
	}
}


static void
add_remote_objfiles (char **fl, int from_tty)
{
  struct cleanup           *cln = make_cleanup (null_cleanup, 0);
  struct section_addr_info *sa;
  int            i,j;

  if ( !fl )
	fl = remote_file_list ();
  if (remote_debug)
	printf_filtered ("Target files:\n");
  for ( i=0; fl && fl[i]; i++ )
	{
	  struct cleanup *cln1 = 0;
	  struct objfile *of;
	  bfd            *sbfd;

	  if (remote_debug)
	    printf_filtered ("  %-40sSection Info:\n",fl[i]);	

	  if ( (sbfd = caught_symfile_bfd_open(fl[i])) )
		{
		  sa = remote_section_list (fl[i]);
		  if ( !sa )
			{
			  bfd_close(sbfd);
			  continue;
			} 
		  cln1 = make_cleanup(null_cleanup, 0);

		  if (remote_debug)
			{
			  for (j = 0; j < sa->num_sections; j++)
				{
				  printf_filtered ("    0x%08lx: %s\n", sa->other[j].addr, sa->other[j].name);
				}
			}
  
		  /* Warn about common symbols */
		  if ( (of = caught_symbol_file_add_from_bfd ( sbfd, from_tty, sa ))
			&& (HAS_SYMS & bfd_get_file_flags( of->obfd )) )
			{
			  int     i = bfd_get_symtab_upper_bound( of->obfd );
			  if ( i > 0 )
				{
			  	  asymbol **syms = (asymbol**)xmalloc(i);
				  make_cleanup(xfree, syms);
				  if ( (i = bfd_canonicalize_symtab( of->obfd, syms )) > 0 )
					{
					  int k,w = 0;
					  for ( k=0; k < i; k++ )
						{
						  if ( bfd_is_com_section(bfd_get_section(syms[k])) )
							{
							  if ( !w )
								{
							  	  warning("COMMON symbols found:");
								  w = 1;
								}
							  printf_filtered(" %s\n", bfd_asymbol_name(syms[k]));
							}
						}
					  if ( w )
						{
						  warning("Unable to determine address of COMMONs -- compile with -fno-common or DO NOT USE the mentioned symbols");
						}
					}
				}
				/* Reload main symbol table AFTER individual modules.
				 * The reason is that the main executable may contain
				 * memory regions where sections of modules are loaded.
				 * If we look for symbols we want to use the module's
				 * symbol and not one from the main executable.
				 */
				if ( symfile_objfile )
					put_objfile_before(of, symfile_objfile);
			}
		  do_cleanups(cln1);
		}
 	}
  reinit_frame_cache ();
  do_cleanups (cln);
}

static char **
split_cmdline (char *args, int *pnargs)
{
  char  *blnk = args;
  char **rval = 0;
  int   nargs = 1; /* extra for arg[0] */

  *pnargs = 0;
  while (blnk)
	{
	  while ( isspace(*blnk) )
		*blnk++ = 0;
	  if ( *blnk )
		{
		  if ( !rval )
			{
			  *pnargs = nargs;
			  blnk    = strdup(blnk);
			  rval    = xcalloc(1,sizeof(*rval));
			}
		  nargs++;
		  rval = xrealloc(rval, nargs*sizeof(*rval));
		  rval[*pnargs] = blnk;
		  *pnargs = nargs;
		  blnk = strchr(blnk,' ');
		}
	  else
		{
		  blnk = 0;
		}
	}
  return rval;
}

/*
static void
rtems_kill()
{
  remote_kill();
  pop_target();
}
*/

static void
rtems_mourn(void)
{
  remote_mourn_1 (&rtems_ops);
}

static void
rtems_disconnect (struct target_ops *target, char *args, int from_tty)
{
	remote_disconnect(target, args, from_tty);
	pop_target();
}

static void
rtems_detach(char *args, int from_tty)
{
	remote_detach(args, from_tty);
	pop_target();
}

static void
rtems_load_file_command (char *args, int from_tty)
{
  struct remote_state      *rs = get_remote_state ();
  int                      nargs, ch;
  int                      unload = 0, refresh = 0;
  char                     **aa   = split_cmdline(args, &nargs);
  char                     *fnam, *bufp, *errmsg = 0;

  if (aa)
	{
	  make_cleanup(xfree, aa[1]);
	  make_cleanup(xfree, aa);
	}

  optind = 0;
  while ( (ch = getopt(nargs, aa, "u")) > 0 )
	{
	  switch (ch)
		{
		  default: error("unknown option: '%c'",ch);
		  case 'u': unload = 1; break;
		}
	}

  if ( optind >= nargs )
	error("need a filename argument");

  fnam = aa[optind];

  if (remote_desc == 0)		/* paranoia */
    error ("need connection to the remote target for loading an object file.");

  gdb_assert (strlen(fnam) < rs->buf_size + 25);
  sprintf (rs->buf,"qCexp%s,%s",unload?"Unld":"Load",fnam);
  putpkt (rs->buf);
  getpkt (&rs->buf, &rs->buf_size, 0);
  bufp = rs->buf;
  if (bufp[0] != '\0')
	{
	  if ( !strcmp("OK", bufp) )
		{
		  refresh = 1;
		}
	  else if ( !strcmp("E10", bufp) )
		{
		  errmsg="Unable to unload -- module still in use";
		}
	  else if ( !strcmp("E02", bufp) )
		{
		  refresh = 1;
		  errmsg="File not found - check PATH on target";
		}
	  else
		{
		  refresh = 1;
		  errmsg="Unknown error";
		}
	}
  else
	{
	  errmsg="qCexpLoad/qCexpUnld not supported by target";
	}

  if (refresh)
	{
	  discard_remote_objfile(0);
	  add_remote_objfiles(0, from_tty);
	}

  if (errmsg)
	error("%s",errmsg);
}

struct cmd_list_element *rtems_cmd_list = 0;

static void rtems_refresh_files_command (char *args, int from_tty)
{
  discard_remote_objfile(0);
  add_remote_objfiles(0, from_tty);
}

static void rtems_load_redirect (char *args, int from_tty)
{
  error("Please use the \"rtems load\" command");
}

static void rtems_cmd (char *args, int from_tty)
{
  printf_unfiltered("\"rtems\" must be followed by the name of a RTEMS specific command.\n");
  help_list(rtems_cmd_list, "rtems ", -1, gdb_stdout);
}

static void init_rtems_cmds ()
{
  add_prefix_cmd ("rtems", all_classes, rtems_cmd,
		  "RTEMS specific commands",
		  &rtems_cmd_list, "rtems ",
		  0/*allow-unknown*/, &cmdlist);
  add_cmd ("sync-objs", class_files, rtems_refresh_files_command,
	   "Synchronize gdb's file/section data with what's current on the target",
	   &rtems_cmd_list);
  add_cmd ("load", class_files, rtems_load_file_command,
       "rtems load [-u] <obj_file_name>\n"
	   "Load/link unload/unlink (use [-u] option) object files on the target\n"
       "NOTE: the object file must be found in GDB 'path' AND target 'PATH'",
	   &rtems_cmd_list);
}


static void 
init_rtems_ops ()
{
  rtems_ops						= remote_ops;

  if ( rtems_ops.to_open != remote_open )
	{
	  internal_error(__FILE__, __LINE__, "'rtems' target MUST be initialized AFTER 'remote'");
	}
  /* inherit from 'remote' */
  memcpy(&rtems_ops, &remote_ops, sizeof(rtems_ops));
  /* need to have 'remote' in the name for the compare-sections command to work */
  rtems_ops.to_shortname    = "rtems-remote";
  rtems_ops.to_longname     = "rtems target via 'remote' protocol";
  rtems_ops.to_doc          = "rtems target; specify link: <serial_dev> | <host>:<port>";
  rtems_ops.to_open         	= rtems_remote_open;
  rtems_ops.to_files_info   	= rtems_files_info;
  rtems_ops.to_pid_to_str   	= rtems_pid_to_str;
  rtems_ops.to_load				= rtems_load_redirect;
/*  rtems_ops.to_kill			= rtems_kill; */
  rtems_ops.to_mourn_inferior	= rtems_mourn;
  rtems_ops.to_disconnect		= rtems_disconnect;
  rtems_ops.to_detach			= rtems_detach;
  rtems_ops.to_magic        	= OPS_MAGIC;
}

void
_initialize_remote_rtems (void)
{
  /* TILL a temporary hack...; should be queried from the
   * target. Also, it seems that we could avoid a lot of grief
   * by having this for any architecture. The red-zone makes
   * our life a LOT easier (no stack switching needed). The
   * architecture needs a 'frame_align' method, though but this
   * should be straightforward to provide (unless I miss something)
   */
  set_gdbarch_frame_red_zone_size( current_gdbarch, 4096 );
  set_main_name("_Thread_Handler");

  init_rtems_ops();
  add_target (&rtems_ops);
  init_rtems_cmds ();
}

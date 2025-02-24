/*
 * This file is part of the hildon-application-manager.
 *
 * Parts of this file are derived from apt.  Apt is copyright 1997,
 * 1998, 1999 Jason Gunthorpe and others.
 *
 * Copyright (C) 2005, 2006, 2007, 2008 Nokia Corporation.  All Rights reserved.
 *
 * Contact: Marius Vollmer <marius.vollmer@nokia.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

/* This is the process that runs as root and does all the work.

   It is started from a separate program (as opposed to being forked
   directly from the GUI process) since that allows us to use sudo for
   starting it.

   This process communicates with the GUI process via some named pipes
   that are created by that process.  You can't really use it from the
   command line.

   It will output stuff to stdin and stderr, which the GUI process is
   supposed to catch and put into its log.

   The program tries hard not to exit prematurely.  Once the
   connection between the GUI process and this process has been
   established, the apt-worker is supposed to stick around until that
   connection is broken, even if it has to fail every request send to
   it.  This allows the user to try and fix the system after something
   went wrong, although the options are limited, of course.  The best
   example is a corrupted /etc/apt/sources.list: even tho you can't do
   anything related to packages, you still need the apt-worker to
   correct /etc/apt/sources.list itself in the UI.
*/

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <signal.h>
#include <ftw.h>

#include <fstream>

#include <apt-pkg/init.h>
#include <apt-pkg/error.h>
#include <apt-pkg/tagfile.h>
#include <apt-pkg/pkgcache.h>
#include <apt-pkg/pkgcachegen.h>
#include <apt-pkg/sourcelist.h>
#include <apt-pkg/acquire.h>
#include <apt-pkg/acquire-item.h>
#include <apt-pkg/cachefile.h>
#include <apt-pkg/strutl.h>
#include <apt-pkg/packagemanager.h>
#include <apt-pkg/deblistparser.h>
#include <apt-pkg/debversion.h>
#include <apt-pkg/dpkgpm.h>
#include <apt-pkg/debsystem.h>
#include <apt-pkg/orderlist.h>
#include <apt-pkg/algorithms.h>
#include <apt-pkg/metaindex.h>
#include <apt-pkg/debmetaindex.h>
#include <apt-pkg/policy.h>
#include <apt-pkg/hashes.h>
#include <apt-pkg/fileutl.h>
#include <apt-pkg/progress.h>
#include <apt-pkg/install-progress.h>

#include <glib.h>

#include "apt-worker-proto.h"
#include "confutils.h"

#include "update-notifier-conf.h"

using namespace std;

static void save_operation_record (const char *package,
				   const char *download_root);
static void erase_operation_record ();
static xexp *read_operation_record ();

/* Table of contents.
 
   COMPILE-TIME CONFIGURATION
   
   RUN-TIME CONFIGURATION

   GENERAL UTILITIES

   COMMUNICATING WITH THE FRONTEND

   STARTUP AND COMMAND DISPATCHER

   CACHE HANDLING
*/


/** COMPILE-TIME CONFIGURATION
 */

/* Defining this to non-zero will also recognize packages in the
   "maemo" section as user packages.  There are still packages
   floating around that follow this old rule.
*/
#define ENABLE_OLD_MAEMO_SECTION_TEST 1

/* Requests up to this size are put into a stack allocated buffer.
 */
#define FIXED_REQUEST_BUF_SIZE 4096

/* The location where we keep our lock.
 */
#define APT_WORKER_LOCK "/var/lib/hildon-application-manager/apt-worker-lock"

/* Temporary catalogues and temporary sources.list */
#define TEMP_CATALOGUE_CONF "/var/lib/hildon-application-manager/catalogues.temp"
#define TEMP_APT_SOURCE_LIST "/etc/apt/sources.list.d/hildon-application-manager-temp.list"

/* APT CACHE ARCHIVES DIRECTORIES */
#define DEFAULT_DIR_CACHE_ARCHIVES "archives/"
#define ALT_DIR_CACHE_ARCHIVES ".apt-archive-cache/"

/* Files related to the 'check for updates' process */
#define FAILED_CATALOGUES_FILE "/var/lib/hildon-application-manager/failed-catalogues"

/* Domain names associated with "OS" and "Nokia" updates */
#define OS_UPDATES_DOMAIN_NAME "nokia-system"
#define NOKIA_UPDATES_DOMAIN_NAME "nokia-certified"

/* Where we keep a journal of the current operation
 */
#define CURRENT_OPERATION_FILE "/var/lib/hildon-application-manager/current-operation"

/* File to store the result of rescue mode execution
 */
#define RESCUE_RESULT_FILE "/var/lib/hildon-application-manager/rescue-result"


/* You know what this means.
 */
/*#define DEBUG*/
/*#define DEBUG_COMMANDS*/

#ifdef DEBUG
#define DBG log_stderr
#else
#define DBG(...)
#endif

/** RUN-TIME CONFIGURATION
 */

/* Setting this to true will break the lock of the dpkg status
   database if necessary.
*/
bool flag_break_locks = false;

/* Setting this to true will not ignore package versions from a wrong
   domain.
 */
bool flag_allow_wrong_domains = false;

/* Setting this to true will use the normal apt-get algorithms for
   installing/removing packages instead of our home-grown ones.
*/
bool flag_use_apt_algorithms = false;

/* Setting this to false will not use MMC to save the packages when
   downloading them.
*/
bool flag_download_packages_to_mmc = false;

/* List of packages found with the 'system-update' flag (SSU packages)
 */
GArray *ssu_packages = NULL;

/* Flag to tell whether the ssu_packages GArray needs to be refreshed
 */
gboolean ssu_packages_needs_refresh = true;

/** GENERAL UTILITIES
 */

void
log_stderr (const char *fmt, ...)
{
  va_list args;
  va_start (args, fmt);
  fprintf (stderr, "apt-worker: ");
  vfprintf (stderr, fmt, args);
  va_end (args);
  fprintf (stderr, "\n");
}

/** APT WORKER MULTI STATE MANAGEMENT
 */

/* Inside this process, domains are identified with small integers.
 */

typedef signed char domain_t;

struct domain_info {
  const char *name;
  xexp *conf;
  int trust_level;
  bool is_certified;
};

xexp *domain_conf = NULL;
domain_info *domains = NULL;
int domains_number = 0;
time_t domains_last_modified = -1;

#define DOMAIN_INVALID  -1
#define DOMAIN_UNSIGNED  0
#define DOMAIN_SIGNED    1 

#define DOMAIN_DEFAULT DOMAIN_UNSIGNED

static time_t
file_last_modified (const char *file_name)
{
  struct stat buf;

  if (stat (file_name, &buf) == -1)
    {
      perror ("error retriving file info");
      return -1;
    }

  return buf.st_mtime;
}

static void
read_domain_conf ()
{
  delete[] domains;
  xexp_free (domain_conf);

  domain_conf = read_domains ();

  int n_domains = 2;
  if (domain_conf)
    n_domains += xexp_length (domain_conf);

  domains = new domain_info[n_domains];

  /* Setup the two implicit domains.
   */
  domains[0].name = "unsigned";
  domains[0].conf = NULL;
  domains[0].trust_level = 0;
  domains[0].is_certified = false;
    
  domains[1].name = "signed";
  domains[1].conf = NULL;
  domains[1].trust_level = 1;
  domains[1].is_certified = false;

  int i = 2;
  if (domain_conf)
    {
      for (xexp *d = xexp_first (domain_conf); d; d = xexp_rest (d))
	{
	  if (!xexp_is (d, "domain"))
	    continue;

	  domains[i].name = xexp_aref_text (d, "name");
	  if (domains[i].name == NULL)
	    continue;

	  domains[i].trust_level = xexp_aref_int (d, "trust-level", 2);
	  domains[i].is_certified = xexp_aref_bool (d, "certified");
	  domains[i].conf = d;

	  i += 1;
	}
    }

  /* Update domains number and last modified timestamp */
  domains_number = i;
  domains_last_modified = file_last_modified (PACKAGE_DOMAINS);
}

static domain_t
find_domain_by_tag (const char *tag, const char *val)
{
  for (int i = 0; i < domains_number; i++)
    {
      if (domains[i].conf == NULL)
	continue;

      for (xexp *x = xexp_aref (domains[i].conf, tag);
	   x;
	   x = xexp_aref_rest (x, tag))
	{
	  if (xexp_is_text (x) && g_str_has_suffix (xexp_text (x), val))
	    return i;
	}
    }

  return DOMAIN_SIGNED;
}

static domain_t
find_domain_by_key (const char *key)
{
  return find_domain_by_tag ("key", key);
}

static domain_t
find_domain_by_uri (const char *uri)
{
  return find_domain_by_tag ("uri", uri);
}

class myCacheFile;

/* This class implements a global static cache managing for
 *  apt-worker. */
class AptWorkerCache
{
public:
  AptWorkerCache ();
  static void Initialize ();
  static AptWorkerCache * GetCurrent ();
  
  bool init_cache_after_request;  
  myCacheFile *cache;
  pkgDepCache::ActionGroup *action_group;
  static AptWorkerCache *current;
  static bool global_initialized;
};

AptWorkerCache *AptWorkerCache::current = 0;

bool AptWorkerCache::global_initialized = false;

AptWorkerCache::AptWorkerCache ()
  : init_cache_after_request (false), cache (0)
{
}

AptWorkerCache *
AptWorkerCache::GetCurrent (void)
{
  return current;
}  

/* Initialization of apt worker cache. It initializes the
 * APT subsystem, and then gets the current instance. */
void
AptWorkerCache::Initialize (void)
{
  if (!global_initialized)
    {
      if (pkgInitConfig (*_config) == false ||
          pkgInitSystem (*_config, _system) == false)
        {
          _error->DumpErrors ();
          return;
        }

      _config->Set ("APT::Architecture", DEB_HOST_ARCH);
      _config->Set ("DPkg::Options::", "--force-confold");
      _config->Set ("Dir::Log", "var/log");
      _config->Set ("Dir::Log::Terminal", "");
      global_initialized = true;
    }

  current = new AptWorkerCache;
}
  
/* This struct describes some status flags for specific packages.
 * myCacheFile includes an array of these, with an entry per
 * package.
 */
struct extra_info_struct
{
  bool autoinst : 1;
  bool related : 1;
  bool soft : 1;
  domain_t cur_domain, new_domain;
};

class myPolicy : public pkgPolicy {

protected:
  myCacheFile *my_cache_file;
  int *pf_domain;

public:
  myPolicy (pkgCache *Owner, myCacheFile *cache_file)
    : pkgPolicy (Owner), my_cache_file (cache_file)
  {
    pf_domain = new int[Owner->Head().PackageFileCount];
  }

  void InitDomains (const pkgSourceList &List);

  virtual pkgCache::VerIterator GetCandidateVer(pkgCache::PkgIterator Pkg);
};

class myCacheFile : public pkgCacheFile {

public:
  bool Open (OpProgress &Progress, bool WithLock = true);

  void load_extra_info (const pkgSourceList &sources);
  void save_extra_info ();

  extra_info_struct *extra_info;

  myCacheFile ()
  {
    extra_info = NULL;
  }

  ~myCacheFile ()
  {
    delete[] extra_info;
  }
};

static void set_sources_for_get_domain (const pkgSourceList *sources);
static int get_domain (pkgIndexFile*);

void
myPolicy::InitDomains (const pkgSourceList &List)
{
  set_sources_for_get_domain (&List);

  for (pkgCache::PkgFileIterator I = Cache->FileBegin();
       I != Cache->FileEnd(); I++)
    pf_domain[I->ID] = DOMAIN_UNSIGNED;

  for (pkgCache::PkgFileIterator I = Cache->FileBegin();
       I != Cache->FileEnd(); I++)
    {
      // Locate the associated index files so we can find its domain
      pkgIndexFile *Indx;
      if (List.FindIndex(I,Indx) || _system->FindIndex(I,Indx))
	{
	  pf_domain[I->ID] = get_domain (Indx);
	  DBG ("%s: %s", Indx->Describe(true).c_str(),
	       domains[pf_domain[I->ID]].name);
	}
    }

  set_sources_for_get_domain (NULL);
}

static bool domain_dominates_or_is_equal (int a, int b);

pkgCache::VerIterator 
myPolicy::GetCandidateVer (pkgCache::PkgIterator Pkg)
{
  // Look for a package pin and evaluate it.
  pkgCache::VerIterator Pref = GetCandidateVer(Pkg);
  signed Max = GetPriority(Pref);
  
  /* Falling through to the default version.. Setting Max to zero
     effectively excludes everything <= 0 which are the non-automatic
     priorities.. The status file is given a prio of 100 which will exclude
     not-automatic sources, except in a single shot not-installed mode.
     The second pseduo-status file is at prio 1000, above which will permit
     the user to force-downgrade things.
     
     The user pin is subject to the same priority rules as default 
     selections. Thus there are two ways to create a pin - a pin that
     tracks the default when the default is taken away, and a permanent
     pin that stays at that setting.
  */
  for (pkgCache::VerIterator Ver = Pkg.VersionList(); Ver.end() == false; Ver++)
    {
      for (pkgCache::VerFileIterator VF = Ver.FileList(); VF.end() == false;
	   VF++)
	{
	  /* If this is the status file, and the current version is not the
	     version in the status file (ie it is not installed, or somesuch)
	     then it is not a candidate for installation, ever. This weeds
	     out bogus entries that may be due to config-file states, or
	     other. */
	  if ((VF.File()->Flags & pkgCache::Flag::NotSource) == pkgCache::Flag::NotSource &&
	      Pkg.CurrentVer() != Ver)
	    continue;

	  /* If requested, skip versions from the wrong domain, but
	     only for sources.
	   */

	  if (!flag_allow_wrong_domains
	      && ((VF.File()->Flags & pkgCache::Flag::NotSource)
		  != pkgCache::Flag::NotSource))
	    {
	      if (!domain_dominates_or_is_equal
		  (pf_domain[VF.File()->ID],
		   my_cache_file->extra_info[Pkg->ID].cur_domain))
		{
		  log_stderr ("Ignoring version from wrong domain: %s %s",
			      Pkg.Name(), Ver.VerStr());
		  log_stderr ("  %s", VF.File().FileName());
		  continue;
		}
	    }
	  
	  signed Prio = PFPriority[VF.File()->ID];
	  if (Prio > Max)
	    {
	      Pref = Ver;
	      Max = Prio;
	    }	 
	}      
      
      if (Pkg.CurrentVer() == Ver && Max < 1000)
	{
	  /* Elevate our current selection (or the status file itself)
	     to the Pseudo-status priority. */
	  if (Pref.end() == true)
	    Pref = Ver;
	  Max = 1000;
	  
	  // Fast path optimize.
	  if (StatusOverride == false)
	    break;
	}       
    }
  return Pref;
}

bool
myCacheFile::Open (OpProgress &Progress, bool WithLock)
{
  if (BuildCaches(&Progress,WithLock) == false)
    return false;
  
  // The policy engine
  myPolicy *pol = new myPolicy (Cache, this);
  Policy = pol;
  if (_error->PendingError() == true)
    return false;
  if (ReadPinFile(*Policy) == false)
    return false;
  

  pkgSourceList List;

  if (!List.ReadMainList ())
    {
      _error->DumpErrors();
      return false;
    }

  pol->InitDomains (List);
  
  load_extra_info (List);

  // Create the dependency cache
  DCache = new pkgDepCache(Cache,Policy);
  if (_error->PendingError() == true)
    return false;
  
  DCache->Init(&Progress);
  Progress.Done();
  if (_error->PendingError() == true)
    return false;
  
  return true;
}

static bool
create_extra_info_dir()
{
  if (mkdir ("/var/lib/hildon-application-manager", 0777) < 0
      && errno != EEXIST)
    {
      log_stderr ("/var/lib/hildon-application-manager: %m");
      return false;
    }

  return true;
}

/* Save the 'extra_info' of the cache.  We first make a copy of the
   Auto flags in our own extra_info storage so that CACHE_RESET
   will reset the Auto flags to the state last saved with this
   function.
*/

void
myCacheFile::save_extra_info ()
{
  if (!create_extra_info_dir())
    return;

  FILE *f = fopen ("/var/lib/hildon-application-manager/autoinst", "w");
  if (f)
    {
      pkgDepCache &cache = *DCache;

      for (pkgCache::PkgIterator pkg = cache.PkgBegin(); !pkg.end (); pkg++)
	{
	  if (cache[pkg].Flags & pkgCache::Flag::Auto)
	    {
	      extra_info[pkg->ID].autoinst = true;
	      fprintf (f, "%s\n", pkg.Name ());
	    }
	  else
	    extra_info[pkg->ID].autoinst = false;
	}
      fflush (f);
      fsync (fileno (f));
      fclose (f);
    }

  for (domain_t i = 0; i < domains_number; i++)
    {
      char *name =
	g_strdup_printf ("/var/lib/hildon-application-manager/domain.%s",
			 domains[i].name);
      
      FILE *f = fopen (name, "w");
      if (f)
	{
	  pkgDepCache &cache = *DCache;

	  for (pkgCache::PkgIterator pkg = cache.PkgBegin();
	       !pkg.end (); pkg++)
	    {
	      if (extra_info[pkg->ID].cur_domain == i)
		fprintf (f, "%s\n", pkg.Name ());
	    }
	  fflush (f);
	  fsync (fileno (f));
	  fclose (f);
	}
    }
}

/* Load the 'extra_info'.  You need to call CACHE_RESET to
   transfer the auto flag into the actual cache.  */

void
myCacheFile::load_extra_info (const pkgSourceList &sources)
{
  pkgCache &cache = *Cache;

  int package_count = cache.Head().PackageCount;

  DBG ("package_count: %d", package_count);

  extra_info = new extra_info_struct[package_count];

  for (int i = 0; i < package_count; i++)
    {
      extra_info[i].autoinst = false;
      extra_info[i].cur_domain = DOMAIN_INVALID;
    }

  FILE *f = fopen ("/var/lib/hildon-application-manager/autoinst", "r");
  if (f)
    {
      char *line = NULL;
      size_t len = 0;
      ssize_t n;

      while ((n = getline (&line, &len, f)) != -1)
	{
	  if (n > 0 && line[n-1] == '\n')
	    line[n-1] = '\0';

	  pkgCache::PkgIterator pkg = cache.FindPkg (line);
	  if (!pkg.end ())
	    {
	      DBG ("auto: %s", pkg.Name ());
	      extra_info[pkg->ID].autoinst = true;
	    }
	}

      free (line);
      fclose (f);
    }

  for (domain_t i = 0; i < domains_number; i++)
    {
      char *name =
	g_strdup_printf ("/var/lib/hildon-application-manager/domain.%s",
			 domains[i].name);

      FILE *f = fopen (name, "r");
      if (f)
	{
	  char *line = NULL;
	  size_t len = 0;
	  ssize_t n;

	  while ((n = getline (&line, &len, f)) != -1)
	    {
	      if (n > 0 && line[n-1] == '\n')
		line[n-1] = '\0';

	      pkgCache::PkgIterator pkg = cache.FindPkg (line);
	      if (!pkg.end ())
		{
		  // DBG ("%s: %s (%d)", domains[i].name, pkg.Name (), pkg->ID);
		  extra_info[pkg->ID].cur_domain = i;
		}
	    }

	  free (line);
	  fclose (f);
	}

      g_free (name);
    }

  bool domains_changed = false;
  set_sources_for_get_domain(&sources);

  for (pkgCache::PkgIterator pkg = cache.PkgBegin(); !pkg.end (); pkg++)
    {
      if (strcmp(pkg.Arch (), DEB_HOST_ARCH))
	continue;

      if (extra_info[pkg->ID].cur_domain == DOMAIN_INVALID)
        {
	  pkgCache::VerIterator cur = pkg.CurrentVer ();
	  domain_t domain = DOMAIN_UNSIGNED;

	  if (!cur.end ())
	    {
	      for (pkgCache::VerFileIterator VF = cur.FileList (); !VF.end (); ++VF)
	      {
		 pkgCache::PkgFileIterator const PF = VF.File ();

		 if (PF.Flagged (pkgCache::Flag::NotSource))
		    continue;

		 pkgIndexFile *Indx;

		 if (sources.FindIndex (PF, Indx))
		   {
		     domain_t candidate = get_domain (Indx);

		     if (domains[candidate].trust_level > domains[domain].trust_level)
		       {
			 extra_info[pkg->ID].cur_domain = candidate;
			 domains_changed = true;
		       }
		   }
		}
	    }
	}
    }

  set_sources_for_get_domain(NULL);

  if (!domains_changed)
    return;

  if (!create_extra_info_dir())
    return;

  for (domain_t i = 0; i < domains_number; i++)
    {
      char *name =
          g_strdup_printf ("/var/lib/hildon-application-manager/domain.%s",
                           domains[i].name);
      FILE *f = fopen (name, "w");

      if (f)
        {
          for (pkgCache::PkgIterator pkg = cache.PkgBegin (); !pkg.end (); pkg++)
            {
              if (extra_info[pkg->ID].cur_domain == i)
                fprintf (f, "%s\n", pkg.Name ());
            }

	  fflush (f);
	  fsync (fileno (f));
	  fclose (f);
	}
    }
}

/* ALLOC_BUF and FREE_BUF can be used to manage a temporary buffer of
   arbitrary size without having to allocate memory from the heap when
   the buffer is small.
   
   The way to use them is to allocate a buffer of 'normal' but fixed
   size statically or on the stack and the use ALLOC_BUF when the
   actual size of the needed buffer is known.  If the actual size is
   small enough, ALLOC_BUF will use the fixed size buffer, otherwise
   it will allocate a new one.  FREE_BUF will free that buffer.
*/

/* Return a pointer to LEN bytes of free storage.  When LEN is less
   than or equal to FIXED_BUF_LEN return FIXED_BUF, otherwise a newly
   allocated block of memory is returned.  ALLOC_BUF never return
   NULL.
*/
char *
alloc_buf (int len, char *fixed_buf, int fixed_buf_len)
{
  if (len <= fixed_buf_len)
    return fixed_buf;
  else
    return new char[len];
}

/* Free the block of memory pointed to by BUF if it is different from
   FIXED_BUF.
*/
void
free_buf (char *buf, char *fixed_buf)
{
  if (buf != fixed_buf)
    delete[] buf;
}

/* Open FILENAME with FLAGS, or die.
 */
static int
must_open (char *filename, int flags)
{
  int fd = open (filename, flags);
  if (fd < 0)
    {
      perror (filename);
      exit (1);
    }
  return fd;
}

static void
must_set_flags (int fd, int flags)
{
  if (fcntl (fd, F_SETFL, flags) < 0)
    {
      perror ("apt-worker fcntl");
      exit (1);
    }
}

static void
block_for_read (int fd)
{
  fd_set set;
  FD_ZERO (&set);
  FD_SET (fd, &set);

  if (select (fd+1, &set, NULL, NULL, NULL) < 0)
    {
      perror ("apt-worker select");
      exit (1);
    }
}

static int
read_byte (int fd)
{
  unsigned char byte;
  if (read (fd, &byte, 1) == 1)
    return byte;
  return -1;
}

/* DRAIN_FD reads all bytes from FD that are available.
*/
static void
drain_fd (int fd)
{
  while (read_byte (fd) >= 0)
    ;
}

/* Get a lock as with GetLock from libapt-pkg, breaking it if needed
   and allowed by flag_break_locks.

   We do this so that the users can not lock themselves out.  We break
   locks instead of not locking since noisily breaking a lock is
   better than silently corrupting stuff.
 */
int
ForceLock (string File, bool Errors = true)
{
  int lock_fd = GetLock (File, false);
  if (lock_fd >= 0)
    return lock_fd;

  if (flag_break_locks)
    {
      int res = unlink (File.c_str ());
      if (res < 0 && errno != ENOENT)
	log_stderr ("Can't remove %s: %m", File.c_str ());
      else if (res == 0)
	log_stderr ("Forcing %s", File.c_str ());
    }

  return GetLock (File, Errors);
}


/** COMMUNICATING WITH THE FRONTEND.
 
   The communication with the frontend happens over four
   unidirectional fifos: requests are read from INPUT_FD and responses
   are sent back via OUTPUT_FD.  No new request is read until the
   response to the current one has been completely sent.

   The data read from INPUT_FD must follow the request format
   specified in <apt-worker-proto.h>.  The data written to OUTPUT_FD
   follows the response format specified there.

   The CANCEL_FD is polled periodically and when something is
   available to be read, the current operation is aborted.  There is
   currently no meaning defined for the actual bytes that are sent,
   the mere arrival of a byte triggers the abort.

   When using the libapt-pkg PackageManager, it is configured in such
   a way that it sends it "pmstatus:" message lines to STATUS_FD.
   Other asynchronous status reports are sent as spontaneous
   APTCMD_STATUS responses via OUTPUT_FD.  'Spontaneous' should mean
   that no request is required to receive APTCMD_STATUS responses.  In
   fact, APTCMD_STATUS requests are treated as an error by the
   apt-worker.

   Logging and debug output, and output from dpkg and the maintainer
   scripts appears normally on stdout and stderr of the apt-worker
   process.
*/

int input_fd, output_fd, status_fd, cancel_fd;

/* MUST_READ and MUST_WRITE read and write blocks of raw bytes from
   INPUT_FD and to OUTPUT_FD.  If they return, they have succeeded and
   read or written the whole block.
*/

void
must_read (void *buf, size_t n)
{
  int r;

  while (n > 0)
    {
      r = read (input_fd, buf, n);
      if (r < 0)
	{
	  perror ("apt-worker read");
	  exit (1);
	}
      else if (r == 0)
	{
	  DBG ("exiting");
	  exit (0);
	}
      n -= r;
      buf = ((char *)buf) + r;
    }
}

static void
must_write (void *buf, ssize_t n)
{
  if (n > 0 && write (output_fd, buf, n) != n)
    {
      perror ("apt-worker write");
      exit (1);
    }
}

/* This function sends a response on OUTPUT_FD with the given CMD and
   SEQ.  It either succeeds or does not return.
*/
void
send_response_raw (int cmd, int seq, void *response, int len)
{
  apt_response_header res = { cmd, seq, len };
  must_write (&res, sizeof (res));
  must_write (response, len);
}

/* Fabricate and send a APTCMD_STATUS response.  Parameters OP,
   ALREADY, and TOTAL are as specified in apt-worker-proto.h.

   A status response is only sent when there is enough change since
   the last time.  The following counts as 'enough': ALREADY has
   decreased, it has increased by more than MIN_CHANGE, it is equal to
   -1, LAST_TOTAL has changed, or OP has changed.
*/

void
send_status (int op, int already, int total, int min_change)
{

  static apt_proto_encoder status_response;
  static int last_op;
  static int last_already;
  static int last_total;

  if (already == -1 
      || already < last_already 
      || already >= last_already + min_change
      || total != last_total
      || op != last_op)
    {
      last_already = already;
      last_total = total;
      last_op = op;
      
      status_response.reset ();
      status_response.encode_int (op);
      status_response.encode_int (already);
      status_response.encode_int (total);
      send_response_raw (APTCMD_STATUS, -1, 
			 status_response.get_buf (),
			 status_response.get_len ());
    }
}


/** STARTUP AND COMMAND DISPATCHER.
 */

/* Since the apt-worker only works on a single command at a time, we
   use two global encoder and decoder engines that manage the
   parameters of the request and the result values of the response.

   Handlers of specific commands will read the parameters from REQUEST
   and put the results into RESPONSE.  The command dispatcher will
   prepare REQUEST before calling the command handler and ship out
   RESPONSE after it returned.
*/
apt_proto_decoder request;
apt_proto_encoder response;

void cmd_get_package_list ();
void cmd_get_package_info ();
void cmd_get_package_details ();
int cmd_check_updates (bool with_status = true);
void cmd_get_catalogues ();
void cmd_set_catalogues ();
void cmd_add_temp_catalogues ();
void cmd_rm_temp_catalogues ();
void cmd_get_free_space ();
void cmd_install_check ();
void cmd_download_package ();
void cmd_install_package ();
void cmd_remove_check ();
void cmd_remove_package ();
void cmd_clean ();
void cmd_get_file_details ();
void cmd_install_file ();
void cmd_save_backup_data ();
void cmd_get_system_update_packages ();
void cmd_reboot ();
void cmd_set_options ();
void cmd_set_env ();
void cmd_third_party_policy_check ();
void cmd_autoremove ();

int cmdline_check_updates (char **argv);
int cmdline_rescue (char **argv);

/** MANAGEMENT FOR FAILED CATALOGUES LOG FILE
 */

/* Since it's needed to save the full report (including errors) after
   any refresh of the catalogues list, four functions were implemented
   to take care of the writting/reading to/from disk process.
*/

static void save_failed_catalogues (xexp *catalogues);
static xexp *load_failed_catalogues ();
static void clean_failed_catalogues ();
static void clean_temp_catalogues ();
static xexp *merge_catalogues_with_errors (xexp *catalogues);

/** MANAGEMENT OF FILE WITH INFO ABOUT AVAILABLE UPDATES */

static void write_available_updates_file ();

/** MAPPING FUNCTION TO FILTER ERROR DETAILS
    IN CATALOGUES CONFIGURATION FILE
*/
static xexp *map_catalogue_error_details (xexp *x);


/* Commands can request the package cache to be refreshed by calling
   NEED_CACHE_INIT before they return.  The cache will then be
   reconstructed after sending the response and before starting to
   handle the next command.  In this way, the cache reconstruction
   happens in the background.

   XXX - However, APTCMD_STATUS messages are still being sent when the
         cache is reconstructed in the background and the UI has some
         ugly logic to deal with that.
*/

void cache_init (bool with_status = true);

void
need_cache_init ()
{
  AptWorkerCache *awc = AptWorkerCache::GetCurrent ();
  awc->init_cache_after_request = true;
}

#ifdef DEBUG_COMMANDS
static const char *cmd_names[] = {
  "NOOP",
  "STATUS",
  "GET_PACKAGE_LIST",
  "GET_PACKAGE_INFO",
  "GET_PACKAGE_DETAILS",
  "CHECK_UPDATES",
  "GET_CATALOGUES",
  "SET_CATALOGUES",
  "ADD_TEMP_CATALOGUES",
  "RM_TEMP_CATALOGUES",
  "GET_FREE_SPACE",
  "INSTALL_CHECK",
  "INSTALL_PACKAGE",
  "REMOVE_CHECK",
  "REMOVE_PACKAGE",
  "GET_FILE_DETAILS",
  "INSTALL_FILE",
  "CLEAN",
  "SAVE_BACKUP_DATA",
  "GET_SYSTEM_UPDATE_PACKAGES",
  "FLASH_AND_REBOOT",
  "SET_OPTIONS",
  "SET_ENV",
  "THIRD_PARTY_POLICY_CHECK",
  "AUTOREMOVE"
};
#endif

void
handle_request ()
{
  apt_request_header req;
  char stack_reqbuf[FIXED_REQUEST_BUF_SIZE];
  char *reqbuf;
  AptWorkerCache * awc = 0;
  time_t last_modified = -1;

  must_read (&req, sizeof (req));

#ifdef DEBUG_COMMANDS
  DBG ("got req %s/%d/%d", cmd_names[req.cmd], req.seq, req.len);
#endif

  reqbuf = alloc_buf (req.len, stack_reqbuf, FIXED_REQUEST_BUF_SIZE);
  must_read (reqbuf, req.len);

  drain_fd (cancel_fd);

  request.reset (reqbuf, req.len);
  response.reset ();

  awc = AptWorkerCache::GetCurrent ();
  awc->init_cache_after_request = false; // let's reset it now

  /* Re-read domains conf file if modified */
  last_modified = file_last_modified (PACKAGE_DOMAINS);
  if (last_modified != domains_last_modified)
    read_domain_conf ();

  switch (req.cmd)
    {

    case APTCMD_NOOP:
      // Nothing to do.
      break;

    case APTCMD_GET_PACKAGE_LIST:
      cmd_get_package_list ();
      break;

    case APTCMD_GET_PACKAGE_INFO:
      cmd_get_package_info ();
      break;

    case APTCMD_GET_PACKAGE_DETAILS:
      cmd_get_package_details ();
      break;

    case APTCMD_CHECK_UPDATES:
      cmd_check_updates ();
      break;

    case APTCMD_GET_CATALOGUES:
      cmd_get_catalogues ();
      break;

    case APTCMD_SET_CATALOGUES:
      cmd_set_catalogues ();
      break;

    case APTCMD_ADD_TEMP_CATALOGUES:
      cmd_add_temp_catalogues ();
      break;

    case APTCMD_RM_TEMP_CATALOGUES:
      cmd_rm_temp_catalogues ();
      break;

    case APTCMD_GET_FREE_SPACE:
      cmd_get_free_space ();
      break;

    case APTCMD_INSTALL_CHECK:
      cmd_install_check ();
      break;

    case APTCMD_DOWNLOAD_PACKAGE:
      cmd_download_package ();
      break;

    case APTCMD_INSTALL_PACKAGE:
      cmd_install_package ();
      break;

    case APTCMD_REMOVE_CHECK:
      cmd_remove_check ();
      break;

    case APTCMD_REMOVE_PACKAGE:
      cmd_remove_package ();
      break;

    case APTCMD_CLEAN:
      cmd_clean ();
      break;

    case APTCMD_GET_FILE_DETAILS:
      cmd_get_file_details ();
      break;

    case APTCMD_INSTALL_FILE:
      cmd_install_file ();
      break;

    case APTCMD_SAVE_BACKUP_DATA:
      cmd_save_backup_data ();
      break;

    case APTCMD_GET_SYSTEM_UPDATE_PACKAGES:
      cmd_get_system_update_packages ();
      break;

    case APTCMD_REBOOT:
      cmd_reboot ();
      break;

    case APTCMD_SET_OPTIONS:
      cmd_set_options ();
      break;

    case APTCMD_SET_ENV:
      cmd_set_env ();
      break;

    case APTCMD_THIRD_PARTY_POLICY_CHECK:
      cmd_third_party_policy_check ();
      break;

    case APTCMD_AUTOREMOVE:
      cmd_autoremove ();
      break;

    case APTCMD_EXIT:
      exit(0);
      break;

    default:
      log_stderr ("unrecognized request: %d", req.cmd);
      break;
    }

  _error->DumpErrors ();

  send_response_raw (req.cmd, req.seq,
		     response.get_buf (), response.get_len ());

#ifdef DEBUG_COMMANDS
  DBG ("sent resp %s/%d/%d",
       cmd_names[req.cmd], req.seq, response.get_len ());
#endif

  free_buf (reqbuf, stack_reqbuf);

  if (awc->init_cache_after_request)
    {
      cache_init (false);
      _error->DumpErrors ();
    }
}

static int index_trust_level_for_package (pkgIndexFile *index,
					  const pkgCache::VerIterator &ver);

static const char *lc_messages;

static void
usage ()
{
  fprintf (stderr, "Usage: apt-worker check-for-updates [http_proxy]\n");
  fprintf (stderr, "       apt-worker rescue [package] [archives]\n");
  exit (1);
}

/* Try to get the lock specified by FILE.  If this fails because
   someone else has the lock, a string is returned with the content of
   the file.  Otherwise MY_CONTENT is written to the file and NULL is
   returned.

   Other kinds of failures (out of space, insufficient permissions,
   etc) will terminate this process.

   The lock will be released when this process exits.
*/

static char *
try_lock (const char *file, const char *my_content)
{
  int lock_fd = open (file, O_RDWR | O_CREAT, 0640);
  if (lock_fd < 0)
    {
      log_stderr ("Can't open %s: %m", file);
      exit (1);
    }

  SetCloseExec (lock_fd, true);

  struct flock fl;
  fl.l_type = F_WRLCK;
  fl.l_whence = SEEK_SET;
  fl.l_start = 0;
  fl.l_len = 0;

  if (fcntl (lock_fd, F_SETLK, &fl) == -1)
    {
      char buf[256];
      int n;
      
      if (errno == ENOLCK)
	{
	  log_stderr ("locking not supported on %s: %m", file);
	  exit (1);
	}
     
      /* We didn't get the lock.
       */

      n = read (lock_fd, buf, 255);
      if (n < 0)
	{
	  log_stderr ("can't read lock %s: %m", file);
	  exit (1);
	}

      buf[n] = '\0';
      return g_strdup (buf);
    }

  /* We have the lock.
   */

  if (ftruncate (lock_fd, 0) < 0)
    {
      log_stderr ("can't truncate lock %s: %m", file);
      exit (1);
    }
    
  int n = strlen (my_content);
  if (write (lock_fd, my_content, n) != n)
    {
      log_stderr ("can't write lock %s: %m", file);
      exit (1);
    }
     
  return NULL;
}

static void
get_apt_worker_lock (bool weak)
{
  char *mine = g_strdup_printf ("%c %d\n", weak? 'w' : 's', getpid ());
  char *his = NULL;
  int termination_attempts = 0;
  int lock_attempts = 0;

  while (true)
    {
      g_free (his);
      his = try_lock (APT_WORKER_LOCK, mine);
      
      if (his)
	{
	  char his_type;
	  int his_pid;
	  
	  if (sscanf (his, "%c %d", &his_type, &his_pid) != 2)
	    {
	      log_stderr ("can't parse lock.");
	      exit (1);
	    }

	  if (weak || his_type != 'w')
	    {
	      if (lock_attempts < 5)
	        {
	          lock_attempts++;
	          sleep (1);
	          continue;
	        }
	      else
	        {
	          log_stderr ("too weak to get lock from %d.", his_pid);
	          exit (1);
	        }
	    }
	  else if (termination_attempts < 5)
	    {
	      termination_attempts += 1;
	      log_stderr ("terminating %d to get lock.", his_pid);
	      kill (his_pid, SIGTERM);
	      sleep (1);
	      continue;
	    }
	  else
	    {
	      /* The big hammer.
	       */
	      log_stderr ("killing %d to get lock.", his_pid);
	      kill (his_pid, SIGKILL);
	      unlink (APT_WORKER_LOCK);
	      sleep (1);
	      continue;
	    }
	}
      else
	{
	  /* It's ours.
	   */
	  g_free (mine);
	  return;
	}
    }
}

/* MMC default mountpoints */
#define INTERNAL_MMC_MOUNTPOINT  "/home/user/MyDocs"
#define REMOVABLE_MMC_MOUNTPOINT "/media/mmc1"
#define HOME_MOUNTPOINT  "/home"

static void
misc_init ()
{
  lc_messages = getenv ("LC_MESSAGES");
  DBG ("LC_MESSAGES %s", lc_messages);

  DBG ("OSSO_PRODUCT_HARDWARE %s", getenv ("OSSO_PRODUCT_HARDWARE"));

  load_system_settings ();
  read_domain_conf ();

  AptWorkerCache::Initialize ();

  cache_init (false);

  clean_temp_catalogues ();

  // initialize the MMC mount points with defaults
  setenv ("INTERNAL_MMC_MOUNTPOINT", INTERNAL_MMC_MOUNTPOINT, 1);
  setenv ("REMOVABLE_MMC_MOUNTPOINT", REMOVABLE_MMC_MOUNTPOINT, 1);
}

void
set_options (const char *options)
{
  if (strchr (options, 'B'))
    flag_break_locks = true;

  if (strchr (options, 'D'))
    flag_allow_wrong_domains = true;

  if (strchr (options, 'M'))
    flag_download_packages_to_mmc = true;

  if (strchr (options, 'A'))
    flag_use_apt_algorithms = true;
}

void
cmd_set_options ()
{
  const char *options = request.decode_string_in_place ();
  set_options (options);
}

void
cmd_set_env ()
{
  const char *http_proxy = request.decode_string_in_place ();
  const char *https_proxy = request.decode_string_in_place ();
  const char *internal_mmc = request.decode_string_in_place ();
  const char *removable_mmc = request.decode_string_in_place ();

  if (http_proxy)
    {
      setenv ("http_proxy", http_proxy, 1);
      DBG ("http_proxy: %s", http_proxy);
    }
  else
    {
      // clear HTTP_PROXY from environment otherwise
      unsetenv ("http_proxy");
    }

  if (https_proxy)
    {
      setenv ("https_proxy", https_proxy, 1);
      DBG ("https_proxy: %s", https_proxy);
    }
  else
    {
      // clear HTTPS_PROXY from environment otherwise
      unsetenv ("https_proxy");
    }

  if (internal_mmc)
    {
      setenv ("INTERNAL_MMC_MOUNTPOINT", internal_mmc, 1);
      DBG ("INTERNAL_MMC_MOUNTPOINT: %s", internal_mmc);
    }

  if (removable_mmc)
    {
      setenv ("REMOVABLE_MMC_MOUNTPOINT", removable_mmc, 1);
      DBG ("REMOVABLE_MMC_MOUNTPOINT: %s", removable_mmc);
    }
}

char*
is_fifo (const char *filename)
{
  struct stat statstruct;

  if ((stat (filename, &statstruct) == 0)
      && (S_ISFIFO (statstruct.st_mode)))
    return g_strdup (filename);

  return NULL;
}

int
main (int argc, char **argv)
{
  if (argc == 1)
    usage ();

  argv += 1;
  argc -= 1;

  if (!strcmp (argv[0], "backend"))
    {
      const char *options;

      if (argc != 6)
	{
	  log_stderr ("wrong invocation");
	  exit (1);
	}

      DBG ("starting up");

      char *input_pipe = is_fifo (argv[1]);
      char *output_pipe = is_fifo (argv[2]);
      char *status_pipe = is_fifo (argv[3]);
      char *cancel_pipe = is_fifo (argv[4]);

      if (!(input_pipe && output_pipe && status_pipe && cancel_pipe))
	{
	  g_free (input_pipe);
	  g_free (output_pipe);
	  g_free (status_pipe);
	  g_free (cancel_pipe);

	  log_stderr ("wrong fifo pipes specified");
	  exit (1);
	}

      input_fd = must_open (input_pipe, O_RDONLY | O_NONBLOCK);
      cancel_fd = must_open (cancel_pipe, O_RDONLY | O_NONBLOCK);
      output_fd = must_open (output_pipe, O_WRONLY);
      status_fd = must_open (status_pipe, O_WRONLY);

      g_free (input_pipe);
      g_free (output_pipe);
      g_free (status_pipe);
      g_free (cancel_pipe);

      /* This tells the frontend that the fifos are open.
       */
      send_status (op_general, 0, 0, -1);

      /* This blocks until the frontend has opened our input fifo for
	 writing.
      */
      block_for_read (input_fd);

      /* Reset the O_NONBLOCK flag for the input_fd since we want to block
	 until a new request arrives.  The cancel_fd remains in
	 non-blocking mode since we just poll it periodically.
      */
      must_set_flags (input_fd, O_RDONLY);

      options = argv[5];

      DBG ("starting with pid %d, in %d, out %d, stat %d, cancel %d, options %s",
	   getpid (), input_fd, output_fd, status_fd, cancel_fd,
	   options);

      set_options (options);

      /* Don't let our heavy lifting starve the UI.
       */
      errno = 0;
      if (nice (20) == -1 && errno != 0)
	log_stderr ("nice: %m");

      get_apt_worker_lock (false);
      misc_init ();

      while (true)
	handle_request ();

      return 0;
    }
  else if (!strcmp (argv[0], "check-for-updates"))
    {
      get_apt_worker_lock (true);
      misc_init ();
      return cmdline_check_updates (argv);
    }
  else if (!strcmp (argv[0], "rescue"))
    {
      return cmdline_rescue (argv);
    }
  else if (!strcmp (argv[0], "sleep"))
    {
      get_apt_worker_lock (argv[1] == NULL);
      while (true)
	{
	  fprintf (stderr, "sleeping...\n");
	  sleep (5);
	}
    }
  else
    usage ();
}

/** CACHE HANDLING

    This section contains some general purpose functions to maintain
    the cache of the package database.

    The package cache can represent both the 'current' situation
    (i.e., the union of the information from /var/lib/dpkg/status and
    the various Packages files downloaded from repositories) and a
    'desired' situation.

    A operation such as installing a package is performed by modifying
    the 'desired' situation in the cache and if that leads to a
    consistent configuration, the 'current' situation is brought in
    line with the 'desired' one by downloading the needed archives and
    running dpkg in an approriate way.

    We have our own idea of what should happen when a new package (or
    a new version of a package) is installed, for example, and the
    functions in this section implement this idea.  These principal
    functions are available:

    We remember which operation the cache currently represents.  That
    way, we can avoid recomputing it when the frontend requests the
    same operation multiple times in a row (which it likes to do).

    - cache_init

    This function creates or recreates the cache from
    /var/lib/dpkg/status and the various Packages file from the
    repositories.

    - ensure_cache

    This function tries to make sure that there is a valid
    PACKAGE_CACHE to work with.  It returns true when it succeeds and
    PACKAGE_CACHE is non-NULL then.  The idea is that if the cache
    couldn't be created in the past because of some transient error,
    it might be able to create it now.  Thus, every command handler
    that needs a cache should call ensure_cache.  When ensure_cache
    actually does some work, it will send STATUS messages if it was
    specified with its only parameter.

    - cache_reset ()

    This function resets the 'desired' state of the cache to be
    identical to the 'current' one.

    - mark_for_install ()

    This function modifies the 'desired' state of the cache to reflect
    the installation of the given package.  It will try to achieve a
    consistent 'desired' configuration by installing missing
    dependencies etc.  In general, it implements our own installation
    smartness.

    - mark_for_remove ()

    This function modifies the 'desired' state of the cache to reflect
    the removal of the given package.  As with mark_for_install,
    mark_for_removal implements our own removal smartness.
 */

/* We only report real progress information when reconstructing the
   cache and during downloads.  Only downloads can be cancelled.

   The following two classes allow us to hook into libapt-pkgs
   progress reporting mechanism.  Instances of UPDATE_PROGESS are used
   for cache related activities, and instances of DOWNLOAD_STATUS are
   used when 'acquiring' things.
*/

class UpdateProgress : public OpProgress
{
  bool with_status;

  virtual void
  Update ()
  {
    if (with_status)
      send_status (op_general, (int)Percent, 100, 5);
  }

public:
  UpdateProgress (bool ws) : with_status (ws) { }
};

class DownloadStatus : public pkgAcquireStatus
{
  virtual bool
  MediaChange (string Media, string Drive)
  {
    return false;
  }

  virtual bool
  Pulse (pkgAcquire *Owner)
  {
    pkgAcquireStatus::Pulse (Owner);

    send_status (op_downloading, (int)CurrentBytes, (int)TotalBytes, 1000);

    /* The cancel_fd is in non-blocking mode.
     */
    if (read_byte (cancel_fd) >= 0)
      return false;

    return true;
  }
};

bool
is_user_section (const char *section, const char *end)
{
  if (!section)
    return false;

  int len = end - section;
#if ENABLE_OLD_MAEMO_SECTION_TEST
  if (len > 6 && !memcmp (section, "maemo/", 6))
    return true;
#endif
  
  return len > 5 && !memcmp (section, "user/", 5);
}

bool
is_user_package (const pkgCache::VerIterator &ver)
{
  const char *section = ver.Section ();

  if (!section)
    return false;

  return is_user_section (section, section + strlen (section));
}

/* Our own version of debSystem.  We override the Lock member function
   to be able to break locks and to avoid failing when dpkg has left a
   journal.
*/

static void
clear_dpkg_updates ()
{
  string File = flNotFile(_config->Find("Dir::State::status")) + "updates/";
  DIR *DirP = opendir(File.c_str());
  if (DirP == 0)
    return;
   
  /* We ignore any files that are not all digits, this skips .,.. and 
     some tmp files dpkg will leave behind.. */

  for (struct dirent *Ent = readdir(DirP); Ent != 0; Ent = readdir(DirP))
    {
      bool ignore = false;

      for (unsigned int I = 0; Ent->d_name[I] != 0; I++)
	{
	  // Check if its not a digit..
	  if (isdigit(Ent->d_name[I]) == 0)
	    {
	      ignore = true;
	      break;
	    }
	}

      if (!ignore)
	{
	  log_stderr ("Running 'dpkg --configure dpkg' "
		      "to clean up the journal.");
	  system ("dpkg --configure dpkg");
	  break;
	}
    }

   closedir(DirP);
}

void cache_reset ();

/* The operation represented by the cache.
 */
static char *current_cache_package = NULL;
static bool current_cache_is_install;

static bool
check_cache_state (const char *package, bool is_install)
{
  if (current_cache_package
      && current_cache_is_install == is_install
      && strcmp (current_cache_package, package) == 0)
    return true;

  if (current_cache_package)
    cache_reset ();

  current_cache_package = g_strdup (package);
  current_cache_is_install = is_install;
  return false;
}

/* Initialize libapt-pkg if this has not been done already and
   (re-)create PACKAGE_CACHE.  If the cache can not be created,
   PACKAGE_CACHE is set to NULL and an appropriate message is output.
   */
void
cache_init (bool with_status)
{
  AptWorkerCache *awc = AptWorkerCache::GetCurrent ();

  /* Closes the cache, to prevent getting blocked by other locks in
   * dpkg structures. If we don't do it, changing the apt worker state
   * does not remove the dpkg state lock and then fails on trying to
   * run dpkg */
  /* @todo do we really keep doing this? */
  if (awc->cache)
    {
      DBG ("closing");
      delete awc->action_group;
      awc->cache->Close ();
      delete awc->cache;
      awc->cache = 0;
      DBG ("done");
    }

  /* We need to dump the errors here since any pending errors will
     cause the following operations to fail.
  */
  _error->DumpErrors ();

  /* Clear out the dpkg journal before construction the cache.
   */
  clear_dpkg_updates ();

  UpdateProgress progress (with_status);
  awc->cache = new myCacheFile;

  DBG ("init.");
  if (!awc->cache->Open (progress))
    {
      DBG ("failed.");
      _error->DumpErrors ();
      delete awc->cache;
      awc->cache = 0;
    }

  if (awc->cache)
    {
      /* We create a ActionGroup here that is active for the whole
	 lifetime of the cache.  This prevents libapt-pkg from
	 performing certain expensive operations that we don't need to
	 be done intermediately, such as garbage collection.
      */
      pkgDepCache &cache = *awc->cache;
      awc->action_group = new pkgDepCache::ActionGroup (cache);
    }

  cache_reset ();

  if (awc->cache)
    write_available_updates_file ();
}

bool
ensure_cache (bool with_status)
{
  AptWorkerCache * awc = 0;
  
  awc = AptWorkerCache::GetCurrent ();
  if (awc->cache == NULL)
    cache_init (with_status);

  return awc->cache != NULL;
}

/* Determine whether a package was installed automatically to satisfy
   a dependency.
*/
bool
is_auto_package (pkgCache::PkgIterator &pkg)
{
  AptWorkerCache *awc = AptWorkerCache::GetCurrent ();
  pkgDepCache &cache = *(awc->cache);
  
  return (cache[pkg].Flags & pkgCache::Flag::Auto) != 0;
}

/* Determine whether a package is related to the current operation.
*/
bool
is_related (pkgCache::PkgIterator &pkg)
{
  AptWorkerCache *awc = AptWorkerCache::GetCurrent ();
  return awc->cache->extra_info[pkg->ID].related;
}

void
mark_related (const pkgCache::VerIterator &ver)
{
  AptWorkerCache *awc = AptWorkerCache::GetCurrent ();
  const pkgCache::PkgIterator &pkg = ver.ParentPkg();

  if (awc->cache->extra_info[pkg->ID].related)
    return;

  awc->cache->extra_info[pkg->ID].related = true;

  pkgDepCache &cache = *awc->cache;

  if (pkg.State() == pkgCache::PkgIterator::NeedsUnpack)
    cache.SetReInstall (pkg, true);

  /* When there are some packages that might need configuring or
     unpacking, we also mark all dependencies of this package as
     related so that we try to unpack/configure them as well.
  */
  if (cache.BadCount () > 0)
    {
      for (pkgCache::DepIterator D = ver.DependsList(); D.end() == false;
	   D++)
	{
	  const pkgCache::PkgIterator &dep_pkg = D.TargetPkg ();
	  const pkgCache::VerIterator &dep_ver = dep_pkg.CurrentVer ();
	  if (!dep_ver.end())
	    mark_related (dep_ver);
	}
    }
}

/* Revert the cache to its initial state.  More concretely, all
   packages are marked as 'keep' and 'unrelated'.

   XXX - let libapt-pkg handle the auto flags.
*/
void
cache_reset_package (pkgCache::PkgIterator &pkg)
{
  AptWorkerCache *awc = AptWorkerCache::GetCurrent ();
  pkgDepCache &cache = *(awc->cache);

  cache.MarkKeep (pkg);

  if (awc->cache->extra_info[pkg->ID].autoinst)
    cache[pkg].Flags |= pkgCache::Flag::Auto;
  else
    cache[pkg].Flags &= ~pkgCache::Flag::Auto;
  
  awc->cache->extra_info[pkg->ID].related = false;
  awc->cache->extra_info[pkg->ID].soft = false;
}

static bool
any_newly_or_related_broken ()
{
  AptWorkerCache *awc = AptWorkerCache::GetCurrent ();
  if (awc->cache == NULL)
    return false;

  pkgDepCache &cache = *(awc->cache);
  for (pkgCache::PkgIterator pkg = cache.PkgBegin(); !pkg.end (); pkg++)
    {
      if (cache[pkg].InstBroken() &&
	  (!cache[pkg].NowBroken() || is_related (pkg)))
	return true;
    }
  return false;
}

void
cache_reset ()
{
  AptWorkerCache *awc = AptWorkerCache::GetCurrent ();
  if (awc->cache == NULL)
    return;

  pkgDepCache &cache = *(awc->cache);

  for (pkgCache::PkgIterator pkg = cache.PkgBegin(); !pkg.end (); pkg++)
    cache_reset_package (pkg);

  g_free (current_cache_package);
  current_cache_package = NULL;
}

/* Try to fix packages that have been broken by undoing soft changes.

   This is not really complicated, the code only looks impenetrable
   because of libapt-pkg's data structures.

   For each package that is broken for the planned operation, we try
   to fix it by undoing the removal of softly removed packages that it
   depends on.  We do this in a loop since a package that has been put
   back might be broken itself and be in need of fixing.
*/
void
fix_soft_packages ()
{
  AptWorkerCache *awc = AptWorkerCache::GetCurrent ();
  if (awc->cache == NULL)
    return;

  pkgDepCache &cache = *(awc->cache);

  bool something_changed;

  do 
    {
      DBG ("FIX");

      something_changed = false;
      for (pkgCache::PkgIterator pkg = cache.PkgBegin(); !pkg.end (); pkg++)
	{
	  if (cache[pkg].InstBroken())
	    {
	      pkgCache::DepIterator Dep =
		cache[pkg].InstVerIter(cache).DependsList();
	      for (; Dep.end() != true;)
		{
		  // Grok or groups
		  pkgCache::DepIterator Start = Dep;
		  bool Result = true;
		  for (bool LastOR = true;
		       Dep.end() == false && LastOR == true;
		       Dep++)
		    {
		      LastOR = ((Dep->CompareOp & pkgCache::Dep::Or)
				== pkgCache::Dep::Or);
		    
		      if ((cache[Dep] & pkgDepCache::DepInstall)
			  == pkgDepCache::DepInstall)
			Result = false;
		    }
		
		  // Dep is satisfied okay.
		  if (Result == false)
		    continue;

		  // Try to fix it by putting back the first softly
		  // removed target

		  for (bool LastOR = true;
		       Start.end() == false && LastOR == true;
		       Start++)
		    {
		      LastOR = ((Start->CompareOp & pkgCache::Dep::Or)
				== pkgCache::Dep::Or);
		    
		      pkgCache::PkgIterator Pkg = Start.TargetPkg ();

		      if (((cache[Start] & pkgDepCache::DepInstall)
			   != pkgDepCache::DepInstall)
			  && !Pkg.end()
			  && cache[Pkg].Delete()
              && awc->cache->extra_info[Pkg->ID].soft)
			{
			  DBG ("= %s", Pkg.Name());
			  cache_reset_package (Pkg);
			  something_changed = true;
			  break;
			}
		    }
		}
	    }
	}
    } while (something_changed);
}

/* Determine whether PKG replaces TARGET.
 */
static bool
package_replaces (pkgCache::PkgIterator &pkg,
		  pkgCache::PkgIterator &target)
{
  AptWorkerCache *awc = AptWorkerCache::GetCurrent ();
  pkgDepCache &cache = *(awc->cache);

  pkgCache::DepIterator Dep = cache[pkg].InstVerIter(cache).DependsList();
  for (; Dep.end() != true; Dep++)
    {
      if (Dep->Type == pkgCache::Dep::Replaces)
	{
	  //SPtrArray<pkgCache::Version *> List = Dep.AllTargets();
	  std::unique_ptr<pkgCache::Version *> List(Dep.AllTargets());
	  for (pkgCache::Version **I = List.get(); *I != 0; I++)
	    {
	      pkgCache::VerIterator Ver(cache,*I);
	      pkgCache::PkgIterator Pkg = Ver.ParentPkg();
	  
	      if (Pkg == target)
		return true;
	    }
	}
    }
  return false;
}

#if 0
/* Determine whether PKG is a critical dependency of other packages
   thata re going to be installed.
 */
static bool
package_is_needed (pkgCache::PkgIterator &pkg)
{
  AptWorkerCache *awc = AptWorkerCache::GetCurrent ();
  pkgDepCache &cache = *(awc->cache);

  pkgCache::DepIterator Dep = pkg.RevDependsList();
  for (; Dep.end() != true; Dep++)
    {
      if (Dep->Type == pkgCache::Dep::PreDepends
	  || Dep->Type == pkgCache::Dep::Depends)
	{
	  pkgCache::PkgIterator other_pkg = Dep.ParentPkg();
	  pkgCache::VerIterator other_ver = Dep.ParentVer();
	  pkgCache::VerIterator inst_ver = cache[other_pkg].InstVerIter(cache);

	  if (other_ver == inst_ver)
	    return true;
	}
    }
  return false;
}
#endif

/* Mark a package for installation, using a 'no-surprises' approach
   suitable for the Application Manager.

   Concretely, installing a package will never automatically remove
   other packages.  Thus, we undo the removals scheduled by
   MarkInstall.  Doing this will break the original package, but that
   is what we want.
*/

static void mark_for_remove_1 (pkgCache::PkgIterator &pkg, bool soft);

static void
mark_for_install_1 (pkgCache::PkgIterator &pkg, int level)
{
  AptWorkerCache *awc = AptWorkerCache::GetCurrent ();
  pkgDepCache &cache = *(awc->cache);

  /* This check is just to be extra robust against infinite
     recursions.  They shouldn't happen, but you never know...
  */
  if (level > 100)
    return;

  mark_related (cache[pkg].CandidateVerIter(cache));

  /* Avoid recursion if package is already marked for installation but
     try to fix it when it is broken.
   */
  if (cache[pkg].Mode == pkgDepCache::ModeInstall
      && !cache[pkg].InstBroken ())
    return;

  DBG ("+ %s", pkg.Name());

  /* Now mark it and return if that fails.  Both ModeInstall and
     ModeKeep are fine.  ModeKeep only happens for broken packages.
   */
  cache.MarkInstall (pkg, false);
  if (cache[pkg].Mode != pkgDepCache::ModeInstall
      && cache[pkg].Mode != pkgDepCache::ModeKeep)
    return;

  /* Try to satisfy dependencies.  We can't use MarkInstall with
     AutoInst == true since we don't like how it handles conflicts,
     and we have our own way of uninstalling packages.

     The code below is lifted from pkgDepCache::MarkInstall.  Sorry
     for introducing this mess here.
  */

  pkgCache::DepIterator Dep = cache[pkg].InstVerIter(cache).DependsList();
  for (; Dep.end() != true;)
    {
      // Grok or groups
      pkgCache::DepIterator Start = Dep;
      bool Result = true;
      unsigned Ors = 0;
      for (bool LastOR = true; Dep.end() == false && LastOR == true;
	   Dep++,Ors++)
	{
	  LastOR = (Dep->CompareOp & pkgCache::Dep::Or) == pkgCache::Dep::Or;

	  if ((cache[Dep] & pkgDepCache::DepInstall) == pkgDepCache::DepInstall)
	    Result = false;
	}
      
      // Dep is satisfied okay.
      if (Result == false)
	continue;

      /* Check if this dep should be consider for install. If it is a user
         defined important dep and we are installed a new package then 
	 it will be installed. Otherwise we only worry about critical deps */
      if (cache.IsImportantDep(Start) == false)
	continue;

      if (pkg->CurrentVer != 0 && Start.IsCritical() == false)
	continue;
      
      /* If we are in an or group locate the first or that can 
         succeed. We have already cached this.. */
      for (; Ors > 1 
	     && (cache[Start] & pkgDepCache::DepCVer) != pkgDepCache::DepCVer;
	   Ors--)
	Start++;

      /* This bit is for processing the possibilty of an install/upgrade
         fixing the problem */
      //SPtrArray<pkgCache::Version *> List = Start.AllTargets();
      std::unique_ptr<pkgCache::Version *> List;
      List.reset(Start.AllTargets());
      if ((cache[Start] & pkgDepCache::DepCVer) == pkgDepCache::DepCVer)
	{
	  // Right, find the best version to install..
	  pkgCache::Version **Cur = List.get();
	  pkgCache::PkgIterator P = Start.TargetPkg();
	  pkgCache::PkgIterator InstPkg(cache,0);

	  // See if there are direct matches (at the start of the list)
	  for (; *Cur != 0 && (*Cur)->ParentPkg == P.MapPointer(); Cur++)
	    {
	      pkgCache &pkgcache = cache.GetCache ();
	      pkgCache::PkgIterator Pkg(pkgcache,
					pkgcache.PkgP + (*Cur)->ParentPkg);
	      if (cache[Pkg].CandidateVer != *Cur)
		continue;
	      InstPkg = Pkg;
	      break;
	    }

	  // Select the highest priority providing package
	  if (InstPkg.end() == true)
	    {
	      pkgPrioSortList(cache,Cur);
	      for (; *Cur != 0; Cur++)
		{
		  pkgCache &pkgcache = cache.GetCache ();
		  pkgCache::PkgIterator
		    Pkg(pkgcache,pkgcache.PkgP + (*Cur)->ParentPkg);
		  if (cache[Pkg].CandidateVer != *Cur)
		    continue;
		  InstPkg = Pkg;
		  break;
		}
	    }
	  
	  if (InstPkg.end() == false)
	    {
	      mark_for_install_1 (InstPkg, level + 1);

	      // Set the autoflag, after MarkInstall because
	      // MarkInstall unsets it
	      if (P->CurrentVer == 0)
		cache[InstPkg].Flags |= pkgCache::Flag::Auto;
	    }

	  continue;
	}

      /* For conflicts/replaces combinations we de-install the package
         with mark_for_remove, but only if it is a non-user package.
         (Conflicts and Replaces may not have or groups.)
      */
      if (Start->Type == pkgCache::Dep::Conflicts
	  || Start->Type == pkgCache::Dep::Obsoletes)
	{
	  for (pkgCache::Version **I = List.get(); *I != 0; I++)
	    {
	      pkgCache::VerIterator Ver(cache,*I);
	      pkgCache::PkgIterator target = Ver.ParentPkg();

	      if (!is_user_package (Ver)
		  && package_replaces (pkg, target))
		mark_for_remove_1 (target, true);
	    }
	  continue;
	}
    }
}

static void
mark_for_install (pkgCache::PkgIterator &pkg)
{
  DBG ("INSTALL %s", pkg.Name());

  if (flag_use_apt_algorithms)
    {
      AptWorkerCache *awc = AptWorkerCache::GetCurrent ();
      pkgDepCache &Cache = *(awc->cache);
      pkgDepCache::StateCache &State = Cache[pkg];

      pkgProblemResolver Fix(&Cache);

      Fix.Clear(pkg);
      Fix.Protect(pkg);   

      Cache.MarkInstall(pkg,false);
      if (State.Install() == false)
	{
	  if (pkg->CurrentVer && pkg.CurrentVer().Downloadable())
	    Cache.SetReInstall(pkg,true);
	} 

      // Install it with autoinstalling enabled (if we not respect the minial
      // required deps or the policy)
      if (State.InstBroken() == true || State.InstPolicyBroken() == true)
	Cache.MarkInstall(pkg,true);

      if (Fix.Resolve(true) == false)
	 _error->Discard();
    }
  else
    {
      mark_for_install_1 (pkg, 0);
      fix_soft_packages ();
    }
}

/* Mark every upgradeable non-user package for installation.
 */
static void
mark_sys_upgrades ()
{
  AptWorkerCache *awc = AptWorkerCache::GetCurrent ();
  pkgDepCache &cache = *(awc->cache);

  DBG ("UPGRADE");

  for (pkgCache::PkgIterator p = cache.PkgBegin (); !p.end (); p++)
    {
      if (!p.CurrentVer().end()
	  && !is_user_package (p.CurrentVer())
	  && cache[p].Keep())
	mark_for_install_1 (p, 0);
    }
  fix_soft_packages ();
}

/* Mark the named package for installation.  This function also
   handles magic packages like "magic:sys".
*/

static bool
mark_named_package_for_install (const char *package)
{
  if (check_cache_state (package, true))
    return true;

  AptWorkerCache *awc = AptWorkerCache::GetCurrent ();
  if (!strcmp (package, "magic:sys"))
    {
      mark_sys_upgrades ();
      return true;
    }
  else
    {
      pkgDepCache &cache = *(awc->cache);
      pkgCache::PkgIterator pkg = cache.FindPkg (package);
      if (!pkg.end())
	{
	  mark_for_install (pkg);
	  return true;
	}
      else
	return false;
    }
}

/* Mark a package for removal and also remove as many of the packages
   that it depends on as possible.
*/
static void
mark_for_remove_1 (pkgCache::PkgIterator &pkg, bool soft)
{
  AptWorkerCache *awc = AptWorkerCache::GetCurrent ();
  pkgDepCache &cache = *(awc->cache);

  if (cache[pkg].Delete ())
    return;

  DBG ("- %s%s", pkg.Name(), soft? " (soft)" : "");

  cache.MarkDelete (pkg);
  cache[pkg].Flags &= ~pkgCache::Flag::Auto;
  awc->cache->extra_info[pkg->ID].soft = soft;

  if (!cache[pkg].Delete ())
    return;

  // Now try to remove all non-user, auto-installed dependencies of
  // this package.

  pkgCache::VerIterator cur = pkg.CurrentVer ();
  if (cur.end ())
    return;

  for (pkgCache::DepIterator dep = cur.DependsList(); dep.end() == false;
       dep++)
    {
      if (dep->Type == pkgCache::Dep::PreDepends ||
	  dep->Type == pkgCache::Dep::Depends)
	{
	  pkgCache::PkgIterator p = dep.TargetPkg ();
	  if (!p.end ()
	      && is_auto_package (p)
	      && !p.CurrentVer().end()
	      && !is_user_package (p.CurrentVer()))
	    mark_for_remove_1 (p, true);
	}
    }
}

static void
mark_for_remove (pkgCache::PkgIterator &pkg)
{
  DBG ("REMOVE %s", pkg.Name());

  if (check_cache_state (pkg.Name (), false))
    return;

  if (flag_use_apt_algorithms)
    {
      AptWorkerCache *awc = AptWorkerCache::GetCurrent ();
      pkgDepCache &Cache = *(awc->cache);

      pkgProblemResolver Fix(&Cache);

      Fix.Clear(pkg);
      Fix.Protect(pkg);   

      Fix.Remove(pkg);
      Cache.MarkDelete (pkg,false);

      if (Fix.Resolve(true) == false)
	 _error->Discard();
    }
  else
    {
      mark_for_remove_1 (pkg, false);
      fix_soft_packages ();
    }
}

/* Getting the package record in a nicely parsable form.
 */

struct package_record {
  package_record ();

  pkgRecords Recs;
  pkgRecords::Parser *P;
  pkgTagSection section;
  bool valid;

  bool has (const char *tag);
  string get_string (const char *tag);
  char *get (const char *tag);
  int get_int (const char *tag, int def);

  string get_localized_string (const char *tag);

  void lookup(const pkgCache::VerIterator &ver)
    {
  const char *start, *stop;
      P = &Recs.Lookup (ver.FileList ());

      P->GetRec (start, stop);

  /* NOTE: pkTagSection::Scan only succeeds when the record ends in
           two newlines, but pkgRecords::Parser::GetRec does not
           include the second newline in its returned region.
           However, that second newline is always there, so we just
           pass one more character to Scan.
  */
  
  valid = section.Scan (start, stop-start+1);
    }
};

package_record::package_record ()
  : Recs (*(AptWorkerCache::GetCurrent ()->cache)),
    P(NULL),
    valid(false)
{
}

bool
package_record::has (const char *tag)
{
  unsigned pos;
  return valid && section.Find (tag, pos);
}

string
package_record::get_string (const char *tag)
{
  if (valid)
    return section.FindS (tag);
  else
    return string ("");
}
 
int
all_white_space (const char *text)
{
  while (*text)
    if (!isspace (*text++))
      return 0;
  return 1;
}

char *
package_record::get (const char *tag)
{
  if (!valid)
    return NULL;

  string res = get_string (tag);
  if (all_white_space (res.c_str ()))
    return NULL;
  else
    return g_strdup (res.c_str());
}

int
package_record::get_int (const char *tag, int def)
{
  if (!valid)
    return def;

  return section.FindI (tag, def);
}

string
package_record::get_localized_string (const char *tag)
{
  if (lc_messages && *lc_messages)
    {
      char *locale_tag = g_strdup_printf ("%s-%s", tag, lc_messages);

      if (has (locale_tag))
	{
	  string res = get_string (locale_tag);
	  g_free (locale_tag);
	  return res;
	}
      g_free (locale_tag);
    }

  return get_string (tag);
}

/** COMMAND HANDLERS
 */

/* APTCMD_GET_PACKAGE_LIST 

   The get_package_list command can do some filtering and we have a
   few utility functions for implementing the necessary checks.  The
   check generally take cache iterators to identify a package or a
   version.
 */

bool
name_matches_pattern (pkgCache::PkgIterator &pkg,
		      const char *pattern)
{
  bool match = false;
  char **words = g_strsplit (pattern, " ", 0);
  int i;

  if (words == NULL)
    return false;

  for (i = 0; words[i] != NULL; i++)
    if (strcasestr (pkg.Name(), words[i]))
      match = true;
    else
      {
        match = false;
        break;
      }

  g_strfreev (words);
  return match;
}

bool
description_matches_pattern (pkgCache::VerIterator &ver,
			     const char *pattern)
{
  bool match = false;
  char **words = g_strsplit (pattern, " ", 0);
  package_record rec;
  rec.lookup(ver);
  const char *desc = rec.P->LongDesc().c_str();
  int i;

  if (words == NULL)
    return false;

  for (i = 0; words[i] != NULL; i++)
    if (strcasestr (desc, words[i]))  // XXX - UTF8?
      match = true;
    else
      {
        match = false;
        break;
      }

  g_strfreev (words);
  return match;
}

static string
get_description (int summary_kind,
		 pkgCache::PkgIterator &pkg,
		 package_record &rec)
{
  string res;

  if (summary_kind == 1 && !pkg.CurrentVer().end())
    res = rec.get_localized_string ("Maemo-Upgrade-Description");

  if (res.empty())
    {
      /* XXX - support apt's own method of localizing descriptions as
	       well.
      */
      res = rec.get_localized_string ("Description");
    }

  return res;
}

static string
get_short_description (int summary_kind,
		       pkgCache::PkgIterator &pkg,
		       package_record &rec)
{
  string res = get_description (summary_kind, pkg, rec);
  string::size_type pos = res.find('\n');
  if (pos != string::npos)
    return string (res,0,pos);
  return res;
}

static string
get_long_description (int summary_kind,
		      pkgCache::PkgIterator &pkg,
		      package_record &rec)
{
  return get_description (summary_kind, pkg, rec);
}

static char *
get_icon (package_record &rec)
{
  return rec.get ("Maemo-Icon-26");
}

struct flag_struct {
  const char *name;
  int flag;
} flag_names[] = {
  { "close-apps",       pkgflag_close_apps },
  { "suggest-backup",   pkgflag_suggest_backup },
  { "reboot",           pkgflag_reboot },
  { "system-update",    pkgflag_system_update },
  { "flash-and-reboot", pkgflag_flash_and_reboot },
  { NULL,               0 }
};

static int
get_flags (package_record &rec)
{
  int flags = 0;
  char *flag_string = rec.get ("Maemo-Flags");
  char *ptr = flag_string, *tok;
  while ((tok = strsep (&ptr, ",")))
    {
      for (int i = 0; flag_names[i].name != NULL; i++)
	if (tokens_equal (flag_names[i].name, tok))
	  {
	    flags |= flag_names[i].flag;
	    break;
	  }
    }
  g_free (flag_string);

  return flags;
}

static string
get_pretty_name (package_record &rec)
{
  return rec.get_localized_string ("Maemo-Display-Name");
}

static int64_t
get_required_free_space (package_record &rec)
{
  return 1024 * (int64_t) rec.get_int ("Maemo-Required-Free-Space", 0);
}

static void
encode_version_info (int summary_kind, package_record &rec,
		     const pkgCache::VerIterator &ver, bool include_size)
{
  char *icon;

  response.encode_string (ver.VerStr ());
  if (include_size)
    response.encode_int64 (ver->InstalledSize);
  response.encode_string (ver.Section ());
  string pretty = get_pretty_name (rec);
  response.encode_string (pretty.empty()? NULL : pretty.c_str());
  pkgCache::PkgIterator pkg = ver.ParentPkg();
  response.encode_string 
    (get_short_description (summary_kind, pkg, rec).c_str());
  icon = get_icon (rec);
  response.encode_string (icon);
  g_free (icon);
}

static void
encode_empty_version_info (bool include_size)
{
  response.encode_string (NULL);
  if (include_size)
    response.encode_int64 (0);
  response.encode_string (NULL);
  response.encode_string (NULL);
  response.encode_string (NULL);
  response.encode_string (NULL);
}

static void
ssu_packages_free ()
{
  if (ssu_packages != NULL)
    {
      g_array_free (ssu_packages, TRUE);
      ssu_packages = NULL;
    }
}

static void
ssu_packages_set (GSList *ssu_list)
{
  ssu_packages_free ();

  if (ssu_list != NULL)
    {
      ssu_packages = g_array_sized_new (TRUE, FALSE, sizeof (gchar*),
                                        g_slist_length (ssu_list));
      for (GSList *item = g_slist_reverse (ssu_list);
           item; item = g_slist_next (item))
        {
          /* Do not strdup strings as they will be used in the GArray */
          ssu_packages = g_array_append_val (ssu_packages, item->data);
        }
    }
}

void
cmd_get_package_list ()
{
  AptWorkerCache *awc = AptWorkerCache::GetCurrent ();
  bool only_user = request.decode_int ();
  bool only_installed = request.decode_int ();
  bool only_available = request.decode_int ();
  const char *pattern = request.decode_string_in_place ();
  bool show_magic_sys = request.decode_int ();
  GSList *ssu_pkgs_found = NULL;

  if (!ensure_cache (true))
    {
      response.encode_int (0);
      return;
    }

  response.encode_int (1);
  pkgDepCache &cache = *(awc->cache);

  package_record irec;
  package_record crec;

  for (pkgCache::PkgIterator pkg = cache.PkgBegin(); !pkg.end (); pkg++)
    {
      int flags = 0;
      bool crec_looked = false;
      bool irec_looked = false;

      if (read_byte (cancel_fd) >= 0)
        return;

      /* Get installed and candidate iterators for current package */
      pkgCache::VerIterator installed = pkg.CurrentVer ();
      pkgDepCache::StateCache& sc = cache[pkg];
      pkgCache::VerIterator candidate = sc.CandidateVerIter(cache);

      bool iend = installed.end ();
      bool cend = candidate.end ();

      // skip non user packages if requested.  Both the installed and
      // candidate versions must be non-user packages for a package to
      // be skipped completely.
      //
      if (only_user
	  && (iend || !is_user_package (installed))
	  && (cend || !is_user_package (candidate)))
	continue;

      // skip not-installed packages if requested
      //
      if (only_installed && iend)
	continue;

      // skip non-available packages if requested
      //
      if (only_available && cend)
	continue;

      // skip foreign architecture packages
      //
      if (!only_installed && strcmp(pkg.Arch (), DEB_HOST_ARCH))
        continue;

      // skip packages that are not installed and not available
      //
      if (iend && cend)
	continue;

      // skip packages that don't match the pattern if requested
      //
      if (pattern
	  && !(name_matches_pattern (pkg, pattern)
	       || (!iend && description_matches_pattern (installed, pattern))
	       || (!cend && description_matches_pattern (candidate, pattern))))
	continue;

      // Look for the SSU package if needed
      //
      if (!iend || !cend)
        {
          if(!cend)
            {
              crec.lookup(candidate);
              crec_looked = true;
              flags = get_flags (crec);
            }
          else
            {
              irec.lookup(installed);
              irec_looked = true;
              flags = get_flags (irec);
            }
          if (flags & pkgflag_system_update)
            {
              if (ssu_packages_needs_refresh)
                {
                  /* Add it to the local GSList */
                  ssu_pkgs_found = g_slist_prepend (ssu_pkgs_found,
                                                    g_strdup (pkg.Name ()));
                }

              // skip system update meta-packages that are not installed
              //
              if (only_user && iend && !cend)
                continue;
            }
        }

      // Name
      response.encode_string (pkg.Name ());

      // Broken.
      bool broken = (sc.NowBroken()
		     || (pkg.State () != pkgCache::PkgIterator::NeedsNothing));
      response.encode_int (broken);

      // Installed version
      if (!iend)
        {
          if (!irec_looked)
            {
              irec.lookup(installed);
              irec_looked = true;
            }
	  encode_version_info (2, irec, installed, true);
        }
      else
	encode_empty_version_info (true);

      // Available version
      //
      // We only offer an available version if the package is not
      // installed at all, or if the available version is newer than
      // the installed one, or if the installed version is broken.

      if (!cend && (iend
	      || installed.CompareVer (candidate) < 0
	      || broken))
      {
        if (!crec_looked)
          {
            crec.lookup(candidate);
            crec_looked = true;
          }
	encode_version_info (1, crec, candidate, false);
      }
      else
	encode_empty_version_info (false);

      if (flags == 0 && !cend && !crec_looked)
	{
	  crec.lookup(candidate);
	  flags = get_flags (crec);
	}
      response.encode_int (flags);
    }

  /* Update the global GArray, if needed */
  if (ssu_packages_needs_refresh)
    {
      ssu_packages_set (ssu_pkgs_found);

      if (ssu_pkgs_found != NULL)
        {
          /* Free local GSList */
          g_slist_free (ssu_pkgs_found);
          ssu_pkgs_found = NULL;
        }

      /* Update global flag */
      ssu_packages_needs_refresh = false;
    }

  if (show_magic_sys)
    {
      // Append the "magic:sys" package that represents all system
      // packages.  This artificial package is identified by its name
      // and handled specially by MARK_NAMED_PACKAGE_FOR_INSTALL, etc.

      // Name
      response.encode_string ("magic:sys");

      // Broken?  XXX - give real information here
      response.encode_int (FALSE);

      // Installed version
      response.encode_string ("");
      response.encode_int64 (1000);
      response.encode_string ("user/system");
      response.encode_string ("Operating System");
      response.encode_string ("All system packages");
      response.encode_string (NULL);

      // Available version
      response.encode_string ("");
      response.encode_string ("user/system");
      response.encode_string ("Operating System");
      response.encode_string ("Updates to all system packages");
      response.encode_string (NULL);
    }
}

void
cmd_get_system_update_packages ()
{
  AptWorkerCache *awc = AptWorkerCache::GetCurrent ();

  if (!ensure_cache (true))
    return;

  pkgDepCache &cache = *(awc->cache);

  for (pkgCache::PkgIterator pkg = cache.PkgBegin(); !pkg.end (); pkg++)
    {
      pkgCache::VerIterator installed = pkg.CurrentVer ();
      pkgCache::VerIterator candidate = cache[pkg].CandidateVerIter(cache);

      if (installed.end () || candidate.end ())
	continue;

      package_record rec;
      rec.lookup(candidate);
      int flags = get_flags (rec);
      if (flags & pkgflag_system_update)
	response.encode_string (pkg.Name ());
    }

  response.encode_string (NULL);
}

/* APTCMD_GET_PACKAGE_INFO

   This command performs a simulated install and removal of the
   specified package to gather the requested information.
 */

static int
installable_status_1 (pkgCache::PkgIterator &pkg)
{
  AptWorkerCache *awc = AptWorkerCache::GetCurrent ();
  pkgDepCache &Cache = *(awc->cache);
  pkgCache::VerIterator Ver = Cache[pkg].InstVerIter(Cache);

  bool some_missing = false, some_conflicting = false;

  if (Ver.end() == true)
    return status_unable;
      
  for (pkgCache::DepIterator D = Ver.DependsList(); D.end() == false;)
    {
      // Compute a single dependency element (glob or)
      pkgCache::DepIterator Start;
      pkgCache::DepIterator End;
      D.GlobOr(Start,End); // advances D

      if ((Cache[End] & pkgDepCache::DepGInstall)
	  == pkgDepCache::DepGInstall)
	continue;

      if (Start->Type == pkgCache::Dep::PreDepends ||
	  Start->Type == pkgCache::Dep::Depends)
	some_missing = true;
      else if (Start->Type == pkgCache::Dep::Conflicts)
	some_conflicting = true;
    }

  if (some_missing)
    return status_missing;
  if (some_conflicting)
    return status_conflicting;

  return status_unable;
}

static int
combine_status (int s1, int s2)
{
  return max (s1, s2);
}

static int
installable_status ()
{
  AptWorkerCache *awc = AptWorkerCache::GetCurrent ();
  pkgDepCache &cache = *(awc->cache);
  int installable_status = status_unable;

  for (pkgCache::PkgIterator pkg = cache.PkgBegin();
       pkg.end() != true;
       pkg++)
    {
      /* If a non-related package gets newly broken, we report this as
	 a conflict.  If a related package is broken, we take a closer
	 look.
      */
      if (cache[pkg].InstBroken())
	{
	  if (is_related (pkg))
	    {
	      installable_status =
		combine_status (installable_status_1 (pkg),
				installable_status);
	    }
	  else if (!cache[pkg].NowBroken())
	    {
	      installable_status =
		combine_status (status_conflicting,
				installable_status);
	    }
	}
    }

  return installable_status;
}

static int
removable_status ()
{
  AptWorkerCache *awc = AptWorkerCache::GetCurrent ();
  pkgDepCache &cache = *(awc->cache);
  for (pkgCache::PkgIterator pkg = cache.PkgBegin();
       pkg.end() != true;
       pkg++)
    {
      if (cache[pkg].InstBroken())
	return status_needed;
    }

  return status_unable;
}

void
cmd_get_package_info ()
{
  const char *package = request.decode_string_in_place ();
  bool only_installable_info = request.decode_int ();

  apt_proto_package_info info;

  info.installable_status = status_unknown;
  info.download_size = 0;
  info.install_user_size_delta = 0;
  info.required_free_space = 0;
  info.install_flags = 0;
  info.removable_status = status_unknown;
  info.remove_user_size_delta = 0;

  if (ensure_cache (true))
    {
      AptWorkerCache *awc = AptWorkerCache::GetCurrent ();
      pkgDepCache &cache = *(awc->cache);
      pkgCache::PkgIterator pkg = cache.FindPkg (package);
      package_record rec;

      // simulate install

      mark_named_package_for_install (package);
      if (any_newly_or_related_broken ())
	info.installable_status = installable_status ();
      else
	info.installable_status = status_able;
      info.download_size = (int64_t) cache.DebSize ();
      info.install_user_size_delta = (int64_t) cache.UsrSize ();

      for (pkgCache::PkgIterator pkg = cache.PkgBegin();
	   pkg.end() != true;
	   pkg++)
	{
	  if (is_related (pkg)
	      && (cache[pkg].Upgrade()
		  || pkg.State() != pkgCache::PkgIterator::NeedsNothing))
	    {
	      pkgCache::VerIterator ver = cache[pkg].CandidateVerIter(cache);

	      rec.lookup(ver);
	      info.install_flags |= get_flags (rec);
	      info.required_free_space += get_required_free_space (rec);
	    }
	}

      if (!only_installable_info)
	{
	  // simulate remove

	  if (!strcmp (package, "magic:sys"))
	    {
	      info.removable_status = status_system_update_unremovable;
	    }
	  else
	    {
	      if (!pkg.end())
		mark_for_remove (pkg);

	      for (pkgCache::PkgIterator pkg = cache.PkgBegin();
		   pkg.end() != true;
		   pkg++)
		{
		  if (cache[pkg].Delete())
		    {
		      pkgCache::VerIterator ver = pkg.CurrentVer ();

		      rec.lookup(ver);
		      int flags = get_flags (rec);
		      if (flags & pkgflag_system_update)
			{
			  info.removable_status =
			    status_system_update_unremovable;
			  break;
			}
		    }
		}

	      if (info.removable_status == status_unknown)
		{
		  if (any_newly_or_related_broken ())
		    info.removable_status = removable_status ();
		  else
		    info.removable_status = status_able;
		}
	      info.remove_user_size_delta = (int64_t) cache.UsrSize ();
	    }
	}
    }

  response.encode_mem (&info, sizeof (apt_proto_package_info));
}

/* APTCMD_GET_PACKAGE_DETAILS
   
   Like APTCMD_GET_PACKAGE_INFO, this command performs a simulated
   install or removal (as requested), but it gathers a lot more
   information about the package and what is happening.
*/

static void
append_display_name (GString *str, const pkgCache::PkgIterator &pkg)
{
  pkgCache::VerIterator ver = pkg.CurrentVer();
  if (!ver.end())
    {
      package_record rec;
      rec.lookup(ver);
      string pretty_name = get_pretty_name (rec);
      if (!pretty_name.empty())
	{
	  g_string_append (str, pretty_name.c_str());
	  return;
	}
    }
  
  g_string_append (str, pkg.Name());
}

void
encode_dependencies (pkgCache::VerIterator &ver)
{
  for (pkgCache::DepIterator dep = ver.DependsList(); !dep.end(); )
    {
      GString *str;
      apt_proto_deptype type;
      pkgCache::DepIterator start;
      pkgCache::DepIterator end;
      dep.GlobOr(start, end);

      if (start->Type == pkgCache::Dep::PreDepends ||
	  start->Type == pkgCache::Dep::Depends)
	type = deptype_depends;
      else if (start->Type == pkgCache::Dep::Conflicts)
	type = deptype_conflicts;
      else 
	continue;

      str = g_string_new ("");
      while (1)
	{
	  g_string_append_printf (str, " %s", start.TargetPkg().Name());
	  if (start.TargetVer() != 0)
	    g_string_append_printf (str, " (%s %s)",
				    start.CompType(), start.TargetVer());
	  
	  if (start == end)
	    break;
	  g_string_append_printf (str, " |");
	  start++;
	}

      response.encode_int (type);
      response.encode_string (str->str);
      g_string_free (str, 1);
    }

  response.encode_int (deptype_end);
}

void
encode_broken (pkgCache::PkgIterator &pkg,
	       const char *want)
{
  AptWorkerCache *awc = AptWorkerCache::GetCurrent ();
  pkgDepCache &Cache = *(awc->cache);
  pkgCache::VerIterator Ver = Cache[pkg].InstVerIter(Cache);
      
  if (Ver.end() == true)
    return;
      
  for (pkgCache::DepIterator D = Ver.DependsList(); D.end() == false;)
    {
      GString *str;
      apt_proto_sumtype type;

      // Compute a single dependency element (glob or)
      pkgCache::DepIterator Start;
      pkgCache::DepIterator End;
      D.GlobOr(Start,End); // advances D

      if ((Cache[End] & pkgDepCache::DepGInstall)
	  == pkgDepCache::DepGInstall)
	continue;

      if (Start->Type == pkgCache::Dep::PreDepends ||
	  Start->Type == pkgCache::Dep::Depends)
	type = sumtype_missing;
      else if (Start->Type == pkgCache::Dep::Conflicts)
	type = sumtype_conflicting;
      else
	continue;

      str = g_string_new ("");
      while (1)
	{
	  /* Show a summary of the target package if possible. In the case
	     of virtual packages we show nothing 
	  */
	  pkgCache::PkgIterator target = Start.TargetPkg ();

	  /* Never blame conflicts on the package that we want to
	     install.
	  */
	  if (strcmp (target.Name(), want) == 0
	      && Start->Type == pkgCache::Dep::Conflicts)
	    append_display_name (str, pkg);
	  else
	    {
	      append_display_name (str, target);
	      if (Start.TargetVer() != 0)
		g_string_append_printf (str, " (%s %s)",
					Start.CompType(), Start.TargetVer());
	    }

	  if (Start != End)
	    g_string_append_printf (str, " | ");
	  
	  if (Start == End)
	    break;
	  Start++;
	}

      response.encode_int (type);
      response.encode_string (str->str);
      g_string_free (str, 1);
    }
}

static const char *
catalogue_name (xexp *x)
{
  const char *name = NULL;
  xexp *n = xexp_aref (x, "name");
  if (n == NULL)
    ;
  else if (xexp_is_text (n))
    name = xexp_text (n);
  else
    {
      xexp *t = (lc_messages && *lc_messages
		 ? xexp_aref (n, lc_messages)
		 : NULL);
      if (t == NULL)
        t = xexp_aref (n, "default");
      if (t && xexp_is_text (t))
        name = xexp_text (t);
    }
  return name;
}

static gchar*
chop_uri (gchar *uri)
{
  while (uri[0] && uri[strlen (uri) - 1] == '/')
    uri[strlen (uri) - 1] = '\0';

  return uri;
}

static gchar*
find_catalogue_by_info (const char* p_uri,
                        const char* p_dist,
                        const char* p_comp)
{
  gchar *catname = NULL;
  xexp *catalogues = read_catalogues ();

  if (!catalogues)
    return NULL;

  for (xexp *cat = xexp_first (catalogues); cat; cat = xexp_rest (cat))
    {
      const gchar *uri = xexp_aref_text (cat, "uri");
      bool enabled = !xexp_aref_bool (cat, "disabled");
      if (enabled && uri && g_strstr_len (uri, -1, p_uri))
        {
          const gchar *dist = xexp_aref_text (cat, "dist");
          if (!dist)
            dist = default_distribution;

          if (!g_strcmp0 (dist, p_dist))
            {
              if (p_comp != NULL && p_comp[0] != '\0')
                {
                  const gchar *comp = xexp_aref_text (cat, "components");
                  gchar **comps = g_strsplit_set (comp, " \t\n", -1);

                  if (comps != NULL)
                    {
                      for (gint i = 0; comps[i]; i++)
                        {
                          if (!g_strcmp0 (comps[i], p_comp))
                            {
                              g_strfreev (comps);
                              catname = g_strdup (catalogue_name (cat));
                              goto done;
                            }
                        }
                      g_strfreev (comps);
                    }
                }
              else
                {
                  // it has no components defined
                  catname = g_strdup (catalogue_name (cat));
                  goto done;
                }
            }
        }
    }

 done:
  xexp_free (catalogues);
  return catname;
}

static void
encode_package_repository (pkgCache::VerIterator Version, int summary_kind)
{
  // XXX - I don't know how to assure that the "selected" VerFile in the
  //       VerFileItertor is the installable VerFile, but always seems
  //       to be the first one in the iterator.

  pkgSourceList List;
  pkgIndexFile *Index;

  if (summary_kind == 1 && List.ReadMainList ())
    {
      if (Version.end () == false)
        {
          pkgCache::VerFileIterator Vf = Version.FileList ();
          if (Vf.end () == false)
            {
              if (List.FindIndex (Vf.File (), Index))
                {
                  char *dist = NULL;

                  // extract the uri
                  char *archive_uri =
                    chop_uri (g_strdup (Index->ArchiveURI ("").c_str ()));

                  char *archive_info =
                    g_strdup (Index->ArchiveInfo (Version).c_str ());

                  // extract the dist
                  char **info = g_strsplit_set (archive_info, " ", -1);
                  if (info != NULL)
                    {
                      if (info[1] != NULL)
                        dist = g_strdup (info[1]);
                      g_strfreev (info);
                    }

                  // the component
                  char *comp = g_strdup (Vf.File ().Component ());

                  // nasty hack for some repositories which join the
                  // distribution into the component (dist/comp)
                  if (comp != NULL && comp[0] != '\0')
                    {
                      char *p = g_strstr_len (dist, -1, comp);
                      if (p != NULL && p[0] != '\0' && p - dist - 1 > 0)
                        {
                          char* tmp = g_strndup (dist, p - dist - 1);
                          g_free (dist);
                          dist = tmp;
                        }
                    }
                  else
                    {
                      // Another nasty hack for some packages which join the
                      // component into the distribution (dist/comp)
                      gchar **tmp = g_strsplit (dist, "/", -1);
                      if (tmp[0] != NULL && tmp[0][0] != '\0'
                          && tmp[1] != NULL && tmp[1][0] != '\0')
                        {
                          g_free (dist);
                          dist = g_strdup (tmp[0]);
                          comp = g_strdup (tmp[1]);
                        }
                      g_strfreev (tmp);
                    }

                  char* catalogue = find_catalogue_by_info (archive_uri,
                                                            dist, comp);

                  if (catalogue != NULL)
                    response.encode_string (catalogue);
                  else
                    {
                      catalogue = g_strdup_printf ("%s %s %s",
                                                   archive_uri ? archive_uri : "",
                                                   dist ? dist : "",
                                                   comp ? comp : "");
                      response.encode_string (catalogue);
                    }

                  g_free (archive_uri);
                  g_free (dist);
                  g_free (comp);
                  g_free (catalogue);

                  return;
                }
            }
        }
    }

    response.encode_string (NULL);
}

void
encode_package_and_version (const pkgCache::VerIterator ver,
                            package_record &rec)
{
  rec.lookup(ver);
  GString *str = g_string_new ("");
  string pretty = get_pretty_name (rec);
  if (!pretty.empty())
    g_string_append (str, pretty.c_str());
  else
    g_string_append (str, ver.ParentPkg().Name());
  g_string_append_printf (str, " (%s)", ver.VerStr());
  response.encode_string (str->str);
  g_string_free (str, 1);
}

void
encode_install_summary (const char *want)
{
  AptWorkerCache *awc = AptWorkerCache::GetCurrent ();
  pkgDepCache &cache = *(awc->cache);
  package_record rec;

  if (cache.BrokenCount() > 0)
    fprintf (stderr, "[ Some installed packages are broken! ]\n");

  mark_named_package_for_install (want);

  for (pkgCache::PkgIterator pkg = cache.PkgBegin();
       pkg.end() != true;
       pkg++)
    {
      pkgDepCache::StateCache& sc = cache[pkg];

      if (sc.NewInstall())
	{
	  response.encode_int (sumtype_installing);
	  encode_package_and_version (sc.CandidateVerIter(cache), rec);
	}
      else if (sc.Upgrade())
	{
	  response.encode_int (sumtype_upgrading);
	  encode_package_and_version (sc.CandidateVerIter(cache), rec);
	}
      else if (sc.Delete())
	{
	  response.encode_int (sumtype_removing);
	  encode_package_and_version (pkg.CurrentVer(), rec);
	}

      if (sc.InstBroken())
	{
	  if (is_related (pkg))
	    encode_broken (pkg, want);
	  else if (!sc.NowBroken())
	    {
	      response.encode_int (sumtype_conflicting);
	      encode_package_and_version (pkg.CurrentVer(), rec);
	    }
	}
    }

  response.encode_int (sumtype_end);
}

void
encode_remove_summary (pkgCache::PkgIterator &want)
{
  AptWorkerCache *awc = AptWorkerCache::GetCurrent ();
  pkgDepCache &cache = *(awc->cache);
  package_record rec;

  if (cache.BrokenCount() > 0)
    log_stderr ("[ Some installed packages are broken! ]\n");

  mark_for_remove (want);

  for (pkgCache::PkgIterator pkg = cache.PkgBegin();
       pkg.end() != true;
       pkg++)
    {
      pkgDepCache::StateCache& sc = cache[pkg];

      if (sc.Delete())
	{
	  response.encode_int (sumtype_removing);
	  encode_package_and_version (pkg.CurrentVer(), rec);
	}

      if (sc.InstBroken() && !sc.NowBroken())
	{
	  response.encode_int (sumtype_needed_by);
	  encode_package_and_version (pkg.CurrentVer(), rec);
	}
    }

  response.encode_int (sumtype_end);
}

bool
find_package_version (pkgCacheFile *cache_file,
		      pkgCache::PkgIterator &pkg,
		      pkgCache::VerIterator &ver,
		      const char *package, const char *version)
{
  if (cache_file == NULL || package == NULL || version == NULL)
    return false;

  pkgDepCache &cache = *cache_file;
  pkg = cache.FindPkg (package);
  if (!pkg.end ())
    {
      for (ver = pkg.VersionList(); ver.end() != true; ver++)
	if (!strcmp (ver.VerStr (), version))
	  return true;
    }
  return false;
}

void
cmd_get_package_details ()
{
  const char *package = request.decode_string_in_place ();
  const char *version = request.decode_string_in_place ();
  int summary_kind = request.decode_int ();

  if (!strcmp (package, "magic:sys"))
    {
      response.encode_string ("");      // maintainer
      response.encode_string 
        ("This is an artificial package that represents all\n"
         "system packages that are installed on your device.");
      response.encode_int (deptype_end);  // dependencies
      response.encode_string (NULL);      // repository
      if (summary_kind == 1)
        encode_install_summary (package);
      else
        response.encode_int (sumtype_end);
    }
  else
    {
      AptWorkerCache *awc = AptWorkerCache::GetCurrent ();
      pkgCache::PkgIterator pkg;
      pkgCache::VerIterator ver;

      if (find_package_version (awc->cache, pkg, ver, package, version))
        {
          package_record rec;
          rec.lookup(ver);

          response.encode_string (rec.P->Maintainer().c_str());
          response.encode_string 
            (get_long_description (summary_kind, pkg, rec).c_str());
          encode_dependencies (ver);
          encode_package_repository (ver, summary_kind);
          if (summary_kind == 1)
              encode_install_summary (package);
          else if (summary_kind == 2)
            encode_remove_summary (pkg);
          else
            response.encode_int (sumtype_end);
        }
      else
        {
          // not found
          response.encode_string (NULL);      // maintainer
          response.encode_string (NULL);      // description
          response.encode_int (deptype_end);  // dependencies
          response.encode_string (NULL);      // repository
          response.encode_int (sumtype_end);  // summary
        }
    }
}

/* APTCMD_THIRD_PARTY_POLICY_CHECK
*/

static bool
is_ssu_dependency (pkgCache::PkgIterator pkg)
{
  // Return false if we do not have an SSU package available
  if (ssu_packages == NULL)
    return false;

  // Find out whether it's a dependency for a SSU package
  for (pkgCache::DepIterator Dep = pkg.RevDependsList ();
       Dep.end () != true;
       Dep++)
    {
      const char *depname = Dep.ParentPkg().Name();

      /* Look through the list of packages with the 'system-update' flag */
      for (guint i = 0; i < ssu_packages->len; i++)
        {
          gchar *ssu_pkg = g_array_index (ssu_packages, gchar*, i);
          if (!strcmp (depname, ssu_pkg))
            return true;
        }
    }

  // not a ssu package if reached
  return false;
}

void
cmd_third_party_policy_check ()
{
  AptWorkerCache *awc = AptWorkerCache::GetCurrent ();
  pkgCache::PkgIterator pkg;
  pkgCache::VerIterator ver;
  const char *package = request.decode_string_in_place ();
  const char *version = request.decode_string_in_place ();
  third_party_policy_status policy_status = third_party_compatible;

  if (find_package_version (awc->cache, pkg, ver, package, version))
    {
      pkgDepCache &cache = *(awc->cache);
      pkgCache::VerIterator candidate = cache[pkg].CandidateVerIter (cache);
      package_record rec;
      rec.lookup(candidate);
      int flags = get_flags (rec);

      // skip non available packages and system update meta-packages
      if (!candidate.end () && !(flags & pkgflag_system_update))
        {
          for (pkgCache::DepIterator Dep = candidate.DependsList ();
               Dep.end () != true;
               Dep++)
            {
              pkgCache::PkgIterator dpkg = Dep.TargetPkg ();

              // Check whether SSU metapackage is dependant on this
              if (!is_ssu_dependency (dpkg))
                continue;

              int op = Dep->CompareOp & 0x0F;

              if (Dep->Type == pkgCache::Dep::Depends)
                {
                  if (op == pkgCache::Dep::NoOp
                      || op == pkgCache::Dep::GreaterEq
                      || op == pkgCache::Dep::Greater)
                    continue;

                  log_stderr ("%s breaks 3rd party dependencies policy:",
                              pkg.Name ());
                  policy_status = third_party_incompatible;
                  break;
                }
              else if (Dep->Type == pkgCache::Dep::Conflicts)
                {
                  if (op == pkgCache::Dep::Less
                      || op == pkgCache::Dep::LessEq
                      || op == pkgCache::Dep::Equals)
                    continue;

                  log_stderr ("%s breaks 3rd party conflicts policy",
                              pkg.Name ());
                  policy_status = third_party_incompatible;
                  break;
                }
            }
        }
    }

  // return result
  response.encode_int (policy_status);
}

/* APTCMD_CHECK_UPDATES
*/

static GList *
find_catalogues_for_item_desc (xexp *catalogues, string desc_uri)
{
  /* This is a hack to associate error messages produced during
     downloading with a specific catalogue so that a good error report
     can be shown to the user.

     DESC_URI is matched against all the catalogues and we return the
     first hit.

     DESC_URI matches a catalogue if it is of the form

        URI/dists/DIST/<no-more-slashes>

     or

        URI/dists/DIST/COMP/<rest-with-slashes>

     or

        URI/DIST<rest-with-slashes>

     or

        URIDIST (when dist is only a '/')

     where URI and DIST are the respective elements of the catalogue,
     and COMP is one of the components of the catalogue.

     XXX - This is not the right thing to do, of course.  Apt-pkg
           should offer a way to easily associate user level objects
           with acquire items.
  */

  if (catalogues == NULL)
    return NULL;

  GList *cat_glist = NULL;

  const char *match_uri = desc_uri.c_str ();

  for (xexp *cat = xexp_first (catalogues); cat; cat = xexp_rest (cat))
    {
      char *uri = g_strdup (xexp_aref_text (cat, "uri"));
      const char *dist = xexp_aref_text (cat, "dist");
      const char *comp_element = xexp_aref_text (cat, "components");
      gchar **comps = (comp_element
		       ? g_strsplit_set (comp_element, " \t\n", 0)
		       : NULL);
      char *pfx = NULL;

      if (dist == NULL)
	dist = default_distribution;

      while (uri[0] && uri[strlen(uri)-1] == '/')
	uri[strlen(uri)-1] = '\0';

      if (dist[0] && dist[strlen(dist)-1] == '/')
	{
	  /* A simple repository without components
	   */

	  if (dist[0] != '/')
	    pfx = g_strconcat (uri, "/", dist, NULL);
	  else /* dist can be only '/' */
	    pfx = g_strconcat (uri, dist, NULL);

	  if (g_str_has_prefix (match_uri, pfx))
	    goto found_it;
	}
      else
	{
	  /* A repository with components
	   */

	  pfx = g_strconcat (uri, "/dists/", dist, "/", NULL);
	  if (!g_str_has_prefix (match_uri, pfx))
	    goto try_next;

	  const char *rest = match_uri + strlen (pfx);

	  if (!strchr (rest, '/'))
	    goto found_it;

	  if (comps)
	    {
	      for (int i = 0; comps[i]; i++)
		{
		  gchar *comp = comps[i];

		  if (comp[0] == '\0')
		    continue;

		  if (g_str_has_prefix (rest, comp)
		      && rest[strlen(comp)] == '/')
		    goto found_it;
		}
	    }
	}

    try_next:
      g_strfreev (comps);
      g_free (pfx);
      g_free (uri);
      continue;

    found_it:
      g_strfreev (comps);
      g_free (pfx);
      g_free (uri);

      /* Append found item to the list */
      cat_glist = g_list_append (cat_glist, cat);
    }

  return cat_glist;
}

static bool
download_lists (xexp *catalogues_for_report,
		bool with_status, int *result)
{
  *result = rescode_failure;

  // Get the source list
  pkgSourceList List;
  if (List.ReadMainList () == false)
    return false;

  // Lock the list directory
  FileFd Lock;
  if (_config->FindB("Debug::NoLocking",false) == false)
    {
      Lock.Fd (ForceLock (_config->FindDir("Dir::State::Lists") + "lock"));
      if (_error->PendingError () == true)
	{
	  _error->Error ("Unable to lock the list directory");
	  return false;
	}
    }
   
  // Create the download object
  DownloadStatus Stat;
  pkgAcquire Fetcher (with_status ? &Stat : NULL);

  // Populate it with the source selection
  if (List.GetIndexes(&Fetcher) == false)
    return false;
   
  // Run it
  if (Fetcher.Run() != pkgAcquire::Continue)
    return false;

  bool some_failed = false;
  for (pkgAcquire::ItemIterator I = Fetcher.ItemsBegin();
       I != Fetcher.ItemsEnd(); I++)
    {
      if ((*I)->Status == pkgAcquire::Item::StatDone)
        continue;

      (*I)->Finished();

      GList *cat_glist = find_catalogues_for_item_desc (catalogues_for_report,
                                                        (*I)->DescURI());

      for (GList *iter = cat_glist; iter; iter = g_list_next (iter))
	{
	  if (iter->data != NULL)
	    {
	      xexp *cat = (xexp *) (iter->data);

	      xexp *errors = xexp_aref (cat, "errors");
	      if (errors == NULL)
		{
		  errors = xexp_list_new ("errors");
		  xexp_append_1 (cat, errors);
		}

	      xexp *error = xexp_list_new ("error");
	      xexp_aset_text (error, "uri", (*I)->DescURI().c_str());
	      xexp_aset_text (error, "msg", (*I)->ErrorText.c_str());

	      xexp_append_1 (errors, error);
	    }
	}

      if (cat_glist != NULL)
	g_list_free (cat_glist);

      _error->Error ("Failed to fetch %s  %s", (*I)->DescURI().c_str(),
                     (*I)->ErrorText.c_str());
      some_failed = true;
    }

  // Clean out any old list files
  if (_config->FindB("APT::Get::List-Cleanup",true) == true)
    {
      Fetcher.Clean (_config->FindDir("Dir::State::lists"));
      Fetcher.Clean (_config->FindDir("Dir::State::lists") + "partial/");
    }

  if (some_failed)
    *result = rescode_partial_success;
  else
    *result = rescode_success;

  return true;
}

/* Duplicate the directory hierarchy at OLD to NEW by creating hard
   links for all the regular files.
*/

static int duplink_base;
static const char *duplink_new;

int
duplink_callback (const char *name, const struct stat *, int m, struct FTW *f)
{
  char *new_name = g_strdup_printf ("%s/%s",
				    duplink_new, name + duplink_base);

  // fprintf (stderr, "%s -> %s\n", new_name, name);

  if (m == FTW_D)
    {
      if (mkdir (new_name, 0777))
	{
	  perror (new_name);
	  g_free (new_name);
	  return -1;
	}
    }
  else if (m == FTW_F)
    {
      if (link (name, new_name))
	{
	  perror (new_name);
	  g_free (new_name);
	  return -1;
	}
    }

  g_free (new_name);
  return 0;
}

int
duplink_file_tree (const char *old_tree, const char *new_tree)
{
  duplink_base = strlen (old_tree);
  duplink_new = new_tree;
  return nftw (old_tree, duplink_callback, 10, 0);
}

/* Unlink a directory hirarchy.
 */

int
unlink_callback (const char *name, const struct stat *, int m, struct FTW *f)
{
  // fprintf (stderr, "- %s\n", name);

  if (m == FTW_DP)
    {
      if (rmdir (name))
	perror (name);
    }
  else if (m == FTW_F)
    {
      if (unlink (name))
	perror (name);
    }

  return 0;
}

int
unlink_file_tree (const char *tree)
{
  return nftw (tree, unlink_callback, 10, FTW_DEPTH);
}

int
update_package_cache (xexp *catalogues_for_report,
		      bool with_status)
{
  /* XXX - We do the downloading in a 'transaction'.  If we get
           interrupted half-way through, all the old files are kept in
           place.  Libapt-pkg is careful not to leave partial files on
           disk, but when more than one file needs to be downloaded
           for a repository, we can still end up with an inconsistent
           state.  Worse, the cleanup will remove the "Packages" files
           when the corresponding "Release" has not been downloaded
           yet.

     Libapt-pkg should take care of this itself, but I am too much of
     a chicken to make that change now.
  */

  int result = rescode_failure;

  string lists_val = _config->Find("Dir::State::Lists");
  string lists_dir = _config->FindDir("Dir::State::Lists");
  if (lists_dir.length() > 0 && lists_dir[lists_dir.length()-1] == '/')
    lists_dir.erase(lists_dir.length()-1, 1);

  string lists_dir_new = lists_dir + ".new";
  string lists_dir_old = lists_dir + ".old";

  unlink_file_tree (lists_dir_new.c_str());
  duplink_file_tree (lists_dir.c_str(), lists_dir_new.c_str());
  _config->Set ("Dir::State::Lists", lists_dir_new);

  if (download_lists (catalogues_for_report, 
		      with_status, &result))
    {
      /* complete transaction */
      unlink_file_tree (lists_dir_old.c_str());
      rename (lists_dir.c_str(), lists_dir_old.c_str());
      rename (lists_dir_new.c_str(), lists_dir.c_str());
      unlink_file_tree (lists_dir_old.c_str());
      _config->Set ("Dir::State::Lists", lists_val);

      cache_init (with_status);
    }
  else
    {
      /* cleanup */
      _config->Set ("Dir::State::Lists", lists_val);
      unlink_file_tree (lists_dir_new.c_str());
    }

  return result;
}

static void
reset_catalogue_errors (xexp *catalogues)
{
  /* Clean the catalogues errors */
  clean_failed_catalogues ();

  if (catalogues == NULL)
    return;

  for (xexp *c = xexp_first (catalogues); c; c = xexp_rest (c))
    xexp_adel (c, "errors");
}

static gboolean
update_sources_list (xexp *catalogues)
{
  gboolean success = FALSE;

  /* Write the new sources list to disk */
  success = (write_user_catalogues (catalogues)
	     && write_sources_list (CATALOGUE_APT_SOURCE, catalogues));

  return success;
}

static gboolean
add_temp_sources_list (xexp *tempcat)
{
  gboolean success;

  success = (xexp_write_file (TEMP_CATALOGUE_CONF, tempcat) &&
             write_sources_list (TEMP_APT_SOURCE_LIST, tempcat));

  return success;
}

int
cmd_check_updates (bool with_status)
{
  xexp *catalogues = read_catalogues ();

  reset_catalogue_errors (catalogues);

  /* Update sources.list file before refreshing */
  update_sources_list (catalogues);

  int result_code = update_package_cache (catalogues, with_status);

  if ((result_code == rescode_success)
      || (result_code == rescode_partial_success))
    {
      /* Some packages with the 'system-update' could
         appear / disappear under this conditions */
      ssu_packages_needs_refresh = true;
    }

  /* Save potential errors to disk */
  save_failed_catalogues (catalogues);

  response.encode_xexp (catalogues);
  response.encode_int (result_code);

  if (catalogues)
    xexp_free (catalogues);

  return result_code;
}

int
cmdline_check_updates (char **argv)
{
  AptWorkerCache * awc = 0;
  int result_code = -1;

  awc = AptWorkerCache::GetCurrent ();
  awc->init_cache_after_request = false;

  if (awc->cache == NULL)
    return 2;

  if (argv[1])
    {
      DBG ("http_proxy: %s", argv[1]);
      setenv ("http_proxy", argv[1], 1);
    }

  response.reset ();
  request.reset (NULL, 0);
  result_code = cmd_check_updates (false);

  _error->DumpErrors ();

  if (result_code == rescode_success
      || result_code == rescode_partial_success)
    return 0;
  else
    return 1;
}

/* APTCMD_GET_CATALOGUES
 *
 * We also return the non-comment lines from all sources.list files in
 * order to let the user at least know that there are sources in use
 * that are not controlled by us.  The code for this is copied from
 * apt-pkg.
 */

void
append_system_sources (xexp *catalogues, string File)
{
   // Open the stream for reading
   ifstream F(File.c_str(),ios::in /*| ios::nocreate*/);
   if (!F != 0)
     return;
   
   // CNC:2003-12-10 - 300 is too short.
   char Buffer[1024];

   int CurLine = 0;
   while (F.eof() == false)
     {
       F.getline(Buffer,sizeof(Buffer));
       CurLine++;
       _strtabexpand(Buffer,sizeof(Buffer));
       if (F.fail() && !F.eof())
	 return;

       char *I;
       // CNC:2003-02-20 - Do not break if '#' is inside [].
       for (I = Buffer; *I != 0 && *I != '#'; I++)
         if (*I == '[')
	   for (I++; *I != 0 && *I != ']'; I++);
       *I = 0;
       
       const char *C = _strstrip(Buffer);
      
       // Comment or blank
       if (C[0] == '#' || C[0] == 0)
	 continue;
      	    
       xexp_append_1 (catalogues, xexp_text_new ("source", C));
     }
}

void
append_system_source_dir (xexp *catalogues, string Dir)
{
   DIR *D = opendir(Dir.c_str());
   if (D == 0)
     return;

   vector<string> List;
   
   for (struct dirent *Ent = readdir(D); Ent != 0; Ent = readdir(D))
     {
       if (Ent->d_name[0] == '.')
	 continue;

       // CNC:2003-12-02 Only accept .list files as valid sourceparts
       if (flExtension(Ent->d_name) != "list")
	 continue;
      
       // Skip bad file names ala run-parts
       const char *C = Ent->d_name;
       for (; *C != 0; C++)
	 if (isalpha(*C) == 0 && isdigit(*C) == 0
             && *C != '_' && *C != '-' && *C != '.')
	   break;
       if (*C != 0)
	 continue;
      
       // Make sure it is a file and not something else
       string File = flCombine(Dir,Ent->d_name);
       struct stat St;
       if (stat(File.c_str(),&St) != 0 || S_ISREG(St.st_mode) == 0)
	 continue;

       // skip our own file
       if (File == CATALOGUE_APT_SOURCE)
	 continue;

       List.push_back(File);      
     }
   closedir(D);
   
   sort(List.begin(),List.end());

   // Read the files
   for (vector<string>::const_iterator I = List.begin(); I != List.end(); I++)
     append_system_sources (catalogues, *I);
}

void
cmd_get_catalogues ()
{
  struct stat buf;
  int stat_result;
  xexp *catalogues = NULL;

  /* Check if there are problems reading the file */
  stat_result = stat (CATALOGUE_CONF, &buf);
  if (!stat_result)
    {
      /* Map the catalogue report to (maybe) delete error reports from it */
      xexp *tmp_catalogues = read_catalogues ();
      catalogues = xexp_list_map (tmp_catalogues, map_catalogue_error_details);
      xexp_free (tmp_catalogues);

      /* Add errors to those catalogues inside the failed-catalogues file */
      merge_catalogues_with_errors (catalogues);
    }
  else
    {
      /* If there's not a conf file on disk, write a empty one */
      catalogues = read_catalogues (); /* @check test this scenario */
      write_user_catalogues (catalogues);
    }

  string Main = _config->FindFile("Dir::Etc::sourcelist");
  if (FileExists(Main) == true)
    append_system_sources (catalogues, Main);

  string Parts = _config->FindDir("Dir::Etc::sourceparts");
  if (FileExists(Parts) == true)
    append_system_source_dir (catalogues, Parts);

  response.encode_xexp (catalogues);
  xexp_free (catalogues);
}

/* APTCMD_SET_CATALOGUES
 */

void
cmd_set_catalogues ()
{
  int success = true;

  xexp *catalogues = request.decode_xexp ();
  xexp_adel (catalogues, "source");

  /* Map the catalogue report to delete error reports from it */
  xexp *mapped_catalogues = xexp_list_map (catalogues,
					   map_catalogue_error_details);

  /* Update failed catalogues file */
  save_failed_catalogues (catalogues);

  /* Prepare mapped_catalogues to be used for updating the
     sources.list file later, freeing catalogues first */
  xexp_free (catalogues);
  catalogues = mapped_catalogues;

  /* Update sources.list file */
  update_sources_list (catalogues);

  /* We should update catalogue cache after modify catalogues */
  need_cache_init ();

  xexp_free (catalogues);
  response.encode_int (success);
}

/* APTCMD_ADD_TEMP_CATALOGUES
 *
 * Stores a new temporal source list in /etc/apt directory
 */
void
cmd_add_temp_catalogues ()
{
  int success;
  xexp *tempcat;

  tempcat = request.decode_xexp ();
  xexp_adel (tempcat, "source");

  /* add a temporal sources.list file */
  success = add_temp_sources_list (tempcat);  

  xexp_free (tempcat);
  response.encode_int (success);
}

/* APTCMD_RM_TEMP_CATALOGUES
 *
 * Remove the temporal source list stored in /etc/apt directory
 */

void
cmd_rm_temp_catalogues ()
{
  int success = true;

  clean_temp_catalogues ();
  
  response.encode_int (success);
}

static int64_t
get_free_space (const char *path)
{
  struct statvfs buf;

  // Sync before we measure the free space for download
  sync ();

  if (statvfs (path, &buf) != 0)
    return -1;

  int64_t res = (int64_t)buf.f_bavail * (int64_t)buf.f_bsize;
  log_stderr ("free space (%s) = %Ld", path, res);
  return res;
}

/* APTCMD_GET_FREE_SPACE
 *
 * Returns the actual amount of free space for installation
 */

void
cmd_get_free_space ()
{
  int64_t free_space = get_free_space ("/");
  response.encode_int64 (free_space);
}

static bool set_dir_cache_archives (const char *alt_download_root);
static int operation (bool check_only,
		      const char *alt_download_root,
		      bool download_only,
		      bool allow_download = true,
		      bool with_status = true);

/* APTCMD_INSTALL_CHECK
 *
 * Check if a package can be installed. It uses the
 * common "operation ()" code, that runs or checks the
 * current operation queue (packages marked for install
 * or uninstall).
 */

void
cmd_install_check ()
{
  const char *package = request.decode_string_in_place ();
  bool found = false;
  int result_code = rescode_failure;
  
  if (ensure_cache (true))
    {
      found = mark_named_package_for_install (package);
      result_code = operation (true, NULL, false);
    }

  response.encode_int (found && result_code == rescode_success);
}

/* APTCMD_DOWNLOAD_PACKAGE
 *
 * Download a package, using the common "operation ()" code, that
 * installs packages marked for install.
 */

#include <mntent.h>

/* global variable to report the download size to the frontend */
static int64_t download_size = 0;

static bool
volume_is_readwrite (char* option)
{
  g_return_val_if_fail (option, FALSE);

  enum
  {
    RW = 0,
    END
  };

  const char * const suboptions[] =
    {
      MNTOPT_RW,
      NULL
    };

  int so;
  char* argument = NULL;

  while (*option != 0)
    {
      so = getsubopt (&option, (char* const*) suboptions, &argument);
      if (so == RW)
        return true;
    }

  return false;
}

static bool
volume_path_is_mounted_writable (const gchar *path)
{
  bool result = false;

  g_return_val_if_fail (path, false);

  FILE *fp = setmntent (MOUNTED, "r");
  g_return_val_if_fail (fp != NULL, false);

  struct mntent *mnt;
  while ((mnt = getmntent (fp)) != NULL)
    {
      gboolean usable = (g_strcmp0 (mnt->mnt_dir, path) == 0 &&
                         volume_is_readwrite (mnt->mnt_opts));

      if (usable)
        {
          /* Try to write a dummy file to be completely sure,
             since getmntent is not always up-to-date about the
             read-only status of a partition, which would in fact
             become read-only only when actually writing to it */
          gchar *dummyfile = g_strdup_printf ("%s/.ham-dummy-file-XXXXXX",
                                              path);
          int fd;

          /* Try to open a temporary file under the selected path */
          fd = mkstemp (dummyfile);
          if (fd != -1)
            {
              /* Try to write some data in the file */
              const char *dummytext = "Dummy text";
              int n = strlen (dummytext);
              bool data_written = (write (fd, dummytext, n) == n
                                   && fsync (fd) != -1);

              /* Close the descriptor and decide the final result */
              result = (close (fd) != -1) && data_written;

              /* Delete the file on success */
              if (result)
                unlink (dummyfile);
            }
          g_free (dummyfile);

          /* Don't keep on iterating if an usable path was found */
          if (result)
            break;
        }
    }

  endmntent (fp);

  return result;
}

void
cmd_download_package ()
{
  const char *package = request.decode_string_in_place ();

  const char *alt_download_root = NULL;
  int result_code = rescode_out_of_space;

  const char *internal_mmc_mountpoint = getenv ("INTERNAL_MMC_MOUNTPOINT");
  if (!internal_mmc_mountpoint)
    internal_mmc_mountpoint = INTERNAL_MMC_MOUNTPOINT;

  const char *removable_mmc_mountpoint = getenv ("REMOVABLE_MMC_MOUNTPOINT");
  if (!removable_mmc_mountpoint)
    removable_mmc_mountpoint = REMOVABLE_MMC_MOUNTPOINT;

  if (ensure_cache (true))
    {
      if (mark_named_package_for_install (package))
        {
          if (flag_download_packages_to_mmc
	      && internal_mmc_mountpoint
              && volume_path_is_mounted_writable (internal_mmc_mountpoint))
            {
              alt_download_root = internal_mmc_mountpoint;
              result_code = operation (false, alt_download_root, true);
            }

          if (flag_download_packages_to_mmc
	      && result_code == rescode_out_of_space
	      && removable_mmc_mountpoint
              && volume_path_is_mounted_writable (removable_mmc_mountpoint))
            {
              alt_download_root = removable_mmc_mountpoint;
              result_code = operation (false, alt_download_root, true);
            }

          if (result_code == rescode_out_of_space
              && volume_path_is_mounted_writable (HOME_MOUNTPOINT))
            {
              alt_download_root = HOME_MOUNTPOINT;
              result_code = operation (false, alt_download_root, true);
            }

          /* default or bailout option */
          if (!flag_download_packages_to_mmc ||
              result_code == rescode_out_of_space)
            {
              alt_download_root = NULL;
              result_code = operation (false, alt_download_root, true);
            }
        }
      else
        result_code = rescode_packages_not_found;
    }

  response.encode_int (result_code);
  response.encode_int64 (download_size);
  response.encode_string (alt_download_root);

  download_size = 0;
}

/* APTCMD_INSTALL_PACKAGE
 *
 * Install a package, using the common "operation ()" code, that
 * installs packages marked for install.
 */

#define PKGNAME_ENVVAR "HAM_PKG_NAME"

/* these functions (un)export the package name to an environment variable
 * in order to be used by the package mantainer scripts.
 * The maemo-confirm-text util uses it.
 */
static void
set_pkgname_envvar (const char *package)
{
  setenv (PKGNAME_ENVVAR, package, 1);
}

static void
unset_pkgname_envvar ()
{
  unsetenv (PKGNAME_ENVVAR);
}

/* Check whether is an SSU package or not */
static bool
is_ssu (const char *package)
{
  if (package && *package && ssu_packages)
    {
      for (guint i = 0; i < ssu_packages->len; i++)
        {
          gchar *ssu_pkg = g_array_index (ssu_packages, gchar*, i);
          if (!g_strcmp0 (package, ssu_pkg))
            return true;
        }
    }

  return false;
}

/* This is another nasty ad-hoc hack for avoiding rootfs
 * space exhaustion when doing an SSU.
 * This function will bind mount the documentation directories
 * to a temporal and non rootfs space
 * Please, take out this function from here and put all
 * those SSU hacks into a special pre-SSU process
 */
static int run_system (bool verbose, const char *fmt, ...);

static void
maybe_bindmount_docsfs (const char *tmpfs)
{
  int i;
  const char* docsfs[] = { "doc", "info", "man", "doc-base", NULL };

  if (tmpfs == NULL || tmpfs[0] == '\0')
    return;

  char* rootdir = g_strdup_printf ("%s/%s", tmpfs, ".doc");
  if (mkdir (rootdir, 0777) < 0
      && errno != EEXIST)
    log_stderr ("%s: %m", rootdir);
  else
    {
      for (i = 0; docsfs[i] != NULL; i++)
        {
          char* dir = g_strdup_printf ("%s/%s", rootdir, docsfs[i]);
          if (mkdir (dir, 0777) == 0
              || errno == EEXIST)
            run_system (true, "/bin/mount -o bind %s /usr/share/%s", dir, docsfs[i]);
          g_free (dir);
        }
    }

  g_free (rootdir);
}

static int
maybe_bindumount_docsfs (const char *tmpfs)
{
  int i;
  const char* docsfs[] = { "doc", "info", "man", "doc-base", NULL };

  if (tmpfs == NULL || tmpfs[0] == '\0')
    return -1;

  char* rootdir = g_strdup_printf ("%s/%s", tmpfs, ".doc");

  for (i = 0; docsfs[i] != NULL; i++)
    run_system (true, "/bin/umount %s/%s", rootdir, docsfs[i]);

  int ret = unlink_file_tree (rootdir);
  g_free (rootdir);

  return ret;
}

/* And an another hack to save space while SSU is ongoing... */
static void
rootfs_set_compression_level (bool high)
{
  gchar *level = NULL;
  gchar *cmd;

  if (high)
    level = g_strdup ("lzo999");
  else
    level = g_strdup ("lzo"); /* Normal compression level */

  cmd = g_strdup_printf ("/bin/mount -o remount,compr=%s /", level);
  g_free (level);

  run_system (true, cmd);

  g_free (cmd);
}

/* This function will check for the /dev/shm fs
 * If it's available and sane return it.
 * Otherwise if /home is available and sane return it.
 * Fallback is NULL and no mount is done
 */
static const char* docfs[] = { "/dev/shm",   // 1st option
                               "/home",      // 2nd option
                               NULL };

static const char*
choose_tmpfs_for_docs ()
{
  // is 30M enough threshold?
  const int64_t threshold = 30 * 1024 * 1024;

  for (int i = 0; docfs[i] != NULL; i++)
    {
      if (volume_path_is_mounted_writable (docfs[i])
          && get_free_space (docfs[i]) >= threshold)
        return docfs[i];
    }

  return NULL;
}

void
cmd_install_package ()
{
  const char *package = request.decode_string_in_place ();
  const char *alt_download_root = request.decode_string_in_place ();

  int result_code = rescode_failure;

  if (ensure_cache (true))
    {
      if (mark_named_package_for_install (package))
	{
          const char* tmpfs = NULL;
          bool pkg_is_ssu = is_ssu (package);

          /* if package is SSU, then mount the
             temporal docsfs */
          if (pkg_is_ssu)
            {
              tmpfs = choose_tmpfs_for_docs ();
              maybe_bindmount_docsfs (tmpfs);
              rootfs_set_compression_level (true);
            }

          set_pkgname_envvar (package);
	  save_operation_record (package, alt_download_root);
 	  result_code = operation (false, alt_download_root, false);

          /* Delete journal on succesful operations only */
          if ((result_code == rescode_success) || !pkg_is_ssu)
            erase_operation_record ();

          unset_pkgname_envvar ();

          if (pkg_is_ssu)
          {
            maybe_bindumount_docsfs (tmpfs);
            rootfs_set_compression_level (false);
          }

	}
      else
	result_code = rescode_packages_not_found;
    }

  need_cache_init ();
  response.encode_int (result_code);
}

void
cmd_remove_check ()
{
  const char *package = request.decode_string_in_place ();

  if (ensure_cache (true))
    {
      AptWorkerCache *awc = AptWorkerCache::GetCurrent ();
      pkgDepCache &cache = *(awc->cache);
      pkgCache::PkgIterator pkg = cache.FindPkg (package);

      if (!pkg.end ())
	{
	  mark_for_remove (pkg);
	  
	  for (pkgCache::PkgIterator pkg = cache.PkgBegin();
	       pkg.end() != true;
	       pkg++)
	    {
	      if (cache[pkg].Delete())
		response.encode_string (pkg.Name());
	    }
	}
    }

  response.encode_string (NULL);
}

void
cmd_remove_package ()
{
  const char *package = request.decode_string_in_place ();
  int result_code = rescode_failure;

  if (ensure_cache (true))
    {
      AptWorkerCache *awc = AptWorkerCache::GetCurrent ();
      pkgDepCache &cache = *(awc->cache);
      pkgCache::PkgIterator pkg = cache.FindPkg (package);

      if (!pkg.end ())
	{
	  mark_for_remove (pkg);
	  result_code = operation (false, NULL, false);
	}
    }

  need_cache_init ();
  response.encode_int (result_code == rescode_success);
}

/* APTCMD_AUTOREMOVE
*/
void
cmd_autoremove ()
{
  AptWorkerCache *awc = AptWorkerCache::GetCurrent ();
  pkgDepCache &cache = *(awc->cache);

  int result_code = rescode_failure;

  // look over the cache to see what can be removed
  for (pkgCache::PkgIterator Pkg = cache.PkgBegin (); ! Pkg.end (); ++Pkg)
    {
      if (cache[Pkg].Garbage)
        {
          if (Pkg.CurrentVer () != 0 || cache[Pkg].Install ())
            log_stderr ("We could delete %s",  string (Pkg.Name ()).c_str ());

          if (Pkg.CurrentVer () != 0 &&
              Pkg->CurrentState != pkgCache::State::ConfigFiles)
            cache.MarkDelete (Pkg, false);
          else
            cache.MarkKeep (Pkg, false, false);
        }
    }

  // Now see if we destroyed anything
   if (cache.BrokenCount () != 0)
     {
       log_stderr ("Seems like the AutoRemover destroyed something which really "
                   "shouldn't happen.\n");
       _error->Error("Internal Error, AutoRemover broke stuff");

       response.encode_int (false);
       return;
     }

  result_code = operation (false, NULL, false);
  need_cache_init ();
  response.encode_int (result_code == rescode_success);
}

/* Package source domains.

   Package sources are classified into domains, depending on which key
   was used to sign their Release file.  When upgrading a package, the
   update must come from the same domain as the installed package, or
   from one that 'dominates' it.

   Domains are identified by symbolic names.  The defining property of
   a domain is the list of keys.  Any source that is signed by one of
   these keys is associated with that domain.

   In addition to the explicitly defined domains (see below), there
   are two implicitly defined domains, "signed" and "unsigned".  The
   "signed" domain includes all sources that are signed but are not
   associated with any of the explicit domains.  The "unsigned" domain
   includes all the unsigned sources.

   The explicitly defined domains can be declared to be "certified".
   When a package is installed or upgraded from such a domain, the
   does not need to agree to a legal disclaimer.

   No explicit domain dominates any other explicit domain.  All trust
   domains dominate the "signed" and "unsigned" domains, and "signed"
   dominates "unsigned".  Thus, the dominance rules are hard coded.

   Domains are also used when installing a package for the first time:
   when a given package version is available from more than one
   source, sources belonging to any explicit domain or the "signed"
   domain are preferred over the "unsigned" domain.  You can influence
   which source is used via the normal apt source priorities.

   One domain is designated as the default domain (usually the Nokia
   certified domain).  When storing and reporting information about
   domains, only the non-default domains are explicitly mentioned.
   This saves some space and time when most of the packages are in the
   default domain, which they usually are.
*/

static const pkgSourceList *cur_sources;

static void
set_sources_for_get_domain (const pkgSourceList *sources)
{
  cur_sources = sources;
}

static debReleaseIndex *
find_deb_meta_index (pkgIndexFile *index)
{
  if (cur_sources == NULL)
    return NULL;

  for (pkgSourceList::const_iterator I = cur_sources->begin();
       I != cur_sources->end(); I++)
    {
      vector<pkgIndexFile *> *Indexes = (*I)->GetIndexFiles();
      for (vector<pkgIndexFile *>::const_iterator J = Indexes->begin();
	   J != Indexes->end(); J++)
	{
	  if ((*J) == index)
	    {
	      if (strcmp ((*I)->GetType(), "deb") == 0)
		return (debReleaseIndex *)(*I);
	      else
		return NULL;
	    }
	}
    }
  
  return NULL;
}

#define VALIDSIG "VALIDSIG"
#define GOODSIG  "GOODSIG"

static std::string URIEncode(std::string const &part)
{
   // The "+" is encoded as a workaround for an S3 bug (LP#1003633 and LP#1086997)
   return QuoteString(part, _config->Find("Acquire::URIEncode", "+~ ").c_str());
}

static std::string MetaIndexURI(std::string URI, std::string const &Dist, char const * const Type)
{
   if (Dist == "/")
      ;
   else if (Dist[Dist.size()-1] == '/')
      URI += URIEncode(Dist);
   else
      URI += "dists/" + URIEncode(Dist) + "/";
   return URI + URIEncode(Type);
}

static std::string MetaIndexFile(debReleaseIndex *meta, const char *Type)
{
   return _config->FindDir("Dir::State::lists") +
      URItoFileName(MetaIndexURI(meta->GetURI(), meta->GetDist(), Type));
}

static char *
get_meta_info_key (debReleaseIndex *meta)
{
  char *key = NULL;
  string file = MetaIndexFile(meta, "InRelease");

  FILE *f = fopen (file.c_str(), "r");
  if (f)
    {
      char *line = NULL;
      size_t len = 0;
      ssize_t n;
      bool open_signature = false;
      string sig;

      while ((n = getline (&line, &len, f)) != -1)
        {
          if (n > 0 && line[n - 1] == '\n')
            line[n - 1] = '\0';

          if (open_signature)
            {
              sig += line;
              if (sig.length() >= 44)
                {
                  gsize l;
                  guchar *k = g_base64_decode(sig.substr(16, 28).c_str(), &l);

                  if (l >= 20)
                    {
                      char *p = g_new(char, 41);

                      key = p;

                      for (int i = 0; i < 20; i++)
                        {
                          sprintf(p, "%02X", k[i]);
                          p += 2;
                        }
                    }

                  g_free(k);
                  break;
                }
            }
          else if (!strcmp(line, "-----BEGIN PGP SIGNATURE-----"))
            open_signature  = true;
        }

      free (line);
      fclose (f);
    }

  return key;
}

static int
get_domain (pkgIndexFile *index)
{
  if (index->IsTrusted ())
    {
      debReleaseIndex *meta = find_deb_meta_index (index);
      if (meta)
	{
	  domain_t d = find_domain_by_uri (meta->GetURI().c_str());
	  if (d != DOMAIN_SIGNED)
	    return d;

	  char *key = get_meta_info_key (meta);
	  if (key)
	    {
	      domain_t d = find_domain_by_key (key);
	      g_free (key);
	      return d;
	    }
	}

      return DOMAIN_SIGNED;
    }
  else
    return DOMAIN_UNSIGNED;
}

static bool
domain_dominates_or_is_equal (int a, int b)
{
  return domains[a].trust_level >= domains[b].trust_level;
}

static void
reset_new_domains ()
{
  AptWorkerCache *awc = AptWorkerCache::GetCurrent ();

  pkgDepCache &cache = *(awc->cache);
  int package_count = cache.Head().PackageCount;

  for (int i = 0; i < package_count; i++)
    awc->cache->extra_info[i].new_domain = DOMAIN_UNSIGNED;
}

static void
collect_new_domains ()
{
  AptWorkerCache *awc = AptWorkerCache::GetCurrent ();

  pkgDepCache &cache = *(awc->cache);
  int package_count = cache.Head().PackageCount;

  for (int i = 0; i < package_count; i++)
    if (awc->cache->extra_info[i].related)
      awc->cache->extra_info[i].cur_domain
        = awc->cache->extra_info[i].new_domain;
}

static int
index_trust_level_for_package (pkgIndexFile *index,
			       const pkgCache::VerIterator &ver)
{
  AptWorkerCache *awc = AptWorkerCache::GetCurrent ();
  pkgDepCache &cache = *(awc->cache);
  pkgCache::PkgIterator pkg = ver.ParentPkg();

  int cur_level =
    domains[awc->cache->extra_info[pkg->ID].cur_domain].trust_level;

  int index_domain = get_domain (index);
  int index_level = domains[index_domain].trust_level;

  DBG ("trust_level: cur %d index %d (%s)",
       cur_level, index_level, domains[index_domain].name);

  /* If we have already found a good domain, accept this one only if
     it is the same domain, or strictly better.
   */
  if (awc->cache->extra_info[pkg->ID].new_domain != DOMAIN_UNSIGNED)
    {
      int new_level =
	domains[awc->cache->extra_info[pkg->ID].new_domain].trust_level;

      if (index_domain == awc->cache->extra_info[pkg->ID].new_domain
	  || index_level > new_level)
	{
	  awc->cache->extra_info[pkg->ID].new_domain = index_domain;
	  return index_level;
	}
      else
	return -1;
    }

  /* If this is a new install, accept the first domain that comes
     along.
   */
  if (cache[pkg].NewInstall())
    {
      DBG ("new: accept index");
      awc->cache->extra_info[pkg->ID].new_domain = index_domain;
      return index_level;
    }

  /* This is an upgrade, only accept the current domain, or ones that
     dominate it.
  */
  if (index_level >= cur_level)
    {
      DBG ("upgrade: accept better");
      awc->cache->extra_info[pkg->ID].new_domain = index_domain;
      return index_level;
    }

  /* This index is as good as any other.
   */
  DBG ("Hmm");
  return cur_level;
}

static void
encode_trust_summary ()
{
  AptWorkerCache *awc = AptWorkerCache::GetCurrent ();
  pkgDepCache &cache = *(awc->cache);

  for (pkgCache::PkgIterator pkg = cache.PkgBegin();
       pkg.end() != true;
       pkg++)
    {
      domain_t cur_domain = awc->cache->extra_info[pkg->ID].cur_domain;
      domain_t new_domain = awc->cache->extra_info[pkg->ID].new_domain;

      if (cache[pkg].Upgrade() && !domains[new_domain].is_certified)
	{
	  DBG ("not certified: %s", pkg.Name());
	  response.encode_int (pkgtrust_not_certified);
	  response.encode_string (pkg.Name());
	}

      if (cache[pkg].Upgrade() && !cache[pkg].NewInstall()
	  && !domain_dominates_or_is_equal (new_domain, cur_domain))
	{
	  log_stderr ("domain change: %s (%s -> %s)",
		      pkg.Name(),
		      domains[cur_domain].name,
		      domains[new_domain].name);
	  response.encode_int (pkgtrust_domains_violated);
	  response.encode_string (pkg.Name());
	}
    }

  response.encode_int (pkgtrust_end);
}

static void
encode_upgrades ()
{
  AptWorkerCache *awc = AptWorkerCache::GetCurrent ();
  if (ensure_cache (true))
    {
      pkgDepCache &cache = *(awc->cache);

      for (pkgCache::PkgIterator pkg = cache.PkgBegin();
	   pkg.end() != true;
	   pkg++)
	{
	  if (cache[pkg].Upgrade() && !cache[pkg].NewInstall())
	    {
	      response.encode_string (pkg.Name());
	      response.encode_string (cache[pkg].CandVersion);
	    }
	}
    }

  response.encode_string (NULL);
}

/* We modify the pkgDPkgPM package manager so that we can provide our
   own method of constructing the 'order list', the ordered list of
   packages to handle.  We do this to ignore packages that should be
   kept.  Ordinarily, if a package should be kept but needs to be
   reinstalled or configured, libapt-pkg will try to do that with
   every operation.

   XXX - there might be a way to get the same effect by cleverly
         manipulating the pkgDepCache, but I would have to look
         harder...
*/

class myTrustLevel
{
public:
  myTrustLevel(pkgSourceList *const Sources, pkgCache::VerIterator const &Version) :
    TrustLevel(0)
  {
      std::vector<pkgCache::VerFileIterator> trusted;
      /* First, find which versions come from trusted sources, keeping the
         'most trusted' sources only
      */
      for (auto i = Version.FileList(); i.end() == false; ++i)
        {
          pkgIndexFile *Index;

          if (!Sources->FindIndex(i.File(), Index))
            continue;

          int l = index_trust_level_for_package(Index, Version);

          if (l >= TrustLevel)
            {
              if (l > TrustLevel)
                trusted.clear();

              trusted.push_back(i);
              TrustLevel = l;
            }
        }

      /* Now, mark all not trusted */
      for (auto i = Version.FileList(); i.end() == false; ++i)
        {
          if (std::find(trusted.begin(), trusted.end(), i) == trusted.end())
            i.File()->Flags |= pkgCache::Flag::NotSource;
        }
  }
protected:
  int TrustLevel;
};

class myAcqArchive : public myTrustLevel, public pkgAcqArchive
{
public:
  explicit myAcqArchive(pkgAcquire *const Owner, pkgSourceList *const Sources,
                        pkgRecords *const Recs, pkgCache::VerIterator const &Version,
                        std::string &StoreFilename) :
    myTrustLevel(Sources, Version),
    pkgAcqArchive(Owner, Sources, Recs, Version, StoreFilename)
  {
  };

  bool IsTrusted()
  {
     return TrustLevel > 0;
  }
};

class myDPkgPM : public pkgDPkgPM
{
public:

  bool CheckDownloadedPkgs (bool clear_corrupted);

  bool CreateOrderList ();

  bool GetArchives(pkgAcquire *Owner,pkgSourceList *Sources, pkgRecords *Recs);

  myDPkgPM(pkgDepCache *Cache);
};

bool myDPkgPM::GetArchives(pkgAcquire *Owner,pkgSourceList *Sources,
                           pkgRecords *Recs)
{
   auto &List = pkgPackageManager::List;

   if (CreateOrderList() == false)
      return false;

   bool const ordering =
        _config->FindB("PackageManager::UnpackAll",true) ?
                List->OrderUnpack() : List->OrderCritical();
   if (ordering == false)
      return _error->Error("Internal ordering error");

   for (pkgOrderList::iterator I = List->begin(); I != List->end(); ++I)
   {
      PkgIterator Pkg(Cache,*I);
      FileNames[Pkg->ID] = string();

      // Skip packages to erase
      if (Cache[Pkg].Delete() == true)
         continue;

      // Skip Packages that need configure only.
      if (Pkg.State() == pkgCache::PkgIterator::NeedsConfigure &&
          Cache[Pkg].Keep() == true)
         continue;

      // Skip already processed packages
      if (List->IsNow(Pkg) == false)
         continue;

      new myAcqArchive(Owner, Sources, Recs, Cache[Pkg].InstVerIter(Cache),
                       FileNames[Pkg->ID]);
   }

   return true;
}

bool
myDPkgPM::CreateOrderList ()
{
  if (pkgPackageManager::List != 0)
    return true;
   
  delete pkgPackageManager::List;
  pkgPackageManager::List = new pkgOrderList(&Cache);
   
  // Generate the list of affected packages and sort it
  for (PkgIterator I = Cache.PkgBegin(); I.end() == false; I++)
    {
      // Ignore no-version packages
      if (I->VersionList == 0)
	continue;
      
      // Not interesting
      if ((Cache[I].Keep() == true || 
	   Cache[I].InstVerIter(Cache) == I.CurrentVer()) && 
	  I.State() == pkgCache::PkgIterator::NeedsNothing &&
	  (Cache[I].iFlags & pkgDepCache::ReInstall) != pkgDepCache::ReInstall &&
	  (I.Purge() != false || Cache[I].Mode != pkgDepCache::ModeDelete ||
	   (Cache[I].iFlags & pkgDepCache::Purge) != pkgDepCache::Purge))
	continue;
      
      // Ignore interesting but kept packages, except when they are
      // related to the current operation.

      if (Cache[I].Keep() == true && !is_related (I))
	{
	  log_stderr ("Not handling unrelated package %s.", I.Name());
	  continue;
	}
      
      DBG ("Handling interesting package %s.", I.Name());

      // Append it to the list
      pkgPackageManager::List->push_back(I);      
    }
   
  return true;
}

bool
myDPkgPM::CheckDownloadedPkgs (bool clean_corrupted)
{
  bool result = true;
  package_record rec;

  for (pkgOrderList::iterator I = pkgPackageManager::List->begin(); 
       I != pkgPackageManager::List->end(); I++)
    {
      bool partial_result = true;
      PkgIterator Pkg(Cache,*I);
      pkgCache::VerIterator cand_ver = Cache[Pkg].CandidateVerIter(Cache);

      rec.lookup(cand_ver);
      string File = FileNames[Pkg->ID];
      if (File.empty())
        continue;
      FileFd Fd (File, FileFd::ReadOnly);
      if (_error->PendingError() == true) // return false?
        continue;

      Hashes hashes = Hashes();
      hashes.AddFD(Fd.Fd(), Fd.Size());

      string file_sha256 = hashes.GetHashString(Hashes::SHA256SUM).HashValue();
      string file_sha1 = hashes.GetHashString(Hashes::SHA1SUM).HashValue();
      string MD5 = hashes.GetHashString(Hashes::MD5SUM).HashValue();

      string ExpectedSHA256 = rec.get_string("SHA256");

      if (!ExpectedSHA256.empty())
        {
          if (file_sha256 != ExpectedSHA256)
            {
              log_stderr ("File %s is corrupted (SHA256).", File.c_str());
              partial_result = false;
            }
        }
      else
        {
          string ExpectedSHA1 = rec.get_string("SHA1");

          if (!ExpectedSHA1.empty())
            {
              if (file_sha1 != ExpectedSHA1)
                {
                  log_stderr ("File %s is corrupted (SHA1).", File.c_str());
                  partial_result = false;
                }
            }
          else
            {
              string ExpectedMD5 = rec.get_string("MD5sum");

              if (!ExpectedMD5.empty())
                {
                  if (MD5 != ExpectedMD5)
                    {
                      log_stderr ("File %s is corrupted (MD5sum).", File.c_str());
                      partial_result = false;
                    }
                }
            }
        }
      Fd.Close();
      result = result && partial_result;
      if (clean_corrupted && !partial_result)
        unlink (File.c_str());
    }
  return result;
}

myDPkgPM::myDPkgPM (pkgDepCache *Cache)
  : pkgDPkgPM (Cache)
{
}

static int
combine_rescodes (int all, int one)
{
  if (all == rescode_success)
    return one;
  else if (all == one)
    return all;
  else
    return rescode_failure;
}

/* Sets an alternative cache directory to download packages.
   If alt_download_root is NULL, then the default path is set */
static bool
set_dir_cache_archives (const char *alt_download_root)
{
  bool result = false;

  if (alt_download_root == NULL)
    {
      /* Setting default location */

      /* If setting to default set just the relative path */
      _config->Set ("Dir::Cache::Archives", DEFAULT_DIR_CACHE_ARCHIVES);

      result = true;
    }
  else
    {
      char *archives_dir = NULL;
      /* Setting an alternative location */

      /* Prepare data to create a temporary directory */
      archives_dir = g_strdup_printf ("%s/%s",
				       alt_download_root,
				       ALT_DIR_CACHE_ARCHIVES);

      /* Create temporary dir to use it as download location */
      if (mkdir (archives_dir, 0777) < 0 && errno != EEXIST)
	{
	  _error->Error ("Can't create directory at %s: %m", archives_dir);
	}
      else
	{
	  char *partial_dir = g_strdup_printf ("%s/partial", archives_dir);
	  if (mkdir (partial_dir, 0777) < 0  && errno != EEXIST)
	    {
	      _error->Error("Can't create directory at %s: %m", partial_dir);
	    }
	  else
	    {
	      /* If not setting to default set the full path */
	      _config->Set ("Dir::Cache::Archives", archives_dir);

	      result = true;
	    }
	  g_free (partial_dir);
	}
      g_free (archives_dir);
    }

  return result;
}

static int64_t
get_pkg_required_free_space ()
{
  AptWorkerCache *awc = AptWorkerCache::GetCurrent ();
  pkgDepCache &cache = *(awc->cache);
  package_record rec;
  int64_t retval = 0;

  for (pkgCache::PkgIterator pkg = cache.PkgBegin (); pkg.end () != true; pkg++)
    { 
      if (is_related (pkg) &&
          (cache[pkg].Upgrade ()
           || pkg.State () != pkgCache::PkgIterator::NeedsNothing))
        {
          pkgCache::VerIterator ver = cache[pkg].CandidateVerIter (cache);

          rec.lookup(ver);
          retval += get_required_free_space (rec);
        }
    }

  return retval;
}

static bool
is_there_enough_free_space (const char *archive_dir, int64_t size)
{
  int64_t free_space = get_free_space (archive_dir);

  if (free_space < 0)
    {
      log_stderr ("Couldn't determine free space in %s", archive_dir);
      return false;
    }

  const char *internal_mmc_mountpoint = getenv ("INTERNAL_MMC_MOUNTPOINT");
  if (!internal_mmc_mountpoint)
    internal_mmc_mountpoint = INTERNAL_MMC_MOUNTPOINT;

  const char *removable_mmc_mountpoint = getenv ("REMOVABLE_MMC_MOUNTPOINT");
  if (!removable_mmc_mountpoint)
    removable_mmc_mountpoint = REMOVABLE_MMC_MOUNTPOINT;

  /* what if after downloaded the bytes in DEFAULT_DIR_CACHE_ARCHIVES
   * there's not enough space to install them? */
  if (internal_mmc_mountpoint
      && !strstr (archive_dir, internal_mmc_mountpoint)
      && removable_mmc_mountpoint
      && !strstr (archive_dir, removable_mmc_mountpoint)
      && !strstr (archive_dir, HOME_MOUNTPOINT))
    {
      /* Should we add install_user_size_delta value */
      size += get_pkg_required_free_space ();
    }

  if (size > free_space)
    {
      log_stderr ("You don't have enough free space in %s", archive_dir);
      return false;
    }

  return true;
}

/* operation () is used to run pending apt operations
 * (removals or installations). If check_only parameter is
 * enabled, it will only check if the operation is doable.
 *
 * operation () is used from cmd_install_package,
 * cmd_install_check and cmd_remove_package
 */

static int
operation (bool check_only,
	   const char *alt_download_root,
	   bool download_only,
	   bool allow_download,
	   bool with_status)
{
  AptWorkerCache *awc = AptWorkerCache::GetCurrent ();
  pkgCacheFile &Cache = *(awc->cache);
  std::unique_ptr<myDPkgPM> Pm;

  if (_config->FindB("APT::Get::Purge",false) == true)
    {
      pkgCache::PkgIterator I = Cache->PkgBegin();
      for (; I.end() == false; I++)
	{
	  if (I.Purge() == false && Cache[I].Mode == pkgDepCache::ModeDelete)
	    Cache->MarkDelete(I,true);
	}
    }

  if (Cache->DelCount() == 0 && Cache->InstCount() == 0 &&
      Cache->BadCount() == 0)
    {
      /* Encode trust summary and upgrades information when running
	 APTCMD_INSTALL_CHECK, even when there's nothing to do */
      if (check_only)
	{
	  response.encode_int (pkgtrust_end);
	  response.encode_string (NULL);
	}
      return rescode_success;
    }

  // Create the text record parser
  pkgRecords Recs (Cache);
  if (_error->PendingError() == true)
    return rescode_failure;

  if (!set_dir_cache_archives (alt_download_root))
    {
      /* Log error, but keep working with default value */
      fprintf (stderr,
	       "Failed using %s to download packages. Using default.\n",
	       alt_download_root);
    }

  // Lock the archive directory
  FileFd Lock;
  if (_config->FindB("Debug::NoLocking",false) == false)
    {
      Lock.Fd(ForceLock(_config->FindDir("Dir::Cache::Archives") + "lock"));
      if (_error->PendingError() == true)
	{
	  _error->Error("Unable to lock the download directory");
	  return rescode_failure;
	}
    }

  // Create the download object
  DownloadStatus Stat;
  pkgAcquire Fetcher (with_status? &Stat : NULL);

  // Read the source list
  pkgSourceList List;
  if (List.ReadMainList() == false)
    {
      _error->Error("The list of sources could not be read.");
      return rescode_failure;
    }

  set_sources_for_get_domain (&List);

  // Create the package manager
  //
  Pm.reset(new myDPkgPM (Cache));

  // Create the order list explicitely in a way that we like.  We
  // have to do it explicitely since CreateOrderList is not virtual.
  //
  if (!Pm->CreateOrderList ())
    return rescode_failure;

  // Prepare to download
  //
  reset_new_domains ();
  if (Pm->GetArchives(&Fetcher,&List,&Recs) == false ||
      _error->PendingError() == true)
    return rescode_failure;

  double FetchBytes = Fetcher.FetchNeeded();
  double FetchPBytes = Fetcher.PartialPresent();

  if (_error->PendingError() == true)
    return rescode_failure;

  if (check_only)
    {
      encode_trust_summary ();
      encode_upgrades ();
      return rescode_success;
    }

  collect_new_domains ();

  if ((int)(FetchBytes - FetchPBytes) > 0)
    {
      if (!allow_download)
	{
	  log_stderr ("would need to download, but it's not allowed");

#ifdef DEBUG
	  for (pkgAcquire::ItemIterator I = Fetcher.ItemsBegin();
	       I != Fetcher.ItemsEnd(); I++)
	    {
	      fprintf (stderr, "Would download %s\n",
		       (*I)->DescURI().c_str());
	    }
#endif

	  return rescode_packages_not_found;
	}

      download_size = FetchBytes - FetchPBytes;
      if (!is_there_enough_free_space
          (_config->FindDir ("Dir::Cache::Archives").c_str (), download_size))
            return rescode_out_of_space;
      
      /* Send a status report now if we are going to download
	 something.  This makes sure that the progress dialog is
	 shown even if the first pulse of the fetcher takes a long
	 time to arrive.
      */

      if (with_status)
	send_status (op_downloading, 0, (int)(FetchBytes - FetchPBytes), 0);
    }

  if (Fetcher.Run() == pkgAcquire::Failed)
    return rescode_failure;

  /* Print out errors and distill the failure reasons into a
     apt_proto_rescode.
  */
  int result = rescode_success;
  for (pkgAcquire::ItemIterator I = Fetcher.ItemsBegin();
       I != Fetcher.ItemsEnd(); I++)
    {
      if ((*I)->Status == pkgAcquire::Item::StatDone &&
	  (*I)->Complete == true)
	continue;

      if ((*I)->Status == pkgAcquire::Item::StatIdle)
	continue;

      fprintf (stderr,
	       "Failed to fetch %s: %s\n",
	       (*I)->DescURI().c_str(),
	       (*I)->ErrorText.c_str());

      int this_result;

      if (g_str_has_prefix ((*I)->ErrorText.c_str(), "404"))
	this_result = rescode_packages_not_found;
      else if (g_str_has_prefix ((*I)->ErrorText.c_str(), 
				 "Size mismatch"))
	this_result = rescode_package_corrupted;
      else if (g_str_has_prefix ((*I)->ErrorText.c_str(), 
				 "MD5Sum mismatch"))
	this_result = rescode_package_corrupted;
      else
	this_result = rescode_failure;

      result = combine_rescodes (result, this_result);
    }

  if (result != rescode_success)
    return (result == rescode_failure)?rescode_download_failed:result;

  /* Make sure that all the packages are written to disk before
     proceeding.  This helps with retrying the operation in case it is
     interrupted.
  */

  if (with_status)
    send_status (op_downloading, -1, 0, 0);

  /* Install packages if not just downloading */
  if (!download_only)
    {
      if (with_status)
	send_status (op_general, -1, 0, 0);

      if (Pm->CheckDownloadedPkgs (true) == false)
        return rescode_package_corrupted;

      // sync before installing
      sync ();

      /* Do install */
      _system->UnLock();
      APT::Progress::PackageManagerProgressFd progress_mgr(status_fd);
      pkgPackageManager::OrderResult Res = Pm->DoInstall (&progress_mgr);
      _system->Lock();

      awc->cache->save_extra_info ();

      if (Res == pkgPackageManager::Failed || 
	  _error->PendingError() == true)
	return rescode_failure;

      if (Res != pkgPackageManager::Completed)
	return rescode_failure;
    }

  return rescode_success;
}

/* APTCMD_CLEAN
 */

void
cmd_clean ()
{
  bool success = true;
  AptWorkerCache *awc = AptWorkerCache::GetCurrent ();

  // Try to lock the archive directory.  If that fails because we are
  // out of space, continue anyway since it is critical to free flash
  // in that case.
  //
  // ForceLock has the same interface as GetLock: it returns -1 in
  // case of failure with errno set appropriately.  However, errnor is
  // always EPERM when the open syscall failed.  "Feh.."

  FileFd Lock;
  if (_config->FindB("Debug::NoLocking",false) == false)
    {
      int fd = ForceLock(_config->FindDir("Dir::Cache::Archives") + "lock");
      if (fd < 0)
	{
	  if (errno != EPERM && errno != ENOSPC)
	    {
	      success = false;
	      _error->Error("Unable to lock the download directory");
	    }
	  else
	    _error->Warning("Unable to lock the download directory, but cleaning it anyway.");
	}
      else
	Lock.Fd (fd);
    }
   
  if (success)
    {
      pkgAcquire Fetcher;
      Fetcher.Clean(_config->FindDir("Dir::Cache::archives"));
      Fetcher.Clean(_config->FindDir("Dir::Cache::archives") + "partial/");

      // Make sure the filesystem is aware of the space freed
      sync();
    }

  response.encode_int (success);

  // As a special case, we try to init the cache again.  Chances are
  // good that it will now succeed because there might be more space
  // available now.  We don't use ensure_cache for this since we want
  // it to happen silently.

  if (awc->cache == NULL)
    need_cache_init ();
}

static char *
escape_for_shell (const char *str)
{
  char buf[2000];
  char *p = buf;

  /* Enclose the string in single quotes and escape single quotes.
   */

  *p++ = '\'';
  while (*str)
    {
      if (p >= buf+sizeof(buf)-6)
	return NULL;

      if (*str == '\'')
	{
	  // Don't you love bourne shell syntax?
	  *p++ = '\'';
	  *p++ = '\\';
	  *p++ = '\'';
	  *p++ = '\'';
	  str++;
	}
      else
	*p++ = *str++;
    }
  *p++ = '\'';
  *p++ = '\0';
  return g_strdup (buf);
}

// XXX - interpret status codes

static char *
get_deb_record (const char *filename)
{
  char *esc_filename = escape_for_shell (filename);
  if (esc_filename == NULL)
    return NULL;

  char *cmd = g_strdup_printf ("/usr/bin/dpkg-deb -f %s", esc_filename);
  fprintf (stderr, "%s\n", cmd);
  FILE *f = popen (cmd, "r");

  g_free (cmd);
  g_free (esc_filename);

  if (f)
    {
      const size_t incr = 2000;
      char *record = NULL;
      size_t size = 0;

      do
	{
	  // increase buffer and try to fill it, leaving room for the
	  // trailing newlines and nul.
	  // XXX - do it properly.

	  char *new_record = new char[size + incr + 3];
	  if (record)
	    {
	      memcpy (new_record, record, size);
	      delete [] record;
	    }
	  record = new_record;

	  size += fread (record + size, 1, incr, f);
	}
      while (!feof (f));

      int status = pclose (f);
      if (status != 0)
	{
	  delete [] record;
	  return NULL;
	}

      record[size] = '\n';
      record[size + 1] = '\n';
      record[size + 2] = '\0';
      return record;
    }
  return NULL;
}

static bool
check_dependency (string &package, string &version, unsigned int op)
{
  AptWorkerCache *awc = AptWorkerCache::GetCurrent ();
  if (!ensure_cache (true))
    return false;

  pkgDepCache &cache = *(awc->cache);
  pkgCache::PkgIterator pkg;
  pkgCache::VerIterator installed;

  pkg = cache.FindPkg (package);
  if (pkg.end ())
    return false;

  installed = pkg.CurrentVer ();
  if (installed.end ())
    {
      // might be a virtual package, check the provides list.

      pkgCache::PrvIterator P = pkg.ProvidesList();

      for (; P.end() != true; P++)
	{
	  // Check if the provides is a hit
	  if (P.OwnerPkg().CurrentVer() != P.OwnerVer())
	    continue;

	  // Compare the versions.
	  if (debVS.CheckDep (P.ProvideVersion(), op, version.c_str ()))
	    return true;
	}

      return false;
    }
  else
    return debVS.CheckDep (installed.VerStr (), op, version.c_str ());
}

static void
add_dep_string (string &str,
		string &package, string &version, unsigned int op)
{
  str += package;
  if (op != pkgCache::Dep::NoOp)
    {
      str += " (";
      str += pkgCache::CompType (op);
      str += " ";
      str += version;
      str += ")";
    }
}

static int
check_and_encode_missing_dependencies (const char *deps, const char *end,
				       bool only_check)
{
  const char *ptr;
  string package, version;
  unsigned int op;
  bool dep_ok = true;

  ptr = deps;
  while (true)
    {
      // check one 'or group'

      bool group_ok = false;
      string group_string = "";

      while (true)
	{
	  ptr = debListParser::ParseDepends (ptr, end,
					     package, version, op,
					     false);
	  if (ptr == NULL)
	    {
	      cerr << "Error parsing depends list\n";
	      return false;
	    }

	  if (only_check && package == "maemo")
	    return status_incompatible_current;

	  add_dep_string (group_string, package, version, op);

	  if (!group_ok)
	    group_ok = check_dependency (package, version,
					 op & ~pkgCache::Dep::Or);

	  if ((op & pkgCache::Dep::Or) == 0)
	    break;

	  group_string += " | ";
	}

      if (!group_ok)
	{
	  if (only_check)
	    cerr << "FAILED: " << group_string << "\n";
	  else
	    {
	      response.encode_int (sumtype_missing);
	      response.encode_string (group_string.c_str ());
	    }
	  dep_ok = false;
	}

      if (ptr == end)
	break;
    }

  return dep_ok? status_able : status_missing;
}

static bool
get_field (pkgTagSection *section, const char *field,
	   const char *&start, const char *&end)
{
  if (section->Find (field, start, end))
    {
      // fprintf (stderr, "%s = %.*s\n", field, end-start, start);
      return true;
    }
  else
    {
      // fprintf (stderr, "%s = <undefined>\n", field);
      return false;
    }
}

static int
get_field_int (pkgTagSection *section, const char *field, int def)
{
  const char *start, *end;
  if (get_field (section, field, start, end))
    return atoi (start);
  else
    return def;
}

static void
encode_field (pkgTagSection *section, const char *field,
	      const char *def = "")
{
  const char *start, *end;
  if (get_field (section, field, start, end))
    response.encode_stringn (start, end-start);
  else
    response.encode_string (def);
}

static void
encode_localized_field (pkgTagSection *section, const char *field,
			const char *def = "")
{
  const char *start, *end;

  if (lc_messages && *lc_messages)
    {
      char *locale_field = g_strdup_printf ("%s-%s", field, lc_messages);
      if (get_field (section, locale_field, start, end))
	{
	  response.encode_stringn (start, end-start);
	  g_free (locale_field);
	  return;
	}
      g_free (locale_field);
    }
  
  if (get_field (section, field, start, end))
    response.encode_stringn (start, end-start);
  else
    response.encode_string (def);
}

static bool
substreq (const char *start, const char *end, const char *str)
{
  return ((size_t)(end-start) == strlen (str)) && !strncmp (start, str, end-start);
}

static int
check_installable (pkgTagSection &section, bool only_user)
{
  int installable_status = status_able;
  const char *start, *end;

  if (!get_field (&section, "Architecture", start, end)
      || !(substreq (start, end, DEB_HOST_ARCH)
	   || substreq (start, end, "all")))
    installable_status = status_incompatible;
    
  if (only_user
      && (!get_field (&section, "Section", start, end)
	  || !is_user_section (start, end)))
    {
      /* Put more information for developers into the log.  They will
	 likely be confused by the "incompatible" error message when
	 testing a package that has not been properly 'sectionized'.
       */
      fprintf (stderr,
	       "Package must have \"Section: user/FOO\" "
	       "to be considered compatible.\n");
      installable_status = status_incompatible;
    }

  if (get_field (&section, "Pre-Depends", start, end))
    installable_status =
      combine_status (check_and_encode_missing_dependencies (start, end, true),
		      installable_status);

  if (get_field (&section, "Depends", start, end))
    installable_status = 
      combine_status (check_and_encode_missing_dependencies (start, end, true),
		      installable_status);

  return installable_status;
}

static void
encode_missing_dependencies (pkgTagSection &section)
{
  const char *start, *end;

  if (get_field (&section, "Pre-Depends", start, end))
    check_and_encode_missing_dependencies (start, end, false);

  if (get_field (&section, "Depends", start, end))
    check_and_encode_missing_dependencies (start, end, false);
}

void
cmd_get_file_details ()
{
  bool only_user = request.decode_int ();
  const char *filename = request.decode_string_in_place ();

  char *record = get_deb_record (filename);
  pkgTagSection section;
  if (record == NULL || !section.Scan (record, strlen (record)))
    {
      response.encode_string (basename (filename));
      response.encode_string (basename (filename));
      response.encode_string (NULL);      // installed_version
      response.encode_int64 (0);          // installed_size
      response.encode_string ("");        // version
      response.encode_string ("");        // maintainer
      response.encode_string ("");        // section
      response.encode_int (status_corrupted);
      response.encode_int64 (0);          // installed size
      response.encode_string ("");        // description
      response.encode_string (NULL);      // icon
      response.encode_int (sumtype_end);
      return;
    }

  int installable_status = check_installable (section, only_user);

  const char *installed_version = NULL;
  int64_t installed_size = 0;

  if (ensure_cache (true))
    {
      AptWorkerCache *awc = AptWorkerCache::GetCurrent ();
      pkgDepCache &cache = *(awc->cache);
      pkgCache::PkgIterator pkg = cache.FindPkg (section.FindS ("Package"));
      if (!pkg.end ())
	{
	  pkgCache::VerIterator cur = pkg.CurrentVer ();
	  if (!cur.end ())
	    {
	      installed_version = cur.VerStr ();
	      installed_size = cur->InstalledSize;
	    }
	}
    }

  encode_field (&section, "Package");
  encode_localized_field (&section, "Maemo-Display-Name", NULL);
  response.encode_string (installed_version);
  response.encode_int64 (installed_size);
  encode_field (&section, "Version");
  encode_field (&section, "Maintainer");
  encode_field (&section, "Section");
  response.encode_int (installable_status);
  response.encode_int64 (1024LL * get_field_int (&section, "Installed-Size", 0)
		       - installed_size);
  encode_localized_field (&section, "Description");
  encode_field (&section, "Maemo-Icon-26", NULL);

  if (installable_status != status_able)
    encode_missing_dependencies (section);
  response.encode_int (sumtype_end);

  delete[] record;
}

void
cmd_install_file ()
{
  const char *filename = request.decode_string_in_place ();
  char *esc_filename = escape_for_shell (filename);
  
  if (esc_filename == NULL)
    {
      response.encode_int (0);
      return;
    }

  _system->UnLock();

  char *cmd = g_strdup_printf ("/usr/bin/dpkg --install %s", esc_filename);
  fprintf (stderr, "%s\n", cmd);
  int res = system (cmd);
  g_free (cmd);

  if (res)
    {
#if 0
      /* We do not remove packages that failed to install since this
	 might have been an update attempt and removing the old
	 version is confusing.  Installations at this point do not
	 fail because of missing dependencies, so it is not that
	 important anymore to try to leave the system in a consistent
	 state.
      */
      char *cmd =
	g_strdup_printf ("/usr/bin/dpkg --purge "
			 "`/usr/bin/dpkg-deb -f %s Package`",
			 esc_filename);
      fprintf (stderr, "%s\n", cmd);
      system (cmd);
      g_free (cmd);
#endif
    }

  g_free (esc_filename);

  _system->Lock();

  need_cache_init ();
  response.encode_int (res == 0);
}

/* APTCMD_SAVE_BACKUP_DATA

   This method is used to store the list of installed packages. It's
   used in backup machinery to restore the installed applications
   from their repositories.
 */

static xexp *
get_backup_packages ()
{
  AptWorkerCache *awc = AptWorkerCache::GetCurrent ();

  if (!ensure_cache (true))
    return NULL;

  xexp *packages = xexp_list_new ("backup");

  pkgDepCache &cache = *(awc->cache);

  for (pkgCache::PkgIterator pkg = cache.PkgBegin(); !pkg.end (); pkg++)
    {
      pkgCache::VerIterator installed = pkg.CurrentVer ();

      // skip not-installed packages
      //
      if (installed.end ())
	continue;

      // skip non user packages
      //
      if (!installed.end ()
	  && !is_user_package (installed))
	continue;

      xexp_cons (packages, xexp_text_new ("pkg", pkg.Name ()));
    }

  return packages;
}

void
cmd_save_backup_data ()
{
  backup_catalogues ();

  xexp *packages = get_backup_packages ();
  if (packages)
    {
      xexp_write_file (BACKUP_PACKAGES, packages);
      xexp_free (packages);
    }
}

/* MANAGEMENT FOR FAILED CATALOGUES LOG FILE */

static void
save_failed_catalogues (xexp *catalogues)
{
  xexp *failed_catalogues = xexp_list_new ("catalogues");

  /* Save ONLY catalogues with errors */
  for (xexp *cat = xexp_first (catalogues); cat; cat = xexp_rest (cat))
    {
      xexp *errors_item = xexp_aref (cat, "errors");

      /* If containing errors details, add it to the list */
      if (errors_item)
	xexp_append_1 (failed_catalogues, xexp_copy (cat));
    }

  if (xexp_length (failed_catalogues) > 0)
    xexp_write_file (FAILED_CATALOGUES_FILE, failed_catalogues);
  else
    clean_failed_catalogues ();

  xexp_free (failed_catalogues);
}

static xexp *
load_failed_catalogues ()
{
  struct stat buf;
  int stat_result;
  xexp *failed_catalogues = NULL;

  /* Check if there are problems reading the file */
  stat_result = stat (FAILED_CATALOGUES_FILE, &buf);
  if (!stat_result)
    {
      failed_catalogues = xexp_read_file (FAILED_CATALOGUES_FILE);
      if (xexp_length (failed_catalogues) <= 0)
	{
	  /* Return NULL and clean failed catalogues file if there
	     are no errors contained inside of it (quite odd) */
	  xexp_free (failed_catalogues);
	  failed_catalogues = NULL;

	  clean_failed_catalogues ();
	}
    }
  else if (errno != ENOENT)
    log_stderr ("error reading file %s: %m", FAILED_CATALOGUES_FILE);

  return failed_catalogues;
}

static void
clean_failed_catalogues ()
{
  if (unlink (FAILED_CATALOGUES_FILE) < 0 && errno != ENOENT)
    log_stderr ("error unlinking %s: %m", FAILED_CATALOGUES_FILE);
}

static void
clean_temp_catalogues ()
{
  if (unlink (TEMP_APT_SOURCE_LIST) < 0 && errno != ENOENT)
    log_stderr ("error unlinking %s: %m", TEMP_APT_SOURCE_LIST);
}

static void
write_available_updates_file ()
{
  if (!ensure_cache (false))
    return;

  xexp *x_updates = xexp_list_new ("updates");
  package_record rec;
  AptWorkerCache *awc = AptWorkerCache::GetCurrent ();
  pkgDepCache &cache = *(awc->cache);

  for (pkgCache::PkgIterator pkg = cache.PkgBegin(); !pkg.end (); pkg++)
    {
      /* This duplicates the logic that determines which packages
	 would be shown in the "Check for Updates" view in blue-pill
	 mode.

	 Namely, a package is shown when it has both a installed and
	 available version, the available version is newer than the
	 installed version, and the available version is user-visible.

	 Packages are classified as "OS", "Certified", and "Other".  A
	 package belongs to the "OS" class when it has the
	 system-update flag set.  Otherwise, it belongs to the
	 "Certified" class when it is in a certified domain.
	 Otherwise it belongs to the "Other" class.  (The update
	 notifier presents the "Certified" class as "Nokia".)

	 XXX - it would be good to not duplicate so much logic, of
	       course.
      */

      pkgCache::VerIterator installed = pkg.CurrentVer ();
      pkgCache::VerIterator candidate = cache[pkg].CandidateVerIter(cache);
      bool broken = (cache[pkg].NowBroken()
		     || (pkg.State () != pkgCache::PkgIterator::NeedsNothing));

      if (!candidate.end ()
	  && !installed.end()
	  && installed.CompareVer (candidate) < 0
	  && is_user_package (candidate)
          && !broken)
	{
	  xexp *x_pkg = NULL;

	  rec.lookup(candidate);
	  int flags = get_flags (rec);
	  int domain_index = awc->cache->extra_info[pkg->ID].cur_domain;

          const char *pkg_name;
          string pretty_name = get_pretty_name (rec);
          if (!pretty_name.empty ())
            pkg_name = pretty_name.c_str ();
          else
            pkg_name = pkg.Name ();

	  if (flags & pkgflag_system_update)
	    x_pkg = xexp_text_new ("os", pkg_name);
	  else if (domains[domain_index].is_certified)
	    x_pkg = xexp_text_new ("certified", pkg_name);
	  else
	    x_pkg = xexp_text_new ("other", pkg_name);

	  xexp_cons (x_updates, x_pkg);
	}
    }

  xexp_write_file (AVAILABLE_UPDATES_FILE, x_updates);

  if (x_updates)
    xexp_free (x_updates);
}

static xexp *
merge_catalogues_with_errors (xexp *catalogues)
{
  xexp *failed_catalogues = NULL;

  /* First check if there's an errors file after the last refresh */
  failed_catalogues = load_failed_catalogues ();

  /* Add errors (if present) for each catalogue */
  if (failed_catalogues != NULL)
    {
      const char *uri = NULL, *f_uri = NULL;
      const char *dist = NULL, *f_dist = NULL;
      const char *comp = NULL, *f_comp = NULL;

      for (xexp *cat = xexp_first (catalogues); cat; cat = xexp_rest (cat))
	{
	  uri = xexp_aref_text (cat, "uri");
	  dist = xexp_aref_text (cat, "dist");
	  comp = xexp_aref_text (cat, "components");

	  /* Look for errors for this catalogue */
	  for (xexp *f_cat = xexp_first (failed_catalogues); f_cat; f_cat = xexp_rest (f_cat))
	    {
	      f_uri = xexp_aref_text (f_cat, "uri");
	      f_dist = xexp_aref_text (f_cat, "dist");
	      f_comp = xexp_aref_text (f_cat, "components");

	      /* If matches the uri (mandatory field), check the other ones */
	      if (!strcmp (uri, f_uri))
		{
		  xexp *errors_item = NULL;

		  /* 'dist' fields must be the same, or NULL, to continue */
		  if ((dist == NULL || f_dist == NULL || strcmp (dist, f_dist)) &&
		      !(dist == NULL && f_dist == NULL))
		    continue;

		  /* 'dist' fields must be the same, or NULL, to continue */
		  if ((comp == NULL || f_comp == NULL || strcmp (comp, f_comp)) &&
		      !(comp == NULL && f_comp == NULL))
		    continue;

		  /* If reached, errors would be about the same
		     catalogue found in the configuration file */
		  errors_item = xexp_aref (f_cat, "errors");
		  if (errors_item != NULL)
		    {
		      /* Append a copy of errors to the catalogues xexp */
		      xexp_append_1 (cat, xexp_copy (errors_item));

		      /* Delete the xexp from errors file, in order
			 not to check it again in later checks */
		      xexp_del (failed_catalogues, f_cat);
		    }
		}
	    }
	}

      /* Free the catalogues errors xexp */
      xexp_free (failed_catalogues);
    }

  return catalogues;
}

static xexp *
map_catalogue_error_details (xexp *x)
{
  xexp *mapped_x = xexp_copy (x);
  xexp *errors_item = xexp_aref (mapped_x, "errors");

  /* If containing errors details, remove errors */
  if (errors_item)
    xexp_del (mapped_x, errors_item);

  /* Return the mapped xexp */
  return mapped_x;
}

void
cmd_reboot ()
{
  if (system ("/usr/sbin/dsmetool -b") == 0)
    return;
  system ("/sbin/reboot");
}

/* Rescue
 */

static void
save_operation_record (const char *package, const char *download_root)
{
  xexp *record = xexp_list_new ("install");
  xexp_aset_text (record, "package", package);
  xexp_aset_text (record, "download-root", download_root);
  xexp_write_file (CURRENT_OPERATION_FILE, record);
  xexp_free (record);
}

static void
erase_operation_record ()
{
  unlink (CURRENT_OPERATION_FILE);
}

static xexp *
read_operation_record ()
{
  struct stat buf;

  /* This check is not strictly necessary but it avoids a distracting
     complaint from xexp_read_file in the common case that the file
     doesn't exist.
  */
  if (stat (CURRENT_OPERATION_FILE, &buf))
    return NULL;

  return xexp_read_file (CURRENT_OPERATION_FILE);
}

static int
run_system (bool verbose, const char *fmt, ...)
{
  va_list ap;
  va_start (ap, fmt);
  char *cmd = g_strdup_vprintf (fmt, ap);
  va_end (ap);

  if (verbose)
    fprintf (stderr, "+ %s\n", cmd);
  int result = system (cmd);
  free (cmd);

  return result;
}

static int
rescue_operation_with_dir (const char *dir)
{
  int result;

  fprintf (stderr, "Installing from %s\n", dir);
  result = operation (false, dir, false, false, false);
  fprintf (stderr, "Result code %d\n", result);
  _error->DumpErrors ();
  return result;
}

static int
rescue_operation_with_dev (const char *dev)
{
  const char *dir = "/rescue";

  fprintf (stderr, "Installing from %s\n", dev);

  if (mkdir (dir, 0777) < 0
      && errno != EEXIST)
    log_stderr ("%s: %m", dir);

  run_system (true, "mount -t vfat '%s' '%s'", dev, dir);
  int result = rescue_operation_with_dir (dir);
  run_system (true, "umount '%s'", dir);

  if (rmdir (dir) < 0)
    log_stderr ("%s: %m", dir);

  return result;
}

static int
rescue_operation_with_devnode (int major, int minor)
{
  const char *node = "/dev.rescue";

  if (mknod (node, S_IFBLK | 0600, major << 8 | minor) < 0)
    log_stderr ("%s: %m", node);

  fprintf (stderr, "Installing from %d:%d\n", major, minor);

  int result = rescue_operation_with_dev (node);

  unlink (node);

  return result;
}

static const char* rescue_devs[] = {
  "/dev/mmcblk0p1", // Internal MMC ~/MyDocs
  "/dev/mmcblk1p1", // External MMC
  "/dev/mmcblk0p2", // Internal MMC /home
  "/dev/mmcblk0p3", // Internal MMC swap
  "/dev/mmcblk0",
  "/dev/mmcblk1",
  NULL
};

static int
rescue_with_all_devs ()
{
  int result = rescode_packages_not_found;

  for (int i = 0;
       (result == rescode_packages_not_found
	&& rescue_devs[i] != NULL);
       i++)
    {
      result = rescue_operation_with_dev (rescue_devs[i]);
    }

  return result;
}

static struct {
  int major, minor;
} rescue_devnodes[] = {
  { 254, 9 },
  { 254, 1 },
  { 254, 8 },
  { 254, 0 },
  { -1 }
};

static int
rescue_with_all_devnodes ()
{
  int result = rescode_packages_not_found;

  for (int i = 0;
       (result == rescode_packages_not_found
	&& rescue_devnodes[i].major != -1);
       i++)
    {
      result = rescue_operation_with_devnode (rescue_devnodes[i].major,
					      rescue_devnodes[i].minor);
    }

  return result;
}

static void
show_fb_text (int line, const char *text)
{
  run_system (false,
	      "/usr/bin/text2screen -s 2 -x 5 -y %d -B -1 -T 0x0000 -t '%s'",
	      400 + 20*line, text);
}

static void
show_fb_status (int percent)
{
  run_system (false,
	      "/usr/bin/text2screen -s 2 -x 5 -y %d -B -1 -T 0x0000 -t '%3d%%'",
	      440, percent);
}

static void
interpret_pmstatus (char *str)
{
  float percentage;
  const char *title;

  if (!strncmp (str, "pmstatus:", 9))
    {
      str += 9;
      str = strchr (str, ':');
      if (str == NULL)
	return;
      str += 1;
      percentage = atof (str);
      str = strchr (str, ':');
      if (str == NULL)
	title = "Working";
      else
	{
	  str += 1;
	  title = str;
	}

      show_fb_status ((int)percentage);
      // show_fb_text (2, title);
    }
}

static void
fork_progress_process ()
{
  int fds[2], child_pid;

  if (pipe (fds) < 0)
    {
      perror ("pipe");
      return;
    }

  if ((child_pid = fork ()) < 0)
    {
      perror ("fork");
      return;
    }

  if (child_pid == 0)
    {
      close (fds[1]);
      FILE *f = fdopen (fds[0], "r");
      if (f)
	{
	  char *line = NULL;
	  size_t len = 0;
	  ssize_t n;

	  while ((n = getline (&line, &len, f)) != -1)
	    {
	      if (n > 0 && line[n-1] == '\n')
		line[n-1] = '\0';

	      interpret_pmstatus (line);
	    }
	}
      exit (0);
    }
  else
    {
      close (fds[0]);
      status_fd = fds[1];
    }
}

static void
fs_setup (const char *tmpfs)
{
  /* We should mount /home partition to proper place, because
   * 3rd party apps probably using /home/opt */
  int retval = run_system (false, "/bin/mount /home");

  if (retval == 0) /* success */
    {
      /* Now we should bind mount /opt. It is not a symlink
       * anymore. Next stop: HAM will be the kernel. :S */
      run_system (false, "/bin/mount -o bind /home/opt /opt");
    }

  /* Remount the root directiory with higher compression */
  rootfs_set_compression_level (true);

  /* Also we should mount the docs fs (nasty-ugly hack for
     rootfs spaces exhaustion )*/
  maybe_bindmount_docsfs (tmpfs);
}

static void
fs_teardown (const char *tmpfs)
{
  run_system (false, "/bin/umount /opt");
  run_system (false, "/bin/umount /home");

  maybe_bindumount_docsfs (tmpfs);

  rootfs_set_compression_level (false);
}

static void
do_rescue (const char *package, const char *download_root,
	   bool erase_record)
{
  int result = rescode_failure;
  xexp *result_xexp = NULL;
  gchar *result_text = NULL;

  show_fb_text (0, "Installing software update.");
  show_fb_text (1, "Please do not interrupt.");

  fork_progress_process ();

  fprintf (stderr, "Installing %s\n", package);

  /* This is just to clean the dpkg journal.  We let libapt-pkg
     configure the rest of the packages since we will get better
     progress reporting that way.
   */
  run_system (true, "dpkg --configure dpkg");

  /* Look for the best tempfs */
  const char *tmpfs = choose_tmpfs_for_docs ();

  fs_setup (tmpfs);

  misc_init ();

  // @todo Is this really necessary?
  AptWorkerCache::GetCurrent ()->init_cache_after_request = false;
  if (ensure_cache (false))
    {
      if (mark_named_package_for_install (package))
	{
	  result = rescue_operation_with_dir (download_root);

	  if (result != rescode_success)
	    result = rescue_with_all_devs ();

	  if (result != rescode_success)
	    result = rescue_with_all_devnodes ();

	  /* If we get this far, we have done everything we can.  If
	     the rescue operation is interrupted before getting here,
	     we try again on the next boot.  This might lead to a
	     reboot loop, but the device is borked anyway and the user
	     should reflash it.
	   */
	  if (erase_record)
	    erase_operation_record ();

	  if (result == rescode_packages_not_found)
            {
              fs_teardown (tmpfs);
            }
          else
            {
              if (result != rescode_success)
                {
                  _system->UnLock();
                  run_system (true, "dpkg --configure -a --force-all");
                  _system->Lock();
                }

              fs_teardown (tmpfs);

              cmd_reboot ();
            }
	}
      else
	fprintf (stderr, "Package %s not found\n", package);
    }
  else
    fprintf (stderr, "Failed to initialize package cache\n");

  /* Write result file */
  result_text = g_strdup_printf ("%d", (result == rescode_success));
  result_xexp = xexp_text_new ("success", result_text);
  g_free (result_text);

  xexp_write_file (RESCUE_RESULT_FILE, result_xexp);
  xexp_free (result_xexp);
}

int
cmdline_rescue (char **argv)
{
  if (argv[1] == NULL)
    {
      xexp *record = read_operation_record ();

      if (record == NULL)
	{
	  fprintf (stderr, "apt-worker: Nothing to rescue.\n");
	  return 0;
	}

      const char *package = xexp_aref_text (record, "package");
      const char *download_root = xexp_aref_text (record, "download-root");

      do_rescue (package, download_root, true);
    }
  else
    do_rescue (argv[1], argv[2], false);

  return 0;
}

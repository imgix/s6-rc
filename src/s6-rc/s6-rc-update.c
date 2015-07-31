/* ISC license. */

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <skalibs/uint32.h>
#include <skalibs/uint.h>
#include <skalibs/bytestr.h>
#include <skalibs/sgetopt.h>
#include <skalibs/strerr2.h>
#include <skalibs/bitarray.h>
#include <skalibs/cdb.h>
#include <skalibs/stralloc.h>
#include <skalibs/djbunix.h>
#include <skalibs/skamisc.h>
#include <skalibs/unix-transactional.h>
#include <execline/execline.h>
#include <s6/s6-supervise.h>
#include <s6-rc/config.h>
#include <s6-rc/s6rc.h>

#define USAGE "s6-rc-update [ -n ] [ -v verbosity ] [ -t timeout ] [ -l live ] newdb"
#define dieusage() strerr_dieusage(100, USAGE)
#define dienomem() strerr_diefu1sys(111, "build string") ;

static char const *live = S6RC_LIVE_BASE ;
static unsigned int verbosity = 1 ;

static int safe_servicedir_update (char const *dst, char const *src, int h)
{
  unsigned int dstlen = str_len(dst) ;
  unsigned int srclen = str_len(src) ;
  int fd ;
  int hasdata = 1, hasenv = 1, ok = 1 ;
  char dstfn[dstlen + 15 + sizeof(S6_SUPERVISE_CTLDIR)] ;
  char srcfn[srclen + 17] ;
  char tmpfn[dstlen + 21] ;
  byte_copy(dstfn, dstlen, dst) ;
  byte_copy(srcfn, srclen, src) ;

  byte_copy(dstfn + dstlen, 6, "/down") ;
  fd = open_trunc(dstfn) ;
  if (fd < 0) strerr_warnwu2sys("touch ", dstfn) ;
  else close(fd) ;
  byte_copy(dstfn + dstlen + 1, 5, "data") ;
  byte_copy(tmpfn, dstlen + 5, dstfn) ;
  byte_copy(tmpfn + dstlen + 5, 5, ".old") ;
  if (rename(dstfn, tmpfn) < 0)
  {
    if (errno == ENOENT) hasdata = 0 ;
    else goto err ;
  }
  byte_copy(dstfn + dstlen + 1, 4, "env") ;
  byte_copy(tmpfn + dstlen + 1, 8, "env.old") ;
  if (rename(dstfn, tmpfn) < 0)
  {
    if (errno == ENOENT) hasenv = 0 ;
    else goto err ;
  }
  byte_copy(dstfn + dstlen + 1, 9, "nosetsid") ;
  if (unlink(dstfn) < 0 && errno != ENOENT) goto err ;
  byte_copy(dstfn + dstlen + 3, 14, "tification-fd") ;
  if (unlink(dstfn) < 0 && errno != ENOENT) goto err ;

  byte_copy(srcfn + srclen, 17, "/notification-fd") ;
  hiercopy(srcfn, dstfn) ;
  byte_copy(srcfn + srclen + 3, 7, "setsid") ;
  byte_copy(dstfn + dstlen + 3, 7, "setsid") ;
  hiercopy(srcfn, dstfn) ;
  byte_copy(srcfn + srclen + 1, 5, "data") ;
  byte_copy(dstfn + dstlen + 1, 5, "data") ;
  hiercopy(srcfn, dstfn) ;
  byte_copy(srcfn + srclen + 1, 4, "env") ;
  byte_copy(dstfn + dstlen + 1, 4, "env") ;
  hiercopy(srcfn, dstfn) ;
  byte_copy(srcfn + srclen + 1, 4, "run") ;
  byte_copy(dstfn + dstlen + 1, 4, "run") ;
  if (!hiercopy(srcfn, dstfn)) goto err ;
  byte_copy(srcfn + srclen + 1, 4, "run") ;
  byte_copy(dstfn + dstlen + 1, 4, "finish") ;
  hiercopy(srcfn, dstfn) ;
  if (h)
  {
    byte_copy(dstfn + dstlen + 1, 5, "down") ;
    if (unlink(dstfn) < 0)
    {
      strerr_warnwu2sys("unlink ", dstfn) ;
      ok = 0 ;
    }
    byte_copy(dstfn + dstlen + 1, 8 + sizeof(S6_SUPERVISE_CTLDIR), S6_SUPERVISE_CTLDIR "/control") ;
    s6_svc_write(dstfn, "u", 1) ;
  }
  byte_copy(dstfn + dstlen + 1, 9, "data.old") ;
  if (rm_rf(dstfn) < 0) strerr_warnwu2sys("remove ", dstfn) ;
  byte_copy(dstfn + dstlen + 1, 8, "env.old") ;
  if (rm_rf(dstfn) < 0) strerr_warnwu2sys("remove ", dstfn) ;
  return 1 ;

 err:
  if (h)
  {
    int e = errno ;
    byte_copy(dstfn + dstlen + 1, 5, "down") ;
    unlink(dstfn) ;
    errno = e ;
  }
  return 0 ;
}

static int servicedir_name_change (char const *live, char const *oldname, char const *newname)
{
  unsigned int livelen = str_len(live) ;
  unsigned int oldlen = str_len(oldname) ;
  unsigned int newlen = str_len(oldname) ;
  char oldfn[livelen + oldlen + 2] ;
  char newfn[livelen + newlen + 2] ;
  byte_copy(oldfn, livelen, live) ;
  oldfn[livelen] = '/' ;
  byte_copy(oldfn + livelen + 1, oldlen + 1, oldname) ;
  byte_copy(newfn, livelen + 1, oldfn) ;
  byte_copy(newfn + livelen + 1, newlen + 1, newname) ;
  if (rename(oldfn, newfn) < 0) return 0 ;
  return 1 ;
} 

static inline void parse_line (stralloc *sa, char const *s, unsigned int slen, unsigned int *newnames, unsigned char *oldstate, cdb_t *oldc, s6rc_db_t const *olddb)
{
  unsigned int base = sa->len ;
  unsigned int oldn = olddb->nshort + olddb->nlong ;
  unsigned int max ;
  register int r ;
  if (!stralloc_readyplus(sa, slen)) dienomem() ;
  sa->len += slen ;
  r = el_parse_from_string(sa, s) ;
  switch (r)
  {
    case -1 : dienomem() ;
    case -2 : strerr_dief2x(100, "syntax error in conversion file: ", s) ; 
    case -3 : strerr_dief2x(100, "mismatched braces in conversion file: ", s) ; 
    default : break ;
  }
  max = sa->len ;
  sa->len = base ;
  if (r >= 2)
  {
    char pack[4] ;
    uint32 x ;
    unsigned int cur ;
    int r = cdb_find(oldc, sa->s + base + slen, str_len(sa->s + base + slen)) ;
    if (r < 0) strerr_diefu3sys(111, "read ", live, "/compiled/resolve.cdb") ;
    if (!r) strerr_dief5x(3, "unknown identifier in ", live, "/compiled/resolve.cdb", ": ", sa->s + base + slen) ;
    if (cdb_datalen(oldc) != 4) strerr_dief5x(5, "identifier ", sa->s + base + slen, " does not represent an atomic service in ", live, "/compiled") ;
    if (cdb_read(oldc, pack, 4, cdb_datapos(oldc)) < 0)
      strerr_diefu3sys(111, "read ", live, "/compiled/resolve.cdb") ;
    uint32_unpack_big(pack, &x) ;
    if (x >= oldn) strerr_dief3x(4, "invalid database in ", live, "/compiled") ;
    cur = base + slen + str_len(sa->s + base + slen) ;
    
  }
}

static inline void parse_conversion_file (stralloc *sa, unsigned int *newnames, unsigned char *oldstate, int fdoldc, s6rc_db_t const *olddb)
{
  unsigned int base = satmp.len ;
  cdb_t oldc = CDB_ZERO ;
  int oldfdres = open_readatb(fdoldc, "resolve.cdb") ;
  if (oldfdres < 0) strerr_diefu3sys(111, "open ", live, "/compiled/resolve.cdb") ;
  if (!cdb_init_map(&oldc, oldfdres, 1))
    strerr_diefu3sys(111, "cdb_init ", live, "/compiled/resolve.cdb") ;

  for (;;)
  {
    register int r = skagetln(buffer_0, &satmp, '\n') ;
    if (!r) break ;
    if (r < 0)
    {
      if (errno != EPIPE) strerr_diefu1sys(111, "read from stdin") ;
      if (!stralloc_0(&satmp)) dienomem() ;
    }
    else satmp.s[satmp.len - 1] = 0 ;
    parse_line(sa, satmp.s + base, satmp.len - base, newnames, oldstate, &oldc, olddb) ;
    satmp.len = base ;
  }

  satmp.len = base ;
  cdb_free(&oldc) ;
  close(oldfdres) ;
}

static inline void fill_convtable_and_flags (unsigned char *conversion_table, unsigned char *oldstate, unsigned char *newstate, char const *namedata, unsigned int const *nameindex, int fdnewc, char const *newfn, s6rc_db_t const *olddb, s6rc_db_t const *newdb)
{
  unsigned int newn = newdb->nshort + newdb->nlong ;
  cdb_t newc = CDB_ZERO ;
  int newfdres = open_readatb(fdnewc, "resolve.cdb") ;
  unsigned int i = olddb->nshort + olddb->nlong ;
  if (newfdres < 0) strerr_diefu3sys(111, "open ", newfn, "/resolve.cdb") ;
  if (!cdb_init_map(&newc, newfdres, 1))
    strerr_diefu3sys(111, "cdb_init ", newfn, "/resolve.cdb") ;

  while (i--)
  {
    char const *newname = oldstate[i] & 16 ? namedata + nameindex[i] : olddb->string + olddb->services[i].name ;
    unsigned int len ;
    int r = cdb_find(&newc, newname, str_len(newname)) ;
    if (r < 0) strerr_diefu3sys(111, "read ", newfn, "/resolve.cdb") ;
    if (!r)
    {
      oldstate[i] |= 2 ; /* disappeared -> want down */
      continue ;
    }
    if (cdb_datalen(&newc) & 3)
      strerr_dief3x(4, "invalid resolve database in ", newfn, "/resolve.cdb") ;
    len = cdb_datalen(&newc) >> 2 ;
    if (len > newn)
      strerr_dief3x(4, "invalid resolve database in ", newfn, "/resolve.cdb") ;
    {
      char pack[cdb_datalen(&newc) + 1] ;
      char const *p = pack ;
      if (cdb_read(&newc, pack, cdb_datalen(&newc), cdb_datapos(&newc)) < 0)
        strerr_diefu3sys(111, "read ", newfn, "/resolve.cdb") ;
      if (len == 1) oldstate[i] |= 8 ; /* atomic or singleton bundle */
      while (len--)
      {
        uint32 x ;
        uint32_unpack_big(p, &x) ; p += 4 ;
        if (x >= newn)
          strerr_dief3x(4, "invalid resolve database in ", newfn, "/resolve.cdb") ;
        if (newstate[x] & 4)
          strerr_dief4x(1, "bad conversion file: new service ", newdb->string + newdb->services[x].name, " is a target for more than one conversion, including old service ", olddb->string + olddb->services[i].name) ;
        newstate[x] |= 4 ;
        bitarray_set(conversion_table + i * bitarray_div8(newn), x) ;
      }
    }
  }

  cdb_free(&newc) ;
  close(newfdres) ;
}

static inline void adjust_newwantup (unsigned char *oldstate, unsigned int oldn, unsigned char *newstate, unsigned int newn, unsigned char const *conversion_table)
{
  unsigned int i = oldn ;
  while (i--) if (oldstate[i] & 1)
  {
    register unsigned int j = newn ;
    if (!(oldstate[i] & 8) || (oldstate[i] & 4)) oldstate[i] |= 34 ;
    while (j--) if (bitarray_peek(conversion_table + i * bitarray_div8(newn), j)) newstate[j] |= 3 ;
  }
}

static inline void adjust_newalreadyup (unsigned char const *oldstate, unsigned int oldn, unsigned char *newstate, unsigned int newn, unsigned char const *conversion_table)
{
  unsigned int i = oldn ;
  while (i--) if (oldstate[i] & 32)
  {
    register unsigned int j = newn ;
    while (j--) if (bitarray_peek(conversion_table + i * bitarray_div8(newn), j))
    {
      newstate[j] &= 254 ;
      newstate[j] |= 2 ;
    }
  }
}

static void compute_transitions (unsigned char *oldstate, int fdoldc, s6rc_db_t const *olddb, unsigned char *newstate, int fdnewc, char const *newfn, s6rc_db_t const *newdb)
{
  unsigned int oldn = olddb->nshort + olddb->nlong ;
  unsigned int newn = newdb->nshort + newdb->nlong ;
  unsigned char conversion_table[oldn * bitarray_div8(newn)] ;
  byte_zero(conversion_table, oldn * bitarray_div8(newn)) ;
  {
    stralloc namedata = STRALLOC_ZERO ;
    unsigned int nameindex[oldn] ;
    parse_conversion_file(&namedata, nameindex, oldstate, fdoldc, olddb) ;
    fill_convtable_and_flags(conversion_table, oldstate, newstate, namedata.s, nameindex, fdnewc, newfn, olddb, newdb) ;
    stralloc_free(&namedata) ;
  }
  adjust_newwantup(oldstate, oldn, newstate, newn, conversion_table) ;
  s6rc_graph_closure(olddb, oldstate, 5, 0) ;
  adjust_newalreadyup(oldstate, oldn, newstate, newn, conversion_table) ;
}

static unsigned int want_count (unsigned char const *state, unsigned int n)
{
  unsigned int count = 0, i = n ;
  while (i--) if (state[i] & 2) count++ ;
  return count ;
}

static void fill_tfmt (char *tfmt, tain_t const *deadline)
{
  int t ;
  tain_t tto ;
  tain_sub(&tto, deadline, &STAMP) ;
  t = tain_to_millisecs(&tto) ;
  if (!t) t = 1 ;
  else if (t < 0) t = 0 ;
  tfmt[uint_fmt(tfmt, t)] = 0 ;
}

int main (int argc, char const *const *argv, char const *const *envp)
{
  tain_t deadline ;
  char const *live = S6RC_LIVE_BASE ;
  unsigned int livelen ;
  int dryrun = 0 ;
  PROG = "s6-rc-update" ;
  strerr_dief1x(100, "this utility has not been written yet.") ;
  {
    unsigned int t = 0 ;
    subgetopt_t l = SUBGETOPT_ZERO ;
    for (;;)
    {
      register int opt = subgetopt_r(argc, argv, "v:t:nl:", &l) ;
      if (opt == -1) break ;
      switch (opt)
      {
        case 'v' : if (!uint0_scan(l.arg, &verbosity)) dieusage() ; break ;
        case 't' : if (!uint0_scan(l.arg, &t)) dieusage() ; break ;
        case 'n' : dryrun = 1 ; break ;
        case 'l' : live = l.arg ; break ;
        default : dieusage() ;
      }
    }
    argc -= l.ind ; argv += l.ind ;
    if (t) tain_from_millisecs(&deadline, t) ;
    else deadline = tain_infinite_relative ;
  }
  if (argc < 2) dieusage() ;

  livelen = str_len(live) ;

  {
    int livelock ;
    int fdoldc, fdnewc ;
    s6rc_db_t olddb, newdb ;
    unsigned int oldn, oldm, newn, newm ;
    char dbfn[livelen + 10] ;

    if (!tain_now_g())
      strerr_warnwu1x("get correct TAI time. (Do you have a valid leap seconds file?)") ;
    tain_add_g(&deadline, &deadline) ;


   /* Take the live lock */

    byte_copy(dbfn, livelen, live) ;
    byte_copy(dbfn + livelen, 6, "/lock") ;
    livelock = open_write(dbfn) ;
    if (livelock < 0) strerr_diefu2sys(111, "open ", dbfn) ;
    if (coe(livelock) < 0) strerr_diefu2sys(111, "coe ", dbfn) ;
    if (lock_ex(livelock) < 0) strerr_diefu2sys(111, "lock ", dbfn) ;


   /* Read the sizes of the compiled dbs */

    byte_copy(dbfn + livelen + 1, 9, "compiled") ;
    fdoldc = open_readb(dbfn) ;
    if (!s6rc_db_read_sizes(fdoldc, &olddb))
      strerr_diefu3sys(111, "read ", dbfn, "/n") ;
    oldn = olddb.nshort + olddb.nlong ;
    oldm = bitarray_div8(oldn) ;
    fdnewc = open_readb(argv[0]) ;
    if (!s6rc_db_read_sizes(fdnewc, &newdb))
      strerr_diefu3sys(111, "read ", argv[0], "/n") ;
    newn = newdb.nshort + newdb.nlong ;
    newm = bitarray_div8(newn) ;


   /* Allocate enough stack for the dbs */

    {
      pid_t pid ;
      s6rc_service_t oldserviceblob[oldn] ;
      char const *oldargvblob[olddb.nargvs] ;
      uint32 olddepsblob[olddb.ndeps << 1] ;
      char oldstringblob[olddb.stringlen] ;
      s6rc_service_t newserviceblob[newn] ;
      char const *newargvblob[newdb.nargvs] ;
      uint32 newdepsblob[newdb.ndeps << 1] ;
      char newstringblob[newdb.stringlen] ;
      unsigned char oldstate[oldn] ;
      unsigned char newstate[newn] ;
      register int r ;

      olddb.services = oldserviceblob ;
      olddb.argvs = oldargvblob ;
      olddb.deps = olddepsblob ;
      olddb.string = oldstringblob ;
      newdb.services = newserviceblob ;
      newdb.argvs = newargvblob ;
      newdb.deps = newdepsblob ;
      newdb.string = newstringblob ;


     /* Read the dbs */

      r = s6rc_db_read(fdoldc, &olddb) ;
      if (r < 0) strerr_diefu3sys(111, "read ", dbfn, "/db") ;
      if (!r) strerr_dief3x(4, "invalid service database in ", dbfn, "/db") ;
      r = s6rc_db_read(fdnewc, &newdb) ;
      if (r < 0) strerr_diefu3sys(111, "read ", argv[0], "/db") ;
      if (!r) strerr_dief3x(4, "invalid service database in ", argv[0], "/db") ;


     /* Initial state */

      byte_zero(newstate, newn) ;
      byte_copy(dbfn + livelen + 1, 6, "state") ;
      {
        r = openreadnclose(dbfn, (char *)oldstate, oldn) ;
        if (r != oldn) strerr_diefu2sys(111, "read ", dbfn) ;
        {
          register unsigned int i = oldn ;
          while (i--) oldstate[i] &= 1 ;
        }
      }


     /* Read the conversion file and compute what to do */

      if (verbosity >= 2) strerr_warni1x("computing state adjustments") ;
      compute_transitions(oldstate, fdoldc, &olddb, newstate, fdnewc, argv[0], &newdb) ;
      tain_now_g() ;
      if (!tain_future(&deadline)) strerr_dief1x(1, "timed out") ;


     /* Down transition */

      {
        char const *newargv[12 + (dryrun * 5) + want_count(oldstate, oldn)] ;
        unsigned int m = 0, i = oldn ;
        int wstat ;
        char vfmt[UINT_FMT] ;
        char tfmt[UINT_FMT] ;
        vfmt[uint_fmt(vfmt, verbosity)] = 0 ;
        fill_tfmt(tfmt, &deadline) ;
        if (dryrun)
        {
          newargv[m++] = S6RC_BINPREFIX "s6-rc-dryrun" ;
          newargv[m++] = "-v" ;
          newargv[m++] = vfmt ;
          newargv[m++] = "-t0" ;
          newargv[m++] = "--" ;
        }
        newargv[m++] = S6RC_BINPREFIX "s6-rc" ;
        newargv[m++] = "-v" ;
        newargv[m++] = vfmt ;
        newargv[m++] = "-t" ;
        newargv[m++] = tfmt ;
        newargv[m++] = "-l" ;
        newargv[m++] = live ;
        newargv[m++] = "-X" ;
        newargv[m++] = "-d" ;
        newargv[m++] = "--" ;
        newargv[m++] = "change" ;
        while (i--) if (oldstate[i] & 2)
          newargv[m++] = olddb.string + olddb.services[i].name ;
        newargv[m++] = 0 ;
        if (verbosity >= 2)
          strerr_warni1x("stopping services in the old database") ;
        pid = child_spawn0(newargv[0], newargv, envp) ;
        if (!pid) strerr_diefu2sys(111, "spawn ", newargv[0]) ;
        if (wait_pid(pid, &wstat) < 0) strerr_diefu1sys(111, "waitpid") ;
        if (WIFSIGNALED(wstat) || WEXITSTATUS(wstat))
          strerr_diefu1sys(wait_estatus(wstat), "first s6-rc invocation failed") ;
      }


     /* Service directory and state switch */

      if (verbosity >= 2)
        strerr_warni1x("updating state and service directories") ;


     /* Up transition */

      if (!tain_future(&deadline)) strerr_dief1x(1, "timed out") ;

      {
        char const *newargv[12 + (dryrun * 5) + want_count(newstate, newn)] ;
        unsigned int m = 0, i = newn ;
        char vfmt[UINT_FMT] ;
        char tfmt[UINT_FMT] ;
        vfmt[uint_fmt(vfmt, verbosity)] = 0 ;
        fill_tfmt(tfmt, &deadline) ;
        if (dryrun)
        {
          newargv[m++] = S6RC_BINPREFIX "s6-rc-dryrun" ;
          newargv[m++] = "-v" ;
          newargv[m++] = vfmt ;
          newargv[m++] = "-t0" ;
          newargv[m++] = "--" ;
        }
        newargv[m++] = S6RC_BINPREFIX "s6-rc" ;
        newargv[m++] = "-v" ;
        newargv[m++] = vfmt ;
        newargv[m++] = "-t" ;
        newargv[m++] = tfmt ;
        newargv[m++] = "-l" ;
        newargv[m++] = live ;
        newargv[m++] = "-aX" ;
        newargv[m++] = "-u" ;
        newargv[m++] = "--" ;
        newargv[m++] = "change" ;
        while (i--) if (newstate[i] & 2)
          newargv[m++] = newdb.string + newdb.services[i].name ;
        newargv[m++] = 0 ;
        if (verbosity >= 2)
          strerr_warni1x("starting services in the new database") ;
        pathexec_run(newargv[0], newargv, envp) ;
        strerr_dieexec(111, newargv[0]) ;
      }
    }
  }
}

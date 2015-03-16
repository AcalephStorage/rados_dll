#ifndef CEPH_CLIENT_FH_H
#define CEPH_CLIENT_FH_H

#include "common/Readahead.h"
#include "include/types.h"

struct Inode;
class Cond;
class ceph_lock_state_t;

// file handle for any open file state

struct Fh {
  Inode    *inode;
  loff_t    pos;
  int       mds;        // have to talk to mds we opened with (for now)
  int       mode;       // the mode i opened the file with

  int flags;
  bool pos_locked;           // pos is currently in use
  list<Cond*> pos_waiters;   // waiters for pos

  Readahead readahead;

  // file lock
  ceph_lock_state_t *fcntl_locks;
  ceph_lock_state_t *flock_locks;

  Fh() : inode(0), pos(0), mds(0), mode(0), flags(0), pos_locked(false),
      readahead(), fcntl_locks(NULL), flock_locks(NULL) {}
};


#endif

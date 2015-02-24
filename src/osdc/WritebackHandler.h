// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
#ifndef CEPH_OSDC_WRITEBACKHANDLER_H
#define CEPH_OSDC_WRITEBACKHANDLER_H

#include "include/Context.h"
#include "include/types.h"
#include "osd/osd_types.h"

class WritebackHandler {
 public:
  WritebackHandler() {}
  virtual ~WritebackHandler() {}

  virtual void read(const object_t& oid, uint64_t object_no,
		    const object_locator_t& oloc, uint64_t off, uint64_t len,
		    snapid_t snapid, bufferlist *pbl, uint64_t trunc_size,
		    __u32 trunc_seq, Context *onfinish) = 0;

  /**
   * check if a given extent read result may change due to a write
   *
   * Check if the content we see at the given read offset may change due to a write to
   * this object.
   *
   * @param oid object
   * @param read_off read offset
   * @param read_len read length
   * @param snapid read snapid
   */
  virtual bool may_copy_on_write(const object_t& oid, uint64_t read_off, uint64_t read_len, snapid_t snapid) = 0;
  virtual ceph_tid_t write(const object_t& oid, const object_locator_t& oloc,
			   uint64_t off, uint64_t len, const SnapContext& snapc,
			   const bufferlist &bl, utime_t mtime,
			   uint64_t trunc_size, __u32 trunc_seq,
			   Context *oncommit) = 0;
  virtual ceph_tid_t lock(const object_t& oid, const object_locator_t& oloc,
			  int op, int flags, Context *onack, Context *oncommit) {
    assert(0 == "this WritebackHandler does not support the lock operation");
  }
};

#endif

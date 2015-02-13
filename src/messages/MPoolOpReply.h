// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#ifndef CEPH_MPOOLOPREPLY_H
#define CEPH_MPOOLOPREPLY_H

#include "common/errno.h"

class MPoolOpReply : public PaxosServiceMessage {
public:
  uuid_d fsid;
  __u32 replyCode;
  epoch_t epoch;
  bufferlist response_data;

  MPoolOpReply() : PaxosServiceMessage(CEPH_MSG_POOLOP_REPLY, 0)
  {}
  MPoolOpReply( uuid_d& f, ceph_tid_t t, int rc, int e, version_t v) :
    PaxosServiceMessage(CEPH_MSG_POOLOP_REPLY, v),
    fsid(f),
    replyCode(rc),
    epoch(e) {
    set_tid(t);
  }
  MPoolOpReply( uuid_d& f, ceph_tid_t t, int rc, int e, version_t v,
		bufferlist *blp) :
    PaxosServiceMessage(CEPH_MSG_POOLOP_REPLY, v),
    fsid(f),
    replyCode(rc),
    epoch(e) {
    set_tid(t);
    if (blp)
      response_data.claim(*blp);
  }

  const char *get_type_name() const { return "poolopreply"; }

  void print(ostream& out) const {
    out << "pool_op_reply(tid " << get_tid()
	<< " " << cpp_strerror(-replyCode)
	<< " v" << version << ")";
  }

  void encode_payload(uint64_t features) {
    paxos_encode();
    ::encode(fsid, payload);
    ::encode(replyCode, payload);
    ::encode(epoch, payload);
    if (response_data.length()) {
      ::encode(true, payload);
      ::encode(response_data, payload);
    } else
      ::encode(false, payload);
  }
  void decode_payload() {
    bufferlist::iterator p = payload.begin();
    paxos_decode(p);
    ::decode(fsid, p);
    ::decode(replyCode, p);
    ::decode(epoch, p);
    bool has_response_data;
    ::decode(has_response_data, p);
    if (has_response_data) {
      ::decode(response_data, p);
    }
  }
};

#endif

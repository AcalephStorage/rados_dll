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

#ifndef CEPH_MEXPORTDIRDISCOVERACK_H
#define CEPH_MEXPORTDIRDISCOVERACK_H

#include "msg/Message.h"
#include "include/types.h"

class MExportDirDiscoverAck : public Message {
  dirfrag_t dirfrag;
  bool success;

 public:
  inodeno_t get_ino() { return dirfrag.ino; }
  dirfrag_t get_dirfrag() { return dirfrag; }
  bool is_success() { return success; }

  MExportDirDiscoverAck() : Message(MSG_MDS_EXPORTDIRDISCOVERACK) {}
  MExportDirDiscoverAck(dirfrag_t df, uint64_t tid, bool s=true) :
    Message(MSG_MDS_EXPORTDIRDISCOVERACK),
    dirfrag(df), success(s) {
    set_tid(tid);
  }
private:
  ~MExportDirDiscoverAck() {}

public:
  const char *get_type_name() const { return "ExDisA"; }
  void print(ostream& o) const {
    o << "export_discover_ack(" << dirfrag;
    if (success) 
      o << " success)";
    else
      o << " failure)";
  }

  void decode_payload() {
    bufferlist::iterator p = payload.begin();
    ::decode(dirfrag, p);
    ::decode(success, p);
  }
  void encode_payload(uint64_t features) {
    ::encode(dirfrag, payload);
    ::encode(success, payload);
  }
};

#endif

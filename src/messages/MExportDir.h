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


#ifndef CEPH_MEXPORTDIR_H
#define CEPH_MEXPORTDIR_H

#include "msg/Message.h"


class MExportDir : public Message {
 public:  
  dirfrag_t dirfrag;
  bufferlist export_data;
  vector<dirfrag_t> bounds;
  bufferlist client_map;

  MExportDir() : Message(MSG_MDS_EXPORTDIR) {}
  MExportDir(dirfrag_t df, uint64_t tid) :
    Message(MSG_MDS_EXPORTDIR), dirfrag(df) {
    set_tid(tid);
  }
private:
  ~MExportDir() {}

public:
  const char *get_type_name() const { return "Ex"; }
  void print(ostream& o) const {
    o << "export(" << dirfrag << ")";
  }

  void add_export(dirfrag_t df) { 
    bounds.push_back(df); 
  }

  void encode_payload(uint64_t features) {
    ::encode(dirfrag, payload);
    ::encode(bounds, payload);
    ::encode(export_data, payload);
    ::encode(client_map, payload);
  }
  void decode_payload() {
    bufferlist::iterator p = payload.begin();
    ::decode(dirfrag, p);
    ::decode(bounds, p);
    ::decode(export_data, p);
    ::decode(client_map, p);
  }

};

#endif

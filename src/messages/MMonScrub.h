// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
* Ceph - scalable distributed file system
*
* Copyright (C) 2013 Inktank, Inc.
*
* This is free software; you can redistribute it and/or
* modify it under the terms of the GNU Lesser General Public
* License version 2.1, as published by the Free Software
* Foundation. See file COPYING.
*/
#ifndef CEPH_MMONSCRUB_H
#define CEPH_MMONSCRUB_H

#include "msg/Message.h"
#include "mon/mon_types.h"

class MMonScrub : public Message
{
  static const int HEAD_VERSION = 1;
  static const int COMPAT_VERSION = 1;

public:
  typedef enum {
    OP_SCRUB = 1,         // leader->peon: scrub (a range of) keys
    OP_RESULT = 2,        // peon->leader: result of a scrub
  } op_type_t;

  static const char *get_opname(op_type_t op) {
    switch (op) {
    case OP_SCRUB: return "scrub";
    case OP_RESULT: return "result";
    default: assert(0 == "unknown op type"); return NULL;
    }
  }

  op_type_t op;
  version_t version;
  ScrubResult result;

  MMonScrub()
    : Message(MSG_MON_SCRUB, HEAD_VERSION, COMPAT_VERSION)
  { }

  MMonScrub(op_type_t op, version_t v)
    : Message(MSG_MON_SCRUB, HEAD_VERSION, COMPAT_VERSION),
      op(op), version(v)
  { }

  const char *get_type_name() const { return "mon_scrub"; }

  void print(ostream& out) const {
    out << "mon_scrub(" << get_opname((op_type_t)op);
    out << " v " << version;
    if (op == OP_RESULT)
      out << " " << result;
    out << ")";
  }

  void encode_payload(uint64_t features) {
    uint8_t o = op;
    ::encode(o, payload);
    ::encode(version, payload);
    ::encode(result, payload);
  }

  void decode_payload() {
    bufferlist::iterator p = payload.begin();
    uint8_t o;
    ::decode(o, p);
    op = (op_type_t)o;
    ::decode(version, p);
    ::decode(result, p);
  }
};

#endif /* CEPH_MMONSCRUB_H */

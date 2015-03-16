// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_RGW_REST_SWIFT_H
#define CEPH_RGW_REST_SWIFT_H
#define TIME_BUF_SIZE 128

#include "rgw_op.h"
#include "rgw_rest.h"

class RGWGetObj_ObjStore_SWIFT : public RGWGetObj_ObjStore {
public:
  RGWGetObj_ObjStore_SWIFT() {}
  ~RGWGetObj_ObjStore_SWIFT() {}

  int send_response_data(bufferlist& bl, off_t ofs, off_t len);
};

class RGWListBuckets_ObjStore_SWIFT : public RGWListBuckets_ObjStore {
  bool need_stats;
public:
  RGWListBuckets_ObjStore_SWIFT() : need_stats(true) {}
  ~RGWListBuckets_ObjStore_SWIFT() {}

  int get_params();
  void send_response_begin(bool has_buckets);
  void send_response_data(RGWUserBuckets& buckets);
  void send_response_end();

  bool should_get_stats() { return need_stats; }
};

class RGWListBucket_ObjStore_SWIFT : public RGWListBucket_ObjStore {
  string path;
public:
  RGWListBucket_ObjStore_SWIFT() {
    default_max = 10000;
  }
  ~RGWListBucket_ObjStore_SWIFT() {}

  int get_params();
  void send_response();
};

class RGWStatAccount_ObjStore_SWIFT : public RGWStatAccount_ObjStore {
public:
  RGWStatAccount_ObjStore_SWIFT() {
  }
  ~RGWStatAccount_ObjStore_SWIFT() {}

  void send_response();
};

class RGWStatBucket_ObjStore_SWIFT : public RGWStatBucket_ObjStore {
public:
  RGWStatBucket_ObjStore_SWIFT() {}
  ~RGWStatBucket_ObjStore_SWIFT() {}

  void send_response();
};

class RGWCreateBucket_ObjStore_SWIFT : public RGWCreateBucket_ObjStore {
public:
  RGWCreateBucket_ObjStore_SWIFT() {}
  ~RGWCreateBucket_ObjStore_SWIFT() {}

  int get_params();
  void send_response();
};

class RGWDeleteBucket_ObjStore_SWIFT : public RGWDeleteBucket_ObjStore {
public:
  RGWDeleteBucket_ObjStore_SWIFT() {}
  ~RGWDeleteBucket_ObjStore_SWIFT() {}

  void send_response();
};

class RGWPutObj_ObjStore_SWIFT : public RGWPutObj_ObjStore {
public:
  RGWPutObj_ObjStore_SWIFT() {}
  ~RGWPutObj_ObjStore_SWIFT() {}

  int get_params();
  void send_response();
};

class RGWPutMetadata_ObjStore_SWIFT : public RGWPutMetadata_ObjStore {
public:
  RGWPutMetadata_ObjStore_SWIFT() {}
  ~RGWPutMetadata_ObjStore_SWIFT() {}

  int get_params();
  void send_response();
};

class RGWSetTempUrl_ObjStore_SWIFT : public RGWSetTempUrl_ObjStore {
public:
  RGWSetTempUrl_ObjStore_SWIFT() {}
  ~RGWSetTempUrl_ObjStore_SWIFT() {}

  int get_params();
  void send_response();
};

class RGWDeleteObj_ObjStore_SWIFT : public RGWDeleteObj_ObjStore {
public:
  RGWDeleteObj_ObjStore_SWIFT() {}
  ~RGWDeleteObj_ObjStore_SWIFT() {}

  void send_response();
};

class RGWCopyObj_ObjStore_SWIFT : public RGWCopyObj_ObjStore {
  bool sent_header;
public:
  RGWCopyObj_ObjStore_SWIFT() : sent_header(false) {}
  ~RGWCopyObj_ObjStore_SWIFT() {}

  int init_dest_policy();
  int get_params();
  void send_response();
  void send_partial_response(off_t ofs);
};

class RGWGetACLs_ObjStore_SWIFT : public RGWGetACLs_ObjStore {
public:
  RGWGetACLs_ObjStore_SWIFT() {}
  ~RGWGetACLs_ObjStore_SWIFT() {}

  void send_response() {}
};

class RGWPutACLs_ObjStore_SWIFT : public RGWPutACLs_ObjStore {
public:
  RGWPutACLs_ObjStore_SWIFT() : RGWPutACLs_ObjStore() {}
  virtual ~RGWPutACLs_ObjStore_SWIFT() {}

  void send_response() {}
};

class RGWOptionsCORS_ObjStore_SWIFT : public RGWOptionsCORS_ObjStore {
public:
  RGWOptionsCORS_ObjStore_SWIFT() {}
  ~RGWOptionsCORS_ObjStore_SWIFT() {}

  void send_response();
};

class RGWHandler_ObjStore_SWIFT : public RGWHandler_ObjStore {
  friend class RGWRESTMgr_SWIFT;
protected:
  virtual bool is_acl_op() {
    return false;
  }

  static int init_from_header(struct req_state *s);
public:
  RGWHandler_ObjStore_SWIFT() {}
  virtual ~RGWHandler_ObjStore_SWIFT() {}

  int validate_bucket_name(const string& bucket);

  int init(RGWRados *store, struct req_state *state, RGWClientIO *cio);
  int authorize();

  RGWAccessControlPolicy *alloc_policy() { return NULL; /* return new RGWAccessControlPolicy_SWIFT; */ }
  void free_policy(RGWAccessControlPolicy *policy) { delete policy; }
};

class RGWHandler_ObjStore_Service_SWIFT : public RGWHandler_ObjStore_SWIFT {
protected:
  RGWOp *op_get();
  RGWOp *op_head();
  RGWOp *op_post();
public:
  RGWHandler_ObjStore_Service_SWIFT() {}
  virtual ~RGWHandler_ObjStore_Service_SWIFT() {}
};

class RGWHandler_ObjStore_Bucket_SWIFT : public RGWHandler_ObjStore_SWIFT {
protected:
  bool is_obj_update_op() {
    return s->op == OP_POST;
  }

  RGWOp *get_obj_op(bool get_data);
  RGWOp *op_get();
  RGWOp *op_head();
  RGWOp *op_put();
  RGWOp *op_delete();
  RGWOp *op_post();
  RGWOp *op_options();
public:
  RGWHandler_ObjStore_Bucket_SWIFT() {}
  virtual ~RGWHandler_ObjStore_Bucket_SWIFT() {}
};

class RGWHandler_ObjStore_Obj_SWIFT : public RGWHandler_ObjStore_SWIFT {
protected:
  bool is_obj_update_op() {
    return s->op == OP_POST;
  }

  RGWOp *get_obj_op(bool get_data);
  RGWOp *op_get();
  RGWOp *op_head();
  RGWOp *op_put();
  RGWOp *op_delete();
  RGWOp *op_post();
  RGWOp *op_copy();
  RGWOp *op_options();
public:
  RGWHandler_ObjStore_Obj_SWIFT() {}
  virtual ~RGWHandler_ObjStore_Obj_SWIFT() {}
};

class RGWRESTMgr_SWIFT : public RGWRESTMgr {
public:
  RGWRESTMgr_SWIFT() {}
  virtual ~RGWRESTMgr_SWIFT() {}

  virtual RGWRESTMgr *get_resource_mgr(struct req_state *s, const string& uri) {
    return this;
  }
  virtual RGWHandler *get_handler(struct req_state *s);
};

#endif

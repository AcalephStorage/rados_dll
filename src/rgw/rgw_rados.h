// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_RGWRADOS_H
#define CEPH_RGWRADOS_H

#include "include/rados/librados.hpp"
#include "include/Context.h"
#include "common/RefCountedObj.h"
#include "common/RWLock.h"
#include "rgw_common.h"
#include "cls/rgw/cls_rgw_types.h"
#include "cls/version/cls_version_types.h"
#include "cls/log/cls_log_types.h"
#include "cls/statelog/cls_statelog_types.h"
#include "rgw_log.h"
#include "rgw_metadata.h"
#include "rgw_rest_conn.h"

class RGWWatcher;
class SafeTimer;
class ACLOwner;
class RGWGC;

/* flags for put_obj_meta() */
#define PUT_OBJ_CREATE      0x01
#define PUT_OBJ_EXCL        0x02
#define PUT_OBJ_CREATE_EXCL (PUT_OBJ_CREATE | PUT_OBJ_EXCL)

#define RGW_OBJ_NS_MULTIPART "multipart"
#define RGW_OBJ_NS_SHADOW    "shadow"

#define RGW_BUCKET_INSTANCE_MD_PREFIX ".bucket.meta."

static inline void prepend_bucket_marker(rgw_bucket& bucket, const string& orig_oid, string& oid)
{
  if (bucket.marker.empty() || orig_oid.empty()) {
    oid = orig_oid;
  } else {
    oid = bucket.marker;
    oid.append("_");
    oid.append(orig_oid);
  }
}

static inline void get_obj_bucket_and_oid_loc(const rgw_obj& obj, rgw_bucket& bucket, string& oid, string& locator)
{
  bucket = obj.bucket;
  prepend_bucket_marker(bucket, obj.get_object(), oid);
  const string& loc = obj.get_loc();
  if (!loc.empty()) {
    prepend_bucket_marker(bucket, obj.get_loc(), locator);
  } else {
    locator.clear();
  }
}

int rgw_policy_from_attrset(CephContext *cct, map<string, bufferlist>& attrset, RGWAccessControlPolicy *policy);

struct RGWOLHInfo {
  rgw_obj target;
  bool removed;

  RGWOLHInfo() : removed(false) {}

  void encode(bufferlist& bl) const {
    ENCODE_START(1, 1, bl);
    ::encode(target, bl);
    ::encode(removed, bl);
    ENCODE_FINISH(bl);
  }

  void decode(bufferlist::iterator& bl) {
     DECODE_START(1, bl);
     ::decode(target, bl);
     ::decode(removed, bl);
     DECODE_FINISH(bl);
  }

  void dump(Formatter *f) const;
};
WRITE_CLASS_ENCODER(RGWOLHInfo)

struct RGWOLHPendingInfo {
  utime_t time;

  RGWOLHPendingInfo() {}

  void encode(bufferlist& bl) const {
    ENCODE_START(1, 1, bl);
    ::encode(time, bl);
    ENCODE_FINISH(bl);
  }

  void decode(bufferlist::iterator& bl) {
     DECODE_START(1, bl);
     ::decode(time, bl);
     DECODE_FINISH(bl);
  }

  void dump(Formatter *f) const;
};
WRITE_CLASS_ENCODER(RGWOLHPendingInfo)

struct RGWUsageBatch {
  map<utime_t, rgw_usage_log_entry> m;

  void insert(utime_t& t, rgw_usage_log_entry& entry, bool *account) {
    bool exists = m.find(t) != m.end();
    *account = !exists;
    m[t].aggregate(entry);
  }
};

struct RGWUsageIter {
  string read_iter;
  uint32_t index;

  RGWUsageIter() : index(0) {}
};

class RGWGetDataCB {
protected:
  uint64_t extra_data_len;
public:
  virtual int handle_data(bufferlist& bl, off_t bl_ofs, off_t bl_len) = 0;
  RGWGetDataCB() : extra_data_len(0) {}
  virtual ~RGWGetDataCB() {}
  virtual void set_extra_data_len(uint64_t len) {
    extra_data_len = len;
  }
};

class RGWAccessListFilter {
public:
  virtual ~RGWAccessListFilter() {}
  virtual bool filter(string& name, string& key) = 0;
};

struct RGWCloneRangeInfo {
  rgw_obj src;
  off_t src_ofs;
  off_t dst_ofs;
  uint64_t len;
};

struct RGWObjManifestPart {
  rgw_obj loc;       /* the object where the data is located */
  uint64_t loc_ofs;  /* the offset at that object where the data is located */
  uint64_t size;     /* the part size */

  RGWObjManifestPart() : loc_ofs(0), size(0) {}

  void encode(bufferlist& bl) const {
    ENCODE_START(2, 2, bl);
    ::encode(loc, bl);
    ::encode(loc_ofs, bl);
    ::encode(size, bl);
    ENCODE_FINISH(bl);
  }

  void decode(bufferlist::iterator& bl) {
     DECODE_START_LEGACY_COMPAT_LEN_32(2, 2, 2, bl);
     ::decode(loc, bl);
     ::decode(loc_ofs, bl);
     ::decode(size, bl);
     DECODE_FINISH(bl);
  }

  void dump(Formatter *f) const;
  static void generate_test_instances(list<RGWObjManifestPart*>& o);
};
WRITE_CLASS_ENCODER(RGWObjManifestPart)

/*
 The manifest defines a set of rules for structuring the object parts.
 There are a few terms to note:
     - head: the head part of the object, which is the part that contains
       the first chunk of data. An object might not have a head (as in the
       case of multipart-part objects).
     - stripe: data portion of a single rgw object that resides on a single
       rados object.
     - part: a collection of stripes that make a contiguous part of an
       object. A regular object will only have one part (although might have
       many stripes), a multipart object might have many parts. Each part
       has a fixed stripe size, although the last stripe of a part might
       be smaller than that. Consecutive parts may be merged if their stripe
       value is the same.
*/

struct RGWObjManifestRule {
  uint32_t start_part_num;
  uint64_t start_ofs;
  uint64_t part_size; /* each part size, 0 if there's no part size, meaning it's unlimited */
  uint64_t stripe_max_size; /* underlying obj max size */
  string override_prefix;

  RGWObjManifestRule() : start_part_num(0), start_ofs(0), part_size(0), stripe_max_size(0) {}
  RGWObjManifestRule(uint32_t _start_part_num, uint64_t _start_ofs, uint64_t _part_size, uint64_t _stripe_max_size) :
                       start_part_num(_start_part_num), start_ofs(_start_ofs), part_size(_part_size), stripe_max_size(_stripe_max_size) {}

  void encode(bufferlist& bl) const {
    ENCODE_START(2, 1, bl);
    ::encode(start_part_num, bl);
    ::encode(start_ofs, bl);
    ::encode(part_size, bl);
    ::encode(stripe_max_size, bl);
    ::encode(override_prefix, bl);
    ENCODE_FINISH(bl);
  }

  void decode(bufferlist::iterator& bl) {
    DECODE_START(2, bl);
    ::decode(start_part_num, bl);
    ::decode(start_ofs, bl);
    ::decode(part_size, bl);
    ::decode(stripe_max_size, bl);
    if (struct_v >= 2)
      ::decode(override_prefix, bl);
    DECODE_FINISH(bl);
  }
  void dump(Formatter *f) const;
};
WRITE_CLASS_ENCODER(RGWObjManifestRule)

class RGWObjManifest {
protected:
  bool explicit_objs; /* old manifest? */
  map<uint64_t, RGWObjManifestPart> objs;

  uint64_t obj_size;

  rgw_obj head_obj;
  uint64_t head_size;

  uint64_t max_head_size;
  string prefix;
  rgw_bucket tail_bucket; /* might be different than the original bucket,
                             as object might have been copied across buckets */
  map<uint64_t, RGWObjManifestRule> rules;

  void convert_to_explicit();
  int append_explicit(RGWObjManifest& m);
  void append_rules(RGWObjManifest& m, map<uint64_t, RGWObjManifestRule>::iterator& iter, string *override_prefix);

  void update_iterators() {
    begin_iter.seek(0);
    end_iter.seek(obj_size);
  }
public:

  RGWObjManifest() : explicit_objs(false), obj_size(0), head_size(0), max_head_size(0),
                     begin_iter(this), end_iter(this) {}
  RGWObjManifest(const RGWObjManifest& rhs) {
    *this = rhs;
  }
  RGWObjManifest& operator=(const RGWObjManifest& rhs) {
    explicit_objs = rhs.explicit_objs;
    objs = rhs.objs;
    obj_size = rhs.obj_size;
    head_obj = rhs.head_obj;
    head_size = rhs.head_size;
    max_head_size = rhs.max_head_size;
    prefix = rhs.prefix;
    tail_bucket = rhs.tail_bucket;
    rules = rhs.rules;

    begin_iter.set_manifest(this);
    end_iter.set_manifest(this);

    begin_iter.seek(rhs.begin_iter.get_ofs());
    end_iter.seek(rhs.end_iter.get_ofs());

    return *this;
  }

  map<uint64_t, RGWObjManifestPart>& get_explicit_objs() {
    return objs;
  }


  void set_explicit(uint64_t _size, map<uint64_t, RGWObjManifestPart>& _objs) {
    explicit_objs = true;
    obj_size = _size;
    objs.swap(_objs);
  }

  void get_implicit_location(uint64_t cur_part_id, uint64_t cur_stripe, uint64_t ofs, string *override_prefix, rgw_obj *location);

  void set_trivial_rule(uint64_t tail_ofs, uint64_t stripe_max_size) {
    RGWObjManifestRule rule(0, tail_ofs, 0, stripe_max_size);
    rules[0] = rule;
    max_head_size = tail_ofs;
  }

  void set_multipart_part_rule(uint64_t stripe_max_size, uint64_t part_num) {
    RGWObjManifestRule rule(0, 0, 0, stripe_max_size);
    rule.start_part_num = part_num;
    rules[0] = rule;
    max_head_size = 0;
  }

  void encode(bufferlist& bl) const {
    ENCODE_START(4, 3, bl);
    ::encode(obj_size, bl);
    ::encode(objs, bl);
    ::encode(explicit_objs, bl);
    ::encode(head_obj, bl);
    ::encode(head_size, bl);
    ::encode(max_head_size, bl);
    ::encode(prefix, bl);
    ::encode(rules, bl);
    ::encode(tail_bucket, bl);
    ENCODE_FINISH(bl);
  }

  void decode(bufferlist::iterator& bl) {
    DECODE_START_LEGACY_COMPAT_LEN_32(4, 2, 2, bl);
    ::decode(obj_size, bl);
    ::decode(objs, bl);
    if (struct_v >= 3) {
      ::decode(explicit_objs, bl);
      ::decode(head_obj, bl);
      ::decode(head_size, bl);
      ::decode(max_head_size, bl);
      ::decode(prefix, bl);
      ::decode(rules, bl);
    } else {
      explicit_objs = true;
    }

    if (struct_v >= 4) {
      ::decode(tail_bucket, bl);
    }

    update_iterators();
    DECODE_FINISH(bl);
  }

  void dump(Formatter *f) const;
  static void generate_test_instances(list<RGWObjManifest*>& o);

  int append(RGWObjManifest& m);

  bool get_rule(uint64_t ofs, RGWObjManifestRule *rule);

  bool empty() {
    if (explicit_objs)
      return objs.empty();
    return rules.empty();
  }

  bool has_explicit_objs() {
    return explicit_objs;
  }

  bool has_tail() {
    if (explicit_objs) {
      if (objs.size() == 1) {
        map<uint64_t, RGWObjManifestPart>::iterator iter = objs.begin();
        rgw_obj& obj = iter->second.loc;
        return !(head_obj == obj);
      }
      return (objs.size() >= 2);
    }
    return (obj_size > head_size);
  }

  void set_head(const rgw_obj& _o) {
    head_obj = _o;
  }

  const rgw_obj& get_head() {
    return head_obj;
  }

  void set_tail_bucket(const rgw_bucket& _b) {
    tail_bucket = _b;
  }

  rgw_bucket& get_tail_bucket() {
    return tail_bucket;
  }

  void set_prefix(const string& _p) {
    prefix = _p;
  }

  const string& get_prefix() {
    return prefix;
  }

  void set_head_size(uint64_t _s) {
    head_size = _s;
  }

  void set_obj_size(uint64_t s) {
    obj_size = s;

    update_iterators();
  }

  uint64_t get_obj_size() {
    return obj_size;
  }

  uint64_t get_head_size() {
    return head_size;
  }

  void set_max_head_size(uint64_t s) {
    max_head_size = s;
  }

  uint64_t get_max_head_size() {
    return max_head_size;
  }

  class obj_iterator {
    RGWObjManifest *manifest;
    uint64_t part_ofs; /* where current part starts */
    uint64_t stripe_ofs; /* where current stripe starts */
    uint64_t ofs;       /* current position within the object */
    uint64_t stripe_size;      /* current part size */

    int cur_part_id;
    int cur_stripe;
    string cur_override_prefix;

    rgw_obj location;

    map<uint64_t, RGWObjManifestRule>::iterator rule_iter;
    map<uint64_t, RGWObjManifestRule>::iterator next_rule_iter;

    map<uint64_t, RGWObjManifestPart>::iterator explicit_iter;

    void init() {
      part_ofs = 0;
      stripe_ofs = 0;
      ofs = 0;
      stripe_size = 0;
      cur_part_id = 0;
      cur_stripe = 0;
    }

    void update_explicit_pos();


  protected:

    void set_manifest(RGWObjManifest *m) {
      manifest = m;
    }

  public:
    obj_iterator() : manifest(NULL) {
      init();
    }
    obj_iterator(RGWObjManifest *_m) : manifest(_m) {
      init();
      if (!manifest->empty()) {
        seek(0);
      }
    }
    obj_iterator(RGWObjManifest *_m, uint64_t _ofs) : manifest(_m) {
      init();
      if (!manifest->empty()) {
        seek(_ofs);
      }
    }
    void seek(uint64_t ofs);

    void operator++();
    bool operator==(const obj_iterator& rhs) {
      return (ofs == rhs.ofs);
    }
    bool operator!=(const obj_iterator& rhs) {
      return (ofs != rhs.ofs);
    }
    const rgw_obj& get_location() {
      return location;
    }

    /* start of current stripe */
    uint64_t get_stripe_ofs() {
      if (manifest->explicit_objs) {
        return explicit_iter->first;
      }
      return stripe_ofs;
    }

    /* current ofs relative to start of rgw object */
    uint64_t get_ofs() const {
      return ofs;
    }

    /* current stripe size */
    uint64_t get_stripe_size() {
      if (manifest->explicit_objs) {
        return explicit_iter->second.size;
      }
      return stripe_size;
    }

    /* offset where data starts within current stripe */
    uint64_t location_ofs() {
      if (manifest->explicit_objs) {
        return explicit_iter->second.loc_ofs;
      }
      return 0; /* all stripes start at zero offset */
    }

    void update_location();

    friend class RGWObjManifest;
  };

  const obj_iterator& obj_begin();
  const obj_iterator& obj_end();
  obj_iterator obj_find(uint64_t ofs);

  obj_iterator begin_iter;
  obj_iterator end_iter;

  /*
   * simple object generator. Using a simple single rule manifest.
   */
  class generator {
    RGWObjManifest *manifest;
    uint64_t last_ofs;
    uint64_t cur_part_ofs;
    int cur_part_id;
    int cur_stripe;
    uint64_t cur_stripe_size;
    string cur_oid;
    
    string oid_prefix;

    rgw_obj cur_obj;
    rgw_bucket bucket;


    RGWObjManifestRule rule;

  public:
    generator() : manifest(NULL), last_ofs(0), cur_part_ofs(0), cur_part_id(0), 
		  cur_stripe(0), cur_stripe_size(0) {}
    int create_begin(CephContext *cct, RGWObjManifest *manifest, rgw_bucket& bucket, rgw_obj& head);

    int create_next(uint64_t ofs);

    const rgw_obj& get_cur_obj() { return cur_obj; }

    /* total max size of current stripe (including head obj) */
    uint64_t cur_stripe_max_size() {
      return cur_stripe_size;
    }
  };
};
WRITE_CLASS_ENCODER(RGWObjManifest)

struct RGWUploadPartInfo {
  uint32_t num;
  uint64_t size;
  string etag;
  utime_t modified;
  RGWObjManifest manifest;

  RGWUploadPartInfo() : num(0), size(0) {}

  void encode(bufferlist& bl) const {
    ENCODE_START(3, 2, bl);
    ::encode(num, bl);
    ::encode(size, bl);
    ::encode(etag, bl);
    ::encode(modified, bl);
    ::encode(manifest, bl);
    ENCODE_FINISH(bl);
  }
  void decode(bufferlist::iterator& bl) {
    DECODE_START_LEGACY_COMPAT_LEN(3, 2, 2, bl);
    ::decode(num, bl);
    ::decode(size, bl);
    ::decode(etag, bl);
    ::decode(modified, bl);
    if (struct_v >= 3)
      ::decode(manifest, bl);
    DECODE_FINISH(bl);
  }
  void dump(Formatter *f) const;
  static void generate_test_instances(list<RGWUploadPartInfo*>& o);
};
WRITE_CLASS_ENCODER(RGWUploadPartInfo)

struct RGWObjState {
  rgw_obj obj;
  bool is_atomic;
  bool has_attrs;
  bool exists;
  uint64_t size;
  time_t mtime;
  uint64_t epoch;
  bufferlist obj_tag;
  string write_tag;
  bool fake_tag;
  RGWObjManifest manifest;
  bool has_manifest;
  string shadow_obj;
  bool has_data;
  bufferlist data;
  bool prefetch_data;
  bool keep_tail;
  bool is_olh;
  bufferlist olh_tag;

  /* important! don't forget to update copy constructor */

  RGWObjVersionTracker objv_tracker;

  map<string, bufferlist> attrset;
  RGWObjState() : is_atomic(false), has_attrs(0), exists(false),
                  size(0), mtime(0), epoch(0), fake_tag(false), has_manifest(false),
                  has_data(false), prefetch_data(false), keep_tail(false), is_olh(false) {}
  RGWObjState(const RGWObjState& rhs) {
    obj = rhs.obj;
    is_atomic = rhs.is_atomic;
    has_attrs = rhs.has_attrs;
    exists = rhs.exists;
    size = rhs.size;
    mtime = rhs.mtime;
    epoch = rhs.epoch;
    if (rhs.obj_tag.length()) {
      obj_tag = rhs.obj_tag;
    }
    write_tag = rhs.write_tag;
    fake_tag = rhs.fake_tag;
    if (rhs.has_manifest) {
      manifest = rhs.manifest;
    }
    has_manifest = rhs.has_manifest;
    shadow_obj = rhs.shadow_obj;
    has_data = rhs.has_data;
    if (rhs.data.length()) {
      data = rhs.data;
    }
    prefetch_data = rhs.prefetch_data;
    keep_tail = rhs.keep_tail;
    is_olh = rhs.is_olh;
    objv_tracker = rhs.objv_tracker;
  }

  bool get_attr(string name, bufferlist& dest) {
    map<string, bufferlist>::iterator iter = attrset.find(name);
    if (iter != attrset.end()) {
      dest = iter->second;
      return true;
    }
    return false;
  }

  void clear() {
    has_attrs = false;
    exists = false;
    fake_tag = false;
    epoch = 0;
    size = 0;
    mtime = 0;
    obj_tag.clear();
    shadow_obj.clear();
    attrset.clear();
    data.clear();
  }
};

struct RGWPoolIterCtx {
  librados::IoCtx io_ctx;
  librados::NObjectIterator iter;
};

struct RGWListRawObjsCtx {
  bool initialized;
  RGWPoolIterCtx iter_ctx;

  RGWListRawObjsCtx() : initialized(false) {}
};

struct RGWRegion;


struct RGWZonePlacementInfo {
  string index_pool;
  string data_pool;
  string data_extra_pool; /* if not set we should use data_pool */

  void encode(bufferlist& bl) const {
    ENCODE_START(4, 1, bl);
    ::encode(index_pool, bl);
    ::encode(data_pool, bl);
    ::encode(data_extra_pool, bl);
    ENCODE_FINISH(bl);
  }

  void decode(bufferlist::iterator& bl) {
    DECODE_START(4, bl);
    ::decode(index_pool, bl);
    ::decode(data_pool, bl);
    if (struct_v >= 4) {
      ::decode(data_extra_pool, bl);
    }
    DECODE_FINISH(bl);
  }
  const string& get_data_extra_pool() {
    if (data_extra_pool.empty()) {
      return data_pool;
    }
    return data_extra_pool;
  }
  void dump(Formatter *f) const;
  void decode_json(JSONObj *obj);
};
WRITE_CLASS_ENCODER(RGWZonePlacementInfo)

struct RGWZoneParams {
  rgw_bucket domain_root;
  rgw_bucket control_pool;
  rgw_bucket gc_pool;
  rgw_bucket log_pool;
  rgw_bucket intent_log_pool;
  rgw_bucket usage_log_pool;

  rgw_bucket user_keys_pool;
  rgw_bucket user_email_pool;
  rgw_bucket user_swift_pool;
  rgw_bucket user_uid_pool;

  string name;
  bool is_master;

  RGWAccessKey system_key;

  map<string, RGWZonePlacementInfo> placement_pools;

  RGWZoneParams() : is_master(false) {}

  static int get_pool_name(CephContext *cct, string *pool_name);
  void init_name(CephContext *cct, RGWRegion& region);
  int init(CephContext *cct, RGWRados *store, RGWRegion& region);
  void init_default(RGWRados *store);
  int store_info(CephContext *cct, RGWRados *store, RGWRegion& region);

  void encode(bufferlist& bl) const {
    ENCODE_START(4, 1, bl);
    ::encode(domain_root, bl);
    ::encode(control_pool, bl);
    ::encode(gc_pool, bl);
    ::encode(log_pool, bl);
    ::encode(intent_log_pool, bl);
    ::encode(usage_log_pool, bl);
    ::encode(user_keys_pool, bl);
    ::encode(user_email_pool, bl);
    ::encode(user_swift_pool, bl);
    ::encode(user_uid_pool, bl);
    ::encode(name, bl);
    ::encode(system_key, bl);
    ::encode(placement_pools, bl);
    ENCODE_FINISH(bl);
  }

  void decode(bufferlist::iterator& bl) {
     DECODE_START(4, bl);
    ::decode(domain_root, bl);
    ::decode(control_pool, bl);
    ::decode(gc_pool, bl);
    ::decode(log_pool, bl);
    ::decode(intent_log_pool, bl);
    ::decode(usage_log_pool, bl);
    ::decode(user_keys_pool, bl);
    ::decode(user_email_pool, bl);
    ::decode(user_swift_pool, bl);
    ::decode(user_uid_pool, bl);
    if (struct_v >= 2)
      ::decode(name, bl);
    if (struct_v >= 3)
      ::decode(system_key, bl);
    if (struct_v >= 4)
      ::decode(placement_pools, bl);
    DECODE_FINISH(bl);
  }
  void dump(Formatter *f) const;
  void decode_json(JSONObj *obj);
};
WRITE_CLASS_ENCODER(RGWZoneParams)

struct RGWZone {
  string name;
  list<string> endpoints;
  bool log_meta;
  bool log_data;

/**
 * Represents the number of shards for the bucket index object, a value of zero
 * indicates there is no sharding. By default (no sharding, the name of the object
 * is '.dir.{marker}', with sharding, the name is '.dir.{markder}.{sharding_id}',
 * sharding_id is zero-based value. It is not recommended to set a too large value
 * (e.g. thousand) as it increases the cost for bucket listing.
 */
  uint32_t bucket_index_max_shards;

  RGWZone() : log_meta(false), log_data(false), bucket_index_max_shards(0) {}

  void encode(bufferlist& bl) const {
    ENCODE_START(3, 1, bl);
    ::encode(name, bl);
    ::encode(endpoints, bl);
    ::encode(log_meta, bl);
    ::encode(log_data, bl);
    ::encode(bucket_index_max_shards, bl);
    ENCODE_FINISH(bl);
  }

  void decode(bufferlist::iterator& bl) {
    DECODE_START(2, bl);
    ::decode(name, bl);
    ::decode(endpoints, bl);
    if (struct_v >= 2) {
      ::decode(log_meta, bl);
      ::decode(log_data, bl);
    }
    if (struct_v >= 3) {
      ::decode(bucket_index_max_shards, bl);
    }
    DECODE_FINISH(bl);
  }
  void dump(Formatter *f) const;
  void decode_json(JSONObj *obj);
};
WRITE_CLASS_ENCODER(RGWZone)

struct RGWDefaultRegionInfo {
  string default_region;

  void encode(bufferlist& bl) const {
    ENCODE_START(1, 1, bl);
    ::encode(default_region, bl);
    ENCODE_FINISH(bl);
  }

  void decode(bufferlist::iterator& bl) {
    DECODE_START(1, bl);
    ::decode(default_region, bl);
    DECODE_FINISH(bl);
  }
  void dump(Formatter *f) const;
  void decode_json(JSONObj *obj);
};
WRITE_CLASS_ENCODER(RGWDefaultRegionInfo)

struct RGWRegionPlacementTarget {
  string name;
  list<string> tags;

  bool user_permitted(list<string>& user_tags) {
    if (tags.empty()) {
      return true;
    }
    for (list<string>::iterator uiter = user_tags.begin(); uiter != user_tags.end(); ++uiter) { /* we don't expect many of either, so we can handle this kind of lookup */
      string& rule = *uiter;
      for (list<string>::iterator iter = tags.begin(); iter != tags.end(); ++iter) {
        if (rule == *iter) {
          return true;
        }
      }
    }
    return false;
  }

  void encode(bufferlist& bl) const {
    ENCODE_START(1, 1, bl);
    ::encode(name, bl);
    ::encode(tags, bl);
    ENCODE_FINISH(bl);
  }

  void decode(bufferlist::iterator& bl) {
    DECODE_START(1, bl);
    ::decode(name, bl);
    ::decode(tags, bl);
    DECODE_FINISH(bl);
  }
  void dump(Formatter *f) const;
  void decode_json(JSONObj *obj);
};
WRITE_CLASS_ENCODER(RGWRegionPlacementTarget)


struct RGWRegion {
  string name;
  string api_name;
  list<string> endpoints;
  bool is_master;

  string master_zone;
  map<string, RGWZone> zones;

  map<string, RGWRegionPlacementTarget> placement_targets;
  string default_placement;

  list<string> hostnames;

  CephContext *cct;
  RGWRados *store;

  RGWRegion() : is_master(false), cct(NULL), store(NULL) {}

  void encode(bufferlist& bl) const {
    ENCODE_START(2, 1, bl);
    ::encode(name, bl);
    ::encode(api_name, bl);
    ::encode(is_master, bl);
    ::encode(endpoints, bl);
    ::encode(master_zone, bl);
    ::encode(zones, bl);
    ::encode(placement_targets, bl);
    ::encode(default_placement, bl);
    ::encode(hostnames, bl);
    ENCODE_FINISH(bl);
  }

  void decode(bufferlist::iterator& bl) {
    DECODE_START(2, bl);
    ::decode(name, bl);
    ::decode(api_name, bl);
    ::decode(is_master, bl);
    ::decode(endpoints, bl);
    ::decode(master_zone, bl);
    ::decode(zones, bl);
    ::decode(placement_targets, bl);
    ::decode(default_placement, bl);
    if (struct_v >= 2) {
      ::decode(hostnames, bl);
    }
    DECODE_FINISH(bl);
  }

  int init(CephContext *_cct, RGWRados *_store, bool setup_region = true);
  int create_default();
  int store_info(bool exclusive);
  int read_info(const string& region_name);
  int read_default(RGWDefaultRegionInfo& default_region);
  int set_as_default();
  int equals(const string& other_region);

  static int get_pool_name(CephContext *cct, string *pool_name);

  void dump(Formatter *f) const;
  void decode_json(JSONObj *obj);
};
WRITE_CLASS_ENCODER(RGWRegion)

struct RGWRegionMap {
  Mutex lock;
  map<string, RGWRegion> regions;
  map<string, RGWRegion> regions_by_api;

  string master_region;

  RGWQuotaInfo bucket_quota;
  RGWQuotaInfo user_quota;

  RGWRegionMap() : lock("RGWRegionMap") {}

  void encode(bufferlist& bl) const;
  void decode(bufferlist::iterator& bl);

  void get_params(CephContext *cct, string& pool_name, string& oid);
  int read(CephContext *cct, RGWRados *store);
  int store(CephContext *cct, RGWRados *store);

  int update(RGWRegion& region);

  void dump(Formatter *f) const;
  void decode_json(JSONObj *obj);
};
WRITE_CLASS_ENCODER(RGWRegionMap)

class RGWDataChangesLog;
class RGWReplicaLogger;
  
class RGWStateLog {
  RGWRados *store;
  int num_shards;
  string module_name;

  void oid_str(int shard, string& oid);
  int get_shard_num(const string& object);
  string get_oid(const string& object);
  int open_ioctx(librados::IoCtx& ioctx);

  struct list_state {
    int cur_shard;
    int max_shard;
    string marker;
    string client_id;
    string op_id;
    string object;

    list_state() : cur_shard(0), max_shard(0) {}
  };

protected:
  virtual bool dump_entry_internal(const cls_statelog_entry& entry, Formatter *f) {
    return false;
  }

public:
  RGWStateLog(RGWRados *_store, int _num_shards, const string& _module_name) :
              store(_store), num_shards(_num_shards), module_name(_module_name) {}
  virtual ~RGWStateLog() {}

  int store_entry(const string& client_id, const string& op_id, const string& object,
                  uint32_t state, bufferlist *bl, uint32_t *check_state);

  int remove_entry(const string& client_id, const string& op_id, const string& object);

  void init_list_entries(const string& client_id, const string& op_id, const string& object,
                         void **handle);

  int list_entries(void *handle, int max_entries, list<cls_statelog_entry>& entries, bool *done);

  void finish_list_entries(void *handle);

  virtual void dump_entry(const cls_statelog_entry& entry, Formatter *f);
};

/*
 * state transitions:
 *
 * unknown -> in-progress -> complete
 *                        -> error
 *
 * user can try setting the 'abort' state, and it can only succeed if state is
 * in-progress.
 *
 * state renewal cannot switch state (stays in the same state)
 *
 * rgw can switch from in-progress to complete
 * rgw can switch from in-progress to error
 *
 * rgw can switch from abort to cancelled
 *
 */

class RGWOpState : public RGWStateLog {
protected:
  bool dump_entry_internal(const cls_statelog_entry& entry, Formatter *f);
public:

  enum OpState {
    OPSTATE_UNKNOWN     = 0,
    OPSTATE_IN_PROGRESS = 1,
    OPSTATE_COMPLETE    = 2,
    OPSTATE_ERROR       = 3,
    OPSTATE_ABORT       = 4,
    OPSTATE_CANCELLED   = 5,
  };

  RGWOpState(RGWRados *_store);

  int state_from_str(const string& s, OpState *state);
  int set_state(const string& client_id, const string& op_id, const string& object, OpState state);
  int renew_state(const string& client_id, const string& op_id, const string& object, OpState state);
};

class RGWOpStateSingleOp
{
  RGWOpState os;
  string client_id;
  string op_id;
  string object;

  CephContext *cct;

  RGWOpState::OpState cur_state;
  utime_t last_update;

public:
  RGWOpStateSingleOp(RGWRados *store, const string& cid, const string& oid, const string& obj);

  int set_state(RGWOpState::OpState state);
  int renew_state();
};

class RGWGetBucketStats_CB : public RefCountedObject {
protected:
  rgw_bucket bucket;
  map<RGWObjCategory, RGWStorageStats> *stats;
public:
  RGWGetBucketStats_CB(rgw_bucket& _bucket) : bucket(_bucket), stats(NULL) {}
  virtual ~RGWGetBucketStats_CB() {}
  virtual void handle_response(int r) = 0;
  virtual void set_response(map<RGWObjCategory, RGWStorageStats> *_stats) {
    stats = _stats;
  }
};

class RGWGetUserStats_CB : public RefCountedObject {
protected:
  string user;
  RGWStorageStats stats;
public:
  RGWGetUserStats_CB(const string& _user) : user(_user) {}
  virtual ~RGWGetUserStats_CB() {}
  virtual void handle_response(int r) = 0;
  virtual void set_response(RGWStorageStats& _stats) {
    stats = _stats;
  }
};

class RGWGetDirHeader_CB;
class RGWGetUserHeader_CB;

struct rgw_rados_ref {
  string oid;
  string key;
  librados::IoCtx ioctx;
};

class RGWChainedCache {
public:
  virtual ~RGWChainedCache() {}
  virtual void chain_cb(const string& key, void *data) = 0;
  virtual void invalidate(const string& key) = 0;

  struct Entry {
    RGWChainedCache *cache;
    const string& key;
    void *data;

    Entry(RGWChainedCache *_c, const string& _k, void *_d) : cache(_c), key(_k), data(_d) {}
  };
};


struct RGWObjectCtx {
  RGWRados *store;
  map<rgw_obj, RGWObjState> objs_state;
  void *user_ctx;

  RGWObjectCtx(RGWRados *_store) : store(_store), user_ctx(NULL) { }
  RGWObjectCtx(RGWRados *_store, void *_user_ctx) : store(_store), user_ctx(_user_ctx) { }

  RGWObjState *get_state(rgw_obj& obj);
  void set_atomic(rgw_obj& obj);
  void set_prefetch_data(rgw_obj& obj);
  void invalidate(rgw_obj& obj);
};

class RGWRados
{
  friend class RGWGC;
  friend class RGWStateLog;
  friend class RGWReplicaLogger;

  /** Open the pool used as root for this gateway */
  int open_root_pool_ctx();
  int open_gc_pool_ctx();

  int open_bucket_pool_ctx(const string& bucket_name, const string& pool, librados::IoCtx&  io_ctx);
  int open_bucket_index_ctx(rgw_bucket& bucket, librados::IoCtx&  index_ctx);
  int open_bucket_data_ctx(rgw_bucket& bucket, librados::IoCtx&  io_ctx);
  int open_bucket_data_extra_ctx(rgw_bucket& bucket, librados::IoCtx&  io_ctx);
  int open_bucket_index(rgw_bucket& bucket, librados::IoCtx&  index_ctx, string& bucket_oid);
  int open_bucket_index_base(rgw_bucket& bucket, librados::IoCtx&  index_ctx,
      string& bucket_oid_base);
  int open_bucket_index_shard(rgw_bucket& bucket, librados::IoCtx& index_ctx,
      const string& obj_key, string *bucket_obj, int *shard_id);
  int open_bucket_index(rgw_bucket& bucket, librados::IoCtx& index_ctx,
      map<int, string>& bucket_objs, int shard_id = -1, map<int, string> *bucket_instance_ids = NULL);
  template<typename T>
  int open_bucket_index(rgw_bucket& bucket, librados::IoCtx& index_ctx,
                        map<int, string>& oids, map<int, T>& bucket_objs,
                        int shard_id = -1, map<int, string> *bucket_instance_ids = NULL);
  void build_bucket_index_marker(const string& shard_id_str, const string& shard_marker,
      string *marker);

  void get_bucket_instance_ids(RGWBucketInfo& bucket_info, int shard_id, map<int, string> *result);

  Mutex lock;
  SafeTimer *timer;

  class C_Tick : public Context {
    RGWRados *rados;
  public:
    C_Tick(RGWRados *_r) : rados(_r) {}
    void finish(int r) {
      rados->tick();
    }
  };

  RGWGC *gc;
  bool use_gc_thread;
  bool quota_threads;

  int num_watchers;
  RGWWatcher **watchers;
  uint64_t *watch_handles;
  librados::IoCtx root_pool_ctx;      // .rgw
  librados::IoCtx control_pool_ctx;   // .rgw.control
  bool watch_initialized;

  Mutex bucket_id_lock;

  // This field represents the number of bucket index object shards
  uint32_t bucket_index_max_shards;

  int get_obj_ioctx(const rgw_obj& obj, librados::IoCtx *ioctx);
  int get_obj_ref(const rgw_obj& obj, rgw_rados_ref *ref, rgw_bucket *bucket, bool ref_system_obj = false);
  uint64_t max_bucket_id;

  int get_olh_target_state(RGWObjectCtx& rctx, rgw_obj& obj, RGWObjState *olh_state,
                           RGWObjState **target_state, RGWObjVersionTracker *objv_tracker);
  int get_obj_state_impl(RGWObjectCtx *rctx, rgw_obj& obj, RGWObjState **state, RGWObjVersionTracker *objv_tracker, bool follow_olh);
  int append_atomic_test(RGWObjectCtx *rctx, rgw_obj& obj,
                         librados::ObjectOperation& op, RGWObjState **state);

  int update_placement_map();
  int store_bucket_info(RGWBucketInfo& info, map<string, bufferlist> *pattrs, RGWObjVersionTracker *objv_tracker, bool exclusive);

  void remove_rgw_head_obj(librados::ObjectWriteOperation& op);
  void cls_obj_check_prefix_exist(librados::ObjectOperation& op, const string& prefix, bool fail_if_exist);
protected:
  CephContext *cct;
  librados::Rados *rados;
  librados::IoCtx gc_pool_ctx;        // .rgw.gc

  bool pools_initialized;

  string region_name;
  string zone_name;

  RGWQuotaHandler *quota_handler;

public:
  RGWRados() : lock("rados_timer_lock"), timer(NULL),
               gc(NULL), use_gc_thread(false), quota_threads(false),
               num_watchers(0), watchers(NULL), watch_handles(NULL),
               watch_initialized(false),
               bucket_id_lock("rados_bucket_id"),
               bucket_index_max_shards(0),
               max_bucket_id(0),
               cct(NULL), rados(NULL),
               pools_initialized(false),
               quota_handler(NULL),
               rest_master_conn(NULL),
               meta_mgr(NULL), data_log(NULL) {}

  void set_context(CephContext *_cct) {
    cct = _cct;
  }

  void set_region(const string& name) {
    region_name = name;
  }

  void set_zone(const string& name) {
    zone_name = name;
  }

  RGWRegion region;
  RGWZoneParams zone; /* internal zone params, e.g., rados pools */
  RGWZone zone_public_config; /* external zone params, e.g., entrypoints, log flags, etc. */
  RGWRegionMap region_map;
  RGWRESTConn *rest_master_conn;
  map<string, RGWRESTConn *> zone_conn_map;
  map<string, RGWRESTConn *> region_conn_map;

  RGWMetadataManager *meta_mgr;

  RGWDataChangesLog *data_log;

  virtual ~RGWRados() {
    if (rados) {
      rados->shutdown();
      delete rados;
    }
  }

  int get_required_alignment(rgw_bucket& bucket, uint64_t *alignment);
  int get_max_chunk_size(rgw_bucket& bucket, uint64_t *max_chunk_size);

  int list_raw_objects(rgw_bucket& pool, const string& prefix_filter, int max,
                       RGWListRawObjsCtx& ctx, list<string>& oids,
                       bool *is_truncated);

  int list_raw_prefixed_objs(string pool_name, const string& prefix, list<string>& result);
  int list_regions(list<string>& regions);
  int list_zones(list<string>& zones);

  void tick();

  CephContext *ctx() { return cct; }
  /** do all necessary setup of the storage device */
  int initialize(CephContext *_cct, bool _use_gc_thread, bool _quota_threads) {
    set_context(_cct);
    use_gc_thread = _use_gc_thread;
    quota_threads = _quota_threads;
    return initialize();
  }
  /** Initialize the RADOS instance and prepare to do other ops */
  virtual int init_rados();
  int init_complete();
  virtual int initialize();
  virtual void finalize();

  /** set up a bucket listing. handle is filled in. */
  virtual int list_buckets_init(RGWAccessHandle *handle);
  /** 
   * get the next bucket in the listing. obj is filled in,
   * handle is updated.
   */
  virtual int list_buckets_next(RGWObjEnt& obj, RGWAccessHandle *handle);

  /// list logs
  int log_list_init(const string& prefix, RGWAccessHandle *handle);
  int log_list_next(RGWAccessHandle handle, string *name);

  /// remove log
  int log_remove(const string& name);

  /// show log
  int log_show_init(const string& name, RGWAccessHandle *handle);
  int log_show_next(RGWAccessHandle handle, rgw_log_entry *entry);

  // log bandwidth info
  int log_usage(map<rgw_user_bucket, RGWUsageBatch>& usage_info);
  int read_usage(string& user, uint64_t start_epoch, uint64_t end_epoch, uint32_t max_entries,
                 bool *is_truncated, RGWUsageIter& read_iter, map<rgw_user_bucket, rgw_usage_log_entry>& usage);
  int trim_usage(string& user, uint64_t start_epoch, uint64_t end_epoch);

  virtual int create_pool(rgw_bucket& bucket);

  /**
   * create a bucket with name bucket and the given list of attrs
   * returns 0 on success, -ERR# otherwise.
   */
  virtual int init_bucket_index(rgw_bucket& bucket, int num_shards);
  int select_bucket_placement(RGWUserInfo& user_info, const string& region_name, const std::string& rule,
                              const std::string& bucket_name, rgw_bucket& bucket, string *pselected_rule);
  int select_legacy_bucket_placement(const string& bucket_name, rgw_bucket& bucket);
  int select_new_bucket_location(RGWUserInfo& user_info, const string& region_name, const string& rule,
                                 const std::string& bucket_name, rgw_bucket& bucket, string *pselected_rule);
  int set_bucket_location_by_rule(const string& location_rule, const std::string& bucket_name, rgw_bucket& bucket);
  virtual int create_bucket(RGWUserInfo& owner, rgw_bucket& bucket,
                            const string& region_name,
                            const string& placement_rule,
                            map<std::string,bufferlist>& attrs,
                            RGWBucketInfo& bucket_info,
                            obj_version *pobjv,
                            obj_version *pep_objv,
                            time_t creation_time,
                            rgw_bucket *master_bucket,
                            bool exclusive = true);
  virtual int add_bucket_placement(std::string& new_pool);
  virtual int remove_bucket_placement(std::string& new_pool);
  virtual int list_placement_set(set<string>& names);
  virtual int create_pools(vector<string>& names, vector<int>& retcodes);

  class SystemObject {
    RGWRados *store;
    RGWObjectCtx& ctx;
    rgw_obj obj;

    RGWObjState *state;

  protected:
    int get_state(RGWObjState **pstate, RGWObjVersionTracker *objv_tracker);

  public:
    SystemObject(RGWRados *_store, RGWObjectCtx& _ctx, rgw_obj& _obj) : store(_store), ctx(_ctx), obj(_obj), state(NULL) {}

    RGWRados *get_store() { return store; }
    rgw_obj& get_obj() { return obj; }
    RGWObjectCtx& get_ctx() { return ctx; }

    struct Read {
      RGWRados::SystemObject *source;

      struct GetObjState {
        librados::IoCtx io_ctx;
        bool has_ioctx;
        uint64_t last_ver;

        GetObjState() : has_ioctx(false), last_ver(0) {}

        int get_ioctx(RGWRados *store, rgw_obj& obj, librados::IoCtx **ioctx);
      } state;
      
      struct StatParams {
        time_t *lastmod;
        uint64_t *obj_size;
        map<string, bufferlist> *attrs;
        struct rgw_err *perr;

        StatParams() : lastmod(NULL), obj_size(NULL), attrs(NULL) {}
      } stat_params;

      struct ReadParams {
        rgw_cache_entry_info *cache_info;
      } read_params;

      Read(RGWRados::SystemObject *_source) : source(_source) {}

      int stat(RGWObjVersionTracker *objv_tracker);
      int read(int64_t ofs, int64_t end, bufferlist& bl, RGWObjVersionTracker *objv_tracker);
      int get_attr(const char *name, bufferlist& dest);
    };
  };

  struct BucketShard {
    RGWRados *store;
    rgw_bucket bucket;
    int shard_id;
    librados::IoCtx index_ctx;
    string bucket_obj;

    BucketShard(RGWRados *_store) : store(_store), shard_id(-1) {}
    int init(rgw_bucket& _bucket, rgw_obj& obj);
  };

  class Object {
    RGWRados *store;
    RGWBucketInfo bucket_info;
    RGWObjectCtx& ctx;
    rgw_obj obj;

    BucketShard bs;

    RGWObjState *state;

    bool versioning_disabled;

    bool bs_initialized;

  protected:
    int get_state(RGWObjState **pstate, bool follow_olh);
    void invalidate_state();

    int prepare_atomic_modification(librados::ObjectWriteOperation& op, bool reset_obj, const string *ptag,
                                    const char *ifmatch, const char *ifnomatch, bool removal_op);
    int complete_atomic_modification();

  public:
    Object(RGWRados *_store, RGWBucketInfo& _bucket_info, RGWObjectCtx& _ctx, rgw_obj& _obj) : store(_store), bucket_info(_bucket_info),
                                                                                               ctx(_ctx), obj(_obj), bs(store),
                                                                                               state(NULL), versioning_disabled(false),
                                                                                               bs_initialized(false) {}

    RGWRados *get_store() { return store; }
    rgw_obj& get_obj() { return obj; }
    RGWObjectCtx& get_ctx() { return ctx; }
    RGWBucketInfo& get_bucket_info() { return bucket_info; }

    int get_bucket_shard(BucketShard **pbs) {
      if (!bs_initialized) {
        int r = bs.init(bucket_info.bucket, obj);
        if (r < 0) {
          return r;
        }
        bs_initialized = true;
      }
      *pbs = &bs;
      return 0;
    }

    void set_versioning_disabled(bool status) {
      versioning_disabled = status;
    }

    bool versioning_enabled() {
      return (!versioning_disabled && bucket_info.versioning_enabled());
    }

    struct Read {
      RGWRados::Object *source;

      struct GetObjState {
        librados::IoCtx io_ctx;
        rgw_obj obj;
      } state;
      
      struct ConditionParams {
        const time_t *mod_ptr;
        const time_t *unmod_ptr;
        const char *if_match;
        const char *if_nomatch;
        
        ConditionParams() : 
                 mod_ptr(NULL), unmod_ptr(NULL), if_match(NULL), if_nomatch(NULL) {}
      } conds;

      struct Params {
        time_t *lastmod;
        uint64_t *read_size;
        uint64_t *obj_size;
        map<string, bufferlist> *attrs;
        struct rgw_err *perr;

        Params() : lastmod(NULL), read_size(NULL), obj_size(NULL), attrs(NULL) {}
      } params;

      Read(RGWRados::Object *_source) : source(_source) {}

      int prepare(int64_t *pofs, int64_t *pend);
      int read(int64_t ofs, int64_t end, bufferlist& bl);
      int iterate(int64_t ofs, int64_t end, RGWGetDataCB *cb);
      int get_attr(const char *name, bufferlist& dest);
    };

    struct Write {
      RGWRados::Object *target;
      
      struct MetaParams {
        time_t *mtime;
        map<std::string, bufferlist>* rmattrs;
        const bufferlist *data;
        RGWObjManifest *manifest;
        const string *ptag;
        list<rgw_obj_key> *remove_objs;
        time_t set_mtime;
        string owner;
        RGWObjCategory category;
        int flags;
        const char *if_match;
        const char *if_nomatch;
        uint64_t olh_epoch;

        MetaParams() : mtime(NULL), rmattrs(NULL), data(NULL), manifest(NULL), ptag(NULL),
                 remove_objs(NULL), set_mtime(0), category(RGW_OBJ_CATEGORY_MAIN), flags(0),
                 if_match(NULL), if_nomatch(NULL), olh_epoch(0) {}
      } meta;

      Write(RGWRados::Object *_target) : target(_target) {}

      int write_meta(uint64_t size,  map<std::string, bufferlist>& attrs);
      int write_data(const char *data, uint64_t ofs, uint64_t len, bool exclusive);
    };

    struct Delete {
      RGWRados::Object *target;

      struct DeleteParams {
        string bucket_owner;
        int versioning_status;
        ACLOwner obj_owner; /* needed for creation of deletion marker */
        uint64_t olh_epoch;
        string marker_version_id;
        uint32_t bilog_flags;
        list<rgw_obj_key> *remove_objs;

        DeleteParams() : versioning_status(0), olh_epoch(0), bilog_flags(0), remove_objs(NULL) {}
      } params;

      struct DeleteResult {
        bool delete_marker;
        string version_id;

        DeleteResult() : delete_marker(false) {}
      } result;
      
      Delete(RGWRados::Object *_target) : target(_target) {}

      int delete_obj();
    };
  };

  class Bucket {
    RGWRados *store;
    rgw_bucket& bucket;

  public:
    Bucket(RGWRados *_store, rgw_bucket& _bucket) : store(_store), bucket(_bucket) {}

    RGWRados *get_store() { return store; }
    rgw_bucket& get_bucket() { return bucket; }

    class UpdateIndex {
      RGWRados::Bucket *target;
      string optag;
      rgw_obj obj;
      RGWObjState *obj_state;
      uint16_t bilog_flags;
      BucketShard bs;
      bool bs_initialized;
    public:

      UpdateIndex(RGWRados::Bucket *_target, rgw_obj& _obj, RGWObjState *_state) : target(_target), obj(_obj), obj_state(_state), bilog_flags(0),
                                                                                   bs(target->get_store()), bs_initialized(false) {}

      int get_bucket_shard(BucketShard **pbs) {
        if (!bs_initialized) {
          int r = bs.init(target->get_bucket(), obj);
          if (r < 0) {
            return r;
          }
          bs_initialized = true;
        }
        *pbs = &bs;
        return 0;
      }

      void set_bilog_flags(uint16_t flags) {
        bilog_flags = flags;
      }

      int prepare(RGWModifyOp);
      int complete(int64_t poolid, uint64_t epoch, uint64_t size,
                   utime_t& ut, string& etag, string& content_type,
                   bufferlist *acl_bl, RGWObjCategory category,
		   list<rgw_obj_key> *remove_objs);
      int complete_del(int64_t poolid, uint64_t epoch,
                       list<rgw_obj_key> *remove_objs);
      int cancel();
    };

    struct List {
      RGWRados::Bucket *target;
      rgw_obj_key next_marker;

      struct Params {
        string prefix;
        string delim;
        rgw_obj_key marker;
        string ns;
        bool enforce_ns;
        RGWAccessListFilter *filter;
        bool list_versions;

        Params() : enforce_ns(true), filter(NULL), list_versions(false) {}
      } params;

    public:
      List(RGWRados::Bucket *_target) : target(_target) {}

      int list_objects(int max, vector<RGWObjEnt> *result, map<string, bool> *common_prefixes, bool *is_truncated);
      rgw_obj_key& get_next_marker() {
        return next_marker;
      }
    };
  };

  /** Write/overwrite an object to the bucket storage. */
  virtual int put_system_obj_impl(rgw_obj& obj, uint64_t size, time_t *mtime,
              map<std::string, bufferlist>& attrs, int flags,
              bufferlist& data,
              RGWObjVersionTracker *objv_tracker,
              time_t set_mtime /* 0 for don't set */);

  virtual int put_obj_data(void *ctx, rgw_obj& obj, const char *data,
              off_t ofs, size_t len, bool exclusive);
  virtual int aio_put_obj_data(void *ctx, rgw_obj& obj, bufferlist& bl,
                               off_t ofs, bool exclusive, void **handle);

  int put_system_obj(void *ctx, rgw_obj& obj, const char *data, size_t len, bool exclusive,
              time_t *mtime, map<std::string, bufferlist>& attrs, RGWObjVersionTracker *objv_tracker,
              time_t set_mtime) {
    bufferlist bl;
    bl.append(data, len);
    int flags = PUT_OBJ_CREATE;
    if (exclusive)
      flags |= PUT_OBJ_EXCL;

    return put_system_obj_impl(obj, len, mtime, attrs, flags, bl, objv_tracker, set_mtime);
  }
  virtual int aio_wait(void *handle);
  virtual bool aio_completed(void *handle);

  int rewrite_obj(RGWBucketInfo& dest_bucket_info, rgw_obj& obj);
  int fetch_remote_obj(RGWObjectCtx& obj_ctx,
                       const string& user_id,
                       const string& client_id,
                       const string& op_id,
                       req_info *info,
                       const string& source_zone,
                       rgw_obj& dest_obj,
                       rgw_obj& src_obj,
                       RGWBucketInfo& dest_bucket_info,
                       RGWBucketInfo& src_bucket_info,
                       time_t *mtime,
                       const time_t *mod_ptr,
                       const time_t *unmod_ptr,
                       const char *if_match,
                       const char *if_nomatch,
                       bool replace_attrs,
                       map<string, bufferlist>& attrs,
                       RGWObjCategory category,
                       uint64_t olh_epoch,
                       string *version_id,
                       string *ptag,
                       string *petag,
                       struct rgw_err *err,
                       void (*progress_cb)(off_t, void *),
                       void *progress_data);
  int copy_obj_to_remote_dest(RGWObjState *astate,
                              map<string, bufferlist>& src_attrs,
                              RGWRados::Object::Read& read_op,
                              const string& user_id,
                              rgw_obj& dest_obj,
                              time_t *mtime);
  /**
   * Copy an object.
   * dest_obj: the object to copy into
   * src_obj: the object to copy from
   * attrs: if replace_attrs is set then these are placed on the new object
   * err: stores any errors resulting from the get of the original object
   * Returns: 0 on success, -ERR# otherwise.
   */
  virtual int copy_obj(RGWObjectCtx& obj_ctx,
               const string& user_id,
               const string& client_id,
               const string& op_id,
               req_info *info,
               const string& source_zone,
               rgw_obj& dest_obj,
               rgw_obj& src_obj,
               RGWBucketInfo& dest_bucket_info,
               RGWBucketInfo& src_bucket_info,
               time_t *mtime,
               const time_t *mod_ptr,
               const time_t *unmod_ptr,
               const char *if_match,
               const char *if_nomatch,
               bool replace_attrs,
               map<std::string, bufferlist>& attrs,
               RGWObjCategory category,
               uint64_t olh_epoch,
               string *version_id,
               string *ptag,
               string *petag,
               struct rgw_err *err,
               void (*progress_cb)(off_t, void *),
               void *progress_data);

  int copy_obj_data(RGWObjectCtx& obj_ctx,
               RGWBucketInfo& dest_bucket_info,
	       RGWRados::Object::Read& read_op, off_t end,
               rgw_obj& dest_obj,
               rgw_obj& src_obj,
               uint64_t max_chunk_size,
	       time_t *mtime,
	       time_t set_mtime,
               map<string, bufferlist>& attrs,
               RGWObjCategory category,
               uint64_t olh_epoch,
               string *version_id,
               string *ptag,
               string *petag,
               struct rgw_err *err);

  /**
   * Delete a bucket.
   * bucket: the name of the bucket to delete
   * Returns 0 on success, -ERR# otherwise.
   */
  virtual int delete_bucket(rgw_bucket& bucket, RGWObjVersionTracker& objv_tracker);

  int set_bucket_owner(rgw_bucket& bucket, ACLOwner& owner);
  int set_buckets_enabled(std::vector<rgw_bucket>& buckets, bool enabled);
  int bucket_suspended(rgw_bucket& bucket, bool *suspended);

  /** Delete an object.*/
  virtual int delete_obj(RGWObjectCtx& obj_ctx, RGWBucketInfo& bucket_owner, rgw_obj& src_obj,
                         int versioning_status, uint16_t bilog_flags = 0);

  /* Delete a system object */
  virtual int delete_system_obj(rgw_obj& src_obj, RGWObjVersionTracker *objv_tracker = NULL);

  /** Remove an object from the bucket index */
  int delete_obj_index(rgw_obj& obj);

  /**
   * Get the attributes for an object.
   * bucket: name of the bucket holding the object.
   * obj: name of the object
   * name: name of the attr to retrieve
   * dest: bufferlist to store the result in
   * Returns: 0 on success, -ERR# otherwise.
   */
  virtual int system_obj_get_attr(rgw_obj& obj, const char *name, bufferlist& dest);

  /**
   * Set an attr on an object.
   * bucket: name of the bucket holding the object
   * obj: name of the object to set the attr on
   * name: the attr to set
   * bl: the contents of the attr
   * Returns: 0 on success, -ERR# otherwise.
   */
  virtual int set_attr(void *ctx, rgw_obj& obj, const char *name, bufferlist& bl,
                       RGWObjVersionTracker *objv_tracker);

  virtual int set_attrs(void *ctx, rgw_obj& obj,
                        map<string, bufferlist>& attrs,
                        map<string, bufferlist>* rmattrs,
                        RGWObjVersionTracker *objv_tracker);

  int get_obj_state(RGWObjectCtx *rctx, rgw_obj& obj, RGWObjState **state, RGWObjVersionTracker *objv_tracker, bool follow_olh);
  int get_obj_state(RGWObjectCtx *rctx, rgw_obj& obj, RGWObjState **state, RGWObjVersionTracker *objv_tracker) {
    return get_obj_state(rctx, obj, state, objv_tracker, true);
  }

  virtual int stat_system_obj(RGWObjectCtx& obj_ctx,
                              RGWRados::SystemObject::Read::GetObjState& state,
                              rgw_obj& obj,
                              map<string, bufferlist> *attrs,
                              time_t *lastmod,
                              uint64_t *obj_size,
                              RGWObjVersionTracker *objv_tracker);

  virtual int get_system_obj(RGWObjectCtx& obj_ctx, RGWRados::SystemObject::Read::GetObjState& read_state,
                             RGWObjVersionTracker *objv_tracker, rgw_obj& obj,
                             bufferlist& bl, off_t ofs, off_t end,
                             rgw_cache_entry_info *cache_info);

  virtual bool chain_cache_entry(list<rgw_cache_entry_info *>& cache_info_entries, RGWChainedCache::Entry *chained_entry) { return false; }

  int iterate_obj(RGWObjectCtx& ctx, rgw_obj& obj,
                  off_t ofs, off_t end,
                  uint64_t max_chunk_size,
                  int (*iterate_obj_cb)(rgw_obj&, off_t, off_t, off_t, bool, RGWObjState *, void *),
                  void *arg);

  int flush_read_list(struct get_obj_data *d);

  int get_obj_iterate_cb(RGWObjectCtx *ctx, RGWObjState *astate,
                         rgw_obj& obj,
                         off_t obj_ofs, off_t read_ofs, off_t len,
                         bool is_head_obj, void *arg);

  void get_obj_aio_completion_cb(librados::completion_t cb, void *arg);

  /**
   * a simple object read without keeping state
   */

  virtual int raw_obj_stat(rgw_obj& obj, uint64_t *psize, time_t *pmtime, uint64_t *epoch,
                       map<string, bufferlist> *attrs, bufferlist *first_chunk,
                       RGWObjVersionTracker *objv_tracker);

  int obj_operate(rgw_obj& obj, librados::ObjectWriteOperation *op);
  int obj_operate(rgw_obj& obj, librados::ObjectReadOperation *op);

  void bucket_index_guard_olh_op(RGWObjState& olh_state, librados::ObjectOperation& op);
  int olh_init_modification(RGWObjState& state, rgw_obj& olh_obj, string *op_tag);
  int olh_init_modification_impl(RGWObjState& state, rgw_obj& olh_obj, string *op_tag);
  int bucket_index_link_olh(RGWObjState& olh_state, rgw_obj& obj_instance, bool delete_marker,
                            const string& op_tag, struct rgw_bucket_dir_entry_meta *meta,
                            uint64_t olh_epoch);
  int bucket_index_unlink_instance(rgw_obj& obj_instance, const string& op_tag, uint64_t olh_epoch);
  int bucket_index_read_olh_log(RGWObjState& state, rgw_obj& obj_instance, uint64_t ver_marker,
                                map<uint64_t, vector<rgw_bucket_olh_log_entry> > *log, bool *is_truncated);
  int bucket_index_trim_olh_log(RGWObjState& obj_state, rgw_obj& obj_instance, uint64_t ver);
  int bucket_index_clear_olh(RGWObjState& state, rgw_obj& obj_instance);
  int apply_olh_log(RGWObjectCtx& ctx, RGWObjState& obj_state, RGWBucketInfo& bucket_info, rgw_obj& obj,
                    bufferlist& obj_tag, map<uint64_t, vector<rgw_bucket_olh_log_entry> >& log,
                    uint64_t *plast_ver);
  int update_olh(RGWObjectCtx& obj_ctx, RGWObjState *state, RGWBucketInfo& bucket_info, rgw_obj& obj);
  int set_olh(RGWObjectCtx& obj_ctx, RGWBucketInfo& bucket_info, rgw_obj& target_obj, bool delete_marker, rgw_bucket_dir_entry_meta *meta,
              uint64_t olh_epoch);
  int unlink_obj_instance(RGWObjectCtx& obj_ctx, RGWBucketInfo& bucket_info, rgw_obj& target_obj,
                          uint64_t olh_epoch);

  void check_pending_olh_entries(map<string, bufferlist>& pending_entries, map<string, bufferlist> *rm_pending_entries);
  int remove_olh_pending_entries(RGWObjState& state, rgw_obj& olh_obj, map<string, bufferlist>& pending_attrs);
  int follow_olh(RGWObjectCtx& ctx, RGWObjState *state, rgw_obj& olh_obj, rgw_obj *target);
  int get_olh(rgw_obj& obj, RGWOLHInfo *olh);

  void gen_rand_obj_instance_name(rgw_obj *target);

  int omap_get_vals(rgw_obj& obj, bufferlist& header, const std::string& marker, uint64_t count, std::map<string, bufferlist>& m);
  virtual int omap_get_all(rgw_obj& obj, bufferlist& header, std::map<string, bufferlist>& m);
  virtual int omap_set(rgw_obj& obj, std::string& key, bufferlist& bl);
  virtual int omap_set(rgw_obj& obj, map<std::string, bufferlist>& m);
  virtual int omap_del(rgw_obj& obj, const std::string& key);
  virtual int update_containers_stats(map<string, RGWBucketEnt>& m);
  virtual int append_async(rgw_obj& obj, size_t size, bufferlist& bl);

  virtual bool need_watch_notify() { return false; }
  virtual int init_watch();
  virtual void finalize_watch();
  virtual int distribute(const string& key, bufferlist& bl);
  virtual int watch_cb(int opcode, uint64_t ver, bufferlist& bl) { return 0; }
  void pick_control_oid(const string& key, string& notify_oid);

  void set_atomic(void *ctx, rgw_obj& obj) {
    RGWObjectCtx *rctx = static_cast<RGWObjectCtx *>(ctx);
    rctx->set_atomic(obj);
  }
  void set_prefetch_data(void *ctx, rgw_obj& obj) {
    RGWObjectCtx *rctx = static_cast<RGWObjectCtx *>(ctx);
    rctx->set_prefetch_data(obj);
  }

  int decode_policy(bufferlist& bl, ACLOwner *owner);
  int get_bucket_stats(rgw_bucket& bucket, string *bucket_ver, string *master_ver,
      map<RGWObjCategory, RGWStorageStats>& stats, string *max_marker);
  int get_bucket_stats_async(rgw_bucket& bucket, RGWGetBucketStats_CB *cb);
  int get_user_stats(const string& user, RGWStorageStats& stats);
  int get_user_stats_async(const string& user, RGWGetUserStats_CB *cb);
  void get_bucket_instance_obj(rgw_bucket& bucket, rgw_obj& obj);
  void get_bucket_instance_entry(rgw_bucket& bucket, string& entry);
  void get_bucket_meta_oid(rgw_bucket& bucket, string& oid);

  int put_bucket_entrypoint_info(const string& bucket_name, RGWBucketEntryPoint& entry_point, bool exclusive, RGWObjVersionTracker& objv_tracker, time_t mtime,
                                 map<string, bufferlist> *pattrs);
  int put_bucket_instance_info(RGWBucketInfo& info, bool exclusive, time_t mtime, map<string, bufferlist> *pattrs);
  int get_bucket_entrypoint_info(RGWObjectCtx& obj_ctx, const string& bucket_name, RGWBucketEntryPoint& entry_point, RGWObjVersionTracker *objv_tracker, time_t *pmtime,
                                 map<string, bufferlist> *pattrs, rgw_cache_entry_info *cache_info = NULL);
  int get_bucket_instance_info(RGWObjectCtx& obj_ctx, const string& meta_key, RGWBucketInfo& info, time_t *pmtime, map<string, bufferlist> *pattrs);
  int get_bucket_instance_info(RGWObjectCtx& obj_ctx, rgw_bucket& bucket, RGWBucketInfo& info, time_t *pmtime, map<string, bufferlist> *pattrs);
  int get_bucket_instance_from_oid(RGWObjectCtx& obj_ctx, string& oid, RGWBucketInfo& info, time_t *pmtime, map<string, bufferlist> *pattrs,
                                   rgw_cache_entry_info *cache_info = NULL);

  int convert_old_bucket_info(RGWObjectCtx& obj_ctx, string& bucket_name);
  virtual int get_bucket_info(RGWObjectCtx& obj_ctx, const string& bucket_name, RGWBucketInfo& info,
                              time_t *pmtime, map<string, bufferlist> *pattrs = NULL);
  virtual int put_linked_bucket_info(RGWBucketInfo& info, bool exclusive, time_t mtime, obj_version *pep_objv,
                                     map<string, bufferlist> *pattrs, bool create_entry_point);

  int cls_rgw_init_index(librados::IoCtx& io_ctx, librados::ObjectWriteOperation& op, string& oid);
  int cls_obj_prepare_op(BucketShard& bs, RGWModifyOp op, string& tag, rgw_obj& obj, uint16_t bilog_flags);
  int cls_obj_complete_op(BucketShard& bs, RGWModifyOp op, string& tag, int64_t pool, uint64_t epoch,
                          RGWObjEnt& ent, RGWObjCategory category, list<rgw_obj_key> *remove_objs, uint16_t bilog_flags);
  int cls_obj_complete_add(BucketShard& bs, string& tag, int64_t pool, uint64_t epoch, RGWObjEnt& ent,
                           RGWObjCategory category, list<rgw_obj_key> *remove_objs, uint16_t bilog_flags);
  int cls_obj_complete_del(BucketShard& bs, string& tag, int64_t pool, uint64_t epoch, rgw_obj& obj,
                           list<rgw_obj_key> *remove_objs, uint16_t bilog_flags);
  int cls_obj_complete_cancel(BucketShard& bs, string& tag, rgw_obj& obj, uint16_t bilog_flags);
  int cls_obj_set_bucket_tag_timeout(rgw_bucket& bucket, uint64_t timeout);
  int cls_bucket_list(rgw_bucket& bucket, rgw_obj_key& start, const string& prefix,
                      uint32_t num_entries, bool list_versions, map<string, RGWObjEnt>& m,
                      bool *is_truncated, rgw_obj_key *last_entry,
                      bool (*force_check_filter)(const string&  name) = NULL);
  int cls_bucket_head(rgw_bucket& bucket, map<string, struct rgw_bucket_dir_header>& headers, map<int, string> *bucket_instance_ids = NULL);
  int cls_bucket_head_async(rgw_bucket& bucket, RGWGetDirHeader_CB *ctx, int *num_aio);
  int list_bi_log_entries(rgw_bucket& bucket, int shard_id, string& marker, uint32_t max, std::list<rgw_bi_log_entry>& result, bool *truncated);
  int trim_bi_log_entries(rgw_bucket& bucket, int shard_id, string& marker, string& end_marker);

  int bi_get_instance(rgw_obj& obj, rgw_bucket_dir_entry *dirent);
  int bi_get(rgw_bucket& bucket, rgw_obj& obj, BIIndexType index_type, rgw_cls_bi_entry *entry);
  int bi_put(rgw_bucket& bucket, rgw_obj& obj, rgw_cls_bi_entry& entry);
  int bi_list(rgw_bucket& bucket, const string& obj_name, const string& marker, uint32_t max,
              list<rgw_cls_bi_entry> *entries, bool *is_truncated);

  int cls_obj_usage_log_add(const string& oid, rgw_usage_log_info& info);
  int cls_obj_usage_log_read(string& oid, string& user, uint64_t start_epoch, uint64_t end_epoch, uint32_t max_entries,
                             string& read_iter, map<rgw_user_bucket, rgw_usage_log_entry>& usage, bool *is_truncated);
  int cls_obj_usage_log_trim(string& oid, string& user, uint64_t start_epoch, uint64_t end_epoch);

  void shard_name(const string& prefix, unsigned max_shards, const string& key, string& name);
  void shard_name(const string& prefix, unsigned max_shards, const string& section, const string& key, string& name);
  void time_log_prepare_entry(cls_log_entry& entry, const utime_t& ut, const string& section, const string& key, bufferlist& bl);
  int time_log_add(const string& oid, list<cls_log_entry>& entries);
  int time_log_add(const string& oid, const utime_t& ut, const string& section, const string& key, bufferlist& bl);
  int time_log_list(const string& oid, utime_t& start_time, utime_t& end_time,
                    int max_entries, list<cls_log_entry>& entries,
		    const string& marker, string *out_marker, bool *truncated);
  int time_log_info(const string& oid, cls_log_header *header);
  int time_log_trim(const string& oid, const utime_t& start_time, const utime_t& end_time,
                    const string& from_marker, const string& to_marker);
  int lock_exclusive(rgw_bucket& pool, const string& oid, utime_t& duration, string& zone_id, string& owner_id);
  int unlock(rgw_bucket& pool, const string& oid, string& zone_id, string& owner_id);

  void update_gc_chain(rgw_obj& head_obj, RGWObjManifest& manifest, cls_rgw_obj_chain *chain);
  int send_chain_to_gc(cls_rgw_obj_chain& chain, const string& tag, bool sync);
  int gc_operate(string& oid, librados::ObjectWriteOperation *op);
  int gc_aio_operate(string& oid, librados::ObjectWriteOperation *op);
  int gc_operate(string& oid, librados::ObjectReadOperation *op, bufferlist *pbl);

  int list_gc_objs(int *index, string& marker, uint32_t max, bool expired_only, std::list<cls_rgw_gc_obj_info>& result, bool *truncated);
  int process_gc();
  int defer_gc(void *ctx, rgw_obj& obj);

  int bucket_check_index(rgw_bucket& bucket,
                         map<RGWObjCategory, RGWStorageStats> *existing_stats,
                         map<RGWObjCategory, RGWStorageStats> *calculated_stats);
  int bucket_rebuild_index(rgw_bucket& bucket);
  int remove_objs_from_index(rgw_bucket& bucket, list<rgw_obj_key>& oid_list);

  int cls_user_get_header(const string& user_id, cls_user_header *header);
  int cls_user_get_header_async(const string& user_id, RGWGetUserHeader_CB *ctx);
  int cls_user_sync_bucket_stats(rgw_obj& user_obj, rgw_bucket& bucket);
  int update_user_bucket_stats(const string& user_id, rgw_bucket& bucket, RGWStorageStats& stats);
  int cls_user_list_buckets(rgw_obj& obj,
                            const string& in_marker, int max_entries,
                            list<cls_user_bucket_entry>& entries,
                            string *out_marker, bool *truncated);
  int cls_user_add_bucket(rgw_obj& obj, const cls_user_bucket_entry& entry);
  int cls_user_update_buckets(rgw_obj& obj, list<cls_user_bucket_entry>& entries, bool add);
  int cls_user_complete_stats_sync(rgw_obj& obj);
  int complete_sync_user_stats(const string& user_id);
  int cls_user_add_bucket(rgw_obj& obj, list<cls_user_bucket_entry>& entries);
  int cls_user_remove_bucket(rgw_obj& obj, const cls_user_bucket& bucket);

  int check_quota(const string& bucket_owner, rgw_bucket& bucket,
                  RGWQuotaInfo& user_quota, RGWQuotaInfo& bucket_quota, uint64_t obj_size);

  string unique_id(uint64_t unique_num) {
    char buf[32];
    snprintf(buf, sizeof(buf), ".%llu.%llu", (unsigned long long)instance_id(), (unsigned long long)unique_num);
    string s = zone.name + buf;
    return s;
  }


  void get_log_pool_name(string& name) {
    name = zone.log_pool.name;
  }

  bool need_to_log_data() {
    return zone_public_config.log_data;
  }

  bool need_to_log_metadata() {
    return zone_public_config.log_meta;
  }

 private:
  /**
   * This is a helper method, it generates a list of bucket index objects with the given
   * bucket base oid and number of shards.
   *
   * bucket_oid_base [in] - base name of the bucket index object;
   * num_shards [in] - number of bucket index object shards.
   * bucket_objs [out] - filled by this method, a list of bucket index objects.
   */
  void get_bucket_index_objects(const string& bucket_oid_base, uint32_t num_shards,
      map<int, string>& bucket_objs, int shard_id = -1);

  /**
   * Get the bucket index object with the given base bucket index object and object key,
   * and the number of bucket index shards.
   *
   * bucket_oid_base [in] - bucket object base name.
   * obj_key [in] - object key.
   * num_shards [in] - number of bucket index shards.
   * hash_type [in] - type of hash to find the shard ID.
   * bucket_obj [out] - the bucket index object for the given object.
   *
   * Return 0 on success, a failure code otherwise.
   */
  int get_bucket_index_object(const string& bucket_oid_base, const string& obj_key,
      uint32_t num_shards, RGWBucketInfo::BIShardsHashType hash_type, string *bucket_obj, int *shard);

  /**
   * Check the actual on-disk state of the object specified
   * by list_state, and fill in the time and size of object.
   * Then append any changes to suggested_updates for
   * the rgw class' dir_suggest_changes function.
   *
   * Note that this can maul list_state; don't use it afterwards. Also
   * it expects object to already be filled in from list_state; it only
   * sets the size and mtime.
   *
   * Returns 0 on success, -ENOENT if the object doesn't exist on disk,
   * and -errno on other failures. (-ENOENT is not a failure, and it
   * will encode that info as a suggested update.)
   */
  int check_disk_state(librados::IoCtx io_ctx,
                       rgw_bucket& bucket,
                       rgw_bucket_dir_entry& list_state,
                       RGWObjEnt& object,
                       bufferlist& suggested_updates);

  bool bucket_is_system(rgw_bucket& bucket) {
    return (bucket.name[0] == '.');
  }

  /**
   * Init pool iteration
   * bucket: pool name in a bucket object
   * ctx: context object to use for the iteration
   * Returns: 0 on success, -ERR# otherwise.
   */
  int pool_iterate_begin(rgw_bucket& bucket, RGWPoolIterCtx& ctx);
  /**
   * Iterate over pool return object names, use optional filter
   * ctx: iteration context, initialized with pool_iterate_begin()
   * num: max number of objects to return
   * objs: a vector that the results will append into
   * is_truncated: if not NULL, will hold true iff iteration is complete
   * filter: if not NULL, will be used to filter returned objects
   * Returns: 0 on success, -ERR# otherwise.
   */
  int pool_iterate(RGWPoolIterCtx& ctx, uint32_t num, vector<RGWObjEnt>& objs,
                   bool *is_truncated, RGWAccessListFilter *filter);

  uint64_t instance_id();
  uint64_t next_bucket_id();
};

class RGWStoreManager {
public:
  RGWStoreManager() {}
  static RGWRados *get_storage(CephContext *cct, bool use_gc_thread, bool quota_threads) {
    RGWRados *store = init_storage_provider(cct, use_gc_thread, quota_threads);
    return store;
  }
  static RGWRados *get_raw_storage(CephContext *cct) {
    RGWRados *store = init_raw_storage_provider(cct);
    return store;
  }
  static RGWRados *init_storage_provider(CephContext *cct, bool use_gc_thread, bool quota_threads);
  static RGWRados *init_raw_storage_provider(CephContext *cct);
  static void close_storage(RGWRados *store);

};

template <class T>
class RGWChainedCacheImpl : public RGWChainedCache {
  RWLock lock;

  map<string, T> entries;

public:
  RGWChainedCacheImpl() : lock("RGWChainedCacheImpl::lock") {}

  bool find(const string& key, T *entry) {
    RWLock::RLocker rl(lock);
    typename map<string, T>::iterator iter = entries.find(key);
    if (iter == entries.end()) {
      return false;
    }

    *entry = iter->second;
    return true;
  }

  bool put(RGWRados *store, const string& key, T *entry, list<rgw_cache_entry_info *>& cache_info_entries) {
    Entry chain_entry(this, key, entry);

    /* we need the store cache to call us under its lock to maintain lock ordering */
    return store->chain_cache_entry(cache_info_entries, &chain_entry);
  }

  void chain_cb(const string& key, void *data) {
    T *entry = static_cast<T *>(data);
    RWLock::WLocker wl(lock);
    entries[key] = *entry;
  }

  void invalidate(const string& key) {
    RWLock::WLocker wl(lock);
    entries.erase(key);
  }
};

class RGWPutObjProcessor
{
protected:
  RGWRados *store;
  RGWObjectCtx& obj_ctx;
  bool is_complete;
  RGWBucketInfo bucket_info;

  virtual int do_complete(string& etag, time_t *mtime, time_t set_mtime,
                          map<string, bufferlist>& attrs,
                          const char *if_match = NULL, const char *if_nomatch = NULL) = 0;

public:
  RGWPutObjProcessor(RGWObjectCtx& _obj_ctx, RGWBucketInfo& _bi) : store(NULL), obj_ctx(_obj_ctx), is_complete(false), bucket_info(_bi) {}
  virtual ~RGWPutObjProcessor() {}
  virtual int prepare(RGWRados *_store, string *oid_rand) {
    store = _store;
    return 0;
  }
  virtual int handle_data(bufferlist& bl, off_t ofs, MD5 *hash, void **phandle, bool *again) = 0;
  virtual int throttle_data(void *handle, bool need_to_wait) = 0;
  virtual void complete_hash(MD5 *hash) {
    assert(0);
  }
  virtual int complete(string& etag, time_t *mtime, time_t set_mtime,
                       map<string, bufferlist>& attrs,
                       const char *if_match = NULL, const char *if_nomatch = NULL);

  CephContext *ctx();
};

struct put_obj_aio_info {
  void *handle;
};

class RGWPutObjProcessor_Aio : public RGWPutObjProcessor
{
  list<struct put_obj_aio_info> pending;
  size_t max_chunks;

  struct put_obj_aio_info pop_pending();
  int wait_pending_front();
  bool pending_has_completed();

  rgw_obj last_written_obj;

protected:
  uint64_t obj_len;

  list<rgw_obj> written_objs;

  void add_written_obj(const rgw_obj& obj) {
    written_objs.push_back(obj);
  }

  int drain_pending();
  int handle_obj_data(rgw_obj& obj, bufferlist& bl, off_t ofs, off_t abs_ofs, void **phandle, bool exclusive);

public:
  int throttle_data(void *handle, bool need_to_wait);

  RGWPutObjProcessor_Aio(RGWObjectCtx& obj_ctx, RGWBucketInfo& bucket_info) : RGWPutObjProcessor(obj_ctx, bucket_info), max_chunks(RGW_MAX_PENDING_CHUNKS), obj_len(0) {}
  virtual ~RGWPutObjProcessor_Aio();
};

class RGWPutObjProcessor_Atomic : public RGWPutObjProcessor_Aio
{
  bufferlist first_chunk;
  uint64_t part_size;
  off_t cur_part_ofs;
  off_t next_part_ofs;
  int cur_part_id;
  off_t data_ofs;

  uint64_t extra_data_len;
  bufferlist extra_data_bl;
  bufferlist pending_data_bl;
  uint64_t max_chunk_size;

  bool versioned_object;
  uint64_t olh_epoch;
  string version_id;

protected:
  rgw_bucket bucket;
  string obj_str;

  string unique_tag;

  rgw_obj head_obj;
  rgw_obj cur_obj;
  RGWObjManifest manifest;
  RGWObjManifest::generator manifest_gen;

  int write_data(bufferlist& bl, off_t ofs, void **phandle, bool exclusive);
  virtual int do_complete(string& etag, time_t *mtime, time_t set_mtime,
                          map<string, bufferlist>& attrs,
                          const char *if_match = NULL, const char *if_nomatch = NULL);

  int prepare_next_part(off_t ofs);
  int complete_parts();
  int complete_writing_data();

  int prepare_init(RGWRados *store, string *oid_rand);

public:
  ~RGWPutObjProcessor_Atomic() {}
  RGWPutObjProcessor_Atomic(RGWObjectCtx& obj_ctx, RGWBucketInfo& bucket_info,
                            rgw_bucket& _b, const string& _o, uint64_t _p, const string& _t, bool versioned) :
                                RGWPutObjProcessor_Aio(obj_ctx, bucket_info),
                                part_size(_p),
                                cur_part_ofs(0),
                                next_part_ofs(_p),
                                cur_part_id(0),
                                data_ofs(0),
                                extra_data_len(0),
                                max_chunk_size(0),
                                versioned_object(versioned),
                                olh_epoch(0),
                                bucket(_b),
                                obj_str(_o),
                                unique_tag(_t) {}
  int prepare(RGWRados *store, string *oid_rand);
  virtual bool immutable_head() { return false; }
  void set_extra_data_len(uint64_t len) {
    extra_data_len = len;
  }
  virtual int handle_data(bufferlist& bl, off_t ofs, MD5 *hash, void **phandle, bool *again);
  virtual void complete_hash(MD5 *hash);
  bufferlist& get_extra_data() { return extra_data_bl; }

  void set_olh_epoch(uint64_t epoch) {
    olh_epoch = epoch;
  }

  void set_version_id(const string& vid) {
    version_id = vid;
  }
};

#endif

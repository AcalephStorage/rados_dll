// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

/** \file
 *
 * This is an OSD class that implements methods for
 * use with rbd.
 *
 * Most of these deal with the rbd header object. Methods prefixed
 * with old_ deal with the original rbd design, in which clients read
 * and interpreted the header object directly.
 *
 * The new format is meant to be opaque to clients - all their
 * interactions with non-data objects should go through this
 * class. The OSD class interface leaves the class to implement its
 * own argument and payload serialization/deserialization, so for ease
 * of implementation we use the existing ceph encoding/decoding
 * methods. Something like json might be preferable, but the rbd
 * kernel module has to be able understand format as well. The
 * datatypes exposed to the clients are strings, unsigned integers,
 * and vectors of those types. The on-wire format can be found in
 * src/include/encoding.h.
 *
 * The methods for interacting with the new format document their
 * parameters as the client sees them - it would be silly to mention
 * in each one that they take an input and an output bufferlist.
 */
#include "include/int_types.h"
#include "include/types.h"

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <errno.h>
#include <iostream>
#include <map>
#include <sstream>
#include <vector>

#include "common/bit_vector.hpp"
#include "common/errno.h"
#include "objclass/objclass.h"
#include "include/rbd_types.h"

#include "cls/rbd/cls_rbd.h"


/*
 * Object keys:
 *
 * <partial list>
 *
 * stripe_unit: size in bytes of the stripe unit.  if not present,
 *   the stripe unit is assumed to match the object size (1 << order).
 *
 * stripe_count: number of objects to stripe over before looping back.
 *   if not present or 1, striping is disabled.  this is the default.
 *
 */

CLS_VER(2,0)
CLS_NAME(rbd)

cls_handle_t h_class;
cls_method_handle_t h_create;
cls_method_handle_t h_get_features;
cls_method_handle_t h_get_size;
cls_method_handle_t h_set_size;
cls_method_handle_t h_get_parent;
cls_method_handle_t h_set_parent;
cls_method_handle_t h_get_protection_status;
cls_method_handle_t h_set_protection_status;
cls_method_handle_t h_get_stripe_unit_count;
cls_method_handle_t h_set_stripe_unit_count;
cls_method_handle_t h_get_flags;
cls_method_handle_t h_set_flags;
cls_method_handle_t h_remove_parent;
cls_method_handle_t h_add_child;
cls_method_handle_t h_remove_child;
cls_method_handle_t h_get_children;
cls_method_handle_t h_get_snapcontext;
cls_method_handle_t h_get_object_prefix;
cls_method_handle_t h_get_snapshot_name;
cls_method_handle_t h_snapshot_add;
cls_method_handle_t h_snapshot_remove;
cls_method_handle_t h_get_all_features;
cls_method_handle_t h_copyup;
cls_method_handle_t h_get_id;
cls_method_handle_t h_set_id;
cls_method_handle_t h_dir_get_id;
cls_method_handle_t h_dir_get_name;
cls_method_handle_t h_dir_list;
cls_method_handle_t h_dir_add_image;
cls_method_handle_t h_dir_remove_image;
cls_method_handle_t h_dir_rename_image;
cls_method_handle_t h_object_map_load;
cls_method_handle_t h_object_map_resize;
cls_method_handle_t h_object_map_update;
cls_method_handle_t h_old_snapshots_list;
cls_method_handle_t h_old_snapshot_add;
cls_method_handle_t h_old_snapshot_remove;

#define RBD_MAX_KEYS_READ 64
#define RBD_SNAP_KEY_PREFIX "snapshot_"
#define RBD_DIR_ID_KEY_PREFIX "id_"
#define RBD_DIR_NAME_KEY_PREFIX "name_"

static int snap_read_header(cls_method_context_t hctx, bufferlist& bl)
{
  unsigned snap_count = 0;
  uint64_t snap_names_len = 0;
  struct rbd_obj_header_ondisk *header;

  CLS_LOG(20, "snapshots_list");

  while (1) {
    int len = sizeof(*header) +
      snap_count * sizeof(struct rbd_obj_snap_ondisk) +
      snap_names_len;

    int rc = cls_cxx_read(hctx, 0, len, &bl);
    if (rc < 0)
      return rc;

    if (bl.length() < sizeof(*header))
      return -EINVAL;

    header = (struct rbd_obj_header_ondisk *)bl.c_str();
    assert(header);

    if ((snap_count != header->snap_count) ||
        (snap_names_len != header->snap_names_len)) {
      snap_count = header->snap_count;
      snap_names_len = header->snap_names_len;
      bl.clear();
      continue;
    }
    break;
  }

  return 0;
}

static void key_from_snap_id(snapid_t snap_id, string *out)
{
  ostringstream oss;
  oss << RBD_SNAP_KEY_PREFIX
      << std::setw(16) << std::setfill('0') << std::hex << snap_id;
  *out = oss.str();
}

static snapid_t snap_id_from_key(const string &key)
{
  istringstream iss(key);
  uint64_t id;
  iss.ignore(strlen(RBD_SNAP_KEY_PREFIX)) >> std::hex >> id;
  return id;
}

template<typename T>
static int read_key(cls_method_context_t hctx, const string &key, T *out)
{
  bufferlist bl;
  int r = cls_cxx_map_get_val(hctx, key, &bl);
  if (r < 0) {
    if (r != -ENOENT) {
      CLS_ERR("error reading omap key %s: %d", key.c_str(), r);
    }
    return r;
  }

  try {
    bufferlist::iterator it = bl.begin();
    ::decode(*out, it);
  } catch (const buffer::error &err) {
    CLS_ERR("error decoding %s", key.c_str());
    return -EIO;
  }

  return 0;
}

static bool is_valid_id(const string &id) {
  if (!id.size())
    return false;
  for (size_t i = 0; i < id.size(); ++i) {
    if (!isalnum(id[i])) {
      return false;
    }
  }
  return true;
}

/**
 * Initialize the header with basic metadata.
 * Extra features may initialize more fields in the future.
 * Everything is stored as key/value pairs as omaps in the header object.
 *
 * If features the OSD does not understand are requested, -ENOSYS is
 * returned.
 *
 * Input:
 * @param size number of bytes in the image (uint64_t)
 * @param order bits to shift to determine the size of data objects (uint8_t)
 * @param features what optional things this image will use (uint64_t)
 * @param object_prefix a prefix for all the data objects
 *
 * Output:
 * @return 0 on success, negative error code on failure
 */
int create(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  string object_prefix;
  uint64_t features, size;
  uint8_t order;

  try {
    bufferlist::iterator iter = in->begin();
    ::decode(size, iter);
    ::decode(order, iter);
    ::decode(features, iter);
    ::decode(object_prefix, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  CLS_LOG(20, "create object_prefix=%s size=%llu order=%u features=%llu",
	  object_prefix.c_str(), (unsigned long long)size, order,
	  (unsigned long long)features);

  if (features & ~RBD_FEATURES_ALL) {
    return -ENOSYS;
  }

  if (!object_prefix.size()) {
    return -EINVAL;
  }

  bufferlist stored_prefixbl;
  int r = cls_cxx_map_get_val(hctx, "object_prefix", &stored_prefixbl);
  if (r != -ENOENT) {
    CLS_ERR("reading object_prefix returned %d", r);
    return -EEXIST;
  }

  bufferlist sizebl;
  ::encode(size, sizebl);
  r = cls_cxx_map_set_val(hctx, "size", &sizebl);
  if (r < 0)
    return r;

  bufferlist orderbl;
  ::encode(order, orderbl);
  r = cls_cxx_map_set_val(hctx, "order", &orderbl);
  if (r < 0)
    return r;

  bufferlist featuresbl;
  ::encode(features, featuresbl);
  r = cls_cxx_map_set_val(hctx, "features", &featuresbl);
  if (r < 0)
    return r;

  bufferlist object_prefixbl;
  ::encode(object_prefix, object_prefixbl);
  r = cls_cxx_map_set_val(hctx, "object_prefix", &object_prefixbl);
  if (r < 0)
    return r;

  bufferlist snap_seqbl;
  uint64_t snap_seq = 0;
  ::encode(snap_seq, snap_seqbl);
  r = cls_cxx_map_set_val(hctx, "snap_seq", &snap_seqbl);
  if (r < 0)
    return r;

  return 0;
}

/**
 * Input:
 * @param snap_id which snapshot to query, or CEPH_NOSNAP (uint64_t)
 *
 * Output:
 * @param features list of enabled features for the given snapshot (uint64_t)
 * @returns 0 on success, negative error code on failure
 */
int get_features(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  uint64_t features, snap_id;
  bool read_only = false;

  bufferlist::iterator iter = in->begin();
  try {
    ::decode(snap_id, iter);
    if (!iter.end()) {
      ::decode(read_only, iter);
    }
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  CLS_LOG(20, "get_features snap_id=%llu", (unsigned long long)snap_id);

  if (snap_id == CEPH_NOSNAP) {
    int r = read_key(hctx, "features", &features);
    if (r < 0) {
      CLS_ERR("failed to read features off disk: %s", cpp_strerror(r).c_str());
      return r;
    }
  } else {
    cls_rbd_snap snap;
    string snapshot_key;
    key_from_snap_id(snap_id, &snapshot_key);
    int r = read_key(hctx, snapshot_key, &snap);
    if (r < 0)
      return r;

    features = snap.features;
  }

  uint64_t incompatible = (read_only ? features & RBD_FEATURES_INCOMPATIBLE :
				       features & RBD_FEATURES_RW_INCOMPATIBLE);
  ::encode(features, *out);
  ::encode(incompatible, *out);

  return 0;
}

/**
 * check that given feature(s) are set
 *
 * @param hctx context
 * @param need features needed
 * @return 0 if features are set, negative error (like ENOEXEC) otherwise
 */
int require_feature(cls_method_context_t hctx, uint64_t need)
{
  uint64_t features;
  int r = read_key(hctx, "features", &features);
  if (r == -ENOENT)   // this implies it's an old-style image with no features
    return -ENOEXEC;
  if (r < 0)
    return r;
  if ((features & need) != need) {
    CLS_LOG(10, "require_feature missing feature %llx, have %llx",
            (unsigned long long)need, (unsigned long long)features);
    return -ENOEXEC;
  }
  return 0;
}

/**
 * Input:
 * @param snap_id which snapshot to query, or CEPH_NOSNAP (uint64_t)
 *
 * Output:
 * @param order bits to shift to get the size of data objects (uint8_t)
 * @param size size of the image in bytes for the given snapshot (uint64_t)
 * @returns 0 on success, negative error code on failure
 */
int get_size(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  uint64_t snap_id, size;
  uint8_t order;

  bufferlist::iterator iter = in->begin();
  try {
    ::decode(snap_id, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  CLS_LOG(20, "get_size snap_id=%llu", (unsigned long long)snap_id);

  int r = read_key(hctx, "order", &order);
  if (r < 0) {
    CLS_ERR("failed to read the order off of disk: %s", cpp_strerror(r).c_str());
    return r;
  }

  if (snap_id == CEPH_NOSNAP) {
    r = read_key(hctx, "size", &size);
    if (r < 0) {
      CLS_ERR("failed to read the image's size off of disk: %s", cpp_strerror(r).c_str());
      return r;
    }
  } else {
    cls_rbd_snap snap;
    string snapshot_key;
    key_from_snap_id(snap_id, &snapshot_key);
    int r = read_key(hctx, snapshot_key, &snap);
    if (r < 0)
      return r;

    size = snap.image_size;
  }

  ::encode(order, *out);
  ::encode(size, *out);

  return 0;
}

/**
 * Input:
 * @param size new capacity of the image in bytes (uint64_t)
 *
 * Output:
 * @returns 0 on success, negative error code on failure
 */
int set_size(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  uint64_t size;

  bufferlist::iterator iter = in->begin();
  try {
    ::decode(size, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  // check that size exists to make sure this is a header object
  // that was created correctly
  uint64_t orig_size;
  int r = read_key(hctx, "size", &orig_size);
  if (r < 0) {
    CLS_ERR("Could not read image's size off disk: %s", cpp_strerror(r).c_str());
    return r;
  }

  CLS_LOG(20, "set_size size=%llu orig_size=%llu", (unsigned long long)size,
          (unsigned long long)orig_size);

  bufferlist sizebl;
  ::encode(size, sizebl);
  r = cls_cxx_map_set_val(hctx, "size", &sizebl);
  if (r < 0) {
    CLS_ERR("error writing snapshot metadata: %d", r);
    return r;
  }

  // if we are shrinking, and have a parent, shrink our overlap with
  // the parent, too.
  if (size < orig_size) {
    cls_rbd_parent parent;
    r = read_key(hctx, "parent", &parent);
    if (r == -ENOENT)
      r = 0;
    if (r < 0)
      return r;
    if (parent.exists() && parent.overlap > size) {
      bufferlist parentbl;
      parent.overlap = size;
      ::encode(parent, parentbl);
      r = cls_cxx_map_set_val(hctx, "parent", &parentbl);
      if (r < 0) {
	CLS_ERR("error writing parent: %d", r);
	return r;
      }
    }
  }

  return 0;
}

/**
 * verify that the header object exists
 *
 * @return 0 if the object exists, -ENOENT if it does not, or other error
 */
int check_exists(cls_method_context_t hctx)
{
  uint64_t size;
  time_t mtime;
  return cls_cxx_stat(hctx, &size, &mtime);
}

/**
 * get the current protection status of the specified snapshot
 *
 * Input:
 * @param snap_id (uint64_t) which snapshot to get the status of
 *
 * Output:
 * @param status (uint8_t) one of:
 * RBD_PROTECTION_STATUS_{PROTECTED, UNPROTECTED, UNPROTECTING}
 *
 * @returns 0 on success, negative error code on failure
 * @returns -EINVAL if snapid is CEPH_NOSNAP
 */
int get_protection_status(cls_method_context_t hctx, bufferlist *in,
			  bufferlist *out)
{
  snapid_t snap_id;

  bufferlist::iterator iter = in->begin();
  try {
    ::decode(snap_id, iter);
  } catch (const buffer::error &err) {
    CLS_LOG(20, "get_protection_status: invalid decode");
    return -EINVAL;
  }

  int r = check_exists(hctx);
  if (r < 0)
    return r;

  CLS_LOG(20, "get_protection_status snap_id=%llu",
         (unsigned long long)snap_id.val);

  if (snap_id == CEPH_NOSNAP)
    return -EINVAL;

  cls_rbd_snap snap;
  string snapshot_key;
  key_from_snap_id(snap_id.val, &snapshot_key);
  r = read_key(hctx, snapshot_key, &snap);
  if (r < 0) {
    CLS_ERR("could not read key for snapshot id %" PRIu64, snap_id.val);
    return r;
  }

  if (snap.protection_status >= RBD_PROTECTION_STATUS_LAST) {
    CLS_ERR("invalid protection status for snap id %llu: %u",
	    (unsigned long long)snap_id.val, snap.protection_status);
    return -EIO;
  }

  ::encode(snap.protection_status, *out);
  return 0;
}

/**
 * set the proctection status of a snapshot
 *
 * Input:
 * @param snapid (uint64_t) which snapshot to set the status of
 * @param status (uint8_t) one of:
 * RBD_PROTECTION_STATUS_{PROTECTED, UNPROTECTED, UNPROTECTING}
 *
 * @returns 0 on success, negative error code on failure
 * @returns -EINVAL if snapid is CEPH_NOSNAP
 */
int set_protection_status(cls_method_context_t hctx, bufferlist *in,
			  bufferlist *out)
{
  snapid_t snap_id;
  uint8_t status;

  bufferlist::iterator iter = in->begin();
  try {
    ::decode(snap_id, iter);
    ::decode(status, iter);
  } catch (const buffer::error &err) {
    CLS_LOG(20, "set_protection_status: invalid decode");
    return -EINVAL;
  }

  int r = check_exists(hctx);
  if (r < 0)
    return r;

  r = require_feature(hctx, RBD_FEATURE_LAYERING);
  if (r < 0) {
    CLS_LOG(20, "image does not support layering");
    return r;
  }

  CLS_LOG(20, "set_protection_status snapid=%llu status=%u",
	  (unsigned long long)snap_id.val, status);

  if (snap_id == CEPH_NOSNAP)
    return -EINVAL;

  if (status >= RBD_PROTECTION_STATUS_LAST) {
    CLS_LOG(10, "invalid protection status for snap id %llu: %u",
	    (unsigned long long)snap_id.val, status);
    return -EINVAL;
  }

  cls_rbd_snap snap;
  string snapshot_key;
  key_from_snap_id(snap_id.val, &snapshot_key);
  r = read_key(hctx, snapshot_key, &snap);
  if (r < 0) {
    CLS_ERR("could not read key for snapshot id %" PRIu64, snap_id.val);
    return r;
  }

  snap.protection_status = status;
  bufferlist snapshot_bl;
  ::encode(snap, snapshot_bl);
  r = cls_cxx_map_set_val(hctx, snapshot_key, &snapshot_bl);
  if (r < 0) {
    CLS_ERR("error writing snapshot metadata: %d", r);
    return r;
  }

  return 0;
}

/**
 * get striping parameters
 *
 * Input:
 * none
 *
 * Output:
 * @param stripe unit (bytes)
 * @param stripe count (num objects)
 *
 * @returns 0 on success
 */
int get_stripe_unit_count(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  int r = check_exists(hctx);
  if (r < 0)
    return r;

  CLS_LOG(20, "get_stripe_unit_count");

  r = require_feature(hctx, RBD_FEATURE_STRIPINGV2);
  if (r < 0)
    return r;

  uint64_t stripe_unit = 0, stripe_count = 0;
  r = read_key(hctx, "stripe_unit", &stripe_unit);
  if (r == -ENOENT) {
    // default to object size
    uint8_t order;
    r = read_key(hctx, "order", &order);
    if (r < 0) {
      CLS_ERR("failed to read the order off of disk: %s", cpp_strerror(r).c_str());
      return -EIO;
    }
    stripe_unit = 1ull << order;
  }
  if (r < 0)
    return r;
  r = read_key(hctx, "stripe_count", &stripe_count);
  if (r == -ENOENT) {
    // default to 1
    stripe_count = 1;
    r = 0;
  }
  if (r < 0)
    return r;

  ::encode(stripe_unit, *out);
  ::encode(stripe_count, *out);
  return 0;
}

/**
 * set striping parameters
 *
 * Input:
 * @param stripe unit (bytes)
 * @param stripe count (num objects)
 *
 * @returns 0 on success
 */
int set_stripe_unit_count(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  uint64_t stripe_unit, stripe_count;

  bufferlist::iterator iter = in->begin();
  try {
    ::decode(stripe_unit, iter);
    ::decode(stripe_count, iter);
  } catch (const buffer::error &err) {
    CLS_LOG(20, "set_stripe_unit_count: invalid decode");
    return -EINVAL;
  }

  if (!stripe_count || !stripe_unit)
    return -EINVAL;

  int r = check_exists(hctx);
  if (r < 0)
    return r;

  CLS_LOG(20, "set_stripe_unit_count");

  r = require_feature(hctx, RBD_FEATURE_STRIPINGV2);
  if (r < 0)
    return r;

  uint8_t order;
  r = read_key(hctx, "order", &order);
  if (r < 0) {
    CLS_ERR("failed to read the order off of disk: %s", cpp_strerror(r).c_str());
    return r;
  }
  if ((1ull << order) % stripe_unit || stripe_unit > (1ull << order)) {
    CLS_ERR("stripe unit %llu is not a factor of the object size %llu",
            (unsigned long long)stripe_unit, 1ull << order);
    return -EINVAL;
  }

  bufferlist bl, bl2;
  ::encode(stripe_unit, bl);
  r = cls_cxx_map_set_val(hctx, "stripe_unit", &bl);
  if (r < 0) {
    CLS_ERR("error writing stripe_unit metadata: %d", r);
    return r;
  }

  ::encode(stripe_count, bl2);
  r = cls_cxx_map_set_val(hctx, "stripe_count", &bl2);
  if (r < 0) {
    CLS_ERR("error writing stripe_count metadata: %d", r);
    return r;
  }

  return 0;
}

/**
 * get the image flags
 *
 * Input:
 * @param snap_id which snapshot to query, to CEPH_NOSNAP (uint64_t)
 *
 * Output:
 * @param flags image flags
 *
 * @returns 0 on success, negative error code upon failure
 */
int get_flags(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  uint64_t snap_id;
  bufferlist::iterator iter = in->begin();
  try {
    ::decode(snap_id, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  CLS_LOG(20, "get_flags snap_id=%llu", (unsigned long long)snap_id);

  uint64_t flags = 0;
  if (snap_id == CEPH_NOSNAP) {
    int r = read_key(hctx, "flags", &flags);
    if (r < 0 && r != -ENOENT) {
      CLS_ERR("failed to read flags off disk: %s", cpp_strerror(r).c_str());
      return r;
    }
  } else {
    cls_rbd_snap snap;
    string snapshot_key;
    key_from_snap_id(snap_id, &snapshot_key);
    int r = read_key(hctx, snapshot_key, &snap);
    if (r < 0) {
      return r;
    }
    flags = snap.flags;
  }

  ::encode(flags, *out);
  return 0;
}

/**
 * set the image flags
 *
 * Input:
 * @params flags image flags
 * @params mask image flag mask
 *
 * Output:
 * none
 *
 * @returns 0 on success, negative error code upon failure
 */
int set_flags(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  uint64_t flags;
  uint64_t mask;
  bufferlist::iterator iter = in->begin();
  try {
    ::decode(flags, iter);
    ::decode(mask, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  // check that size exists to make sure this is a header object
  // that was created correctly
  uint64_t orig_flags = 0;
  int r = read_key(hctx, "flags", &orig_flags);
  if (r < 0 && r != -ENOENT) {
    CLS_ERR("Could not read image's flags off disk: %s",
            cpp_strerror(r).c_str());
    return r;
  }

  flags = (orig_flags & ~mask) | (flags & mask);
  CLS_LOG(20, "set_flags flags=%llu orig_flags=%llu", (unsigned long long)flags,
          (unsigned long long)orig_flags);

  bufferlist flagsbl;
  ::encode(flags, flagsbl);
  r = cls_cxx_map_set_val(hctx, "flags", &flagsbl);
  if (r < 0) {
    CLS_ERR("error updating flags: %d", r);
    return r;
  }
  return 0;
}

/**
 * get the current parent, if any
 *
 * Input:
 * @param snap_id which snapshot to query, or CEPH_NOSNAP (uint64_t)
 *
 * Output:
 * @param pool parent pool id (-1 if parent does not exist)
 * @param image parent image id
 * @param snapid parent snapid
 * @param size portion of parent mapped under the child
 *
 * @returns 0 on success or parent does not exist, negative error code on failure
 */
int get_parent(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  uint64_t snap_id;

  bufferlist::iterator iter = in->begin();
  try {
    ::decode(snap_id, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  int r = check_exists(hctx);
  if (r < 0)
    return r;

  CLS_LOG(20, "get_parent snap_id=%llu", (unsigned long long)snap_id);

  cls_rbd_parent parent;
  r = require_feature(hctx, RBD_FEATURE_LAYERING);
  if (r == 0) {
    if (snap_id == CEPH_NOSNAP) {
      r = read_key(hctx, "parent", &parent);
      if (r < 0 && r != -ENOENT)
	return r;
    } else {
      cls_rbd_snap snap;
      string snapshot_key;
      key_from_snap_id(snap_id, &snapshot_key);
      r = read_key(hctx, snapshot_key, &snap);
      if (r < 0 && r != -ENOENT)
	return r;
      parent = snap.parent;
    }
  }

  ::encode(parent.pool, *out);
  ::encode(parent.id, *out);
  ::encode(parent.snapid, *out);
  ::encode(parent.overlap, *out);
  return 0;
}

/**
 * set the image parent
 *
 * Input:
 * @param pool parent pool
 * @param id parent image id
 * @param snapid parent snapid
 * @param size parent size
 *
 * @returns 0 on success, or negative error code
 */
int set_parent(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  int64_t pool;
  string id;
  snapid_t snapid;
  uint64_t size;

  bufferlist::iterator iter = in->begin();
  try {
    ::decode(pool, iter);
    ::decode(id, iter);
    ::decode(snapid, iter);
    ::decode(size, iter);
  } catch (const buffer::error &err) {
    CLS_LOG(20, "cls_rbd::set_parent: invalid decode");
    return -EINVAL;
  }

  int r = check_exists(hctx);
  if (r < 0) {
    CLS_LOG(20, "cls_rbd::set_parent: child already exists");
    return r;
  }

  r = require_feature(hctx, RBD_FEATURE_LAYERING);
  if (r < 0) {
    CLS_LOG(20, "cls_rbd::set_parent: child does not support layering");
    return r;
  }

  CLS_LOG(20, "set_parent pool=%llu id=%s snapid=%llu size=%llu",
	  (unsigned long long)pool, id.c_str(), (unsigned long long)snapid.val,
	  (unsigned long long)size);

  if (pool < 0 || id.length() == 0 || snapid == CEPH_NOSNAP || size == 0) {
    return -EINVAL;
  }

  // make sure there isn't already a parent
  cls_rbd_parent parent;
  r = read_key(hctx, "parent", &parent);
  if (r == 0) {
    CLS_LOG(20, "set_parent existing parent pool=%llu id=%s snapid=%llu"
            "overlap=%llu", (unsigned long long)parent.pool, parent.id.c_str(),
	    (unsigned long long)parent.snapid.val,
	    (unsigned long long)parent.overlap);
    return -EEXIST;
  }

  // our overlap is the min of our size and the parent's size.
  uint64_t our_size;
  r = read_key(hctx, "size", &our_size);
  if (r < 0)
    return r;

  bufferlist parentbl;
  parent.pool = pool;
  parent.id = id;
  parent.snapid = snapid;
  parent.overlap = MIN(our_size, size);
  ::encode(parent, parentbl);
  r = cls_cxx_map_set_val(hctx, "parent", &parentbl);
  if (r < 0) {
    CLS_ERR("error writing parent: %d", r);
    return r;
  }

  return 0;
}


/**
 * remove the parent pointer
 *
 * This can only happen on the head, not on a snapshot.  No arguments.
 *
 * @returns 0 on success, negative error code on failure.
 */
int remove_parent(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  int r = check_exists(hctx);
  if (r < 0)
    return r;

  r = require_feature(hctx, RBD_FEATURE_LAYERING);
  if (r < 0)
    return r;

  cls_rbd_parent parent;
  r = read_key(hctx, "parent", &parent);
  if (r < 0)
    return r;

  r = cls_cxx_map_remove_key(hctx, "parent");
  if (r < 0) {
    CLS_ERR("error removing parent: %d", r);
    return r;
  }

  return 0;
}

/**
 * methods for dealing with rbd_children object
 */

static int decode_parent_common(bufferlist::iterator& it, uint64_t *pool_id,
				string *image_id, snapid_t *snap_id)
{
  try {
    ::decode(*pool_id, it);
    ::decode(*image_id, it);
    ::decode(*snap_id, it);
  } catch (const buffer::error &err) {
    CLS_ERR("error decoding parent spec");
    return -EINVAL;
  }
  return 0;
}

static int decode_parent(bufferlist *in, uint64_t *pool_id,
			 string *image_id, snapid_t *snap_id)
{
  bufferlist::iterator it = in->begin();
  return decode_parent_common(it, pool_id, image_id, snap_id);
}

static int decode_parent_and_child(bufferlist *in, uint64_t *pool_id,
			           string *image_id, snapid_t *snap_id,
				   string *c_image_id)
{
  bufferlist::iterator it = in->begin();
  int r = decode_parent_common(it, pool_id, image_id, snap_id);
  if (r < 0)
    return r;
  try {
    ::decode(*c_image_id, it);
  } catch (const buffer::error &err) {
    CLS_ERR("error decoding child image id");
    return -EINVAL;
  }
  return 0;
}

static string parent_key(uint64_t pool_id, string image_id, snapid_t snap_id)
{
  bufferlist key_bl;
  ::encode(pool_id, key_bl);
  ::encode(image_id, key_bl);
  ::encode(snap_id, key_bl);
  return string(key_bl.c_str(), key_bl.length());
}

/**
 * add child to rbd_children directory object
 *
 * rbd_children is a map of (p_pool_id, p_image_id, p_snap_id) to
 * [c_image_id, [c_image_id ... ]]
 *
 * Input:
 * @param p_pool_id parent pool id
 * @param p_image_id parent image oid
 * @param p_snap_id parent snapshot id
 * @param c_image_id new child image oid to add
 *
 * @returns 0 on success, negative error on failure
 */

int add_child(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  int r;

  uint64_t p_pool_id;
  snapid_t p_snap_id;
  string p_image_id, c_image_id;
  // Use set for ease of erase() for remove_child()
  std::set<string> children;

  r = decode_parent_and_child(in, &p_pool_id, &p_image_id, &p_snap_id,
			      &c_image_id);
  if (r < 0)
    return r;

  CLS_LOG(20, "add_child %s to (%" PRIu64 ", %s, %" PRIu64 ")", c_image_id.c_str(),
	  p_pool_id, p_image_id.c_str(), p_snap_id.val);

  string key = parent_key(p_pool_id, p_image_id, p_snap_id);

  // get current child list for parent, if any
  r = read_key(hctx, key, &children);
  if ((r < 0) && (r != -ENOENT)) {
    CLS_LOG(20, "add_child: omap read failed: %d", r);
    return r;
  }

  if (children.find(c_image_id) != children.end()) {
    CLS_LOG(20, "add_child: child already exists: %s", c_image_id.c_str());
    return -EEXIST;
  }
  // add new child
  children.insert(c_image_id);

  // write back
  bufferlist childbl;
  ::encode(children, childbl);
  r = cls_cxx_map_set_val(hctx, key, &childbl);
  if (r < 0)
    CLS_LOG(20, "add_child: omap write failed: %d", r);
  return r;
}

/**
 * remove child from rbd_children directory object
 *
 * Input:
 * @param p_pool_id parent pool id
 * @param p_image_id parent image oid
 * @param p_snap_id parent snapshot id
 * @param c_image_id new child image oid to add
 *
 * @returns 0 on success, negative error on failure
 */

int remove_child(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  int r;

  uint64_t p_pool_id;
  snapid_t p_snap_id;
  string p_image_id, c_image_id;
  std::set<string> children;

  r = decode_parent_and_child(in, &p_pool_id, &p_image_id, &p_snap_id,
			      &c_image_id);
  if (r < 0)
    return r;

  CLS_LOG(20, "remove_child %s from (%" PRIu64 ", %s, %" PRIu64 ")",
	       c_image_id.c_str(), p_pool_id, p_image_id.c_str(),
	       p_snap_id.val);

  string key = parent_key(p_pool_id, p_image_id, p_snap_id);

  // get current child list for parent.  Unlike add_child(), an empty list
  // is an error (how can we remove something that doesn't exist?)
  r = read_key(hctx, key, &children);
  if (r < 0) {
    CLS_LOG(20, "remove_child: read omap failed: %d", r);
    return r;
  }

  if (children.find(c_image_id) == children.end()) {
    CLS_LOG(20, "remove_child: child not found: %s", c_image_id.c_str());
    return -ENOENT;
  }
  // find and remove child
  children.erase(c_image_id);

  // now empty?  remove key altogether
  if (children.empty()) {
    r = cls_cxx_map_remove_key(hctx, key);
    if (r < 0)
      CLS_LOG(20, "remove_child: remove key failed: %d", r);
  } else {
    // write back shortened children list
    bufferlist childbl;
    ::encode(children, childbl);
    r = cls_cxx_map_set_val(hctx, key, &childbl);
    if (r < 0)
      CLS_LOG(20, "remove_child: write omap failed: %d ", r);
  }
  return r;
}

/**
 * Input:
 * @param p_pool_id parent pool id
 * @param p_image_id parent image oid
 * @param p_snap_id parent snapshot id
 * @param c_image_id new child image oid to add
 *
 * Output:
 * @param children set<string> of children
 *
 * @returns 0 on success, negative error on failure
 */
int get_children(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  int r;
  uint64_t p_pool_id;
  snapid_t p_snap_id;
  string p_image_id;
  std::set<string> children;

  r = decode_parent(in, &p_pool_id, &p_image_id, &p_snap_id);
  if (r < 0)
    return r;

  CLS_LOG(20, "get_children of (%" PRIu64 ", %s, %" PRIu64 ")",
	  p_pool_id, p_image_id.c_str(), p_snap_id.val);

  string key = parent_key(p_pool_id, p_image_id, p_snap_id);

  r = read_key(hctx, key, &children);
  if (r < 0) {
    if (r != -ENOENT)
      CLS_LOG(20, "get_children: read omap failed: %d", r);
    return r;
  }
  ::encode(children, *out);
  return 0;
}


/**
 * Get the information needed to create a rados snap context for doing
 * I/O to the data objects. This must include all snapshots.
 *
 * Output:
 * @param snap_seq the highest snapshot id ever associated with the image (uint64_t)
 * @param snap_ids existing snapshot ids in descending order (vector<uint64_t>)
 * @returns 0 on success, negative error code on failure
 */
int get_snapcontext(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  CLS_LOG(20, "get_snapcontext");

  int r;
  int max_read = RBD_MAX_KEYS_READ;
  vector<snapid_t> snap_ids;
  string last_read = RBD_SNAP_KEY_PREFIX;

  do {
    set<string> keys;
    r = cls_cxx_map_get_keys(hctx, last_read, max_read, &keys);
    if (r < 0)
      return r;

    for (set<string>::const_iterator it = keys.begin();
	 it != keys.end(); ++it) {
      if ((*it).find(RBD_SNAP_KEY_PREFIX) != 0)
	break;
      snapid_t snap_id = snap_id_from_key(*it);
      snap_ids.push_back(snap_id);
    }
    if (!keys.empty())
      last_read = *(keys.rbegin());
  } while (r == max_read);

  uint64_t snap_seq;
  r = read_key(hctx, "snap_seq", &snap_seq);
  if (r < 0) {
    CLS_ERR("could not read the image's snap_seq off disk: %s", cpp_strerror(r).c_str());
    return r;
  }

  // snap_ids must be descending in a snap context
  std::reverse(snap_ids.begin(), snap_ids.end());

  ::encode(snap_seq, *out);
  ::encode(snap_ids, *out);

  return 0;
}

/**
 * Output:
 * @param object_prefix prefix for data object names (string)
 * @returns 0 on success, negative error code on failure
 */
int get_object_prefix(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  CLS_LOG(20, "get_object_prefix");

  string object_prefix;
  int r = read_key(hctx, "object_prefix", &object_prefix);
  if (r < 0) {
    CLS_ERR("failed to read the image's object prefix off of disk: %s",
            cpp_strerror(r).c_str());
    return r;
  }

  ::encode(object_prefix, *out);

  return 0;
}

int get_snapshot_name(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  uint64_t snap_id;

  bufferlist::iterator iter = in->begin();
  try {
    ::decode(snap_id, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  CLS_LOG(20, "get_snapshot_name snap_id=%llu", (unsigned long long)snap_id);

  if (snap_id == CEPH_NOSNAP)
    return -EINVAL;

  cls_rbd_snap snap;
  string snapshot_key;
  key_from_snap_id(snap_id, &snapshot_key);
  int r = read_key(hctx, snapshot_key, &snap);
  if (r < 0)
    return r;

  ::encode(snap.name, *out);

  return 0;
}

/**
 * Adds a snapshot to an rbd header. Ensures the id and name are unique.
 *
 * Input:
 * @param snap_name name of the snapshot (string)
 * @param snap_id id of the snapshot (uint64_t)
 *
 * Output:
 * @returns 0 on success, negative error code on failure.
 * @returns -ESTALE if the input snap_id is less than the image's snap_seq
 * @returns -EEXIST if the id or name are already used by another snapshot
 */
int snapshot_add(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  bufferlist snap_namebl, snap_idbl;
  cls_rbd_snap snap_meta;

  try {
    bufferlist::iterator iter = in->begin();
    ::decode(snap_meta.name, iter);
    ::decode(snap_meta.id, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  CLS_LOG(20, "snapshot_add name=%s id=%llu", snap_meta.name.c_str(),
	 (unsigned long long)snap_meta.id.val);

  if (snap_meta.id > CEPH_MAXSNAP)
    return -EINVAL;

  uint64_t cur_snap_seq;
  int r = read_key(hctx, "snap_seq", &cur_snap_seq);
  if (r < 0) {
    CLS_ERR("Could not read image's snap_seq off disk: %s", cpp_strerror(r).c_str());
    return r;
  }

  // client lost a race with another snapshot creation.
  // snap_seq must be monotonically increasing.
  if (snap_meta.id < cur_snap_seq)
    return -ESTALE;

  r = read_key(hctx, "size", &snap_meta.image_size);
  if (r < 0) {
    CLS_ERR("Could not read image's size off disk: %s", cpp_strerror(r).c_str());
    return r;
  }
  r = read_key(hctx, "features", &snap_meta.features);
  if (r < 0) {
    CLS_ERR("Could not read image's features off disk: %s", cpp_strerror(r).c_str());
    return r;
  }
  r = read_key(hctx, "flags", &snap_meta.flags);
  if (r < 0 && r != -ENOENT) {
    CLS_ERR("Could not read image's flags off disk: %s", cpp_strerror(r).c_str());
    return r;
  }

  int max_read = RBD_MAX_KEYS_READ;
  string last_read = RBD_SNAP_KEY_PREFIX;
  do {
    map<string, bufferlist> vals;
    r = cls_cxx_map_get_vals(hctx, last_read, RBD_SNAP_KEY_PREFIX,
			     max_read, &vals);
    if (r < 0)
      return r;

    for (map<string, bufferlist>::iterator it = vals.begin();
	 it != vals.end(); ++it) {
      cls_rbd_snap old_meta;
      bufferlist::iterator iter = it->second.begin();
      try {
	::decode(old_meta, iter);
      } catch (const buffer::error &err) {
	snapid_t snap_id = snap_id_from_key(it->first);
	CLS_ERR("error decoding snapshot metadata for snap_id: %llu",
	        (unsigned long long)snap_id.val);
	return -EIO;
      }
      if (snap_meta.name == old_meta.name || snap_meta.id == old_meta.id) {
	CLS_LOG(20, "snap_name %s or snap_id %llu matches existing snap %s %llu",
		snap_meta.name.c_str(), (unsigned long long)snap_meta.id.val,
		old_meta.name.c_str(), (unsigned long long)old_meta.id.val);
	return -EEXIST;
      }
    }

    if (!vals.empty())
      last_read = vals.rbegin()->first;
  } while (r == RBD_MAX_KEYS_READ);

  // snapshot inherits parent, if any
  cls_rbd_parent parent;
  r = read_key(hctx, "parent", &parent);
  if (r < 0 && r != -ENOENT)
    return r;
  if (r == 0) {
    snap_meta.parent = parent;
  }

  bufferlist snap_metabl, snap_seqbl;
  ::encode(snap_meta, snap_metabl);
  ::encode(snap_meta.id, snap_seqbl);

  string snapshot_key;
  key_from_snap_id(snap_meta.id, &snapshot_key);
  map<string, bufferlist> vals;
  vals["snap_seq"] = snap_seqbl;
  vals[snapshot_key] = snap_metabl;
  r = cls_cxx_map_set_vals(hctx, &vals);
  if (r < 0) {
    CLS_ERR("error writing snapshot metadata: %d", r);
    return r;
  }

  return 0;
}

/**
 * Removes a snapshot from an rbd header.
 *
 * Input:
 * @param snap_id the id of the snapshot to remove (uint64_t)
 *
 * Output:
 * @returns 0 on success, negative error code on failure
 */
int snapshot_remove(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  snapid_t snap_id;

  try {
    bufferlist::iterator iter = in->begin();
    ::decode(snap_id, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  CLS_LOG(20, "snapshot_remove id=%llu", (unsigned long long)snap_id.val);

  // check if the key exists. we can't rely on remove_key doing this for
  // us, since OMAPRMKEYS returns success if the key is not there.
  // bug or feature? sounds like a bug, since tmap did not have this
  // behavior, but cls_rgw may rely on it...
  cls_rbd_snap snap;
  string snapshot_key;
  key_from_snap_id(snap_id, &snapshot_key);
  int r = read_key(hctx, snapshot_key, &snap);
  if (r == -ENOENT)
    return -ENOENT;

  if (snap.protection_status != RBD_PROTECTION_STATUS_UNPROTECTED)
    return -EBUSY;

  r = cls_cxx_map_remove_key(hctx, snapshot_key);
  if (r < 0) {
    CLS_ERR("error writing snapshot metadata: %d", r);
    return r;
  }

  return 0;
}

/**
 * Returns a uint64_t of all the features supported by this class.
 */
int get_all_features(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  uint64_t all_features = RBD_FEATURES_ALL;
  ::encode(all_features, *out);
  return 0;
}

/**
 * "Copy up" data from the parent of a clone to the clone's object(s).
 * Used for implementing copy-on-write for a clone image.  Client
 * will pass down a chunk of data that fits completely within one
 * clone block (one object), and is aligned (starts at beginning of block),
 * but may be shorter (for non-full parent blocks).  The class method
 * can't know the object size to validate the requested length,
 * so it just writes the data as given if the child object doesn't
 * already exist, and returns success if it does.
 *
 * Input:
 * @param in bufferlist of data to write
 *
 * Output:
 * @returns 0 on success, or if block already exists in child
 *  negative error code on other error
 */

int copyup(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  // check for existence; if child object exists, just return success
  if (cls_cxx_stat(hctx, NULL, NULL) == 0)
    return 0;
  CLS_LOG(20, "copyup: writing length %d\n", in->length());
  return cls_cxx_write(hctx, 0, in->length(), in);
}


/************************ rbd_id object methods **************************/

/**
 * Input:
 * @param in ignored
 *
 * Output:
 * @param id the id stored in the object
 * @returns 0 on success, negative error code on failure
 */
int get_id(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  uint64_t size;
  int r = cls_cxx_stat(hctx, &size, NULL);
  if (r < 0)
    return r;

  if (size == 0)
    return -ENOENT;

  bufferlist read_bl;
  r = cls_cxx_read(hctx, 0, size, &read_bl);
  if (r < 0) {
    CLS_ERR("get_id: could not read id: %d", r);
    return r;
  }

  string id;
  try {
    bufferlist::iterator iter = read_bl.begin();
    ::decode(id, iter);
  } catch (const buffer::error &err) {
    return -EIO;
  }

  ::encode(id, *out);
  return 0;
}

/**
 * Set the id of an image. The object must already exist.
 *
 * Input:
 * @param id the id of the image, as an alpha-numeric string
 *
 * Output:
 * @returns 0 on success, -EEXIST if the atomic create fails,
 *          negative error code on other error
 */
int set_id(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  int r = check_exists(hctx);
  if (r < 0)
    return r;

  string id;
  try {
    bufferlist::iterator iter = in->begin();
    ::decode(id, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  if (!is_valid_id(id)) {
    CLS_ERR("set_id: invalid id '%s'", id.c_str());
    return -EINVAL;
  }

  uint64_t size;
  r = cls_cxx_stat(hctx, &size, NULL);
  if (r < 0)
    return r;
  if (size != 0)
    return -EEXIST;

  CLS_LOG(20, "set_id: id=%s", id.c_str());

  bufferlist write_bl;
  ::encode(id, write_bl);
  return cls_cxx_write(hctx, 0, write_bl.length(), &write_bl);
}

/*********************** methods for rbd_directory ***********************/

static const string dir_key_for_id(const string &id)
{
  return RBD_DIR_ID_KEY_PREFIX + id;
}

static const string dir_key_for_name(const string &name)
{
  return RBD_DIR_NAME_KEY_PREFIX + name;
}

static const string dir_name_from_key(const string &key)
{
  return key.substr(strlen(RBD_DIR_NAME_KEY_PREFIX));
}

static int dir_add_image_helper(cls_method_context_t hctx,
				const string &name, const string &id,
				bool check_for_unique_id)
{
  if (!name.size() || !is_valid_id(id)) {
    CLS_ERR("dir_add_image_helper: invalid name '%s' or id '%s'",
	    name.c_str(), id.c_str());
    return -EINVAL;
  }

  CLS_LOG(20, "dir_add_image_helper name=%s id=%s", name.c_str(), id.c_str());

  string tmp;
  string name_key = dir_key_for_name(name);
  string id_key = dir_key_for_id(id);
  int r = read_key(hctx, name_key, &tmp);
  if (r != -ENOENT) {
    CLS_LOG(10, "name already exists");
    return -EEXIST;
  }
  r = read_key(hctx, id_key, &tmp);
  if (r != -ENOENT && check_for_unique_id) {
    CLS_LOG(10, "id already exists");
    return -EBADF;
  }
  bufferlist id_bl, name_bl;
  ::encode(id, id_bl);
  ::encode(name, name_bl);
  map<string, bufferlist> omap_vals;
  omap_vals[name_key] = id_bl;
  omap_vals[id_key] = name_bl;
  return cls_cxx_map_set_vals(hctx, &omap_vals);
}

static int dir_remove_image_helper(cls_method_context_t hctx,
				   const string &name, const string &id)
{
  CLS_LOG(20, "dir_remove_image_helper name=%s id=%s",
	  name.c_str(), id.c_str());

  string stored_name, stored_id;
  string name_key = dir_key_for_name(name);
  string id_key = dir_key_for_id(id);
  int r = read_key(hctx, name_key, &stored_id);
  if (r < 0) {
    if (r != -ENOENT)
      CLS_ERR("error reading name to id mapping: %d", r);
    return r;
  }
  r = read_key(hctx, id_key, &stored_name);
  if (r < 0) {
    CLS_ERR("error reading id to name mapping: %d", r);
    return r;
  }

  // check if this op raced with a rename
  if (stored_name != name || stored_id != id) {
    CLS_ERR("stored name '%s' and id '%s' do not match args '%s' and '%s'",
	    stored_name.c_str(), stored_id.c_str(), name.c_str(), id.c_str());
    return -ESTALE;
  }

  r = cls_cxx_map_remove_key(hctx, name_key);
  if (r < 0) {
    CLS_ERR("error removing name: %d", r);
    return r;
  }

  r = cls_cxx_map_remove_key(hctx, id_key);
  if (r < 0) {
    CLS_ERR("error removing id: %d", r);
    return r;
  }

  return 0;
}

/**
 * Rename an image in the directory, updating both indexes
 * atomically. This can't be done from the client calling
 * dir_add_image and dir_remove_image in one transaction because the
 * results of the first method are not visibale to later steps.
 *
 * Input:
 * @param src original name of the image
 * @param dest new name of the image
 * @param id the id of the image
 *
 * Output:
 * @returns -ESTALE if src and id do not map to each other
 * @returns -ENOENT if src or id are not in the directory
 * @returns -EEXIST if dest already exists
 * @returns 0 on success, negative error code on failure
 */
int dir_rename_image(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  string src, dest, id;
  try {
    bufferlist::iterator iter = in->begin();
    ::decode(src, iter);
    ::decode(dest, iter);
    ::decode(id, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  int r = dir_remove_image_helper(hctx, src, id);
  if (r < 0)
    return r;
  // ignore duplicate id because the result of
  // remove_image_helper is not visible yet
  return dir_add_image_helper(hctx, dest, id, false);
}

/**
 * Get the id of an image given its name.
 *
 * Input:
 * @param name the name of the image
 *
 * Output:
 * @param id the id of the image
 * @returns 0 on success, negative error code on failure
 */
int dir_get_id(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  string name;

  try {
    bufferlist::iterator iter = in->begin();
    ::decode(name, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  CLS_LOG(20, "dir_get_id: name=%s", name.c_str());

  string id;
  int r = read_key(hctx, dir_key_for_name(name), &id);
  if (r < 0) {
    if (r != -ENOENT)
      CLS_ERR("error reading id for name '%s': %d", name.c_str(), r);
    return r;
  }
  ::encode(id, *out);
  return 0;
}

/**
 * Get the name of an image given its id.
 *
 * Input:
 * @param id the id of the image
 *
 * Output:
 * @param name the name of the image
 * @returns 0 on success, negative error code on failure
 */
int dir_get_name(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  string id;

  try {
    bufferlist::iterator iter = in->begin();
    ::decode(id, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  CLS_LOG(20, "dir_get_name: id=%s", id.c_str());

  string name;
  int r = read_key(hctx, dir_key_for_id(id), &name);
  if (r < 0) {
    CLS_ERR("error reading name for id '%s': %d", id.c_str(), r);
    return r;
  }
  ::encode(name, *out);
  return 0;
}

/**
 * List the names and ids of the images in the directory, sorted by
 * name.
 *
 * Input:
 * @param start_after which name to begin listing after
 *        (use the empty string to start at the beginning)
 * @param max_return the maximum number of names to list
 *
 * Output:
 * @param images map from name to id of up to max_return images
 * @returns 0 on success, negative error code on failure
 */
int dir_list(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  string start_after;
  uint64_t max_return;

  try {
    bufferlist::iterator iter = in->begin();
    ::decode(start_after, iter);
    ::decode(max_return, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  int max_read = RBD_MAX_KEYS_READ;
  int r = max_read;
  map<string, string> images;
  string last_read = dir_key_for_name(start_after);

  while (r == max_read && images.size() < max_return) {
    map<string, bufferlist> vals;
    CLS_LOG(20, "last_read = '%s'", last_read.c_str());
    r = cls_cxx_map_get_vals(hctx, last_read, RBD_DIR_NAME_KEY_PREFIX,
			     max_read, &vals);
    if (r < 0) {
      CLS_ERR("error reading directory by name: %d", r);
      return r;
    }

    for (map<string, bufferlist>::iterator it = vals.begin();
	 it != vals.end(); ++it) {
      string id;
      bufferlist::iterator iter = it->second.begin();
      try {
	::decode(id, iter);
      } catch (const buffer::error &err) {
	CLS_ERR("could not decode id of image '%s'", it->first.c_str());
	return -EIO;
      }
      CLS_LOG(20, "adding '%s' -> '%s'", dir_name_from_key(it->first).c_str(), id.c_str());
      images[dir_name_from_key(it->first)] = id;
      if (images.size() >= max_return)
	break;
    }
    if (!vals.empty()) {
      last_read = dir_key_for_name(images.rbegin()->first);
    }
  }

  ::encode(images, *out);

  return 0;
}

/**
 * Add an image to the rbd directory. Creates the directory object if
 * needed, and updates the index from id to name and name to id.
 *
 * Input:
 * @param name the name of the image
 * @param id the id of the image
 *
 * Output:
 * @returns -EEXIST if the image name is already in the directory
 * @returns -EBADF if the image id is already in the directory
 * @returns 0 on success, negative error code on failure
 */
int dir_add_image(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  int r = cls_cxx_create(hctx, false);
  if (r < 0) {
    CLS_ERR("could not create directory: error %d", r);
    return r;
  }

  string name, id;
  try {
    bufferlist::iterator iter = in->begin();
    ::decode(name, iter);
    ::decode(id, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  return dir_add_image_helper(hctx, name, id, true);
}

/**
 * Remove an image from the rbd directory.
 *
 * Input:
 * @param name the name of the image
 * @param id the id of the image
 *
 * Output:
 * @returns -ESTALE if the name and id do not map to each other
 * @returns 0 on success, negative error code on failure
 */
int dir_remove_image(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  string name, id;
  try {
    bufferlist::iterator iter = in->begin();
    ::decode(name, iter);
    ::decode(id, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  return dir_remove_image_helper(hctx, name, id);
}

int object_map_read(cls_method_context_t hctx, BitVector<2> &object_map)
{
  uint64_t size;
  int r = cls_cxx_stat(hctx, &size, NULL);
  if (r < 0) {
    return r;
  }

  bufferlist bl;
  r = cls_cxx_read(hctx, 0, size, &bl);
  if (r < 0) {
   return r;
  }

  try {
    bufferlist::iterator iter = bl.begin();
    ::decode(object_map, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }
  return 0;
}

/**
 * Load an rbd image's object map
 *
 * Input:
 * none
 *
 * Output:
 * @param object map bit vector
 * @returns 0 on success, negative error code on failure
 */
int object_map_load(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  BitVector<2> object_map;
  int r = object_map_read(hctx, object_map);
  if (r < 0) {
    return r;
  }

  ::encode(object_map, *out);
  return 0;
}

/**
 * Resize an rbd image's object map
 *
 * Input:
 * @param object_count the max number of objects in the image
 * @param default_state the default state of newly created objects
 *
 * Output:
 * @returns 0 on success, negative error code on failure
 */
int object_map_resize(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  uint64_t object_count;
  uint8_t default_state;
  try {
    bufferlist::iterator iter = in->begin();
    ::decode(object_count, iter);
    ::decode(default_state, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  BitVector<2> object_map;
  int r = object_map_read(hctx, object_map);
  if ((r < 0) && (r != -ENOENT)) {
    return r;
  }

  size_t orig_object_map_size = object_map.size();
  if (object_count < orig_object_map_size) {
    for (uint64_t i = object_count + 1; i < orig_object_map_size; ++i) {
      if (object_map[i] != default_state) {
	CLS_ERR("object map indicates object still exists: %" PRIu64, i);
	return -ESTALE;
      }
    }
    object_map.resize(object_count);
  } else if (object_count > orig_object_map_size) {
    object_map.resize(object_count);
    for (uint64_t i = orig_object_map_size; i < object_count; ++i) {
      object_map[i] = default_state;
    }
  }

  bufferlist map;
  ::encode(object_map, map);
  CLS_LOG(20, "object_map_resize: object size=%llu, byte size=%llu",
	  static_cast<unsigned long long>(object_count),
	  static_cast<unsigned long long>(map.length()));
  return cls_cxx_write_full(hctx, &map);
}

/**
 * Update an rbd image's object map
 *
 * Input:
 * @param start_object_no the start object iterator
 * @param end_object_no the end object iterator
 * @param new_object_state the new object state
 * @param current_object_state optional current object state filter
 *
 * Output:
 * @returns 0 on success, negative error code on failure
 */
int object_map_update(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  uint64_t start_object_no;
  uint64_t end_object_no;
  uint8_t new_object_state;
  boost::optional<uint8_t> current_object_state;
  try {
    bufferlist::iterator iter = in->begin();
    ::decode(start_object_no, iter);
    ::decode(end_object_no, iter);
    ::decode(new_object_state, iter);
    ::decode(current_object_state, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  BitVector<2> object_map;
  bufferlist header_bl;
  int r = cls_cxx_read(hctx, 0, object_map.get_header_length(), &header_bl);
  if (r < 0) {
    return r;
  }

  try {
    bufferlist::iterator it = header_bl.begin();
    object_map.decode_header(it);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  if (start_object_no >= end_object_no || end_object_no > object_map.size()) {
    return -ERANGE;
  }

  uint64_t byte_offset;
  uint64_t byte_length;
  object_map.get_data_extents(start_object_no,
			      end_object_no - start_object_no,
			      &byte_offset, &byte_length);

  bufferlist data_bl;
  r = cls_cxx_read(hctx, object_map.get_header_length() + byte_offset,
		   byte_length, &data_bl); 
  if (r < 0) {
    return r;
  }

  try {
    bufferlist::iterator it = data_bl.begin();
    object_map.decode_data(it, byte_offset);
  } catch (const buffer::error &err) {
    return -EINVAL;
  } 

  bool updated = false;
  for (uint64_t object_no = start_object_no; object_no < end_object_no;
       ++object_no) {
    if ((!current_object_state || object_map[object_no] == *current_object_state) &&
	object_map[object_no] != new_object_state) {
      object_map[object_no] = new_object_state;
      updated = true;
    }
  }

  if (updated) {
    CLS_LOG(20, "object_map_update: %llu~%llu -> %llu",
	    static_cast<unsigned long long>(byte_offset),
	    static_cast<unsigned long long>(byte_length),
	    static_cast<unsigned long long>(object_map.get_header_length() +
					    byte_offset));

    bufferlist update;
    object_map.encode_data(update, byte_offset, byte_length); 
    r = cls_cxx_write(hctx, object_map.get_header_length() + byte_offset,
		      update.length(), &update);
  }
  return r;
}

/****************************** Old format *******************************/

int old_snapshots_list(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  bufferlist bl;
  struct rbd_obj_header_ondisk *header;
  int rc = snap_read_header(hctx, bl);
  if (rc < 0)
    return rc;

  header = (struct rbd_obj_header_ondisk *)bl.c_str();
  bufferptr p(header->snap_names_len);
  char *buf = (char *)header;
  char *name = buf + sizeof(*header) + header->snap_count * sizeof(struct rbd_obj_snap_ondisk);
  char *end = name + header->snap_names_len;
  memcpy(p.c_str(),
         buf + sizeof(*header) + header->snap_count * sizeof(struct rbd_obj_snap_ondisk),
         header->snap_names_len);

  ::encode(header->snap_seq, *out);
  ::encode(header->snap_count, *out);

  for (unsigned i = 0; i < header->snap_count; i++) {
    string s = name;
    ::encode(header->snaps[i].id, *out);
    ::encode(header->snaps[i].image_size, *out);
    ::encode(s, *out);

    name += strlen(name) + 1;
    if (name > end)
      return -EIO;
  }

  return 0;
}

int old_snapshot_add(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  bufferlist bl;
  struct rbd_obj_header_ondisk *header;
  bufferlist newbl;
  bufferptr header_bp(sizeof(*header));
  struct rbd_obj_snap_ondisk *new_snaps;

  int rc = snap_read_header(hctx, bl);
  if (rc < 0)
    return rc;

  header = (struct rbd_obj_header_ondisk *)bl.c_str();

  int snaps_id_ofs = sizeof(*header);
  int names_ofs = snaps_id_ofs + sizeof(*new_snaps) * header->snap_count;
  const char *snap_name;
  const char *snap_names = ((char *)header) + names_ofs;
  const char *end = snap_names + header->snap_names_len;
  bufferlist::iterator iter = in->begin();
  string s;
  uint64_t snap_id;

  try {
    ::decode(s, iter);
    ::decode(snap_id, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }
  snap_name = s.c_str();

  if (header->snap_seq > snap_id)
    return -ESTALE;

  const char *cur_snap_name;
  for (cur_snap_name = snap_names; cur_snap_name < end; cur_snap_name += strlen(cur_snap_name) + 1) {
    if (strncmp(cur_snap_name, snap_name, end - cur_snap_name) == 0)
      return -EEXIST;
  }
  if (cur_snap_name > end)
    return -EIO;

  int snap_name_len = strlen(snap_name);

  bufferptr new_names_bp(header->snap_names_len + snap_name_len + 1);
  bufferptr new_snaps_bp(sizeof(*new_snaps) * (header->snap_count + 1));

  /* copy snap names and append to new snap name */
  char *new_snap_names = new_names_bp.c_str();
  strcpy(new_snap_names, snap_name);
  memcpy(new_snap_names + snap_name_len + 1, snap_names, header->snap_names_len);

  /* append new snap id */
  new_snaps = (struct rbd_obj_snap_ondisk *)new_snaps_bp.c_str();
  memcpy(new_snaps + 1, header->snaps, sizeof(*new_snaps) * header->snap_count);

  header->snap_count = header->snap_count + 1;
  header->snap_names_len = header->snap_names_len + snap_name_len + 1;
  header->snap_seq = snap_id;

  new_snaps[0].id = snap_id;
  new_snaps[0].image_size = header->image_size;

  memcpy(header_bp.c_str(), header, sizeof(*header));

  newbl.push_back(header_bp);
  newbl.push_back(new_snaps_bp);
  newbl.push_back(new_names_bp);

  rc = cls_cxx_write_full(hctx, &newbl);
  if (rc < 0)
    return rc;

  return 0;
}

int old_snapshot_remove(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  bufferlist bl;
  struct rbd_obj_header_ondisk *header;
  bufferlist newbl;
  bufferptr header_bp(sizeof(*header));

  int rc = snap_read_header(hctx, bl);
  if (rc < 0)
    return rc;

  header = (struct rbd_obj_header_ondisk *)bl.c_str();

  int snaps_id_ofs = sizeof(*header);
  int names_ofs = snaps_id_ofs + sizeof(struct rbd_obj_snap_ondisk) * header->snap_count;
  const char *snap_name;
  const char *snap_names = ((char *)header) + names_ofs;
  const char *orig_names = snap_names;
  const char *end = snap_names + header->snap_names_len;
  bufferlist::iterator iter = in->begin();
  string s;
  unsigned i;
  bool found = false;
  struct rbd_obj_snap_ondisk snap;

  try {
    ::decode(s, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }
  snap_name = s.c_str();

  for (i = 0; snap_names < end; i++) {
    if (strcmp(snap_names, snap_name) == 0) {
      snap = header->snaps[i];
      found = true;
      break;
    }
    snap_names += strlen(snap_names) + 1;
  }
  if (!found) {
    CLS_ERR("couldn't find snap %s\n", snap_name);
    return -ENOENT;
  }

  header->snap_names_len  = header->snap_names_len - (s.length() + 1);
  header->snap_count = header->snap_count - 1;

  bufferptr new_names_bp(header->snap_names_len);
  bufferptr new_snaps_bp(sizeof(header->snaps[0]) * header->snap_count);

  memcpy(header_bp.c_str(), header, sizeof(*header));
  newbl.push_back(header_bp);

  if (header->snap_count) {
    int snaps_len = 0;
    int names_len = 0;
    CLS_LOG(20, "i=%d\n", i);
    if (i > 0) {
      snaps_len = sizeof(header->snaps[0]) * i;
      names_len =  snap_names - orig_names;
      memcpy(new_snaps_bp.c_str(), header->snaps, snaps_len);
      memcpy(new_names_bp.c_str(), orig_names, names_len);
    }
    snap_names += s.length() + 1;

    if (i < header->snap_count) {
      memcpy(new_snaps_bp.c_str() + snaps_len,
             header->snaps + i + 1,
             sizeof(header->snaps[0]) * (header->snap_count - i));
      memcpy(new_names_bp.c_str() + names_len, snap_names , end - snap_names);
    }
    newbl.push_back(new_snaps_bp);
    newbl.push_back(new_names_bp);
  }

  rc = cls_cxx_write_full(hctx, &newbl);
  if (rc < 0)
    return rc;

  return 0;
}


void __cls_init()
{
  CLS_LOG(20, "Loaded rbd class!");

  cls_register("rbd", &h_class);
  cls_register_cxx_method(h_class, "create",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  create, &h_create);
  cls_register_cxx_method(h_class, "get_features",
			  CLS_METHOD_RD,
			  get_features, &h_get_features);
  cls_register_cxx_method(h_class, "get_size",
			  CLS_METHOD_RD,
			  get_size, &h_get_size);
  cls_register_cxx_method(h_class, "set_size",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  set_size, &h_set_size);
  cls_register_cxx_method(h_class, "get_snapcontext",
			  CLS_METHOD_RD,
			  get_snapcontext, &h_get_snapcontext);
  cls_register_cxx_method(h_class, "get_object_prefix",
			  CLS_METHOD_RD,
			  get_object_prefix, &h_get_object_prefix);
  cls_register_cxx_method(h_class, "get_snapshot_name",
			  CLS_METHOD_RD,
			  get_snapshot_name, &h_get_snapshot_name);
  cls_register_cxx_method(h_class, "snapshot_add",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  snapshot_add, &h_snapshot_add);
  cls_register_cxx_method(h_class, "snapshot_remove",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  snapshot_remove, &h_snapshot_remove);
  cls_register_cxx_method(h_class, "get_all_features",
			  CLS_METHOD_RD,
			  get_all_features, &h_get_all_features);
  cls_register_cxx_method(h_class, "copyup",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  copyup, &h_copyup);
  cls_register_cxx_method(h_class, "get_parent",
			  CLS_METHOD_RD,
			  get_parent, &h_get_parent);
  cls_register_cxx_method(h_class, "set_parent",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  set_parent, &h_set_parent);
  cls_register_cxx_method(h_class, "remove_parent",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  remove_parent, &h_remove_parent);
  cls_register_cxx_method(h_class, "set_protection_status",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  set_protection_status, &h_set_protection_status);
  cls_register_cxx_method(h_class, "get_protection_status",
			  CLS_METHOD_RD,
			  get_protection_status, &h_get_protection_status);
  cls_register_cxx_method(h_class, "get_stripe_unit_count",
			  CLS_METHOD_RD,
			  get_stripe_unit_count, &h_get_stripe_unit_count);
  cls_register_cxx_method(h_class, "set_stripe_unit_count",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  set_stripe_unit_count, &h_set_stripe_unit_count);
  cls_register_cxx_method(h_class, "get_flags",
                          CLS_METHOD_RD,
                          get_flags, &h_get_flags);
  cls_register_cxx_method(h_class, "set_flags",
                          CLS_METHOD_RD | CLS_METHOD_WR,
                          set_flags, &h_set_flags);

  /* methods for the rbd_children object */
  cls_register_cxx_method(h_class, "add_child",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  add_child, &h_add_child);
  cls_register_cxx_method(h_class, "remove_child",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  remove_child, &h_remove_child);
  cls_register_cxx_method(h_class, "get_children",
			  CLS_METHOD_RD,
			  get_children, &h_get_children);

  /* methods for the rbd_id.$image_name objects */
  cls_register_cxx_method(h_class, "get_id",
			  CLS_METHOD_RD,
			  get_id, &h_get_id);
  cls_register_cxx_method(h_class, "set_id",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  set_id, &h_set_id);

  /* methods for the rbd_directory object */
  cls_register_cxx_method(h_class, "dir_get_id",
			  CLS_METHOD_RD,
			  dir_get_id, &h_dir_get_id);
  cls_register_cxx_method(h_class, "dir_get_name",
			  CLS_METHOD_RD,
			  dir_get_name, &h_dir_get_name);
  cls_register_cxx_method(h_class, "dir_list",
			  CLS_METHOD_RD,
			  dir_list, &h_dir_list);
  cls_register_cxx_method(h_class, "dir_add_image",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  dir_add_image, &h_dir_add_image);
  cls_register_cxx_method(h_class, "dir_remove_image",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  dir_remove_image, &h_dir_remove_image);
  cls_register_cxx_method(h_class, "dir_rename_image",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  dir_rename_image, &h_dir_rename_image);

  /* methods for the rbd_object_map.$image_id object */
  cls_register_cxx_method(h_class, "object_map_load",
                          CLS_METHOD_RD,
			  object_map_load, &h_object_map_load);
  cls_register_cxx_method(h_class, "object_map_resize",
                          CLS_METHOD_RD | CLS_METHOD_WR,
			  object_map_resize, &h_object_map_resize);
  cls_register_cxx_method(h_class, "object_map_update",
                          CLS_METHOD_RD | CLS_METHOD_WR,
			  object_map_update, &h_object_map_update);

  /* methods for the old format */
  cls_register_cxx_method(h_class, "snap_list",
			  CLS_METHOD_RD,
			  old_snapshots_list, &h_old_snapshots_list);
  cls_register_cxx_method(h_class, "snap_add",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  old_snapshot_add, &h_old_snapshot_add);
  cls_register_cxx_method(h_class, "snap_remove",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  old_snapshot_remove, &h_old_snapshot_remove);

  return;
}

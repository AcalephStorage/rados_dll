// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "common/Formatter.h"
#include "common/utf8.h"
#include "rgw_swift.h"
#include "rgw_rest_swift.h"
#include "rgw_acl_swift.h"
#include "rgw_cors_swift.h"
#include "rgw_formats.h"
#include "rgw_client_io.h"

#include <sstream>

#define dout_subsys ceph_subsys_rgw

int RGWListBuckets_ObjStore_SWIFT::get_params()
{
  marker = s->info.args.get("marker");
  string limit_str;
  limit_str = s->info.args.get("limit");
  long l = strtol(limit_str.c_str(), NULL, 10);
  if (l > (long)limit_max || l < 0)
    return -ERR_PRECONDITION_FAILED;

  limit = (uint64_t)l;

  if (limit == 0)
    limit = limit_max;

  need_stats = (s->format != RGW_FORMAT_PLAIN);

  if (need_stats) {
    bool stats, exists;
    int r = s->info.args.get_bool("stats", &stats, &exists);

    if (r < 0)
      return r;

    if (exists) {
      need_stats = stats;
    }
  }

  return 0;
}

void RGWListBuckets_ObjStore_SWIFT::send_response_begin(bool has_buckets)
{
  if (ret) {
    set_req_state_err(s, ret);
  } else if (!has_buckets && s->format == RGW_FORMAT_PLAIN) {
    ret = STATUS_NO_CONTENT;
    set_req_state_err(s, ret);
  }
  dump_errno(s);
  end_header(s, NULL);

  if (!ret) {
    dump_start(s);
    s->formatter->open_array_section("account");

    sent_data = true;
  }
}

void RGWListBuckets_ObjStore_SWIFT::send_response_data(RGWUserBuckets& buckets)
{
  map<string, RGWBucketEnt>& m = buckets.get_buckets();
  map<string, RGWBucketEnt>::iterator iter;

  if (!sent_data)
    return;

  for (iter = m.begin(); iter != m.end(); ++iter) {
    RGWBucketEnt obj = iter->second;
    s->formatter->open_object_section("container");
    s->formatter->dump_string("name", obj.bucket.name);
    if (need_stats) {
      s->formatter->dump_int("count", obj.count);
      s->formatter->dump_int("bytes", obj.size);
    }
    s->formatter->close_section();
    rgw_flush_formatter(s, s->formatter);
  }
}

void RGWListBuckets_ObjStore_SWIFT::send_response_end()
{
  if (sent_data) {
    s->formatter->close_section();
    rgw_flush_formatter_and_reset(s, s->formatter);
  }
}

int RGWListBucket_ObjStore_SWIFT::get_params()
{
  prefix = s->info.args.get("prefix");
  marker = s->info.args.get("marker");
  max_keys = s->info.args.get("limit");
  ret = parse_max_keys();
  if (ret < 0) {
    return ret;
  }
  if (max > default_max)
    return -ERR_PRECONDITION_FAILED;

  delimiter = s->info.args.get("delimiter");

  string path_args;
  if (s->info.args.exists("path")) { // should handle empty path
    path_args = s->info.args.get("path");
    if (!delimiter.empty() || !prefix.empty()) {
      return -EINVAL;
    }
    prefix = path_args;
    delimiter="/";

    path = prefix;
    if (path.size() && path[path.size() - 1] != '/')
      path.append("/");
  }

  int len = prefix.size();
  int delim_size = delimiter.size();
  if (len >= delim_size) {
    if (prefix.substr(len - delim_size).compare(delimiter) != 0)
      prefix.append(delimiter);
  }

  return 0;
}

void RGWListBucket_ObjStore_SWIFT::send_response()
{
  vector<RGWObjEnt>::iterator iter = objs.begin();
  map<string, bool>::iterator pref_iter = common_prefixes.begin();

  dump_start(s);

  s->formatter->open_array_section_with_attrs("container", FormatterAttrs("name", s->bucket.name.c_str(), NULL));

  while (iter != objs.end() || pref_iter != common_prefixes.end()) {
    bool do_pref = false;
    bool do_objs = false;
    rgw_obj_key& key = iter->key;
    if (pref_iter == common_prefixes.end())
      do_objs = true;
    else if (iter == objs.end())
      do_pref = true;
    else if (key.name.compare(pref_iter->first) == 0) {
      do_objs = true;
      ++pref_iter;
    } else if (key.name.compare(pref_iter->first) <= 0)
      do_objs = true;
    else
      do_pref = true;

    if (do_objs && (marker.empty() || marker < key)) {
      if (key.name.compare(path) == 0)
        goto next;

      s->formatter->open_object_section("object");
      s->formatter->dump_string("name", key.name);
      s->formatter->dump_string("hash", iter->etag);
      s->formatter->dump_int("bytes", iter->size);
      string single_content_type = iter->content_type;
      if (iter->content_type.size()) {
        // content type might hold multiple values, just dump the last one
        ssize_t pos = iter->content_type.rfind(',');
        if (pos > 0) {
          ++pos;
          while (single_content_type[pos] == ' ')
            ++pos;
          single_content_type = single_content_type.substr(pos);
        }
        s->formatter->dump_string("content_type", single_content_type);
      }
      time_t mtime = iter->mtime.sec();
      dump_time(s, "last_modified", &mtime);
      s->formatter->close_section();
    }

    if (do_pref &&  (marker.empty() || pref_iter->first.compare(marker.name) > 0)) {
      const string& name = pref_iter->first;
      if (name.compare(delimiter) == 0)
        goto next;

        s->formatter->open_object_section_with_attrs("subdir", FormatterAttrs("name", name.c_str(), NULL));

        /* swift is a bit inconsistent here */
        switch (s->format) {
          case RGW_FORMAT_XML:
            s->formatter->dump_string("name", name);
            break;
          default:
            s->formatter->dump_string("subdir", name);
        }
        s->formatter->close_section();
    }
next:
    if (do_objs)
      ++iter;
    else
      ++pref_iter;
  }

  s->formatter->close_section();

  if (!ret && s->formatter->get_len() == 0)
    ret = STATUS_NO_CONTENT;
  else if (ret > 0)
    ret = 0;

  set_req_state_err(s, ret);
  dump_errno(s);
  end_header(s, this);
  if (ret < 0) {
    return;
  }

  rgw_flush_formatter_and_reset(s, s->formatter);
}

static void dump_container_metadata(struct req_state *s, RGWBucketEnt& bucket)
{
  char buf[32];
  snprintf(buf, sizeof(buf), "%lld", (long long)bucket.count);
  s->cio->print("X-Container-Object-Count: %s\r\n", buf);
  snprintf(buf, sizeof(buf), "%lld", (long long)bucket.size);
  s->cio->print("X-Container-Bytes-Used: %s\r\n", buf);
  snprintf(buf, sizeof(buf), "%lld", (long long)bucket.size_rounded);
  s->cio->print("X-Container-Bytes-Used-Actual: %s\r\n", buf);

  if (s->object.empty()) {
    RGWAccessControlPolicy_SWIFT *swift_policy = static_cast<RGWAccessControlPolicy_SWIFT *>(s->bucket_acl);
    string read_acl, write_acl;
    swift_policy->to_str(read_acl, write_acl);
    if (read_acl.size()) {
      s->cio->print("X-Container-Read: %s\r\n", read_acl.c_str());
    }
    if (write_acl.size()) {
      s->cio->print("X-Container-Write: %s\r\n", write_acl.c_str());
    }
    if (!s->bucket_info.placement_rule.empty()) {
      s->cio->print("X-Storage-Policy: %s\r\n", s->bucket_info.placement_rule.c_str());
    }
    // Dump user-defined metadata items
    const size_t PREFIX_LEN = sizeof(RGW_ATTR_META_PREFIX) - 1;
    map<string, bufferlist>::iterator iter;
    for (iter = s->bucket_attrs.lower_bound(RGW_ATTR_META_PREFIX); iter != s->bucket_attrs.end(); ++iter) {
      const char *name = iter->first.c_str();
      if (strncmp(name, RGW_ATTR_META_PREFIX, PREFIX_LEN) == 0) {
        s->cio->print("X-Container-Meta-%s: %s\r\n", name + PREFIX_LEN, iter->second.c_str());
      } else {
        break;
      }
    }
  }
}

static void dump_account_metadata(struct req_state *s, uint32_t buckets_count,
                                  uint64_t buckets_object_count, uint64_t buckets_size, uint64_t buckets_size_rounded)
{
  char buf[32];
  snprintf(buf, sizeof(buf), "%lld", (long long)buckets_count);
  s->cio->print("X-Account-Container-Count: %s\r\n", buf);
  snprintf(buf, sizeof(buf), "%lld", (long long)buckets_object_count);
  s->cio->print("X-Account-Object-Count: %s\r\n", buf);
  snprintf(buf, sizeof(buf), "%lld", (long long)buckets_size);
  s->cio->print("X-Account-Bytes-Used: %s\r\n", buf);
  snprintf(buf, sizeof(buf), "%lld", (long long)buckets_size_rounded);
  s->cio->print("X-Account-Bytes-Used-Actual: %s\r\n", buf);
}

void RGWStatAccount_ObjStore_SWIFT::send_response()
{
  if (ret >= 0) {
    ret = STATUS_NO_CONTENT;
    dump_account_metadata(s, buckets_count, buckets_objcount, buckets_size, buckets_size_rounded);
  }

  set_req_state_err(s, ret);
  dump_errno(s);

  end_header(s, NULL);
  dump_start(s);
}

void RGWStatBucket_ObjStore_SWIFT::send_response()
{
  if (ret >= 0) {
    ret = STATUS_NO_CONTENT;
    dump_container_metadata(s, bucket);
  }

  set_req_state_err(s, ret);
  dump_errno(s);

  end_header(s, this);
  dump_start(s);
}

static int get_swift_container_settings(req_state *s, RGWRados *store, RGWAccessControlPolicy *policy, bool *has_policy,
                                        RGWCORSConfiguration *cors_config, bool *has_cors)
{
  string read_list, write_list;

  const char *read_attr = s->info.env->get("HTTP_X_CONTAINER_READ");
  if (read_attr) {
    read_list = read_attr;
  }
  const char *write_attr = s->info.env->get("HTTP_X_CONTAINER_WRITE");
  if (write_attr) {
    write_list = write_attr;
  }

  *has_policy = false;

  if (read_attr || write_attr) {
    RGWAccessControlPolicy_SWIFT swift_policy(s->cct);
    int r = swift_policy.create(store, s->user.user_id, s->user.display_name, read_list, write_list);
    if (r < 0)
      return r;

    *policy = swift_policy;
    *has_policy = true;
  }

  *has_cors = false;

  /*Check and update CORS configuration*/
  const char *allow_origins = s->info.env->get("HTTP_X_CONTAINER_META_ACCESS_CONTROL_ALLOW_ORIGIN");
  const char *allow_headers = s->info.env->get("HTTP_X_CONTAINER_META_ACCESS_CONTROL_ALLOW_HEADERS");
  const char *expose_headers = s->info.env->get("HTTP_X_CONTAINER_META_ACCESS_CONTROL_EXPOSE_HEADERS");
  const char *max_age = s->info.env->get("HTTP_X_CONTAINER_META_ACCESS_CONTROL_MAX_AGE");
  if (allow_origins) {
    RGWCORSConfiguration_SWIFT *swift_cors = new RGWCORSConfiguration_SWIFT;
    int r = swift_cors->create_update(allow_origins, allow_headers, expose_headers, max_age);
    if (r < 0) {
      dout(0) << "Error creating/updating the cors configuration" << dendl;
      delete swift_cors;
      return r;
    }
    *has_cors = true;
    *cors_config = *swift_cors;
    cors_config->dump();
    delete swift_cors;
  }

  return 0;
}

int RGWCreateBucket_ObjStore_SWIFT::get_params()
{
  bool has_policy;

  int r = get_swift_container_settings(s, store, &policy, &has_policy, &cors_config, &has_cors);
  if (r < 0) {
    return r;
  }

  if (!has_policy) {
    policy.create_default(s->user.user_id, s->user.display_name);
  }

  location_constraint = store->region.api_name;
  placement_rule = s->info.env->get("HTTP_X_STORAGE_POLICY", "");

  return 0;
}

void RGWCreateBucket_ObjStore_SWIFT::send_response()
{
  if (!ret)
    ret = STATUS_CREATED;
  else if (ret == -ERR_BUCKET_EXISTS)
    ret = STATUS_ACCEPTED;
  set_req_state_err(s, ret);
  dump_errno(s);
  end_header(s, NULL);
  rgw_flush_formatter_and_reset(s, s->formatter);
}

void RGWDeleteBucket_ObjStore_SWIFT::send_response()
{
  int r = ret;
  if (!r)
    r = STATUS_NO_CONTENT;

  set_req_state_err(s, r);
  dump_errno(s);
  end_header(s, this);
  rgw_flush_formatter_and_reset(s, s->formatter);
}

int RGWPutObj_ObjStore_SWIFT::get_params()
{
  if (s->has_bad_meta)
    return -EINVAL;

  if (!s->length) {
    const char *encoding = s->info.env->get("HTTP_TRANSFER_ENCODING");
    if (!encoding || strcmp(encoding, "chunked") != 0)
      return -ERR_LENGTH_REQUIRED;

    chunked_upload = true;
  }

  supplied_etag = s->info.env->get("HTTP_ETAG");

  if (!s->generic_attrs.count(RGW_ATTR_CONTENT_TYPE)) {
    dout(5) << "content type wasn't provided, trying to guess" << dendl;
    const char *suffix = strrchr(s->object.name.c_str(), '.');
    if (suffix) {
      suffix++;
      if (*suffix) {
        string suffix_str(suffix);
	const char *mime = rgw_find_mime_by_ext(suffix_str);
	if (mime) {
          s->generic_attrs[RGW_ATTR_CONTENT_TYPE] = mime;
	}
      }
    }
  }

  policy.create_default(s->user.user_id, s->user.display_name);

  obj_manifest = s->info.env->get("HTTP_X_OBJECT_MANIFEST");

  return RGWPutObj_ObjStore::get_params();
}

void RGWPutObj_ObjStore_SWIFT::send_response()
{
  if (!ret)
    ret = STATUS_CREATED;
  dump_etag(s, etag.c_str());
  set_req_state_err(s, ret);
  dump_errno(s);
  end_header(s, this);
  rgw_flush_formatter_and_reset(s, s->formatter);
}

#define REMOVE_ATTR_PREFIX     "HTTP_X_REMOVE_CONTAINER_META_"
#define PUT_ATTR_PREFIX        "HTTP_X_CONTAINER_META_"
#define REMOVE_ATTR_PREFIX_LEN sizeof(REMOVE_ATTR_PREFIX) - 1
#define PUT_ATTR_PREFIX_LEN    sizeof(PUT_ATTR_PREFIX) - 1

int RGWPutMetadata_ObjStore_SWIFT::get_params()
{
  if (s->has_bad_meta)
    return -EINVAL;

  if (s->object.empty()) {
    int r = get_swift_container_settings(s, store, &policy, &has_policy, &cors_config, &has_cors);
    if (r < 0) {
      return r;
    }
    map<string, string, ltstr_nocase>& m = s->info.env->get_map();
    map<string, string, ltstr_nocase>::iterator iter;
    for (iter = m.begin(); iter != m.end(); ++iter) {
      size_t prefix_len = 0;
      const char *p = iter->first.c_str();
      if (strncasecmp(p, REMOVE_ATTR_PREFIX, REMOVE_ATTR_PREFIX_LEN) == 0) {
        // Explicitly requested removal
        prefix_len = REMOVE_ATTR_PREFIX_LEN;
      } else if ((strncasecmp(p, PUT_ATTR_PREFIX, PUT_ATTR_PREFIX_LEN) == 0) && iter->second.empty()) {
        // Removal requested by putting an empty value
        prefix_len = PUT_ATTR_PREFIX_LEN;
      }
      if (prefix_len > 0) {
        string name(RGW_ATTR_META_PREFIX);
        name.append(lowercase_dash_http_attr(p + prefix_len));
        rmattr_names.insert(name);
      }
    }
  }
  placement_rule = s->info.env->get("HTTP_X_STORAGE_POLICY", "");

  return 0;
}

void RGWPutMetadata_ObjStore_SWIFT::send_response()
{
  if (!ret) {
    /* Return 204 when post metadata on a container */
    if (s->object.empty())
      ret = STATUS_NO_CONTENT;
    else
      ret = STATUS_ACCEPTED;
  }
  set_req_state_err(s, ret);
  dump_errno(s);
  end_header(s, this);
  rgw_flush_formatter_and_reset(s, s->formatter);
}

int RGWSetTempUrl_ObjStore_SWIFT::get_params()
{
  const char *temp_url = s->info.env->get("HTTP_X_ACCOUNT_META_TEMP_URL_KEY");
  if (temp_url) {
    temp_url_keys[0] = temp_url;
  }

  temp_url = s->info.env->get("HTTP_X_ACCOUNT_META_TEMP_URL_KEY_2");
  if (temp_url) {
    temp_url_keys[1] = temp_url;
  }

  if (temp_url_keys.empty())
    return -EINVAL;

  return 0;
}

void RGWSetTempUrl_ObjStore_SWIFT::send_response()
{
  int r = ret;
  if (!r)
    r = STATUS_NO_CONTENT;

  set_req_state_err(s, r);
  dump_errno(s);
  end_header(s, this);
  rgw_flush_formatter_and_reset(s, s->formatter);
}

void RGWDeleteObj_ObjStore_SWIFT::send_response()
{
  int r = ret;
  if (!r)
    r = STATUS_NO_CONTENT;

  set_req_state_err(s, r);
  dump_errno(s);
  end_header(s, this);
  rgw_flush_formatter_and_reset(s, s->formatter);
}

int RGWCopyObj_ObjStore_SWIFT::init_dest_policy()
{
  dest_policy.create_default(s->user.user_id, s->user.display_name);

  return 0;
}

int RGWCopyObj_ObjStore_SWIFT::get_params()
{
  if_mod = s->info.env->get("HTTP_IF_MODIFIED_SINCE");
  if_unmod = s->info.env->get("HTTP_IF_UNMODIFIED_SINCE");
  if_match = s->info.env->get("HTTP_COPY_IF_MATCH");
  if_nomatch = s->info.env->get("HTTP_COPY_IF_NONE_MATCH");

  src_bucket_name = s->src_bucket_name;
  src_object = s->src_object;
  dest_bucket_name = s->bucket_name_str;
  dest_object = s->object.name;

  return 0;
}

void RGWCopyObj_ObjStore_SWIFT::send_partial_response(off_t ofs)
{
  if (!sent_header) {
    if (!ret)
      ret = STATUS_CREATED;
    set_req_state_err(s, ret);
    dump_errno(s);
    end_header(s, this);

    /* Send progress information. Note that this diverge from the original swift
     * spec. We do this in order to keep connection alive.
     */
    if (ret == 0) {
      s->formatter->open_array_section("progress");
    }
    sent_header = true;
  } else {
    s->formatter->dump_int("ofs", (uint64_t)ofs);
  }
  rgw_flush_formatter(s, s->formatter);
}

void RGWCopyObj_ObjStore_SWIFT::send_response()
{
  if (!sent_header) {
   if (!ret)
      ret = STATUS_CREATED;
    set_req_state_err(s, ret);
    dump_errno(s);
    end_header(s, this);
  } else {
    s->formatter->close_section();
    rgw_flush_formatter(s, s->formatter);
  }
}

int RGWGetObj_ObjStore_SWIFT::send_response_data(bufferlist& bl, off_t bl_ofs, off_t bl_len)
{
  const char *content_type = NULL;
  map<string, string> response_attrs;
  map<string, string>::iterator riter;

  if (sent_header)
    goto send_data;

  if (range_str)
    dump_range(s, ofs, end, s->obj_size);

  dump_content_length(s, total_len);
  dump_last_modified(s, lastmod);
  s->cio->print("X-Timestamp: %lld\r\n", (long long)lastmod);

  if (!ret) {
    map<string, bufferlist>::iterator iter = attrs.find(RGW_ATTR_ETAG);
    if (iter != attrs.end()) {
      bufferlist& bl = iter->second;
      if (bl.length()) {
        char *etag = bl.c_str();
        dump_etag(s, etag);
      }
    }

    for (iter = attrs.begin(); iter != attrs.end(); ++iter) {
      const char *name = iter->first.c_str();
      map<string, string>::iterator aiter = rgw_to_http_attrs.find(name);
      if (aiter != rgw_to_http_attrs.end()) {
        if (aiter->first.compare(RGW_ATTR_CONTENT_TYPE) == 0) { // special handling for content_type
          content_type = iter->second.c_str();
          continue;
        }
        response_attrs[aiter->second] = iter->second.c_str();
      } else {
        if (strncmp(name, RGW_ATTR_META_PREFIX, sizeof(RGW_ATTR_META_PREFIX)-1) == 0) {
          name += sizeof(RGW_ATTR_META_PREFIX) - 1;
          s->cio->print("X-Object-Meta-%s: %s\r\n", name, iter->second.c_str());
        }
      }
    }
  }

  set_req_state_err(s, (partial_content && !ret) ? STATUS_PARTIAL_CONTENT : ret);
  dump_errno(s);

  for (riter = response_attrs.begin(); riter != response_attrs.end(); ++riter) {
    s->cio->print("%s: %s\r\n", riter->first.c_str(), riter->second.c_str());
  }

  if (!content_type)
    content_type = "binary/octet-stream";
  end_header(s, this, content_type);

  sent_header = true;

send_data:
  if (get_data && !ret) {
    int r = s->cio->write(bl.c_str() + bl_ofs, bl_len);
    if (r < 0)
      return r;
  }
  rgw_flush_formatter_and_reset(s, s->formatter);

  return 0;
}

void RGWOptionsCORS_ObjStore_SWIFT::send_response()
{
  string hdrs, exp_hdrs;
  uint32_t max_age = CORS_MAX_AGE_INVALID;
  /*EACCES means, there is no CORS registered yet for the bucket
   *ENOENT means, there is no match of the Origin in the list of CORSRule
   */
  if (ret == -ENOENT)
    ret = -EACCES;
  if (ret < 0) {
    set_req_state_err(s, ret);
    dump_errno(s);
    end_header(s, NULL);
    return;
  }
  get_response_params(hdrs, exp_hdrs, &max_age);
  dump_errno(s);
  dump_access_control(s, origin, req_meth, hdrs.c_str(), exp_hdrs.c_str(), max_age);
  end_header(s, NULL);
}

RGWOp *RGWHandler_ObjStore_Service_SWIFT::op_get()
{
  return new RGWListBuckets_ObjStore_SWIFT;
}

RGWOp *RGWHandler_ObjStore_Service_SWIFT::op_head()
{
  return new RGWStatAccount_ObjStore_SWIFT;
}

RGWOp *RGWHandler_ObjStore_Service_SWIFT::op_post()
{
  const char *temp_url = s->info.env->get("HTTP_X_ACCOUNT_META_TEMP_URL_KEY");
  if (temp_url) {
    return new RGWSetTempUrl_ObjStore_SWIFT;
  }
  temp_url = s->info.env->get("HTTP_X_ACCOUNT_META_TEMP_URL_KEY_2");
  if (temp_url) {
    return new RGWSetTempUrl_ObjStore_SWIFT;
  }
  return NULL;
}

RGWOp *RGWHandler_ObjStore_Bucket_SWIFT::get_obj_op(bool get_data)
{
  if (is_acl_op()) {
    return new RGWGetACLs_ObjStore_SWIFT;
  }

  if (get_data)
    return new RGWListBucket_ObjStore_SWIFT;
  else
    return new RGWStatBucket_ObjStore_SWIFT;
}

RGWOp *RGWHandler_ObjStore_Bucket_SWIFT::op_get()
{
  if (is_acl_op()) {
    return new RGWGetACLs_ObjStore_SWIFT;
  }
  return get_obj_op(true);
}

RGWOp *RGWHandler_ObjStore_Bucket_SWIFT::op_head()
{
  if (is_acl_op()) {
    return new RGWGetACLs_ObjStore_SWIFT;
  }
  return get_obj_op(false);
}

RGWOp *RGWHandler_ObjStore_Bucket_SWIFT::op_put()
{
  if (is_acl_op()) {
    return new RGWPutACLs_ObjStore_SWIFT;
  }
  return new RGWCreateBucket_ObjStore_SWIFT;
}

RGWOp *RGWHandler_ObjStore_Bucket_SWIFT::op_delete()
{
  return new RGWDeleteBucket_ObjStore_SWIFT;
}

RGWOp *RGWHandler_ObjStore_Bucket_SWIFT::op_post()
{
  return new RGWPutMetadata_ObjStore_SWIFT;
}

RGWOp *RGWHandler_ObjStore_Bucket_SWIFT::op_options()
{
  return new RGWOptionsCORS_ObjStore_SWIFT;
}

RGWOp *RGWHandler_ObjStore_Obj_SWIFT::get_obj_op(bool get_data)
{
  if (is_acl_op()) {
    return new RGWGetACLs_ObjStore_SWIFT;
  }

  RGWGetObj_ObjStore_SWIFT *get_obj_op = new RGWGetObj_ObjStore_SWIFT;
  get_obj_op->set_get_data(get_data);
  return get_obj_op;
}

RGWOp *RGWHandler_ObjStore_Obj_SWIFT::op_get()
{
  if (is_acl_op()) {
    return new RGWGetACLs_ObjStore_SWIFT;
  }
  return get_obj_op(true);
}

RGWOp *RGWHandler_ObjStore_Obj_SWIFT::op_head()
{
  if (is_acl_op()) {
    return new RGWGetACLs_ObjStore_SWIFT;
  }
  return get_obj_op(false);
}

RGWOp *RGWHandler_ObjStore_Obj_SWIFT::op_put()
{
  if (is_acl_op()) {
    return new RGWPutACLs_ObjStore_SWIFT;
  }
  if (s->src_bucket_name.empty())
    return new RGWPutObj_ObjStore_SWIFT;
  else
    return new RGWCopyObj_ObjStore_SWIFT;
}

RGWOp *RGWHandler_ObjStore_Obj_SWIFT::op_delete()
{
  return new RGWDeleteObj_ObjStore_SWIFT;
}

RGWOp *RGWHandler_ObjStore_Obj_SWIFT::op_post()
{
  return new RGWPutMetadata_ObjStore_SWIFT;
}

RGWOp *RGWHandler_ObjStore_Obj_SWIFT::op_copy()
{
  return new RGWCopyObj_ObjStore_SWIFT;
}

RGWOp *RGWHandler_ObjStore_Obj_SWIFT::op_options()
{
  return new RGWOptionsCORS_ObjStore_SWIFT;
}

int RGWHandler_ObjStore_SWIFT::authorize()
{
  if ((!s->os_auth_token && s->info.args.get("temp_url_sig").empty()) ||
      (s->op == OP_OPTIONS)) {
    /* anonymous access */
    rgw_get_anon_user(s->user);
    s->perm_mask = RGW_PERM_FULL_CONTROL;
    return 0;
  }

  bool authorized = rgw_swift->verify_swift_token(store, s);
  if (!authorized)
    return -EPERM;

  return 0;
}

int RGWHandler_ObjStore_SWIFT::validate_bucket_name(const string& bucket)
{
  int ret = RGWHandler_ObjStore::validate_bucket_name(bucket);
  if (ret < 0)
    return ret;

  int len = bucket.size();

  if (len == 0)
    return 0;

  if (bucket[0] == '.')
    return -ERR_INVALID_BUCKET_NAME;

  if (check_utf8(bucket.c_str(), len))
    return -ERR_INVALID_UTF8;

  const char *s = bucket.c_str();

  for (int i = 0; i < len; ++i, ++s) {
    if (*(unsigned char *)s == 0xff)
      return -ERR_INVALID_BUCKET_NAME;
  }

  return 0;
}

static void next_tok(string& str, string& tok, char delim)
{
  if (str.size() == 0) {
    tok = "";
    return;
  }
  tok = str;
  int pos = str.find(delim);
  if (pos > 0) {
    tok = str.substr(0, pos);
    str = str.substr(pos + 1);
  } else {
    str = "";
  }
}

int RGWHandler_ObjStore_SWIFT::init_from_header(struct req_state *s)
{
  string req;
  string first;

  s->prot_flags |= RGW_REST_SWIFT;

  const char *req_name = s->decoded_uri.c_str();
  const char *p;

  if (*req_name == '?') {
    p = req_name;
  } else {
    p = s->info.request_params.c_str();
  }

  s->info.args.set(p);
  s->info.args.parse();

  if (*req_name != '/')
    return 0;

  req_name++;

  if (!*req_name)
    return 0;

  req = req_name;

  int pos = req.find('/');
  if (pos >= 0) {
    bool cut_url = g_conf->rgw_swift_url_prefix.length();
    first = req.substr(0, pos);
    if (first.compare(g_conf->rgw_swift_url_prefix) == 0) {
      if (cut_url) {
        next_tok(req, first, '/');
      }
    }
  } else {
    if (req.compare(g_conf->rgw_swift_url_prefix) == 0) {
      s->formatter = new RGWFormatter_Plain;
      return -ERR_BAD_URL;
    }
    first = req;
  }

  string tenant_path;
  if (!g_conf->rgw_swift_tenant_name.empty()) {
    tenant_path = "/AUTH_";
    tenant_path.append(g_conf->rgw_swift_tenant_name);
  }

  /* verify that the request_uri conforms with what's expected */
  char buf[g_conf->rgw_swift_url_prefix.length() + 16 + tenant_path.length()];
  int blen = sprintf(buf, "/%s/v1%s", g_conf->rgw_swift_url_prefix.c_str(), tenant_path.c_str());
  if (s->decoded_uri[0] != '/' ||
    s->decoded_uri.compare(0, blen, buf) !=  0) {
    return -ENOENT;
  }

  int ret = allocate_formatter(s, RGW_FORMAT_PLAIN, true);
  if (ret < 0)
    return ret;

  string ver;

  next_tok(req, ver, '/');

  string tenant;
  if (!tenant_path.empty()) {
    next_tok(req, tenant, '/');
  }

  s->os_auth_token = s->info.env->get("HTTP_X_AUTH_TOKEN");
  next_tok(req, first, '/');

  dout(10) << "ver=" << ver << " first=" << first << " req=" << req << dendl;
  if (first.size() == 0)
    return 0;

  s->bucket_name_str = first;

  s->info.effective_uri = "/" + s->bucket_name_str;

  if (req.size()) {
    s->object = rgw_obj_key(req, s->info.env->get("HTTP_X_OBJECT_VERSION_ID", "")); /* rgw swift extension */
    s->info.effective_uri.append("/" + s->object.name);
  }

  return 0;
}

int RGWHandler_ObjStore_SWIFT::init(RGWRados *store, struct req_state *s, RGWClientIO *cio)
{
  dout(10) << "s->object=" << (!s->object.empty() ? s->object : rgw_obj_key("<NULL>")) << " s->bucket=" << (!s->bucket_name_str.empty() ? s->bucket_name_str : "<NULL>") << dendl;

  int ret = validate_bucket_name(s->bucket_name_str.c_str());
  if (ret)
    return ret;
  ret = validate_object_name(s->object.name);
  if (ret)
    return ret;

  s->copy_source = s->info.env->get("HTTP_X_COPY_FROM");
  if (s->copy_source) {
    bool result = RGWCopyObj::parse_copy_location(s->copy_source, s->src_bucket_name, s->src_object);
    if (!result)
       return -ERR_BAD_URL;
  }

  s->dialect = "swift";

  if (s->op == OP_COPY) {
    const char *req_dest = s->info.env->get("HTTP_DESTINATION");
    if (!req_dest)
      return -ERR_BAD_URL;

    string dest_bucket_name;
    rgw_obj_key dest_obj_key;
    bool result = RGWCopyObj::parse_copy_location(req_dest, dest_bucket_name, dest_obj_key);
    if (!result)
       return -ERR_BAD_URL;

    string dest_object = dest_obj_key.name;
    if (dest_bucket_name != s->bucket_name_str) {
      ret = validate_bucket_name(dest_bucket_name.c_str());
      if (ret < 0)
        return ret;
    }

    /* convert COPY operation into PUT */
    s->src_bucket_name = s->bucket_name_str;
    s->src_object = s->object;
    s->bucket_name_str = dest_bucket_name;
    s->object = rgw_obj_key(dest_object);
    s->op = OP_PUT;
  }

  return RGWHandler_ObjStore::init(store, s, cio);
}


RGWHandler *RGWRESTMgr_SWIFT::get_handler(struct req_state *s)
{
  int ret = RGWHandler_ObjStore_SWIFT::init_from_header(s);
  if (ret < 0)
    return NULL;

  if (s->bucket_name_str.empty())
    return new RGWHandler_ObjStore_Service_SWIFT;
  if (s->object.empty())
    return new RGWHandler_ObjStore_Bucket_SWIFT;

  return new RGWHandler_ObjStore_Obj_SWIFT;
}

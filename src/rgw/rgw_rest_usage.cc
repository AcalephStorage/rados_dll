// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "rgw_op.h"
#include "rgw_usage.h"
#include "rgw_rest_usage.h"

#include "include/str_list.h"

#define dout_subsys ceph_subsys_rgw

class RGWOp_Usage_Get : public RGWRESTOp {

public:
  RGWOp_Usage_Get() {}

  int check_caps(RGWUserCaps& caps) {
    return caps.check_cap("usage", RGW_CAP_READ);
  }
  void execute();

  virtual const string name() { return "get_usage"; }
};

void RGWOp_Usage_Get::execute() {
  map<std::string, bool> categories;

  string uid;
  uint64_t start, end;
  bool show_entries;
  bool show_summary;

  RESTArgs::get_string(s, "uid", uid, &uid);
  RESTArgs::get_epoch(s, "start", 0, &start);
  RESTArgs::get_epoch(s, "end", (uint64_t)-1, &end);
  RESTArgs::get_bool(s, "show-entries", true, &show_entries);
  RESTArgs::get_bool(s, "show-summary", true, &show_summary);

  string cat_str;
  RESTArgs::get_string(s, "categories", cat_str, &cat_str);

  if (!cat_str.empty()) {
    list<string> cat_list;
    list<string>::iterator iter;
    get_str_list(cat_str, cat_list);
    for (iter = cat_list.begin(); iter != cat_list.end(); ++iter) {
      categories[*iter] = true;
    }
  }

  http_ret = RGWUsage::show(store, uid, start, end, show_entries, show_summary, &categories, flusher);
}

class RGWOp_Usage_Delete : public RGWRESTOp {

public:
  RGWOp_Usage_Delete() {}

  int check_caps(RGWUserCaps& caps) {
    return caps.check_cap("usage", RGW_CAP_WRITE);
  }
  void execute();

  virtual const string name() { return "trim_usage"; }
};

void RGWOp_Usage_Delete::execute() {
  string uid;
  uint64_t start, end;

  RESTArgs::get_string(s, "uid", uid, &uid);
  RESTArgs::get_epoch(s, "start", 0, &start);
  RESTArgs::get_epoch(s, "end", (uint64_t)-1, &end);

  if (uid.empty() &&
      !start &&
      end == (uint64_t)-1) {
    bool remove_all;
    RESTArgs::get_bool(s, "remove-all", false, &remove_all);
    if (!remove_all) {
      http_ret = -EINVAL;
      return;
    }
  }

  http_ret = RGWUsage::trim(store, uid, start, end);
}

RGWOp *RGWHandler_Usage::op_get()
{
  return new RGWOp_Usage_Get;
}

RGWOp *RGWHandler_Usage::op_delete()
{
  return new RGWOp_Usage_Delete;
}



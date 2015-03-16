// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include "rgw_client_io.h"

#define dout_subsys ceph_subsys_rgw

void RGWClientIO::init(CephContext *cct) {
  init_env(cct);

  if (cct->_conf->subsys.should_gather(ceph_subsys_rgw, 20)) {
    std::map<string, string, ltstr_nocase>& env_map = env.get_map();
    std::map<string, string, ltstr_nocase>::iterator iter = env_map.begin();

    for (iter = env_map.begin(); iter != env_map.end(); ++iter) {
      ldout(cct, 20) << iter->first << "=" << iter->second << dendl;
    }
  }
}


int RGWClientIO::print(const char *format, ...)
{
#define LARGE_ENOUGH 128
  int size = LARGE_ENOUGH;

  va_list ap;

  while(1) {
    char buf[size];
    va_start(ap, format);
    int ret = vsnprintf(buf, size, format, ap);
    va_end(ap);

    if (ret >= 0 && ret < size) {
      return write(buf, ret);
    }

    if (ret >= 0)
      size = ret + 1;
    else
      size *= 2;
  }

  /* not reachable */
}

int RGWClientIO::write(const char *buf, int len)
{
  int ret = write_data(buf, len);
  if (ret < 0)
    return ret;

  if (account)
    bytes_sent += len;

  return 0;
}


int RGWClientIO::read(char *buf, int max, int *actual)
{
  int ret = read_data(buf, max);
  if (ret < 0)
    return ret;

  *actual = ret;

  bytes_received += *actual;

  return 0;
}


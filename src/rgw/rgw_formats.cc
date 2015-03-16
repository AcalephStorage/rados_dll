// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2011 New Dream Network
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include "common/escape.h"
#include "common/Formatter.h"
#include "rgw/rgw_common.h"
#include "rgw/rgw_formats.h"

#define LARGE_SIZE 8192

#define dout_subsys ceph_subsys_rgw

RGWFormatter_Plain::RGWFormatter_Plain()
  : buf(NULL), len(0), max_len(0), min_stack_level(0)
{
}

RGWFormatter_Plain::~RGWFormatter_Plain()
{
  free(buf);
}

void RGWFormatter_Plain::flush(ostream& os)
{
  if (!buf)
    return;

  if (len) {
    os << buf << "\n";
    os.flush();
  }

  reset_buf();
}

void RGWFormatter_Plain::reset_buf()
{
  free(buf);
  buf = NULL;
  len = 0;
  max_len = 0;
}

void RGWFormatter_Plain::reset()
{
  reset_buf();
  stack.clear();
  min_stack_level = 0;
}

void RGWFormatter_Plain::open_array_section(const char *name)
{
  struct plain_stack_entry new_entry;
  new_entry.is_array = true;
  new_entry.size = 0;
  stack.push_back(new_entry);
}

void RGWFormatter_Plain::open_array_section_in_ns(const char *name, const char *ns)
{
  ostringstream oss;
  oss << name << " " << ns;
  open_array_section(oss.str().c_str());
}

void RGWFormatter_Plain::open_object_section(const char *name)
{
  struct plain_stack_entry new_entry;
  new_entry.is_array = false;
  new_entry.size = 0;
  stack.push_back(new_entry);
}

void RGWFormatter_Plain::open_object_section_in_ns(const char *name,
						   const char *ns)
{
  ostringstream oss;
  oss << name << " " << ns;
  open_object_section(oss.str().c_str());
}

void RGWFormatter_Plain::close_section()
{
  stack.pop_back();
}

void RGWFormatter_Plain::dump_unsigned(const char *name, uint64_t u)
{
  dump_value_int(name, "%" PRIu64, u);
}

void RGWFormatter_Plain::dump_int(const char *name, int64_t u)
{
  dump_value_int(name, "%" PRId64, u);
}

void RGWFormatter_Plain::dump_float(const char *name, double d)
{
  dump_value_int(name, "%f", d);
}

void RGWFormatter_Plain::dump_string(const char *name, const std::string& s)
{
  dump_format(name, "%s", s.c_str());
}

std::ostream& RGWFormatter_Plain::dump_stream(const char *name)
{
  // TODO: implement this!
  assert(0);
}

void RGWFormatter_Plain::dump_format_va(const char *name, const char *ns, bool quoted, const char *fmt, va_list ap)
{
  char buf[LARGE_SIZE];
  const char *format;

  struct plain_stack_entry& entry = stack.back();

  if (!min_stack_level)
    min_stack_level = stack.size();

  bool should_print = (stack.size() == min_stack_level && !entry.size);

  entry.size++;

  if (!should_print)
    return;

  vsnprintf(buf, LARGE_SIZE, fmt, ap);
  if (len)
    format = "\n%s";
  else
    format = "%s";

  write_data(format, buf);
}

int RGWFormatter_Plain::get_len() const
{
  // don't include null termination in length
  return (len ? len - 1 : 0);
}

void RGWFormatter_Plain::write_raw_data(const char *data)
{
  write_data("%s", data);
}

void RGWFormatter_Plain::write_data(const char *fmt, ...)
{
#define LARGE_ENOUGH_LEN 128
  int n, size = LARGE_ENOUGH_LEN;
  char s[size + 8];
  char *p, *np;
  bool p_on_stack;
  va_list ap;
  int pos;

  p = s;
  p_on_stack = true;

  while (1) {
    va_start(ap, fmt);
    n = vsnprintf(p, size, fmt, ap);
    va_end(ap);

    if (n > -1 && n < size)
      goto done;
    /* Else try again with more space. */
    if (n > -1)    /* glibc 2.1 */
      size = n+1; /* precisely what is needed */
    else           /* glibc 2.0 */
      size *= 2;  /* twice the old size */
    if (p_on_stack)
      np = (char *)malloc(size + 8);
    else
      np = (char *)realloc(p, size + 8);
    if (!np)
      goto done_free;
    p = np;
    p_on_stack = false;
  }
done:
#define LARGE_ENOUGH_BUF 4096
  if (!buf) {
    max_len = max(LARGE_ENOUGH_BUF, size);
    buf = (char *)malloc(max_len);
    if (!buf) {
      cerr << "ERROR: RGWFormatter_Plain::write_data: failed allocating " << max_len << " bytes" << std::endl;
      goto done_free;
    }
  }

  if (len + size > max_len) {
    max_len = len + size + LARGE_ENOUGH_BUF;
    void *_realloc = NULL;
    if ((_realloc = realloc(buf, max_len)) == NULL) {
      cerr << "ERROR: RGWFormatter_Plain::write_data: failed allocating " << max_len << " bytes" << std::endl;
      goto done_free;
    } else {
      buf = (char *)_realloc;
    }
  }

  pos = len;
  if (len)
    pos--; // squash null termination
  strcpy(buf + pos, p);
  len = pos + strlen(p) + 1;
done_free:
  if (!p_on_stack)
    free(p);
}

void RGWFormatter_Plain::dump_value_int(const char *name, const char *fmt, ...)
{
  char buf[LARGE_SIZE];
  va_list ap;

  if (!min_stack_level)
    min_stack_level = stack.size();

  struct plain_stack_entry& entry = stack.back();
  bool should_print = (stack.size() == min_stack_level && !entry.size);

  entry.size++;

  if (!should_print)
    return;

  va_start(ap, fmt);
  vsnprintf(buf, LARGE_SIZE, fmt, ap);
  va_end(ap);

  const char *eol;
  if (len)
    eol = "\n";
  else
    eol = "";

  write_data("%s%s", eol, buf);
}

// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2013 Cloudwatt <libre.licensing@cloudwatt.com>
 *
 * Author: Loic Dachary <loic@dachary.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Library Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library Public License for more details.
 *
 */

#include <stdio.h>
#include <signal.h>
#include "mon/Monitor.h"
#include "common/ceph_argparse.h"
#include "global/global_init.h"

static void usage(ostream &out)
{
  out << "usage: get_command_descriptions [options ...]" << std::endl;
  out << "print on stdout the result of JSON formatted options\n";
  out << "found in mon/MonCommands.h as produced by the\n";
  out << "Monitor.cc::get_command_descriptions function.\n";
  out << "Designed as a helper for ceph_argparse.py unit tests.\n";
  out << "\n";
  out << "  --all               all of mon/MonCommands.h \n";
  out << "  --pull585           reproduce the bug fixed by #585\n";
  out << "\n";
  out << "Examples:\n";
  out << "  get_command_descriptions --all\n";
  out << "  get_command_descriptions --pull585\n";
}

static void json_print(const MonCommand *mon_commands, int size)
{
  bufferlist rdata;
  Formatter *f = Formatter::create("json");
  Monitor::format_command_descriptions(mon_commands, size, f, &rdata);
  delete f;
  string data(rdata.c_str(), rdata.length());
  cout << data << std::endl;
}

static void all()
{
#undef COMMAND
#undef COMMAND_WITH_FLAG
  MonCommand mon_commands[] = {
#define COMMAND(parsesig, helptext, modulename, req_perms, avail)	\
    {parsesig, helptext, modulename, req_perms, avail, 0},
#define COMMAND_WITH_FLAG(parsesig, helptext, modulename, req_perms, avail, flag) \
    {parsesig, helptext, modulename, req_perms, avail, MonCommand::FLAG_##flag},
#include <mon/MonCommands.h>
  };

  json_print(mon_commands, ARRAY_SIZE(mon_commands));
}

// syntax error https://github.com/ceph/ceph/pull/585
static void pull585()
{
  MonCommand mon_commands[] = {
    { "osd pool create "		       
      "name=pool,type=CephPoolname " 
      "name=pg_num,type=CephInt,range=0 " 
      "name=pgp_num,type=CephInt,range=0,req=false" // !!! missing trailing space
      "name=properties,type=CephString,n=N,req=false,goodchars=[A-Za-z0-9-_.=]", 
      "create pool", "osd", "rw", "cli,rest" }
  };

  json_print(mon_commands, ARRAY_SIZE(mon_commands));
}

int main(int argc, char **argv) {
  vector<const char*> args;
  argv_to_vec(argc, (const char **)argv, args);

  global_init(NULL, args, CEPH_ENTITY_TYPE_CLIENT, CODE_ENVIRONMENT_UTILITY, 0);
  common_init_finish(g_ceph_context);

  if (args.empty()) {
    usage(cerr);
    exit(1);
  }
  for (std::vector<const char*>::iterator i = args.begin(); i != args.end(); ++i) {
    string err;

    if (*i == string("help") || *i == string("-h") || *i == string("--help")) {
      usage(cout);
      exit(0);
    } else if (*i == string("--all")) {
      all();
    } else if (*i == string("--pull585")) {
      pull585();
    }
  }  
}

/*
 * Local Variables:
 * compile-command: "cd ../.. ; 
 *   make get_command_descriptions && 
 *   ./get_command_descriptions --all --pull585"
 * End:
 */


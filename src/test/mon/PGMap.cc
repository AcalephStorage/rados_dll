// -*- mode:C; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2014 Inktank <info@inktank.com>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License version 2, as published by the Free Software
 * Foundation.  See file COPYING.
 */

#include "mon/PGMap.h"
#include "gtest/gtest.h"

#include "common/ceph_argparse.h"
#include "global/global_init.h"
#include "global/global_context.h"

TEST(pgmap, min_last_epoch_clean)
{
  PGMap pg_map;
  PGMap::Incremental inc;
  osd_stat_t os;
  pg_stat_t ps;

  ps.last_epoch_clean = 999;
  inc.pg_stat_updates[pg_t(9,9)] = ps;
  inc.version = 1;
  inc.update_stat(0, 123, os);
  pg_map.apply_incremental(g_ceph_context, inc);
  ASSERT_EQ(123u, pg_map.get_min_last_epoch_clean());

  inc = PGMap::Incremental();
  inc.version = 2;
  inc.update_stat(1, 222, os);
  pg_map.apply_incremental(g_ceph_context, inc);
  ASSERT_EQ(123u, pg_map.get_min_last_epoch_clean());

  inc = PGMap::Incremental();
  inc.version = 3;
  inc.update_stat(0, 222, os);
  pg_map.apply_incremental(g_ceph_context, inc);
  ASSERT_EQ(222u, pg_map.get_min_last_epoch_clean());

  inc = PGMap::Incremental();
  inc.version = 4;
  inc.update_stat(0, 333, os);
  inc.update_stat(1, 333, os);
  pg_map.apply_incremental(g_ceph_context, inc);
  ASSERT_EQ(333u, pg_map.get_min_last_epoch_clean());

  ps.last_epoch_clean = 222;
  inc = PGMap::Incremental();
  inc.version = 5;
  inc.pg_stat_updates[pg_t(1,1)] = ps;
  pg_map.apply_incremental(g_ceph_context, inc);
  ASSERT_EQ(222u, pg_map.get_min_last_epoch_clean());

  ps.last_epoch_clean = 223;
  inc = PGMap::Incremental();
  inc.version = 6;
  inc.pg_stat_updates[pg_t(1,1)] = ps;
  pg_map.apply_incremental(g_ceph_context, inc);
  ASSERT_EQ(223u, pg_map.get_min_last_epoch_clean());

  ps.last_epoch_clean = 224;
  inc = PGMap::Incremental();
  inc.version = 7;
  inc.pg_stat_updates[pg_t(2,2)] = ps;
  pg_map.apply_incremental(g_ceph_context, inc);
  ASSERT_EQ(223u, pg_map.get_min_last_epoch_clean());

  ps.last_epoch_clean = 225;
  inc = PGMap::Incremental();
  inc.version = 8;
  inc.pg_stat_updates[pg_t(1,1)] = ps;
  pg_map.apply_incremental(g_ceph_context, inc);
  ASSERT_EQ(224u, pg_map.get_min_last_epoch_clean());

}



int main(int argc, char **argv) {
  vector<const char*> args;
  argv_to_vec(argc, (const char **)argv, args);
  env_to_vec(args);

  vector<const char*> def_args;
  global_init(&def_args, args, CEPH_ENTITY_TYPE_CLIENT, CODE_ENVIRONMENT_UTILITY, 0);
  common_init_finish(g_ceph_context);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

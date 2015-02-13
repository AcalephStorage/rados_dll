// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph distributed storage system
 *
 * Copyright (C) 2014 Red Hat <contact@redhat.com>
 *
 * Author: Loic Dachary <loic@dachary.org>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 */

#include <stdio.h>

#include "arch/probe.h"
#include "arch/intel.h"
#include "arch/arm.h"
#include "global/global_init.h"
#include "common/ceph_argparse.h"
#include "global/global_context.h"
#include "gtest/gtest.h"

#define FLAGS_SIZE 4096

TEST(Arch, all)
{
  ceph_arch_probe();
  EXPECT_TRUE(ceph_arch_probed);
  
#if (__arm__ || __aarch64__ || __x86_64__) && __linux__
  char flags[FLAGS_SIZE];
  FILE *f = popen("grep '^\\(flags\\|Features\\)[	 ]*:' "
                  "/proc/cpuinfo | head -1", "r");
  if(f == NULL || fgets(flags, FLAGS_SIZE - 1, f) == NULL) {
    // silently do nothing if /proc/cpuinfo does exist, is not
    // readable or does not contain the expected information
    if (f)
      pclose(f);
    return;
  }
  pclose(f);
  flags[strlen(flags) - 1] = ' ';

  int expected;

  expected = (strstr(flags, " neon ") || strstr(flags, " asimd ")) ? 1 : 0;
  EXPECT_EQ(expected, ceph_arch_neon);

  expected = strstr(flags, " pclmulqdq ") ? 1 : 0;
  EXPECT_EQ(expected, ceph_arch_intel_pclmul);

  expected = strstr(flags, " sse4_2 ") ? 1 : 0;
  EXPECT_EQ(expected, ceph_arch_intel_sse42);

  expected = strstr(flags, " sse4_1 ") ? 1 : 0;
  EXPECT_EQ(expected, ceph_arch_intel_sse41);

  expected = (strstr(flags, " sse3 ") || strstr(flags, " ssse3 ")) ? 1 : 0;
  EXPECT_EQ(expected, ceph_arch_intel_sse3);

  expected = strstr(flags, " ssse3 ") ? 1 : 0;
  EXPECT_EQ(expected, ceph_arch_intel_ssse3);

  expected = strstr(flags, " sse2 ") ? 1 : 0;
  EXPECT_EQ(expected, ceph_arch_intel_sse2);
#endif
}

int main(int argc, char **argv)
{
  vector<const char*> args;
  argv_to_vec(argc, (const char **)argv, args);

  global_init(NULL, args, CEPH_ENTITY_TYPE_CLIENT, CODE_ENVIRONMENT_UTILITY, 0);
  common_init_finish(g_ceph_context);

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}


/*
 * Local Variables:
 * compile-command: "cd .. ; make -j4 &&
 *   make unittest_arch &&
 *   valgrind --tool=memcheck ./unittest_arch --gtest_filter=*.*"
 * End:
 */

// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph distributed storage system
 *
 * Copyright (C) 2013,2014 Cloudwatt <libre.licensing@cloudwatt.com>
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

#include <errno.h>
#include <signal.h>
#include "common/Thread.h"
#include "global/global_init.h"
#include "erasure-code/ErasureCodePlugin.h"
#include "common/ceph_argparse.h"
#include "global/global_context.h"
#include "gtest/gtest.h"

class ErasureCodePluginRegistryTest : public ::testing::Test {
protected:

  class Thread_factory : public Thread {
  public:
    virtual void *entry() {
      map<std::string,std::string> parameters;
      parameters["directory"] = ".libs";
      ErasureCodePluginRegistry &instance = ErasureCodePluginRegistry::instance();
      ErasureCodeInterfaceRef erasure_code;
      stringstream ss;
      instance.factory("hangs", parameters, &erasure_code, ss);
      return NULL;
    }
  };

};

TEST_F(ErasureCodePluginRegistryTest, factory_mutex) {
  ErasureCodePluginRegistry &instance = ErasureCodePluginRegistry::instance();

  EXPECT_TRUE(instance.lock.TryLock());
  instance.lock.Unlock();

  // 
  // Test that the loading of a plugin is protected by a mutex.
  //
  useconds_t delay = 0;
  const useconds_t DELAY_MAX = 20 * 1000 * 1000;
  Thread_factory sleep_forever;
  sleep_forever.create();
  do {
    cout << "Trying (1) with delay " << delay << "us\n";
    if (delay > 0)
      usleep(delay);
    if (!instance.loading)
      delay = ( delay + 1 ) * 2;
  } while(!instance.loading && delay < DELAY_MAX);
  ASSERT_TRUE(delay < DELAY_MAX);

  EXPECT_FALSE(instance.lock.TryLock());

  EXPECT_EQ(0, pthread_cancel(sleep_forever.get_thread_id()));
  EXPECT_EQ(0, sleep_forever.join());
}

TEST_F(ErasureCodePluginRegistryTest, all)
{
  map<std::string,std::string> parameters;
  string directory(".libs");
  parameters["directory"] = directory;
  ErasureCodeInterfaceRef erasure_code;
  ErasureCodePluginRegistry &instance = ErasureCodePluginRegistry::instance();
  stringstream ss;
  EXPECT_FALSE(erasure_code);
  EXPECT_EQ(-EIO, instance.factory("invalid", parameters, &erasure_code, ss));
  EXPECT_FALSE(erasure_code);
  EXPECT_EQ(-EXDEV, instance.factory("missing_version", parameters,
				     &erasure_code, ss));
  EXPECT_FALSE(erasure_code);
  EXPECT_EQ(-ENOENT, instance.factory("missing_entry_point", parameters,
				      &erasure_code, ss));
  EXPECT_FALSE(erasure_code);
  EXPECT_EQ(-ESRCH, instance.factory("fail_to_initialize", parameters,
				     &erasure_code, ss));
  EXPECT_FALSE(erasure_code);
  EXPECT_EQ(-EBADF, instance.factory("fail_to_register", parameters,
				     &erasure_code, ss));
  EXPECT_FALSE(erasure_code);
  EXPECT_EQ(0, instance.factory("example", parameters, &erasure_code, ss));
  EXPECT_TRUE(erasure_code);
  ErasureCodePlugin *plugin = 0;
  {
    Mutex::Locker l(instance.lock);
    EXPECT_EQ(-EEXIST, instance.load("example", directory, &plugin, ss));
    EXPECT_EQ(-ENOENT, instance.remove("does not exist"));
    EXPECT_EQ(0, instance.remove("example"));
    EXPECT_EQ(0, instance.load("example", directory, &plugin, ss));
  }
}

int main(int argc, char **argv) {
  vector<const char*> args;
  argv_to_vec(argc, (const char **)argv, args);

  global_init(NULL, args, CEPH_ENTITY_TYPE_CLIENT, CODE_ENVIRONMENT_UTILITY, 0);
  common_init_finish(g_ceph_context);

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

/*
 * Local Variables:
 * compile-command: "cd ../.. ; make -j4 && 
 *   make unittest_erasure_code_plugin && 
 *   valgrind  --leak-check=full --tool=memcheck \
 *      ./unittest_erasure_code_plugin \
 *      --gtest_filter=*.* --log-to-stderr=true --debug-osd=20"
 * End:
 */

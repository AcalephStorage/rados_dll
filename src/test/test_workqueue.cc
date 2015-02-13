#include "gtest/gtest.h"

#include "common/WorkQueue.h"
#include "global/global_context.h"
#include "common/ceph_argparse.h"
#include "global/global_init.h"
#include "common/common_init.h"

TEST(WorkQueue, StartStop)
{
  ThreadPool tp(g_ceph_context, "foo", 10, "");
  
  tp.start();
  tp.pause();
  tp.pause_new();
  tp.unpause();
  tp.unpause();
  tp.drain();
  tp.stop();
}

TEST(WorkQueue, Resize)
{
  ThreadPool tp(g_ceph_context, "bar", 2, "osd_op_threads");
  
  tp.start();

  sleep(1);
  ASSERT_EQ(2, tp.get_num_threads());

  g_conf->set_val("osd op threads", "5");
  g_conf->apply_changes(&cout);
  sleep(1);
  ASSERT_EQ(5, tp.get_num_threads());

  g_conf->set_val("osd op threads", "3");
  g_conf->apply_changes(&cout);
  sleep(1);
  ASSERT_EQ(3, tp.get_num_threads());

  g_conf->set_val("osd op threads", "15");
  g_conf->apply_changes(&cout);
  sleep(1);
  ASSERT_EQ(15, tp.get_num_threads());

  g_conf->set_val("osd op threads", "0");
  g_conf->apply_changes(&cout);
  sleep(1);
  ASSERT_EQ(15, tp.get_num_threads());

  g_conf->set_val("osd op threads", "-1");
  g_conf->apply_changes(&cout);
  sleep(1);
  ASSERT_EQ(15, tp.get_num_threads());

  sleep(1);
  tp.stop();
}


int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);

  vector<const char*> args;
  argv_to_vec(argc, (const char **)argv, args);

  global_init(NULL, args, CEPH_ENTITY_TYPE_CLIENT, CODE_ENVIRONMENT_UTILITY, 0);
  common_init_finish(g_ceph_context);

  return RUN_ALL_TESTS();
}

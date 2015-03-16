#include "gtest/gtest.h"
#include "global/global_context.h"
#include "common/ceph_argparse.h"
#include "global/global_init.h"
#include "common/common_init.h"

#include "osdc/Striper.h"

TEST(Striper, Stripe1)
{
  ceph_file_layout l;
  memset(&l, 0, sizeof(l));

  l.fl_object_size = 262144;
  l.fl_stripe_unit = 4096;
  l.fl_stripe_count = 3;

  vector<ObjectExtent> ex;
  Striper::file_to_extents(g_ceph_context, 1, &l, 5006035, 46419, 5006035, ex);

  cout << "result " << ex << std::endl;

  ASSERT_EQ(3u, ex.size());
  ASSERT_EQ(98304u, ex[0].truncate_size);
  ASSERT_EQ(ex[1].offset, ex[1].truncate_size);
  ASSERT_EQ(94208u, ex[2].truncate_size);
}

TEST(Striper, EmptyPartialResult)
{
  ceph_file_layout l;
  memset(&l, 0, sizeof(l));

  l.fl_object_size = 4194304;
  l.fl_stripe_unit = 4194304;
  l.fl_stripe_count = 1;

  vector<ObjectExtent> ex;
  Striper::file_to_extents(g_ceph_context, 1, &l, 725549056, 131072, 72554905600, ex);
  cout << "ex " << ex << std::endl;
  ASSERT_EQ(2u, ex.size());

  Striper::StripedReadResult r;

  bufferlist bl;
  r.add_partial_result(g_ceph_context, bl, ex[1].buffer_extents);

  bufferptr bp(65536);
  bp.zero();
  bl.append(bp);

  r.add_partial_result(g_ceph_context, bl, ex[0].buffer_extents);

  bufferlist outbl;
  r.assemble_result(g_ceph_context, outbl, false);

  ASSERT_EQ(65536u, outbl.length());
}

TEST(Striper, GetNumObj)
{
  ceph_file_layout l;
  memset(&l, 0, sizeof(l));

  l.fl_object_size = 262144;
  l.fl_stripe_unit = 4096;
  l.fl_stripe_count = 3;
  uint64_t size,numobjs;
  size = 6999;
  numobjs = Striper::get_num_objects(l, size);
  ASSERT_EQ(2u, numobjs);
  size = 793320;
  numobjs = Striper::get_num_objects(l, size);
  ASSERT_EQ(5u, numobjs);
  size = 805608;
  numobjs = Striper::get_num_objects(l, size);
  ASSERT_EQ(6u, numobjs);
}


int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);

  vector<const char*> args;
  argv_to_vec(argc, (const char **)argv, args);
  env_to_vec(args);

  global_init(NULL, args, CEPH_ENTITY_TYPE_CLIENT, CODE_ENVIRONMENT_UTILITY, 0);
  common_init_finish(g_ceph_context);

  return RUN_ALL_TESTS();
}

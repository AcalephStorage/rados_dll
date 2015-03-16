// -*- mode:C; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "common/snap_types.h"
#include "include/encoding.h"
#include "include/types.h"
#include "include/rados/librados.h"
#include "include/stringify.h"
#include "cls/rbd/cls_rbd.h"
#include "cls/rbd/cls_rbd_client.h"

#include "gtest/gtest.h"
#include "test/librados/test.h"

#include <errno.h>
#include <string>
#include <vector>

using namespace std;
using ::librbd::cls_client::create_image;
using ::librbd::cls_client::get_features;
using ::librbd::cls_client::get_size;
using ::librbd::cls_client::get_object_prefix;
using ::librbd::cls_client::set_size;
using ::librbd::cls_client::get_parent;
using ::librbd::cls_client::set_parent;
using ::librbd::cls_client::remove_parent;
using ::librbd::cls_client::snapshot_add;
using ::librbd::cls_client::snapshot_remove;
using ::librbd::cls_client::add_child;
using ::librbd::cls_client::remove_child;
using ::librbd::cls_client::get_children;
using ::librbd::cls_client::get_snapcontext;
using ::librbd::cls_client::snapshot_list;
using ::librbd::cls_client::copyup;
using ::librbd::cls_client::get_id;
using ::librbd::cls_client::set_id;
using ::librbd::cls_client::dir_get_id;
using ::librbd::cls_client::dir_get_name;
using ::librbd::cls_client::dir_list;
using ::librbd::cls_client::dir_add_image;
using ::librbd::cls_client::dir_remove_image;
using ::librbd::cls_client::dir_rename_image;
using ::librbd::parent_info;
using ::librbd::parent_spec;
using ::librbd::cls_client::get_protection_status;
using ::librbd::cls_client::set_protection_status;
using ::librbd::cls_client::get_stripe_unit_count;
using ::librbd::cls_client::set_stripe_unit_count;
using ::librbd::cls_client::old_snapshot_add;
using ::librbd::cls_client::get_mutable_metadata;
using ::librbd::cls_client::object_map_load;
using ::librbd::cls_client::object_map_resize;
using ::librbd::cls_client::object_map_update;
using ::librbd::cls_client::get_flags;
using ::librbd::cls_client::set_flags;

static char *random_buf(size_t len)
{
  char *b = new char[len];
  for (size_t i = 0; i < len; i++)
    b[i] = (rand() % (128 - 32)) + 32;
  return b;
}

class TestClsRbd : public ::testing::Test {
public:

  static void SetUpTestCase() {
    _pool_name = get_temp_pool_name();
    ASSERT_EQ("", create_one_pool_pp(_pool_name, _rados));
  }

  static void TearDownTestCase() {
    ASSERT_EQ(0, destroy_one_pool_pp(_pool_name, _rados));
  }

  std::string get_temp_image_name() {
    ++_image_number;
    return "image" + stringify(_image_number);
  }

  static std::string _pool_name;
  static librados::Rados _rados;
  static uint64_t _image_number;

};

std::string TestClsRbd::_pool_name;
librados::Rados TestClsRbd::_rados;
uint64_t TestClsRbd::_image_number = 0;

TEST_F(TestClsRbd, copyup)
{
  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(_pool_name.c_str(), ioctx));

  string oid = get_temp_image_name();
  bufferlist inbl, outbl;

  // copyup of 0-len nonexistent object should create new 0-len object
  ioctx.remove(oid);
  ASSERT_EQ(0, copyup(&ioctx, oid, inbl));
  uint64_t size;
  ASSERT_EQ(0, ioctx.stat(oid, &size, NULL));
  ASSERT_EQ(0U, size);

  // create some random data to write
  size_t l = 4 << 20;
  char *b = random_buf(l);
  inbl.append(b, l);
  delete [] b;
  ASSERT_EQ(l, inbl.length());

  // copyup to nonexistent object should create new object
  ioctx.remove(oid);
  ASSERT_EQ(-ENOENT, ioctx.remove(oid));
  ASSERT_EQ(0, copyup(&ioctx, oid, inbl));
  // and its contents should match
  ASSERT_EQ(l, (size_t)ioctx.read(oid, outbl, l, 0));
  ASSERT_TRUE(outbl.contents_equal(inbl));

  // now send different data, but with a preexisting object
  bufferlist inbl2;
  b = random_buf(l);
  inbl2.append(b, l);
  delete [] b;
  ASSERT_EQ(l, inbl2.length());

  // should still succeed
  ASSERT_EQ(0, copyup(&ioctx, oid, inbl));
  ASSERT_EQ(l, (size_t)ioctx.read(oid, outbl, l, 0));
  // but contents should not have changed
  ASSERT_FALSE(outbl.contents_equal(inbl2));
  ASSERT_TRUE(outbl.contents_equal(inbl));

  ASSERT_EQ(0, ioctx.remove(oid));
  ioctx.close();
}

TEST_F(TestClsRbd, get_and_set_id)
{
  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(_pool_name.c_str(), ioctx));

  string oid = get_temp_image_name();
  string id;
  string valid_id = "0123abcxyzZYXCBA";
  string invalid_id = ".abc";
  string empty_id;

  ASSERT_EQ(-ENOENT, get_id(&ioctx, oid, &id));
  ASSERT_EQ(-ENOENT, set_id(&ioctx, oid, valid_id));

  ASSERT_EQ(0, ioctx.create(oid, true));
  ASSERT_EQ(-EINVAL, set_id(&ioctx, oid, invalid_id));
  ASSERT_EQ(-EINVAL, set_id(&ioctx, oid, empty_id));
  ASSERT_EQ(-ENOENT, get_id(&ioctx, oid, &id));

  ASSERT_EQ(0, set_id(&ioctx, oid, valid_id));
  ASSERT_EQ(-EEXIST, set_id(&ioctx, oid, valid_id));
  ASSERT_EQ(-EEXIST, set_id(&ioctx, oid, valid_id + valid_id));
  ASSERT_EQ(0, get_id(&ioctx, oid, &id));
  ASSERT_EQ(id, valid_id);

  ioctx.close();
}

TEST_F(TestClsRbd, add_remove_child)
{
  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(_pool_name.c_str(), ioctx));

  string oid = get_temp_image_name();
  ASSERT_EQ(0, ioctx.create(oid, true));

  string snapname = "parent_snap";
  snapid_t snapid(10);
  string parent_image = "parent_id";
  set<string>children;
  parent_spec pspec(ioctx.get_id(), parent_image, snapid);

  // nonexistent children cannot be listed or removed
  ASSERT_EQ(-ENOENT, get_children(&ioctx, oid, pspec, children));
  ASSERT_EQ(-ENOENT, remove_child(&ioctx, oid, pspec, "child1"));

  // create the parent and snapshot
  ASSERT_EQ(0, create_image(&ioctx, parent_image, 2<<20, 0,
			    RBD_FEATURE_LAYERING, parent_image));
  ASSERT_EQ(0, snapshot_add(&ioctx, parent_image, snapid, snapname));

  // add child to it, verify it showed up
  ASSERT_EQ(0, add_child(&ioctx, oid, pspec, "child1"));
  ASSERT_EQ(0, get_children(&ioctx, oid, pspec, children));
  ASSERT_TRUE(children.find("child1") != children.end());
  // add another child to it, verify it showed up
  ASSERT_EQ(0, add_child(&ioctx, oid, pspec, "child2"));
  ASSERT_EQ(0, get_children(&ioctx, oid, pspec, children));
  ASSERT_TRUE(children.find("child2") != children.end());
  // add child2 again, expect -EEXIST
  ASSERT_EQ(-EEXIST, add_child(&ioctx, oid, pspec, "child2"));
  // remove first, verify it's gone
  ASSERT_EQ(0, remove_child(&ioctx, oid, pspec, "child1"));
  ASSERT_EQ(0, get_children(&ioctx, oid, pspec, children));
  ASSERT_FALSE(children.find("child1") != children.end());
  // remove second, verify list empty
  ASSERT_EQ(0, remove_child(&ioctx, oid, pspec, "child2"));
  ASSERT_EQ(-ENOENT, get_children(&ioctx, oid, pspec, children));
  // try to remove again, validate -ENOENT to that as well
  ASSERT_EQ(-ENOENT, remove_child(&ioctx, oid, pspec, "child2"));

  ioctx.close();
}

TEST_F(TestClsRbd, directory_methods)
{
  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(_pool_name.c_str(), ioctx));

  string oid = get_temp_image_name();
  string id, name;
  string imgname = get_temp_image_name();
  string imgname2 = get_temp_image_name();
  string imgname3 = get_temp_image_name();
  string valid_id = "0123abcxyzZYXCBA";
  string valid_id2 = "5";
  string invalid_id = ".abc";
  string empty;

  ASSERT_EQ(-ENOENT, dir_get_id(&ioctx, oid, imgname, &id));
  ASSERT_EQ(-ENOENT, dir_get_name(&ioctx, oid, valid_id, &name));
  ASSERT_EQ(-ENOENT, dir_remove_image(&ioctx, oid, imgname, valid_id));

  ASSERT_EQ(-EINVAL, dir_add_image(&ioctx, oid, imgname, invalid_id));
  ASSERT_EQ(-EINVAL, dir_add_image(&ioctx, oid, imgname, empty));
  ASSERT_EQ(-EINVAL, dir_add_image(&ioctx, oid, empty, valid_id));

  map<string, string> images;
  ASSERT_EQ(-ENOENT, dir_list(&ioctx, oid, "", 30, &images));

  ASSERT_EQ(0, ioctx.create(oid, true));
  ASSERT_EQ(0, dir_list(&ioctx, oid, "", 30, &images));
  ASSERT_EQ(0u, images.size());
  ASSERT_EQ(0, ioctx.remove(oid));

  ASSERT_EQ(0, dir_add_image(&ioctx, oid, imgname, valid_id));
  ASSERT_EQ(-EEXIST, dir_add_image(&ioctx, oid, imgname, valid_id2));
  ASSERT_EQ(-EBADF, dir_add_image(&ioctx, oid, imgname2, valid_id));
  ASSERT_EQ(0, dir_list(&ioctx, oid, "", 30, &images));
  ASSERT_EQ(1u, images.size());
  ASSERT_EQ(valid_id, images[imgname]);
  ASSERT_EQ(0, dir_list(&ioctx, oid, "", 0, &images));
  ASSERT_EQ(0u, images.size());
  ASSERT_EQ(0, dir_get_name(&ioctx, oid, valid_id, &name));
  ASSERT_EQ(imgname, name);
  ASSERT_EQ(0, dir_get_id(&ioctx, oid, imgname, &id));
  ASSERT_EQ(valid_id, id);

  ASSERT_EQ(0, dir_add_image(&ioctx, oid, imgname2, valid_id2));
  ASSERT_EQ(0, dir_list(&ioctx, oid, "", 30, &images));
  ASSERT_EQ(2u, images.size());
  ASSERT_EQ(valid_id, images[imgname]);
  ASSERT_EQ(valid_id2, images[imgname2]);
  ASSERT_EQ(0, dir_list(&ioctx, oid, imgname, 0, &images));
  ASSERT_EQ(0u, images.size());
  ASSERT_EQ(0, dir_list(&ioctx, oid, imgname, 2, &images));
  ASSERT_EQ(1u, images.size());
  ASSERT_EQ(valid_id2, images[imgname2]);
  ASSERT_EQ(0, dir_get_name(&ioctx, oid, valid_id2, &name));
  ASSERT_EQ(imgname2, name);
  ASSERT_EQ(0, dir_get_id(&ioctx, oid, imgname2, &id));
  ASSERT_EQ(valid_id2, id);

  ASSERT_EQ(-ESTALE, dir_rename_image(&ioctx, oid, imgname, imgname2, valid_id2));
  ASSERT_EQ(-ESTALE, dir_remove_image(&ioctx, oid, imgname, valid_id2));
  ASSERT_EQ(-EEXIST, dir_rename_image(&ioctx, oid, imgname, imgname2, valid_id));
  ASSERT_EQ(0, dir_get_id(&ioctx, oid, imgname, &id));
  ASSERT_EQ(valid_id, id);
  ASSERT_EQ(0, dir_get_name(&ioctx, oid, valid_id2, &name));
  ASSERT_EQ(imgname2, name);

  ASSERT_EQ(0, dir_rename_image(&ioctx, oid, imgname, imgname3, valid_id));
  ASSERT_EQ(0, dir_get_id(&ioctx, oid, imgname3, &id));
  ASSERT_EQ(valid_id, id);
  ASSERT_EQ(0, dir_get_name(&ioctx, oid, valid_id, &name));
  ASSERT_EQ(imgname3, name);
  ASSERT_EQ(0, dir_rename_image(&ioctx, oid, imgname3, imgname, valid_id));

  ASSERT_EQ(0, dir_remove_image(&ioctx, oid, imgname, valid_id));
  ASSERT_EQ(0, dir_list(&ioctx, oid, "", 30, &images));
  ASSERT_EQ(1u, images.size());
  ASSERT_EQ(valid_id2, images[imgname2]);
  ASSERT_EQ(0, dir_list(&ioctx, oid, imgname2, 30, &images));
  ASSERT_EQ(0u, images.size());
  ASSERT_EQ(0, dir_get_name(&ioctx, oid, valid_id2, &name));
  ASSERT_EQ(imgname2, name);
  ASSERT_EQ(0, dir_get_id(&ioctx, oid, imgname2, &id));
  ASSERT_EQ(valid_id2, id);
  ASSERT_EQ(-ENOENT, dir_get_name(&ioctx, oid, valid_id, &name));
  ASSERT_EQ(-ENOENT, dir_get_id(&ioctx, oid, imgname, &id));

  ASSERT_EQ(0, dir_add_image(&ioctx, oid, imgname, valid_id));
  ASSERT_EQ(0, dir_list(&ioctx, oid, "", 30, &images));
  ASSERT_EQ(2u, images.size());
  ASSERT_EQ(valid_id, images[imgname]);
  ASSERT_EQ(valid_id2, images[imgname2]);
  ASSERT_EQ(0, dir_remove_image(&ioctx, oid, imgname, valid_id));
  ASSERT_EQ(-ENOENT, dir_remove_image(&ioctx, oid, imgname, valid_id));
  ASSERT_EQ(0, dir_remove_image(&ioctx, oid, imgname2, valid_id2));
  ASSERT_EQ(0, dir_list(&ioctx, oid, "", 30, &images));
  ASSERT_EQ(0u, images.size());

  ioctx.close();
}

TEST_F(TestClsRbd, create)
{
  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(_pool_name.c_str(), ioctx));

  string oid = get_temp_image_name();
  uint64_t size = 20ULL << 30;
  uint64_t features = 0;
  uint8_t order = 22;
  string object_prefix = oid;

  ASSERT_EQ(0, create_image(&ioctx, oid, size, order,
			    features, object_prefix));
  ASSERT_EQ(-EEXIST, create_image(&ioctx, oid, size, order,
				  features, object_prefix));
  ASSERT_EQ(0, ioctx.remove(oid));

  ASSERT_EQ(-EINVAL, create_image(&ioctx, oid, size, order,
				  features, ""));
  ASSERT_EQ(-ENOENT, ioctx.remove(oid));

  ASSERT_EQ(0, create_image(&ioctx, oid, 0, order,
			    features, object_prefix));
  ASSERT_EQ(0, ioctx.remove(oid));

  ASSERT_EQ(-ENOSYS, create_image(&ioctx, oid, size, order,
				  -1, object_prefix));
  ASSERT_EQ(-ENOENT, ioctx.remove(oid));

  bufferlist inbl, outbl;
  ASSERT_EQ(-EINVAL, ioctx.exec(oid, "rbd", "create", inbl, outbl));

  ioctx.close();
}

TEST_F(TestClsRbd, get_features)
{
  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(_pool_name.c_str(), ioctx));

  string oid = get_temp_image_name();

  uint64_t features;
  ASSERT_EQ(-ENOENT, get_features(&ioctx, oid, CEPH_NOSNAP, &features));

  ASSERT_EQ(0, create_image(&ioctx, oid, 0, 22, 0, oid));
  ASSERT_EQ(0, get_features(&ioctx, oid, CEPH_NOSNAP, &features));
  ASSERT_EQ(0u, features);

  ASSERT_EQ(-ENOENT, get_features(&ioctx, oid, 1, &features));

  ioctx.close();
}

TEST_F(TestClsRbd, get_object_prefix)
{
  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(_pool_name.c_str(), ioctx));

  string oid = get_temp_image_name();

  string object_prefix;
  ASSERT_EQ(-ENOENT, get_object_prefix(&ioctx, oid, &object_prefix));

  ASSERT_EQ(0, create_image(&ioctx, oid, 0, 22, 0, oid));
  ASSERT_EQ(0, get_object_prefix(&ioctx, oid, &object_prefix));
  ASSERT_EQ(oid, object_prefix);

  ioctx.close();
}

TEST_F(TestClsRbd, get_size)
{
  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(_pool_name.c_str(), ioctx));

  string oid = get_temp_image_name();
  uint64_t size;
  uint8_t order;
  ASSERT_EQ(-ENOENT, get_size(&ioctx, oid, CEPH_NOSNAP, &size, &order));

  ASSERT_EQ(0, create_image(&ioctx, oid, 0, 22, 0, oid));
  ASSERT_EQ(0, get_size(&ioctx, oid, CEPH_NOSNAP, &size, &order));
  ASSERT_EQ(0u, size);
  ASSERT_EQ(22, order);
  ASSERT_EQ(0, ioctx.remove(oid));

  ASSERT_EQ(0, create_image(&ioctx, oid, 2 << 22, 0, 0, oid));
  ASSERT_EQ(0, get_size(&ioctx, oid, CEPH_NOSNAP, &size, &order));
  ASSERT_EQ(2u << 22, size);
  ASSERT_EQ(0, order);

  ASSERT_EQ(-ENOENT, get_size(&ioctx, oid, 1, &size, &order));

  ioctx.close();
}

TEST_F(TestClsRbd, set_size)
{
  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(_pool_name.c_str(), ioctx));

  string oid = get_temp_image_name();
  ASSERT_EQ(-ENOENT, set_size(&ioctx, oid, 5));

  uint64_t size;
  uint8_t order;
  ASSERT_EQ(0, create_image(&ioctx, oid, 0, 22, 0, oid));
  ASSERT_EQ(0, get_size(&ioctx, oid, CEPH_NOSNAP, &size, &order));
  ASSERT_EQ(0u, size);
  ASSERT_EQ(22, order);

  ASSERT_EQ(0, set_size(&ioctx, oid, 0));
  ASSERT_EQ(0, get_size(&ioctx, oid, CEPH_NOSNAP, &size, &order));
  ASSERT_EQ(0u, size);
  ASSERT_EQ(22, order);

  ASSERT_EQ(0, set_size(&ioctx, oid, 3 << 22));
  ASSERT_EQ(0, get_size(&ioctx, oid, CEPH_NOSNAP, &size, &order));
  ASSERT_EQ(3u << 22, size);
  ASSERT_EQ(22, order);

  ioctx.close();
}

TEST_F(TestClsRbd, protection_status)
{
  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(_pool_name.c_str(), ioctx));

  string oid = get_temp_image_name();
  string oid2 = get_temp_image_name();
  uint8_t status = RBD_PROTECTION_STATUS_UNPROTECTED;
  ASSERT_EQ(-ENOENT, get_protection_status(&ioctx, oid,
					   CEPH_NOSNAP, &status));
  ASSERT_EQ(-ENOENT, set_protection_status(&ioctx, oid,
					   CEPH_NOSNAP, status));

  ASSERT_EQ(0, create_image(&ioctx, oid, 0, 22, RBD_FEATURE_LAYERING, oid));
  ASSERT_EQ(0, create_image(&ioctx, oid2, 0, 22, 0, oid));
  ASSERT_EQ(-EINVAL, get_protection_status(&ioctx, oid2,
					   CEPH_NOSNAP, &status));
  ASSERT_EQ(-ENOEXEC, set_protection_status(&ioctx, oid2,
					   CEPH_NOSNAP, status));
  ASSERT_EQ(-EINVAL, get_protection_status(&ioctx, oid,
					   CEPH_NOSNAP, &status));
  ASSERT_EQ(-EINVAL, set_protection_status(&ioctx, oid,
					   CEPH_NOSNAP, status));
  ASSERT_EQ(-ENOENT, get_protection_status(&ioctx, oid,
					   2, &status));
  ASSERT_EQ(-ENOENT, set_protection_status(&ioctx, oid,
					   2, status));

  ASSERT_EQ(0, snapshot_add(&ioctx, oid, 10, "snap1"));
  ASSERT_EQ(0, get_protection_status(&ioctx, oid,
				     10, &status));
  ASSERT_EQ(+RBD_PROTECTION_STATUS_UNPROTECTED, status);

  ASSERT_EQ(0, set_protection_status(&ioctx, oid,
				     10, RBD_PROTECTION_STATUS_PROTECTED));
  ASSERT_EQ(0, get_protection_status(&ioctx, oid,
				     10, &status));
  ASSERT_EQ(+RBD_PROTECTION_STATUS_PROTECTED, status);
  ASSERT_EQ(-EBUSY, snapshot_remove(&ioctx, oid, 10));

  ASSERT_EQ(0, set_protection_status(&ioctx, oid,
				     10, RBD_PROTECTION_STATUS_UNPROTECTING));
  ASSERT_EQ(0, get_protection_status(&ioctx, oid,
				     10, &status));
  ASSERT_EQ(+RBD_PROTECTION_STATUS_UNPROTECTING, status);
  ASSERT_EQ(-EBUSY, snapshot_remove(&ioctx, oid, 10));

  ASSERT_EQ(-EINVAL, set_protection_status(&ioctx, oid,
					   10, RBD_PROTECTION_STATUS_LAST));
  ASSERT_EQ(0, get_protection_status(&ioctx, oid,
				     10, &status));
  ASSERT_EQ(+RBD_PROTECTION_STATUS_UNPROTECTING, status);

  ASSERT_EQ(0, snapshot_add(&ioctx, oid, 20, "snap2"));
  ASSERT_EQ(0, get_protection_status(&ioctx, oid,
				     20, &status));
  ASSERT_EQ(+RBD_PROTECTION_STATUS_UNPROTECTED, status);
  ASSERT_EQ(0, set_protection_status(&ioctx, oid,
				     10, RBD_PROTECTION_STATUS_UNPROTECTED));
  ASSERT_EQ(0, get_protection_status(&ioctx, oid,
				     10, &status));
  ASSERT_EQ(+RBD_PROTECTION_STATUS_UNPROTECTED, status);

  ASSERT_EQ(0, snapshot_remove(&ioctx, oid, 10));
  ASSERT_EQ(0, snapshot_remove(&ioctx, oid, 20));

  ioctx.close();
}

TEST_F(TestClsRbd, parents)
{
  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(_pool_name.c_str(), ioctx));

  string oid = get_temp_image_name();
  parent_spec pspec;
  uint64_t size;

  ASSERT_EQ(-ENOENT, get_parent(&ioctx, "doesnotexist", CEPH_NOSNAP, &pspec, &size));

  // old image should fail
  ASSERT_EQ(0, create_image(&ioctx, "old", 33<<20, 22, 0, "old_blk."));
  // get nonexistent parent: succeed, return (-1, "", CEPH_NOSNAP), overlap 0
  ASSERT_EQ(0, get_parent(&ioctx, "old", CEPH_NOSNAP, &pspec, &size));
  ASSERT_EQ(pspec.pool_id, -1);
  ASSERT_STREQ("", pspec.image_id.c_str());
  ASSERT_EQ(pspec.snap_id, CEPH_NOSNAP);
  ASSERT_EQ(size, 0ULL);
  pspec = parent_spec(-1, "parent", 3);
  ASSERT_EQ(-ENOEXEC, set_parent(&ioctx, "old", parent_spec(-1, "parent", 3), 10<<20));
  ASSERT_EQ(-ENOEXEC, remove_parent(&ioctx, "old"));

  // new image will work
  ASSERT_EQ(0, create_image(&ioctx, oid, 33<<20, 22, RBD_FEATURE_LAYERING, "foo."));

  ASSERT_EQ(0, get_parent(&ioctx, oid, CEPH_NOSNAP, &pspec, &size));
  ASSERT_EQ(-1, pspec.pool_id);
  ASSERT_EQ(0, get_parent(&ioctx, oid, 123, &pspec, &size));
  ASSERT_EQ(-1, pspec.pool_id);

  ASSERT_EQ(-EINVAL, set_parent(&ioctx, oid, parent_spec(-1, "parent", 3), 10<<20));
  ASSERT_EQ(-EINVAL, set_parent(&ioctx, oid, parent_spec(1, "", 3), 10<<20));
  ASSERT_EQ(-EINVAL, set_parent(&ioctx, oid, parent_spec(1, "parent", CEPH_NOSNAP), 10<<20));
  ASSERT_EQ(-EINVAL, set_parent(&ioctx, oid, parent_spec(1, "parent", 3), 0));

  pspec = parent_spec(1, "parent", 3);
  ASSERT_EQ(0, set_parent(&ioctx, oid, pspec, 10<<20));
  ASSERT_EQ(-EEXIST, set_parent(&ioctx, oid, pspec, 10<<20));
  ASSERT_EQ(-EEXIST, set_parent(&ioctx, oid, parent_spec(2, "parent", 34), 10<<20));

  ASSERT_EQ(0, get_parent(&ioctx, oid, CEPH_NOSNAP, &pspec, &size));
  ASSERT_EQ(pspec.pool_id, 1);
  ASSERT_EQ(pspec.image_id, "parent");
  ASSERT_EQ(pspec.snap_id, snapid_t(3));

  ASSERT_EQ(0, remove_parent(&ioctx, oid));
  ASSERT_EQ(-ENOENT, remove_parent(&ioctx, oid));
  ASSERT_EQ(0, get_parent(&ioctx, oid, CEPH_NOSNAP, &pspec, &size));
  ASSERT_EQ(-1, pspec.pool_id);

  // snapshots
  ASSERT_EQ(0, set_parent(&ioctx, oid, parent_spec(1, "parent", 3), 10<<20));
  ASSERT_EQ(0, snapshot_add(&ioctx, oid, 10, "snap1"));
  ASSERT_EQ(0, get_parent(&ioctx, oid, 10, &pspec, &size));
  ASSERT_EQ(pspec.pool_id, 1);
  ASSERT_EQ(pspec.image_id, "parent");
  ASSERT_EQ(pspec.snap_id, snapid_t(3));
  ASSERT_EQ(size, 10ull<<20);

  ASSERT_EQ(0, remove_parent(&ioctx, oid));
  ASSERT_EQ(0, set_parent(&ioctx, oid, parent_spec(4, "parent2", 6), 5<<20));
  ASSERT_EQ(0, snapshot_add(&ioctx, oid, 11, "snap2"));
  ASSERT_EQ(0, get_parent(&ioctx, oid, 10, &pspec, &size));
  ASSERT_EQ(pspec.pool_id, 1);
  ASSERT_EQ(pspec.image_id, "parent");
  ASSERT_EQ(pspec.snap_id, snapid_t(3));
  ASSERT_EQ(size, 10ull<<20);
  ASSERT_EQ(0, get_parent(&ioctx, oid, 11, &pspec, &size));
  ASSERT_EQ(pspec.pool_id, 4);
  ASSERT_EQ(pspec.image_id, "parent2");
  ASSERT_EQ(pspec.snap_id, snapid_t(6));
  ASSERT_EQ(size, 5ull<<20);

  ASSERT_EQ(0, remove_parent(&ioctx, oid));
  ASSERT_EQ(0, snapshot_add(&ioctx, oid, 12, "snap3"));
  ASSERT_EQ(0, get_parent(&ioctx, oid, 10, &pspec, &size));
  ASSERT_EQ(pspec.pool_id, 1);
  ASSERT_EQ(pspec.image_id, "parent");
  ASSERT_EQ(pspec.snap_id, snapid_t(3));
  ASSERT_EQ(size, 10ull<<20);
  ASSERT_EQ(0, get_parent(&ioctx, oid, 11, &pspec, &size));
  ASSERT_EQ(pspec.pool_id, 4);
  ASSERT_EQ(pspec.image_id, "parent2");
  ASSERT_EQ(pspec.snap_id, snapid_t(6));
  ASSERT_EQ(size, 5ull<<20);
  ASSERT_EQ(0, get_parent(&ioctx, oid, 12, &pspec, &size));
  ASSERT_EQ(-1, pspec.pool_id);

  // make sure set_parent takes min of our size and parent's size
  ASSERT_EQ(0, set_parent(&ioctx, oid, parent_spec(1, "parent", 3), 1<<20));
  ASSERT_EQ(0, get_parent(&ioctx, oid, CEPH_NOSNAP, &pspec, &size));
  ASSERT_EQ(pspec.pool_id, 1);
  ASSERT_EQ(pspec.image_id, "parent");
  ASSERT_EQ(pspec.snap_id, snapid_t(3));
  ASSERT_EQ(size, 1ull<<20);
  ASSERT_EQ(0, remove_parent(&ioctx, oid));

  ASSERT_EQ(0, set_parent(&ioctx, oid, parent_spec(1, "parent", 3), 100<<20));
  ASSERT_EQ(0, get_parent(&ioctx, oid, CEPH_NOSNAP, &pspec, &size));
  ASSERT_EQ(pspec.pool_id, 1);
  ASSERT_EQ(pspec.image_id, "parent");
  ASSERT_EQ(pspec.snap_id, snapid_t(3));
  ASSERT_EQ(size, 33ull<<20);
  ASSERT_EQ(0, remove_parent(&ioctx, oid));

  // make sure resize adjust parent overlap
  ASSERT_EQ(0, set_parent(&ioctx, oid, parent_spec(1, "parent", 3), 10<<20));

  ASSERT_EQ(0, snapshot_add(&ioctx, oid, 14, "snap4"));
  ASSERT_EQ(0, set_size(&ioctx, oid, 3 << 20));
  ASSERT_EQ(0, get_parent(&ioctx, oid, CEPH_NOSNAP, &pspec, &size));
  ASSERT_EQ(pspec.pool_id, 1);
  ASSERT_EQ(pspec.image_id, "parent");
  ASSERT_EQ(pspec.snap_id, snapid_t(3));
  ASSERT_EQ(size, 3ull<<20);
  ASSERT_EQ(0, get_parent(&ioctx, oid, 14, &pspec, &size));
  ASSERT_EQ(pspec.pool_id, 1);
  ASSERT_EQ(pspec.image_id, "parent");
  ASSERT_EQ(pspec.snap_id, snapid_t(3));
  ASSERT_EQ(size, 10ull<<20);

  ASSERT_EQ(0, snapshot_add(&ioctx, oid, 15, "snap5"));
  ASSERT_EQ(0, set_size(&ioctx, oid, 30 << 20));
  ASSERT_EQ(0, get_parent(&ioctx, oid, CEPH_NOSNAP, &pspec, &size));
  ASSERT_EQ(pspec.pool_id, 1);
  ASSERT_EQ(pspec.image_id, "parent");
  ASSERT_EQ(pspec.snap_id, snapid_t(3));
  ASSERT_EQ(size, 3ull<<20);
  ASSERT_EQ(0, get_parent(&ioctx, oid, 14, &pspec, &size));
  ASSERT_EQ(pspec.pool_id, 1);
  ASSERT_EQ(pspec.image_id, "parent");
  ASSERT_EQ(pspec.snap_id, snapid_t(3));
  ASSERT_EQ(size, 10ull<<20);
  ASSERT_EQ(0, get_parent(&ioctx, oid, 15, &pspec, &size));
  ASSERT_EQ(pspec.pool_id, 1);
  ASSERT_EQ(pspec.image_id, "parent");
  ASSERT_EQ(pspec.snap_id, snapid_t(3));
  ASSERT_EQ(size, 3ull<<20);

  ASSERT_EQ(0, set_size(&ioctx, oid, 2 << 20));
  ASSERT_EQ(0, get_parent(&ioctx, oid, CEPH_NOSNAP, &pspec, &size));
  ASSERT_EQ(pspec.pool_id, 1);
  ASSERT_EQ(pspec.image_id, "parent");
  ASSERT_EQ(pspec.snap_id, snapid_t(3));
  ASSERT_EQ(size, 2ull<<20);

  ASSERT_EQ(0, snapshot_add(&ioctx, oid, 16, "snap6"));
  ASSERT_EQ(0, get_parent(&ioctx, oid, 16, &pspec, &size));
  ASSERT_EQ(pspec.pool_id, 1);
  ASSERT_EQ(pspec.image_id, "parent");
  ASSERT_EQ(pspec.snap_id, snapid_t(3));
  ASSERT_EQ(size, 2ull<<20);

  ioctx.close();
}

TEST_F(TestClsRbd, snapshots)
{
  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(_pool_name.c_str(), ioctx));

  string oid = get_temp_image_name();
  ASSERT_EQ(-ENOENT, snapshot_add(&ioctx, oid, 0, "snap1"));

  ASSERT_EQ(0, create_image(&ioctx, oid, 10, 22, 0, oid));

  vector<string> snap_names;
  vector<uint64_t> snap_sizes;
  vector<uint64_t> snap_features;
  SnapContext snapc;
  vector<parent_info> parents;
  vector<uint8_t> protection_status;

  ASSERT_EQ(0, get_snapcontext(&ioctx, oid, &snapc));
  ASSERT_EQ(0u, snapc.snaps.size());
  ASSERT_EQ(0u, snapc.seq);
  ASSERT_EQ(0, snapshot_list(&ioctx, oid, snapc.snaps, &snap_names,
			     &snap_sizes, &snap_features, &parents,
			     &protection_status));
  ASSERT_EQ(0u, snap_names.size());
  ASSERT_EQ(0u, snap_sizes.size());
  ASSERT_EQ(0u, snap_features.size());

  ASSERT_EQ(0, snapshot_add(&ioctx, oid, 0, "snap1"));
  ASSERT_EQ(0, get_snapcontext(&ioctx, oid, &snapc));
  ASSERT_EQ(1u, snapc.snaps.size());
  ASSERT_EQ(0u, snapc.snaps[0]);
  ASSERT_EQ(0u, snapc.seq);
  ASSERT_EQ(0, snapshot_list(&ioctx, oid, snapc.snaps, &snap_names,
			     &snap_sizes, &snap_features, &parents,
			     &protection_status));
  ASSERT_EQ(1u, snap_names.size());
  ASSERT_EQ("snap1", snap_names[0]);
  ASSERT_EQ(10u, snap_sizes[0]);
  ASSERT_EQ(0u, snap_features[0]);

  // snap with same id and name
  ASSERT_EQ(-EEXIST, snapshot_add(&ioctx, oid, 0, "snap1"));
  ASSERT_EQ(0, get_snapcontext(&ioctx, oid, &snapc));
  ASSERT_EQ(1u, snapc.snaps.size());
  ASSERT_EQ(0u, snapc.snaps[0]);
  ASSERT_EQ(0u, snapc.seq);
  ASSERT_EQ(0, snapshot_list(&ioctx, oid, snapc.snaps, &snap_names,
			     &snap_sizes, &snap_features, &parents,
			     &protection_status));
  ASSERT_EQ(1u, snap_names.size());
  ASSERT_EQ("snap1", snap_names[0]);
  ASSERT_EQ(10u, snap_sizes[0]);
  ASSERT_EQ(0u, snap_features[0]);

  // snap with same id, different name
  ASSERT_EQ(-EEXIST, snapshot_add(&ioctx, oid, 0, "snap2"));
  ASSERT_EQ(0, get_snapcontext(&ioctx, oid, &snapc));
  ASSERT_EQ(1u, snapc.snaps.size());
  ASSERT_EQ(0u, snapc.snaps[0]);
  ASSERT_EQ(0u, snapc.seq);
  ASSERT_EQ(0, snapshot_list(&ioctx, oid, snapc.snaps, &snap_names,
			     &snap_sizes, &snap_features, &parents,
			     &protection_status));
  ASSERT_EQ(1u, snap_names.size());
  ASSERT_EQ("snap1", snap_names[0]);
  ASSERT_EQ(10u, snap_sizes[0]);
  ASSERT_EQ(0u, snap_features[0]);

  // snap with different id, same name
  ASSERT_EQ(-EEXIST, snapshot_add(&ioctx, oid, 1, "snap1"));
  ASSERT_EQ(0, get_snapcontext(&ioctx, oid, &snapc));
  ASSERT_EQ(1u, snapc.snaps.size());
  ASSERT_EQ(0u, snapc.snaps[0]);
  ASSERT_EQ(0u, snapc.seq);
  ASSERT_EQ(0, snapshot_list(&ioctx, oid, snapc.snaps, &snap_names,
			     &snap_sizes, &snap_features, &parents,
			     &protection_status));
  ASSERT_EQ(snap_names.size(), 1u);
  ASSERT_EQ(snap_names[0], "snap1");
  ASSERT_EQ(snap_sizes[0], 10u);
  ASSERT_EQ(snap_features[0], 0u);

  // snap with different id, different name
  ASSERT_EQ(0, snapshot_add(&ioctx, oid, 1, "snap2"));
  ASSERT_EQ(0, get_snapcontext(&ioctx, oid, &snapc));
  ASSERT_EQ(2u, snapc.snaps.size());
  ASSERT_EQ(1u, snapc.snaps[0]);
  ASSERT_EQ(0u, snapc.snaps[1]);
  ASSERT_EQ(1u, snapc.seq);
  ASSERT_EQ(0, snapshot_list(&ioctx, oid, snapc.snaps, &snap_names,
			     &snap_sizes, &snap_features, &parents,
			     &protection_status));
  ASSERT_EQ(2u, snap_names.size());
  ASSERT_EQ("snap2", snap_names[0]);
  ASSERT_EQ(10u, snap_sizes[0]);
  ASSERT_EQ(0u, snap_features[0]);
  ASSERT_EQ("snap1", snap_names[1]);
  ASSERT_EQ(10u, snap_sizes[1]);
  ASSERT_EQ(0u, snap_features[1]);

  ASSERT_EQ(0, snapshot_remove(&ioctx, oid, 0));
  ASSERT_EQ(0, get_snapcontext(&ioctx, oid, &snapc));
  ASSERT_EQ(1u, snapc.snaps.size());
  ASSERT_EQ(1u, snapc.snaps[0]);
  ASSERT_EQ(1u, snapc.seq);
  ASSERT_EQ(0, snapshot_list(&ioctx, oid, snapc.snaps, &snap_names,
			     &snap_sizes, &snap_features, &parents,
			     &protection_status));
  ASSERT_EQ(1u, snap_names.size());
  ASSERT_EQ("snap2", snap_names[0]);
  ASSERT_EQ(10u, snap_sizes[0]);
  ASSERT_EQ(0u, snap_features[0]);

  uint64_t size;
  uint8_t order;
  ASSERT_EQ(0, set_size(&ioctx, oid, 0));
  ASSERT_EQ(0, get_size(&ioctx, oid, CEPH_NOSNAP, &size, &order));
  ASSERT_EQ(0u, size);
  ASSERT_EQ(22u, order);

  uint64_t large_snap_id = 1ull << 63;
  ASSERT_EQ(0, snapshot_add(&ioctx, oid, large_snap_id, "snap3"));
  ASSERT_EQ(0, get_snapcontext(&ioctx, oid, &snapc));
  ASSERT_EQ(2u, snapc.snaps.size());
  ASSERT_EQ(large_snap_id, snapc.snaps[0]);
  ASSERT_EQ(1u, snapc.snaps[1]);
  ASSERT_EQ(large_snap_id, snapc.seq);
  ASSERT_EQ(0, snapshot_list(&ioctx, oid, snapc.snaps, &snap_names,
			     &snap_sizes, &snap_features, &parents,
			     &protection_status));
  ASSERT_EQ(2u, snap_names.size());
  ASSERT_EQ("snap3", snap_names[0]);
  ASSERT_EQ(0u, snap_sizes[0]);
  ASSERT_EQ(0u, snap_features[0]);
  ASSERT_EQ("snap2", snap_names[1]);
  ASSERT_EQ(10u, snap_sizes[1]);
  ASSERT_EQ(0u, snap_features[1]);

  ASSERT_EQ(0, get_size(&ioctx, oid, large_snap_id, &size, &order));
  ASSERT_EQ(0u, size);
  ASSERT_EQ(22u, order);

  ASSERT_EQ(0, get_size(&ioctx, oid, 1, &size, &order));
  ASSERT_EQ(10u, size);
  ASSERT_EQ(22u, order);

  ASSERT_EQ(0, snapshot_remove(&ioctx, oid, large_snap_id));
  ASSERT_EQ(0, get_snapcontext(&ioctx, oid, &snapc));
  ASSERT_EQ(1u, snapc.snaps.size());
  ASSERT_EQ(1u, snapc.snaps[0]);
  ASSERT_EQ(large_snap_id, snapc.seq);
  ASSERT_EQ(0, snapshot_list(&ioctx, oid, snapc.snaps, &snap_names,
			     &snap_sizes, &snap_features, &parents,
			     &protection_status));
  ASSERT_EQ(1u, snap_names.size());
  ASSERT_EQ("snap2", snap_names[0]);
  ASSERT_EQ(10u, snap_sizes[0]);
  ASSERT_EQ(0u, snap_features[0]);

  ASSERT_EQ(-ENOENT, snapshot_remove(&ioctx, oid, large_snap_id));
  ASSERT_EQ(0, snapshot_remove(&ioctx, oid, 1));
  ASSERT_EQ(0, get_snapcontext(&ioctx, oid, &snapc));
  ASSERT_EQ(0u, snapc.snaps.size());
  ASSERT_EQ(large_snap_id, snapc.seq);
  ASSERT_EQ(0, snapshot_list(&ioctx, oid, snapc.snaps, &snap_names,
			     &snap_sizes, &snap_features, &parents,
			     &protection_status));
  ASSERT_EQ(0u, snap_names.size());
  ASSERT_EQ(0u, snap_sizes.size());
  ASSERT_EQ(0u, snap_features.size());

  ioctx.close();
}

TEST_F(TestClsRbd, snapid_race)
{
  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(_pool_name.c_str(), ioctx));

  buffer::list bl;
  buffer::ptr bp(4096);
  bp.zero();
  bl.append(bp);

  string oid = get_temp_image_name();
  ASSERT_EQ(0, ioctx.write(oid, bl, 4096, 0));
  ASSERT_EQ(0, old_snapshot_add(&ioctx, oid, 1, "test1"));
  ASSERT_EQ(0, old_snapshot_add(&ioctx, oid, 3, "test3"));
  ASSERT_EQ(-ESTALE, old_snapshot_add(&ioctx, oid, 2, "test2"));

  ioctx.close();
}

TEST_F(TestClsRbd, stripingv2)
{
  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(_pool_name.c_str(), ioctx));

  string oid = get_temp_image_name();
  string oid2 = get_temp_image_name();
  ASSERT_EQ(0, create_image(&ioctx, oid, 10, 22, 0, oid));

  uint64_t su = 65536, sc = 12;
  ASSERT_EQ(-ENOEXEC, get_stripe_unit_count(&ioctx, oid, &su, &sc));
  ASSERT_EQ(-ENOEXEC, set_stripe_unit_count(&ioctx, oid, su, sc));

  ASSERT_EQ(0, create_image(&ioctx, oid2, 10, 22, RBD_FEATURE_STRIPINGV2, oid2));
  ASSERT_EQ(0, get_stripe_unit_count(&ioctx, oid2, &su, &sc));
  ASSERT_EQ(1ull << 22, su);
  ASSERT_EQ(1ull, sc);
  su = 8192;
  sc = 456;
  ASSERT_EQ(0, set_stripe_unit_count(&ioctx, oid2, su, sc));
  su = sc = 0;
  ASSERT_EQ(0, get_stripe_unit_count(&ioctx, oid2, &su, &sc));
  ASSERT_EQ(8192ull, su);
  ASSERT_EQ(456ull, sc);

  // su must not be larger than an object
  ASSERT_EQ(-EINVAL, set_stripe_unit_count(&ioctx, oid2, 1 << 23, 1));
  // su must be a factor of object size
  ASSERT_EQ(-EINVAL, set_stripe_unit_count(&ioctx, oid2, 511, 1));
  // su and sc must be non-zero
  ASSERT_EQ(-EINVAL, set_stripe_unit_count(&ioctx, oid2, 0, 1));
  ASSERT_EQ(-EINVAL, set_stripe_unit_count(&ioctx, oid2, 1, 0));
  ASSERT_EQ(-EINVAL, set_stripe_unit_count(&ioctx, oid2, 0, 0));

  ioctx.close();
}

TEST_F(TestClsRbd, get_mutable_metadata_features)
{
  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(_pool_name.c_str(), ioctx));

  string oid = get_temp_image_name();
  ASSERT_EQ(0, create_image(&ioctx, oid, 10, 22, RBD_FEATURE_EXCLUSIVE_LOCK,
                            oid));

  uint64_t size, features, incompatible_features;
  std::map<rados::cls::lock::locker_id_t,
           rados::cls::lock::locker_info_t> lockers;
  bool exclusive_lock;
  std::string lock_tag;
  ::SnapContext snapc;
  parent_info parent;

  ASSERT_EQ(0, get_mutable_metadata(&ioctx, oid, true, &size, &features,
				    &incompatible_features, &lockers,
				    &exclusive_lock, &lock_tag, &snapc,
                                    &parent));
  ASSERT_EQ(static_cast<uint64_t>(RBD_FEATURE_EXCLUSIVE_LOCK), features);
  ASSERT_EQ(0U, incompatible_features);

  ASSERT_EQ(0, get_mutable_metadata(&ioctx, oid, false, &size, &features,
                                    &incompatible_features, &lockers,
                                    &exclusive_lock, &lock_tag, &snapc,
                                    &parent));
  ASSERT_EQ(static_cast<uint64_t>(RBD_FEATURE_EXCLUSIVE_LOCK), features);
  ASSERT_EQ(static_cast<uint64_t>(RBD_FEATURE_EXCLUSIVE_LOCK),
	    incompatible_features);

  ioctx.close();
}

TEST_F(TestClsRbd, object_map_resize)
{
  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(_pool_name.c_str(), ioctx));

  string oid = get_temp_image_name();
  BitVector<2> ref_bit_vector;
  ref_bit_vector.resize(32);
  for (uint64_t i = 0; i < ref_bit_vector.size(); ++i) {
    ref_bit_vector[i] = 1;
  }

  librados::ObjectWriteOperation op1;
  object_map_resize(&op1, ref_bit_vector.size(), 1);
  ASSERT_EQ(0, ioctx.operate(oid, &op1));

  BitVector<2> osd_bit_vector;
  ASSERT_EQ(0, object_map_load(&ioctx, oid, &osd_bit_vector));
  ASSERT_EQ(ref_bit_vector, osd_bit_vector);

  ref_bit_vector.resize(64);
  for (uint64_t i = 32; i < ref_bit_vector.size(); ++i) {
    ref_bit_vector[i] = 2;
  }

  librados::ObjectWriteOperation op2;
  object_map_resize(&op2, ref_bit_vector.size(), 2);
  ASSERT_EQ(0, ioctx.operate(oid, &op2));
  ASSERT_EQ(0, object_map_load(&ioctx, oid, &osd_bit_vector));
  ASSERT_EQ(ref_bit_vector, osd_bit_vector);

  ref_bit_vector.resize(32);

  librados::ObjectWriteOperation op3;
  object_map_resize(&op3, ref_bit_vector.size(), 1);
  ASSERT_EQ(-ESTALE, ioctx.operate(oid, &op3));

  librados::ObjectWriteOperation op4;
  object_map_resize(&op4, ref_bit_vector.size(), 2);
  ASSERT_EQ(0, ioctx.operate(oid, &op4));

  ASSERT_EQ(0, object_map_load(&ioctx, oid, &osd_bit_vector));
  ASSERT_EQ(ref_bit_vector, osd_bit_vector);

  ioctx.close();
}

TEST_F(TestClsRbd, object_map_update)
{
  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(_pool_name.c_str(), ioctx));

  string oid = get_temp_image_name();
  BitVector<2> ref_bit_vector;
  ref_bit_vector.resize(16);
  for (uint64_t i = 0; i < ref_bit_vector.size(); ++i) {
    ref_bit_vector[i] = 2;
  }

  BitVector<2> osd_bit_vector;

  librados::ObjectWriteOperation op1;
  object_map_resize(&op1, ref_bit_vector.size(), 2);
  ASSERT_EQ(0, ioctx.operate(oid, &op1));
  ASSERT_EQ(0, object_map_load(&ioctx, oid, &osd_bit_vector));
  ASSERT_EQ(ref_bit_vector, osd_bit_vector);

  ref_bit_vector[7] = 1;
  ref_bit_vector[8] = 1;

  librados::ObjectWriteOperation op2;
  object_map_update(&op2, 7, 9, 1, boost::optional<uint8_t>());
  ASSERT_EQ(0, ioctx.operate(oid, &op2));
  ASSERT_EQ(0, object_map_load(&ioctx, oid, &osd_bit_vector));
  ASSERT_EQ(ref_bit_vector, osd_bit_vector);

  ref_bit_vector[7] = 3;
  ref_bit_vector[8] = 3;

  librados::ObjectWriteOperation op3;
  object_map_update(&op3, 6, 10, 3, 1);
  ASSERT_EQ(0, ioctx.operate(oid, &op3));
  ASSERT_EQ(0, object_map_load(&ioctx, oid, &osd_bit_vector));
  ASSERT_EQ(ref_bit_vector, osd_bit_vector);

  ioctx.close();
}

TEST_F(TestClsRbd, object_map_load_enoent)
{
  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(_pool_name.c_str(), ioctx));

  string oid = get_temp_image_name();
  BitVector<2> osd_bit_vector;
  ASSERT_EQ(-ENOENT, object_map_load(&ioctx, oid, &osd_bit_vector));

  ioctx.close();
}

TEST_F(TestClsRbd, flags)
{
  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(_pool_name.c_str(), ioctx));

  string oid = get_temp_image_name();
  ASSERT_EQ(0, create_image(&ioctx, oid, 0, 22, 0, oid));

  uint64_t flags;
  std::vector<snapid_t> snap_ids;
  std::vector<uint64_t> snap_flags;
  ASSERT_EQ(0, get_flags(&ioctx, oid, &flags, snap_ids, &snap_flags));
  ASSERT_EQ(0U, flags);

  librados::ObjectWriteOperation op1;
  set_flags(&op1, 3, 2);
  ASSERT_EQ(0, ioctx.operate(oid, &op1));
  ASSERT_EQ(0, get_flags(&ioctx, oid, &flags, snap_ids, &snap_flags));
  ASSERT_EQ(2U, flags);

  uint64_t snap_id = 10;
  snap_ids.push_back(snap_id);
  ASSERT_EQ(-ENOENT, get_flags(&ioctx, oid, &flags, snap_ids, &snap_flags));
  ASSERT_EQ(0, snapshot_add(&ioctx, oid, snap_id, "snap"));

  librados::ObjectWriteOperation op2;
  set_flags(&op2, 31, 4);
  ASSERT_EQ(0, ioctx.operate(oid, &op2));
  ASSERT_EQ(0, get_flags(&ioctx, oid, &flags, snap_ids, &snap_flags));
  ASSERT_EQ(6U, flags);
  ASSERT_EQ(snap_ids.size(), snap_flags.size());
  ASSERT_EQ(2U, snap_flags[0]);

  ioctx.close();
}

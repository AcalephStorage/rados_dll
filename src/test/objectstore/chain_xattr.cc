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
#include "os/chain_xattr.h"
#include "include/Context.h"
#include "common/errno.h"
#include "common/ceph_argparse.h"
#include "global/global_init.h"
#include <gtest/gtest.h>

#define LARGE_BLOCK_LEN CHAIN_XATTR_MAX_BLOCK_LEN + 1024
#define FILENAME "chain_xattr"

TEST(chain_xattr, get_and_set) {
  const char* file = FILENAME;
  ::unlink(file);
  int fd = ::open(file, O_CREAT|O_WRONLY|O_TRUNC, 0700);
  const string user("user.");
  
  {
    const string name = user + string(CHAIN_XATTR_MAX_NAME_LEN - user.size(), '@');
    const string x(LARGE_BLOCK_LEN, 'X');

    {
      char y[LARGE_BLOCK_LEN];
      ASSERT_EQ(LARGE_BLOCK_LEN, chain_setxattr(file, name.c_str(), x.c_str(), LARGE_BLOCK_LEN));
      ASSERT_EQ(LARGE_BLOCK_LEN, chain_getxattr(file, name.c_str(), 0, 0));
      ASSERT_EQ(LARGE_BLOCK_LEN, chain_getxattr(file, name.c_str(), y, LARGE_BLOCK_LEN));
      ASSERT_EQ(0, chain_removexattr(file, name.c_str()));
      ASSERT_EQ(0, memcmp(x.c_str(), y, LARGE_BLOCK_LEN));
    }

    {
      char y[LARGE_BLOCK_LEN];
      ASSERT_EQ(LARGE_BLOCK_LEN, chain_fsetxattr(fd, name.c_str(), x.c_str(), LARGE_BLOCK_LEN));
      ASSERT_EQ(LARGE_BLOCK_LEN, chain_fgetxattr(fd, name.c_str(), 0, 0));
      ASSERT_EQ(LARGE_BLOCK_LEN, chain_fgetxattr(fd, name.c_str(), y, LARGE_BLOCK_LEN));
      ASSERT_EQ(0, chain_fremovexattr(fd, name.c_str()));
      ASSERT_EQ(0, memcmp(x.c_str(), y, LARGE_BLOCK_LEN));
    }
  }

  //
  // when chain_setxattr is used to store value that is
  // CHAIN_XATTR_MAX_BLOCK_LEN * 2 + 10 bytes long it 
  //
  // add user.foo => CHAIN_XATTR_MAX_BLOCK_LEN bytes
  // add user.foo@1 => CHAIN_XATTR_MAX_BLOCK_LEN bytes
  // add user.foo@2 => 10 bytes
  //
  // then ( no chain_removexattr in between ) when it is used to
  // override with a value that is exactly CHAIN_XATTR_MAX_BLOCK_LEN
  // bytes long it will
  //
  // replace user.foo => CHAIN_XATTR_MAX_BLOCK_LEN bytes
  // remove user.foo@1 => CHAIN_XATTR_MAX_BLOCK_LEN bytes
  // leak user.foo@2 => 10 bytes
  // 
  // see http://marc.info/?l=ceph-devel&m=136027076615853&w=4 for the 
  // discussion
  //
  {
    const string name = user + string(CHAIN_XATTR_MAX_NAME_LEN - user.size(), '@');
    const string x(LARGE_BLOCK_LEN, 'X');

    {
      char y[CHAIN_XATTR_MAX_BLOCK_LEN];
      ASSERT_EQ(LARGE_BLOCK_LEN, chain_setxattr(file, name.c_str(), x.c_str(), LARGE_BLOCK_LEN));
      ASSERT_EQ(CHAIN_XATTR_MAX_BLOCK_LEN, chain_setxattr(file, name.c_str(), x.c_str(), CHAIN_XATTR_MAX_BLOCK_LEN));
      ASSERT_EQ(CHAIN_XATTR_MAX_BLOCK_LEN, chain_getxattr(file, name.c_str(), 0, 0));
      ASSERT_EQ(CHAIN_XATTR_MAX_BLOCK_LEN, chain_getxattr(file, name.c_str(), y, CHAIN_XATTR_MAX_BLOCK_LEN));
      ASSERT_EQ(0, chain_removexattr(file, name.c_str()));
      ASSERT_EQ(0, memcmp(x.c_str(), y, CHAIN_XATTR_MAX_BLOCK_LEN));
    }

    {
      char y[CHAIN_XATTR_MAX_BLOCK_LEN];
      ASSERT_EQ(LARGE_BLOCK_LEN, chain_fsetxattr(fd, name.c_str(), x.c_str(), LARGE_BLOCK_LEN));
      ASSERT_EQ(CHAIN_XATTR_MAX_BLOCK_LEN, chain_fsetxattr(fd, name.c_str(), x.c_str(), CHAIN_XATTR_MAX_BLOCK_LEN));
      ASSERT_EQ(CHAIN_XATTR_MAX_BLOCK_LEN, chain_fgetxattr(fd, name.c_str(), 0, 0));
      ASSERT_EQ(CHAIN_XATTR_MAX_BLOCK_LEN, chain_fgetxattr(fd, name.c_str(), y, CHAIN_XATTR_MAX_BLOCK_LEN));
      ASSERT_EQ(0, chain_fremovexattr(fd, name.c_str()));
      ASSERT_EQ(0, memcmp(x.c_str(), y, CHAIN_XATTR_MAX_BLOCK_LEN));
    }
  }

  {
    int x = 0;
    ASSERT_EQ(-ENOENT, chain_setxattr("UNLIKELY_TO_EXIST", "NAME", &x, sizeof(x)));
    ASSERT_EQ(-ENOENT, chain_getxattr("UNLIKELY_TO_EXIST", "NAME", 0, 0));
    ASSERT_EQ(-ENOENT, chain_getxattr("UNLIKELY_TO_EXIST", "NAME", &x, sizeof(x)));
    ASSERT_EQ(-ENOENT, chain_removexattr("UNLIKELY_TO_EXIST", "NAME"));
    int unlikely_to_be_a_valid_fd = 400;
    ASSERT_EQ(-EBADF, chain_fsetxattr(unlikely_to_be_a_valid_fd, "NAME", &x, sizeof(x)));
    ASSERT_EQ(-EBADF, chain_fgetxattr(unlikely_to_be_a_valid_fd, "NAME", 0, 0));
    ASSERT_EQ(-EBADF, chain_fgetxattr(unlikely_to_be_a_valid_fd, "NAME", &x, sizeof(x)));
    ASSERT_EQ(-EBADF, chain_fremovexattr(unlikely_to_be_a_valid_fd, "NAME"));
  }

  {
    int x;
    const string name = user + string(CHAIN_XATTR_MAX_NAME_LEN * 2, '@');
    ASSERT_THROW(chain_setxattr(file, name.c_str(), &x, sizeof(x)), FailedAssertion);
    ASSERT_THROW(chain_fsetxattr(fd, name.c_str(), &x, sizeof(x)), FailedAssertion);
  }

  {
    const string name = user + string(CHAIN_XATTR_MAX_NAME_LEN - user.size(), '@');
    const string x(LARGE_BLOCK_LEN, 'X');
    {
      char y[LARGE_BLOCK_LEN];
      ASSERT_EQ(LARGE_BLOCK_LEN, chain_setxattr(file, name.c_str(), x.c_str(), LARGE_BLOCK_LEN));
      ASSERT_EQ(-ERANGE, chain_getxattr(file, name.c_str(), y, LARGE_BLOCK_LEN - 1));
      ASSERT_EQ(-ERANGE, chain_getxattr(file, name.c_str(), y, CHAIN_XATTR_MAX_BLOCK_LEN));
      ASSERT_EQ(0, chain_removexattr(file, name.c_str()));
    }

    {
      char y[LARGE_BLOCK_LEN];
      ASSERT_EQ(LARGE_BLOCK_LEN, chain_fsetxattr(fd, name.c_str(), x.c_str(), LARGE_BLOCK_LEN));
      ASSERT_EQ(-ERANGE, chain_fgetxattr(fd, name.c_str(), y, LARGE_BLOCK_LEN - 1));
      ASSERT_EQ(-ERANGE, chain_fgetxattr(fd, name.c_str(), y, CHAIN_XATTR_MAX_BLOCK_LEN));
      ASSERT_EQ(0, chain_fremovexattr(fd, name.c_str()));
    }
  }

  ::close(fd);
  ::unlink(file);
}

TEST(chain_xattr, listxattr) {
  const char* file = FILENAME;
  ::unlink(file);
  int fd = ::open(file, O_CREAT|O_WRONLY|O_TRUNC, 0700);
  const string user("user.");
  const string name1 = user + string(CHAIN_XATTR_MAX_NAME_LEN - user.size(), '1');
  const string name2 = user + string(CHAIN_XATTR_MAX_NAME_LEN - user.size(), '@');
  const string x(LARGE_BLOCK_LEN, 'X');
  const int y = 1234;
  
  ASSERT_EQ(LARGE_BLOCK_LEN, chain_setxattr(file, name1.c_str(), x.c_str(), LARGE_BLOCK_LEN));
  ASSERT_EQ((int)sizeof(y), chain_setxattr(file, name2.c_str(), &y, sizeof(y)));

  int buffer_size = name1.size() + sizeof('\0') + name2.size() + sizeof('\0');
  char* expected = (char*)malloc(buffer_size);
  ::strcpy(expected, name1.c_str());
  ::strcpy(expected + name1.size() + 1, name2.c_str());
  char* actual = (char*)calloc(1, buffer_size);
  ASSERT_LT(buffer_size, chain_listxattr(file, NULL, 0)); // size evaluation is conservative
  chain_listxattr(file, actual, buffer_size);
  ::memset(actual, '\0', buffer_size);
  chain_flistxattr(fd, actual, buffer_size);
  ASSERT_EQ(0, ::memcmp(expected, actual, buffer_size));

  int unlikely_to_be_a_valid_fd = 400;
  ASSERT_GT(0, chain_listxattr("UNLIKELY_TO_EXIST", actual, 0));
  ASSERT_GT(0, chain_listxattr("UNLIKELY_TO_EXIST", actual, buffer_size));
  ASSERT_GT(0, chain_flistxattr(unlikely_to_be_a_valid_fd, actual, 0));
  ASSERT_GT(0, chain_flistxattr(unlikely_to_be_a_valid_fd, actual, buffer_size));
  ASSERT_EQ(-ERANGE, chain_listxattr(file, actual, 1));
  ASSERT_EQ(-ERANGE, chain_flistxattr(fd, actual, 1));

  ASSERT_EQ(0, chain_removexattr(file, name1.c_str()));
  ASSERT_EQ(0, chain_removexattr(file, name2.c_str()));

  free(expected);
  ::unlink(file);
}

int main(int argc, char **argv) {
  vector<const char*> args;
  argv_to_vec(argc, (const char **)argv, args);

  global_init(NULL, args, CEPH_ENTITY_TYPE_CLIENT, CODE_ENVIRONMENT_UTILITY, 0);
  common_init_finish(g_ceph_context);
  g_ceph_context->_conf->set_val("err_to_stderr", "false");
  g_ceph_context->_conf->set_val("log_to_stderr", "false");
  g_ceph_context->_conf->apply_changes(NULL);

  const char* file = FILENAME;
  int x = 1234;
  int y = 0;
  int tmpfd = ::open(file, O_CREAT|O_WRONLY|O_TRUNC, 0700);
  int ret = ::ceph_os_fsetxattr(tmpfd, "user.test", &x, sizeof(x));
  if (ret >= 0)
    ret = ::ceph_os_fgetxattr(tmpfd, "user.test", &y, sizeof(y));
  ::close(tmpfd);
  ::unlink(file);
  if ((ret < 0) || (x != y)) {
    cerr << "SKIP all tests because extended attributes don't appear to work in the file system in which the tests are run: " << cpp_strerror(ret) << std::endl;
  } else {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
  }
}

// Local Variables:
// compile-command: "cd ../.. ; make unittest_chain_xattr ; valgrind --tool=memcheck ./unittest_chain_xattr # --gtest_filter=chain_xattr.get_and_set"
// End:

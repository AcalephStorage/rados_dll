// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 * Copyright 2013 Inktank
 */

// install the librados-dev package to get this
#include "rados/librados.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

int main(int argc, char **argv)
{
  int ret = 0;

  // we will use all of these below
  const char *pool_name = "pool";
  const char* hello = "hello world!";
  const char* object_name = "hello_object";
  uint64_t flags;
  rados_ioctx_t io_ctx;
  int pool_created = 0;

  // first, we create a Rados object and initialize it
   rados_t cluster;
    char cluster_name[] = "ceph";
    char user_name[] = "client.admin";


    /* Initialize the cluster handle with the "ceph" cluster name and the "client.admin" user */
    int err;
    err = rados_create2(&cluster, cluster_name, user_name, flags);

    if (err < 0) {
            fprintf(stderr, "%s: Couldn't create the cluster handle! %s\n", argv[0], strerror(-err));
            exit(EXIT_FAILURE);
    } else {
            printf("\nCreated a cluster handle.\n");
    }


    /* Read a Ceph configuration file to configure the cluster handle. */
    err = rados_conf_read_file(cluster, "ceph.conf");
    if (err < 0) {
            fprintf(stderr, "%s: cannot read config file: %s\n", argv[0], strerror(-err));
            exit(EXIT_FAILURE);
    } else {
            printf("\nRead the config file.\n");
    }

    /* Read command line arguments */
    err = rados_conf_parse_argv(cluster, argc, argv);
    if (err < 0) {
            fprintf(stderr, "%s: cannot parse command line arguments: %s\n", argv[0], strerror(-err));
            exit(EXIT_FAILURE);
    } else {
            printf("\nRead the command line arguments.\n");
    }

    /* Connect to the cluster */

    err = rados_connect(cluster);
    if (err < 0) {
            fprintf(stderr, "%s: cannot connect to cluster: %s\n", argv[0], strerror(-err));
            exit(EXIT_FAILURE);
    } else {
            printf("\nConnected to the cluster.\n");
    }


  /*
   * let's create our own pool instead of scribbling over real data.
   * Note that this command creates pools with default PG counts specified
   * by the monitors, which may not be appropriate for real use -- it's fine
   * for testing, though.
   */
  {
    printf("1\n");
    ret = rados_pool_create(cluster, pool_name);
    printf("2\n");
    if (ret < 0) {
      printf("couldn't create pool! error %d\n", ret);
      return EXIT_FAILURE;
    } else {
      printf("we just created a new pool named %s\n", pool_name);
    }
    pool_created = 1;
  }

  /*
   * create an "IoCtx" which is used to do IO to a pool
   */
  {
    ret = rados_ioctx_create(cluster, pool_name, &io_ctx);
    if (ret < 0) {
      printf("couldn't set up ioctx! error %d\n", ret);
      ret = EXIT_FAILURE;
      goto out;
    } else {
      printf("we just created an ioctx for our pool\n");
    }
  }

  /*
   * now let's do some IO to the pool! We'll write "hello world!" to a
   * new object.
   */
  {
    /*
     * now that we have the data to write, let's send it to an object.
     * We'll use the synchronous interface for simplicity.
     */
    ret = rados_write_full(io_ctx, object_name, hello, strlen(hello));
    if (ret < 0) {
      printf("couldn't write object! error %d\n", ret);
      ret = EXIT_FAILURE;
      goto out;
    } else {
      printf("we just wrote new object %s, with contents '%s'\n", object_name, hello);
    }
  }

  /*
   * now let's read that object back! Just for fun, we'll do it using
   * async IO instead of synchronous. (This would be more useful if we
   * wanted to send off multiple reads at once; see
   * http://ceph.com/docs/master/rados/api/librados/#asychronous-io )
   */
  {
    int read_len = 4194304; // this is way more than we need
    char* read_buf = malloc(read_len + 1); // add one for the terminating 0 we'll add later
    if (!read_buf) {
      printf("couldn't allocate read buffer\n");
      ret = EXIT_FAILURE;
      goto out;
    }
    // allocate the completion from librados
    rados_completion_t read_completion;
    ret = rados_aio_create_completion(NULL, NULL, NULL, &read_completion);
    if (ret < 0) {
      printf("couldn't create completion! error %d\n", ret);
      ret = EXIT_FAILURE;
      free(read_buf);
      goto out;
    } else {
      printf("we just created a new completion\n");
    }
    // send off the request.
    ret = rados_aio_read(io_ctx, object_name, read_completion, read_buf, read_len, 0);
    if (ret < 0) {
      printf("couldn't start read object! error %d\n", ret);
      ret = EXIT_FAILURE;
      free(read_buf);
      rados_aio_release(read_completion);
      goto out;
    }
    // wait for the request to complete, and check that it succeeded.
    rados_aio_wait_for_complete(read_completion);
    ret = rados_aio_get_return_value(read_completion);
    if (ret < 0) {
      printf("couldn't read object! error %d\n", ret);
      ret = EXIT_FAILURE;
      free(read_buf);
      rados_aio_release(read_completion);
      goto out;
    } else {
      read_buf[ret] = 0; // null-terminate the string
      printf("we read our object %s, and got back %d bytes with contents\n%s\n", object_name, ret, read_buf);
    }

    free(read_buf);
    rados_aio_release(read_completion);
  }

  /*
   * We can also use xattrs that go alongside the object.
   */
  {
    const char* version = "1";
    ret = rados_setxattr(io_ctx, object_name, "version", version, strlen(version));
    if (ret < 0) {
      printf("failed to set xattr version entry! error %d\n", ret);
      ret = EXIT_FAILURE;
      goto out;
    } else {
      printf("we set the xattr 'version' on our object!\n");
    }
  }

  /*
   * And if we want to be really cool, we can do multiple things in a single
   * atomic operation. For instance, we can update the contents of our object
   * and set the version at the same time.
   */
  {
    const char* content = "v2";
    rados_write_op_t write_op = rados_create_write_op();
    if (!write_op) {
      printf("failed to allocate write op\n");
      ret = EXIT_FAILURE;
      goto out;
    }
    rados_write_op_write_full(write_op, content, strlen(content));
    const char* version = "2";
    rados_write_op_setxattr(write_op, "version", version, strlen(version));
    ret = rados_write_op_operate(write_op, io_ctx, object_name, NULL, 0);
    if (ret < 0) {
      printf("failed to do compound write! error %d\n", ret);
      ret = EXIT_FAILURE;
      rados_release_write_op(write_op);
      goto out;
    } else {
      printf("we overwrote our object %s with contents\n%s\n", object_name, content);
    }
    rados_release_write_op(write_op);
  }

  /*
   * And to be even cooler, we can make sure that the object looks the
   * way we expect before doing the write! Notice how this attempt fails
   * because the xattr differs.
   */
  {
    rados_write_op_t failed_write_op = rados_create_write_op();
    if (!failed_write_op) {
      printf("failed to allocate write op\n");
      ret = EXIT_FAILURE;
      goto out;
    }
    const char* content = "v2";
    const char* version = "2";
    const char* old_version = "1";
    rados_write_op_cmpxattr(failed_write_op, "version", LIBRADOS_CMPXATTR_OP_EQ, old_version, strlen(old_version));
    rados_write_op_write_full(failed_write_op, content, strlen(content));
    rados_write_op_setxattr(failed_write_op, "version", version, strlen(version));
    ret = rados_write_op_operate(failed_write_op, io_ctx, object_name, NULL, 0);
    if (ret < 0) {
      printf("we just failed a write because the xattr wasn't as specified\n");
    } else {
      printf("we succeeded on writing despite an xattr comparison mismatch!\n");
      ret = EXIT_FAILURE;
      rados_release_write_op(failed_write_op);
      goto out;
    }
    rados_release_write_op(failed_write_op);

    /*
     * Now let's do the update with the correct xattr values so it
     * actually goes through
     */
    content = "v3";
    old_version = "2";
    version = "3";
    rados_write_op_t update_op = rados_create_write_op();
    if (!failed_write_op) {
      printf("failed to allocate write op\n");
      ret = EXIT_FAILURE;
      goto out;
    }
    rados_write_op_cmpxattr(update_op, "version", LIBRADOS_CMPXATTR_OP_EQ, old_version, strlen(old_version));
    rados_write_op_write_full(update_op, content, strlen(content));
    rados_write_op_setxattr(update_op, "version", version, strlen(version));
    ret = rados_write_op_operate(update_op, io_ctx, object_name, NULL, 0);
    if (ret < 0) {
      printf("failed to do a compound write update! error %d\n", ret);
      ret = EXIT_FAILURE;
      rados_release_write_op(update_op);
      goto out;
    } else {
      printf("we overwrote our object %s following an xattr test with contents\n%s\n", object_name, content);
    }
    rados_release_write_op(update_op);
  }

  ret = EXIT_SUCCESS;

 out:
  if (io_ctx) {
    rados_ioctx_destroy(io_ctx);
  }

  if (pool_created) {
    /*
     * And now we're done, so let's remove our pool and then
     * shut down the connection gracefully.
     */
    int delete_ret = rados_pool_delete(cluster, pool_name);
    if (delete_ret < 0) {
      // be careful not to
      printf("We failed to delete our test pool!\n");
      ret = EXIT_FAILURE;
    }
  }

  rados_shutdown(cluster);

  return ret;
}

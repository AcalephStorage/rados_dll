/*
 * librados_client.c
 *
 *  Created on: Aug 5, 2014
 *      Author: aisrael
 */
#include "rados/librados.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <list>

int main(int argc, const char **argv)
{
  int ret = 0;

  /*
   * Errors are not checked to avoid pollution.
   * After each Ceph operation:
   * if (ret < 0) error_condition
   * else success
   */

  // Get cluster handle and connect to cluster
  std::string cluster_name("ceph");
  std::string user_name("client.admin");
  librados::Rados cluster;
  cluster.init2(user_name.c_str(), cluster_name.c_str(), 0);
  cluster.conf_read_file("ceph.conf");
  cluster.connect();

  // IO context
  librados::IoCtx io_ctx;
  std::string pool_name("data");
  cluster.ioctx_create(pool_name.c_str(), io_ctx);

  // Write an object synchronously
  librados::bufferlist bl;
  std::string objectId("hw");
  std::string objectContent("Hello World!");
  bl.append(objectContent);
  io_ctx.write(objectId, bl, objectContent.size(), 0);

  // Add an xattr to the object.
  librados::bufferlist lang_bl;
  lang_bl.append("en_US");
  io_ctx.setxattr(objectId, "lang", lang_bl);

  // Read the object back asynchronously
  librados::bufferlist read_buf;
  int read_len = 4194304;
  //Create I/O Completion.
  librados::AioCompletion *read_completion = librados::Rados::aio_create_completion();
  //Send read request.
  io_ctx.aio_read(objectId, read_completion, &read_buf, read_len, 0);

  // Wait for the request to complete, and print content
  read_completion->wait_for_complete();
  read_completion->get_return_value();
  std::cout << "Object name: " << objectId << "\n"
            << "Content: " << read_buf.c_str() << std::endl;

  // Read the xattr.
  librados::bufferlist lang_res;
  io_ctx.getxattr(objectId, "lang", lang_res);
  std::cout << "Object xattr: " << lang_res.c_str() << std::endl;


  // Print the list of pools
  std::list<std::string> pools;
  cluster.pool_list(pools);
  std::cout << "List of pools from this cluster handle" << std::endl;
  for (auto pool_id : pools) {
    std::cout << "\t" << pool_id << std::endl;
  }

  // Print the list of objects
  librados::ObjectIterator oit = io_ctx.objects_begin();
  librados::ObjectIterator oet = io_ctx.objects_end();
  std::cout << "List of objects from this pool" << std::endl;
  for (; oit != oet; oit++) {
    std::cout << "\t" << oit->first << std::endl;
  }

  // Remove the xattr
  io_ctx.rmxattr(objectId, "lang");

  // Remove the object.
  io_ctx.remove(objectId);

  // Cleanup
  io_ctx.close();
  cluster.shutdown();

  return 0;
}

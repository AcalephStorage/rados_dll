// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2012 Sage Weil <sage@newdream.net> and others
 *
 * LGPL2.  See file COPYING.
 *
 */
#include "include/int_types.h"

#include "mon/MonClient.h"
#include "common/config.h"

#include "common/errno.h"
#include "common/ceph_argparse.h"
#include "common/strtol.h"
#include "global/global_init.h"
#include "common/safe_io.h"
#include "include/krbd.h"
#include "include/stringify.h"
#include "include/rados/librados.hpp"
#include "include/rbd/librbd.hpp"
#include "include/byteorder.h"

#include "include/intarith.h"

#include "include/compat.h"
#include "common/blkdev.h"

#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics/stats.hpp>
#include <boost/accumulators/statistics/rolling_sum.hpp>
#include <boost/assign/list_of.hpp>
#include <boost/scope_exit.hpp>
#include <boost/scoped_ptr.hpp>
#include <errno.h>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdlib.h>
#include <sys/types.h>
#include <time.h>
#include "include/memory.h"
#include <sys/ioctl.h>

#include "include/rbd_types.h"
#include "common/TextTable.h"
#include "include/util.h"

#include "common/Formatter.h"
#include "common/Throttle.h"

#if defined(__linux__)
#include <linux/fs.h>
#endif

#if defined(__FreeBSD__)
#include <sys/param.h>
#endif

#define MAX_SECRET_LEN 1000
#define MAX_POOL_NAME_SIZE 128

#define RBD_DIFF_BANNER "rbd diff v1\n"

static string dir_oid = RBD_DIRECTORY;
static string dir_info_oid = RBD_INFO;

bool progress = true;
bool resize_allow_shrink = false;

map<string, string> map_options; // -o / --options map

#define dout_subsys ceph_subsys_rbd

void usage()
{
  cout <<
"usage: rbd [-n <auth user>] [OPTIONS] <cmd> ...\n"
"where 'pool' is a rados pool name (default is 'rbd') and 'cmd' is one of:\n"
"  (ls | list) [-l | --long ] [pool-name] list rbd images\n"
"                                              (-l includes snapshots/clones)\n"
"  info <image-name>                           show information about image size,\n"
"                                              striping, etc.\n"
"  create [--order <bits>] [--image-shared] --size <MB> <name>\n"
"                                              create an empty image\n"
"  clone [--order <bits>] [--image-shared] <parentsnap> <clonename>\n"
"                                              clone a snapshot into a COW\n"
"                                              child image\n"
"  children <snap-name>                        display children of snapshot\n"
"  flatten <image-name>                        fill clone with parent data\n"
"                                              (make it independent)\n"
"  resize --size <MB> <image-name>             resize (expand or contract) image\n"
"  rm <image-name>                             delete an image\n"
"  export <image-name> <path>                  export image to file\n"
"                                              \"-\" for stdout\n"
"  import [--image-shared] <path> <image-name> import image from file\n"
"                                              (dest defaults\n"
"                                               as the filename part of file)\n"
"                                              \"-\" for stdin\n"
"  diff <image-name> [--from-snap <snap-name>] print extents that differ since\n"
"                                              a previous snap, or image creation\n"
"  export-diff <image-name> [--from-snap <snap-name>] <path>\n"
"                                              export an incremental diff to\n"
"                                              path, or \"-\" for stdout\n"
"  merge-diff <diff1> <diff2> <path>           merge <diff1> and <diff2> into\n"
"                                              <path>, <diff1> could be \"-\"\n"
"                                              for stdin, and <path> could be \"-\"\n"
"                                              for stdout\n"
"  import-diff <path> <image-name>             import an incremental diff from\n"
"                                              path or \"-\" for stdin\n"
"  (cp | copy) <src> <dest>                    copy src image to dest\n"
"  (mv | rename) <src> <dest>                  rename src image to dest\n"
"  snap ls <image-name>                        dump list of image snapshots\n"
"  snap create <snap-name>                     create a snapshot\n"
"  snap rollback <snap-name>                   rollback image to snapshot\n"
"  snap rm <snap-name>                         deletes a snapshot\n"
"  snap purge <image-name>                     deletes all snapshots\n"
"  snap protect <snap-name>                    prevent a snapshot from being deleted\n"
"  snap unprotect <snap-name>                  allow a snapshot to be deleted\n"
"  watch <image-name>                          watch events on image\n"
"  status <image-name>                         show the status of this image\n"
"  map <image-name>                            map image to a block device\n"
"                                              using the kernel\n"
"  unmap <device>                              unmap a rbd device that was\n"
"                                              mapped by the kernel\n"
"  showmapped                                  show the rbd images mapped\n"
"                                              by the kernel\n"
"  lock list <image-name>                      show locks held on an image\n"
"  lock add <image-name> <id> [--shared <tag>] take a lock called id on an image\n"
"  lock remove <image-name> <id> <locker>      release a lock on an image\n"
"  bench-write <image-name>                    simple write benchmark\n"
"                 --io-size <bytes>              write size\n"
"                 --io-threads <num>             ios in flight\n"
"                 --io-total <bytes>             total bytes to write\n"
"                 --io-pattern <seq|rand>        write pattern\n"
"\n"
"<image-name>, <snap-name> are [pool/]name[@snap], or you may specify\n"
"individual pieces of names with -p/--pool, --image, and/or --snap.\n"
"\n"
"Other input options:\n"
"  -p, --pool <pool>                  source pool name\n"
"  --image <image-name>               image name\n"
"  --dest <image-name>                destination [pool and] image name\n"
"  --snap <snap-name>                 snapshot name\n"
"  --dest-pool <name>                 destination pool name\n"
"  --path <path-name>                 path name for import/export\n"
"  --size <size in MB>                size of image for create and resize\n"
"  --order <bits>                     the object size in bits; object size will be\n"
"                                     (1 << order) bytes. Default is 22 (4 MB).\n"
"  --image-format <format-number>     format to use when creating an image\n"
"                                     format 1 is the original format (default)\n"
"                                     format 2 supports cloning\n"
"  --image-shared                     image will be used concurrently (disables\n"
"                                     RBD exclusive lock and dependent features)\n"
"  --id <username>                    rados user (without 'client.'prefix) to\n"
"                                     authenticate as\n"
"  --keyfile <path>                   file containing secret key for use with cephx\n"
"  --shared <tag>                     take a shared (rather than exclusive) lock\n"
"  --format <output-format>           output format (default: plain, json, xml)\n"
"  --pretty-format                    make json or xml output more readable\n"
"  --no-progress                      do not show progress for long-running commands\n"
"  -o, --options <map-options>        options to use when mapping an image\n"
"  --read-only                        set device readonly when mapping image\n"
"  --allow-shrink                     allow shrinking of an image when resizing\n";
}

static string feature_str(uint64_t feature)
{
  switch (feature) {
  case RBD_FEATURE_LAYERING:
    return "layering";
  case RBD_FEATURE_STRIPINGV2:
    return "striping";
  case RBD_FEATURE_EXCLUSIVE_LOCK:
    return "exclusive";
  case RBD_FEATURE_OBJECT_MAP:
    return "object map";
  default:
    return "";
  }
}

static string features_str(uint64_t features)
{
  string s = "";

  for (uint64_t feature = 1; feature <= RBD_FEATURE_OBJECT_MAP;
       feature <<= 1) {
    if (feature & features) {
      if (s.size())
	s += ", ";
      s += feature_str(feature);
    }
  }
  return s;
}

static void format_features(Formatter *f, uint64_t features)
{
  f->open_array_section("features");
  for (uint64_t feature = 1; feature <= RBD_FEATURE_OBJECT_MAP;
       feature <<= 1) {
    f->dump_string("feature", feature_str(feature));
  }
  f->close_section();
}

static void format_flags(Formatter *f, uint64_t flags)
{
  int count = 0;
  std::map<uint64_t, std::string> flag_mapping = boost::assign::map_list_of(
    RBD_FLAG_OBJECT_MAP_INVALID, "object map invalid");

  if (f == NULL) {
    cout << "\tflags: ";
  } else {
    f->open_array_section("flags");
  }
  for (std::map<uint64_t, std::string>::iterator it = flag_mapping.begin();
       it != flag_mapping.end(); ++it) {
    if ((it->first & flags) == 0) {
      continue;
    }

    if (f == NULL) {
      if (count++ > 0) {
        cout << ", ";
      }
      cout << it->second;
    } else {
      f->dump_string("flag", it->second);
    }
  }
  if (f == NULL) {
    cout << std::endl;
  } else {
    f->close_section();
  }
}

struct MyProgressContext : public librbd::ProgressContext {
  const char *operation;
  int last_pc;

  MyProgressContext(const char *o) : operation(o), last_pc(0) {
  }

  int update_progress(uint64_t offset, uint64_t total) {
    if (progress) {
      int pc = total ? (offset * 100ull / total) : 0;
      if (pc != last_pc) {
	cerr << "\r" << operation << ": "
	  //	   << offset << " / " << total << " "
	     << pc << "% complete...";
	cerr.flush();
	last_pc = pc;
      }
    }
    return 0;
  }
  void finish() {
    if (progress) {
      cerr << "\r" << operation << ": 100% complete...done." << std::endl;
    }
  }
  void fail() {
    if (progress) {
      cerr << "\r" << operation << ": " << last_pc << "% complete...failed."
	   << std::endl;
    }
  }
};

static int get_outfmt(const char *output_format,
		      bool pretty,
		      boost::scoped_ptr<Formatter> *f)
{
  if (!strcmp(output_format, "json")) {
    f->reset(new JSONFormatter(pretty));
  } else if (!strcmp(output_format, "xml")) {
    f->reset(new XMLFormatter(pretty));
  } else if (strcmp(output_format, "plain")) {
    cerr << "rbd: unknown format '" << output_format << "'" << std::endl;
    return -EINVAL;
  }

  return 0;
}

static int do_list(librbd::RBD &rbd, librados::IoCtx& io_ctx, bool lflag,
		   Formatter *f)
{
  std::vector<string> names;
  int r = rbd.list(io_ctx, names);
  if (r == -ENOENT)
    r = 0;
  if (r < 0)
    return r;

  if (!lflag) {
    if (f)
      f->open_array_section("images");
    for (std::vector<string>::const_iterator i = names.begin();
       i != names.end(); ++i) {
       if (f)
	 f->dump_string("name", *i);
       else
	 cout << *i << std::endl;
    }
    if (f) {
      f->close_section();
      f->flush(cout);
    }
    return 0;
  }

  TextTable tbl;

  if (f) {
    f->open_array_section("images");
  } else {
    tbl.define_column("NAME", TextTable::LEFT, TextTable::LEFT);
    tbl.define_column("SIZE", TextTable::RIGHT, TextTable::RIGHT);
    tbl.define_column("PARENT", TextTable::LEFT, TextTable::LEFT);
    tbl.define_column("FMT", TextTable::RIGHT, TextTable::RIGHT);
    tbl.define_column("PROT", TextTable::LEFT, TextTable::LEFT);
    tbl.define_column("LOCK", TextTable::LEFT, TextTable::LEFT);
  }

  string pool, image, snap, parent;

  for (std::vector<string>::const_iterator i = names.begin();
       i != names.end(); ++i) {
    librbd::image_info_t info;
    librbd::Image im;

    r = rbd.open_read_only(io_ctx, im, i->c_str(), NULL);
    // image might disappear between rbd.list() and rbd.open(); ignore
    // that, warn about other possible errors (EPERM, say, for opening
    // an old-format image, because you need execute permission for the
    // class method)
    if (r < 0) {
      if (r != -ENOENT) {
	cerr << "rbd: error opening " << *i << ": " << cpp_strerror(r)
	     << std::endl;
      }
      // in any event, continue to next image
      continue;
    }

    // handle second-nth trips through loop
    parent.clear();
    r = im.parent_info(&pool, &image, &snap);
    if (r < 0 && r != -ENOENT)
      return r;

    bool has_parent = false;
    if (r != -ENOENT) {
      parent = pool + "/" + image + "@" + snap;
      has_parent = true;
    }

    if (im.stat(info, sizeof(info)) < 0)
      return -EINVAL;

    uint8_t old_format;
    im.old_format(&old_format);

    list<librbd::locker_t> lockers;
    bool exclusive;
    r = im.list_lockers(&lockers, &exclusive, NULL);
    if (r < 0)
      return r;
    string lockstr;
    if (!lockers.empty()) {
      lockstr = (exclusive) ? "excl" : "shr";
    }

    if (f) {
      f->open_object_section("image");
      f->dump_string("image", *i);
      f->dump_unsigned("size", info.size);
      if (has_parent) {
	f->open_object_section("parent");
	f->dump_string("pool", pool);
	f->dump_string("image", image);
	f->dump_string("snapshot", snap);
	f->close_section();
      }
      f->dump_int("format", old_format ? 1 : 2);
      if (!lockers.empty())
	f->dump_string("lock_type", exclusive ? "exclusive" : "shared");
      f->close_section();
    } else {
      tbl << *i
	  << stringify(si_t(info.size))
	  << parent
	  << ((old_format) ? '1' : '2')
	  << ""				// protect doesn't apply to images
	  << lockstr
	  << TextTable::endrow;
    }

    vector<librbd::snap_info_t> snaplist;
    if (im.snap_list(snaplist) >= 0 && !snaplist.empty()) {
      for (std::vector<librbd::snap_info_t>::iterator s = snaplist.begin();
	   s != snaplist.end(); ++s) {
	bool is_protected;
	bool has_parent = false;
	parent.clear();
	im.snap_set(s->name.c_str());
	r = im.snap_is_protected(s->name.c_str(), &is_protected);
	if (r < 0)
	  return r;
	if (im.parent_info(&pool, &image, &snap) >= 0) {
	  parent = pool + "/" + image + "@" + snap;
	  has_parent = true;
	}
	if (f) {
	  f->open_object_section("snapshot");
	  f->dump_string("image", *i);
	  f->dump_string("snapshot", s->name);
	  f->dump_unsigned("size", s->size);
	  if (has_parent) {
	    f->open_object_section("parent");
	    f->dump_string("pool", pool);
	    f->dump_string("image", image);
	    f->dump_string("snapshot", snap);
	    f->close_section();
	  }
	  f->dump_int("format", old_format ? 1 : 2);
	  f->dump_string("protected", is_protected ? "true" : "false");
	  f->close_section();
	} else {
	  tbl << *i + "@" + s->name
	      << stringify(si_t(s->size))
	      << parent
	      << ((old_format) ? '1' : '2')
	      << (is_protected ? "yes" : "")
	      << "" 			// locks don't apply to snaps
	      << TextTable::endrow;
	}
      }
    }
  }
  if (f) {
    f->close_section();
    f->flush(cout);
  } else if (!names.empty()) {
    cout << tbl;
  }

  return 0;
}

static int do_create(librbd::RBD &rbd, librados::IoCtx& io_ctx,
		     const char *imgname, uint64_t size, int *order,
		     int format, uint64_t features,
		     uint64_t stripe_unit, uint64_t stripe_count)
{
  int r;

  if (format == 1) {
    // weird striping not allowed with format 1!
    if ((stripe_unit || stripe_count) &&
	(stripe_unit != (1ull << *order) && stripe_count != 1)) {
      cerr << "non-default striping not allowed with format 1; use --image-format 2"
	   << std::endl;
      return -EINVAL;
    }
    r = rbd.create(io_ctx, imgname, size, order);
  } else {
    if (features == 0) {
      features = RBD_FEATURE_LAYERING | RBD_FEATURE_EXCLUSIVE_LOCK;
    }
    if (g_conf->rbd_object_map) {
      features |= RBD_FEATURE_OBJECT_MAP;
    }
    if ((stripe_unit || stripe_count) &&
	(stripe_unit != (1ull << *order) && stripe_count != 1)) {
      features |= RBD_FEATURE_STRIPINGV2;
    }
    r = rbd.create3(io_ctx, imgname, size, features, order,
		    stripe_unit, stripe_count);
  }
  if (r < 0)
    return r;
  return 0;
}

static int do_clone(librbd::RBD &rbd, librados::IoCtx &p_ioctx,
		    const char *p_name, const char *p_snapname,
		    librados::IoCtx &c_ioctx, const char *c_name,
		    uint64_t features, int *c_order)
{
  if (features == 0) {
    features = (RBD_FEATURES_ALL & ~RBD_FEATURE_OBJECT_MAP);
  }
  else if ((features & RBD_FEATURE_LAYERING) != RBD_FEATURE_LAYERING) {
    return -EINVAL;
  }
  if (g_conf->rbd_object_map) {
    features |= RBD_FEATURE_OBJECT_MAP;
  }

  return rbd.clone(p_ioctx, p_name, p_snapname, c_ioctx, c_name, features,
		    c_order);
}

static int do_flatten(librbd::Image& image)
{
  MyProgressContext pc("Image flatten");
  int r = image.flatten_with_progress(pc);
  if (r < 0) {
    pc.fail();
    return r;
  }
  pc.finish();
  return 0;
}

static int do_rename(librbd::RBD &rbd, librados::IoCtx& io_ctx,
		     const char *imgname, const char *destname)
{
  int r = rbd.rename(io_ctx, imgname, destname);
  if (r < 0)
    return r;
  return 0;
}

static int do_show_info(const char *imgname, librbd::Image& image,
			const char *snapname, Formatter *f)
{
  librbd::image_info_t info;
  string parent_pool, parent_name, parent_snapname;
  uint8_t old_format;
  uint64_t overlap, features, flags;
  bool snap_protected = false;
  int r;

  r = image.stat(info, sizeof(info));
  if (r < 0)
    return r;

  r = image.old_format(&old_format);
  if (r < 0)
    return r;

  r = image.overlap(&overlap);
  if (r < 0)
    return r;

  r = image.features(&features);
  if (r < 0)
    return r;

  r = image.get_flags(&flags);
  if (r < 0) {
    return r;
  }

  if (snapname) {
    r = image.snap_is_protected(snapname, &snap_protected);
    if (r < 0)
      return r;
  }

  char prefix[RBD_MAX_BLOCK_NAME_SIZE + 1];
  strncpy(prefix, info.block_name_prefix, RBD_MAX_BLOCK_NAME_SIZE);
  prefix[RBD_MAX_BLOCK_NAME_SIZE] = '\0';

  if (f) {
    f->open_object_section("image");
    f->dump_string("name", imgname);
    f->dump_unsigned("size", info.size);
    f->dump_unsigned("objects", info.num_objs);
    f->dump_int("order", info.order);
    f->dump_unsigned("object_size", info.obj_size);
    f->dump_string("block_name_prefix", prefix);
    f->dump_int("format", (old_format ? 1 : 2));
  } else {
    cout << "rbd image '" << imgname << "':\n"
	 << "\tsize " << prettybyte_t(info.size) << " in "
	 << info.num_objs << " objects"
	 << std::endl
	 << "\torder " << info.order
	 << " (" << prettybyte_t(info.obj_size) << " objects)"
	 << std::endl
	 << "\tblock_name_prefix: " << prefix
	 << std::endl
	 << "\tformat: " << (old_format ? "1" : "2")
	 << std::endl;
  }

  if (!old_format) {
    if (f)
      format_features(f, features);
    else
      cout << "\tfeatures: " << features_str(features) << std::endl;
    format_flags(f, flags);
  }

  // snapshot info, if present
  if (snapname) {
    if (f) {
      f->dump_string("protected", snap_protected ? "true" : "false");
    } else {
      cout << "\tprotected: " << (snap_protected ? "True" : "False")
	   << std::endl;
    }
  }

  // parent info, if present
  if ((image.parent_info(&parent_pool, &parent_name, &parent_snapname) == 0) &&
      parent_name.length() > 0) {
    if (f) {
      f->open_object_section("parent");
      f->dump_string("pool", parent_pool);
      f->dump_string("image", parent_name);
      f->dump_string("snapshot", parent_snapname);
      f->dump_unsigned("overlap", overlap);
      f->close_section();
    } else {
      cout << "\tparent: " << parent_pool << "/" << parent_name
	   << "@" << parent_snapname << std::endl;
      cout << "\toverlap: " << prettybyte_t(overlap) << std::endl;
    }
  }

  // striping info, if feature is set
  if (features & RBD_FEATURE_STRIPINGV2) {
    if (f) {
      f->dump_unsigned("stripe_unit", image.get_stripe_unit());
      f->dump_unsigned("stripe_count", image.get_stripe_count());
    } else {
      cout << "\tstripe unit: " << prettybyte_t(image.get_stripe_unit())
	   << std::endl
	   << "\tstripe count: " << image.get_stripe_count() << std::endl;
    }
  }

  if (f) {
    f->close_section();
    f->flush(cout);
  }

  return 0;
}

static int do_delete(librbd::RBD &rbd, librados::IoCtx& io_ctx,
		     const char *imgname)
{
  MyProgressContext pc("Removing image");
  int r = rbd.remove_with_progress(io_ctx, imgname, pc);
  if (r < 0) {
    pc.fail();
    return r;
  }
  pc.finish();
  return 0;
}

static int do_resize(librbd::Image& image, uint64_t size)
{
  MyProgressContext pc("Resizing image");
  int r = image.resize_with_progress(size, pc);
  if (r < 0) {
    pc.fail();
    return r;
  }
  pc.finish();
  return 0;
}

static int do_list_snaps(librbd::Image& image, Formatter *f)
{
  std::vector<librbd::snap_info_t> snaps;
  TextTable t;
  int r;

  r = image.snap_list(snaps);
  if (r < 0)
    return r;

  if (f) {
    f->open_array_section("snapshots");
  } else {
    t.define_column("SNAPID", TextTable::RIGHT, TextTable::RIGHT);
    t.define_column("NAME", TextTable::LEFT, TextTable::LEFT);
    t.define_column("SIZE", TextTable::RIGHT, TextTable::RIGHT);
  }

  for (std::vector<librbd::snap_info_t>::iterator s = snaps.begin();
       s != snaps.end(); ++s) {
    if (f) {
      f->open_object_section("snapshot");
      f->dump_unsigned("id", s->id);
      f->dump_string("name", s->name);
      f->dump_unsigned("size", s->size);
      f->close_section();
    } else {
      t << s->id << s->name << stringify(prettybyte_t(s->size))
	<< TextTable::endrow;
    }
  }

  if (f) {
    f->close_section();
    f->flush(cout);
  } else if (snaps.size()) {
    cout << t;
  }

  return 0;
}

static int do_add_snap(librbd::Image& image, const char *snapname)
{
  int r = image.snap_create(snapname);
  if (r < 0)
    return r;

  return 0;
}

static int do_remove_snap(librbd::Image& image, const char *snapname)
{
  int r = image.snap_remove(snapname);
  if (r < 0)
    return r;

  return 0;
}

static int do_rollback_snap(librbd::Image& image, const char *snapname)
{
  MyProgressContext pc("Rolling back to snapshot");
  int r = image.snap_rollback_with_progress(snapname, pc);
  if (r < 0) {
    pc.fail();
    return r;
  }
  pc.finish();
  return 0;
}

static int do_purge_snaps(librbd::Image& image)
{
  MyProgressContext pc("Removing all snapshots");
  std::vector<librbd::snap_info_t> snaps;
  int r = image.snap_list(snaps);
  if (r < 0) {
    pc.fail();
    return r;
  }

  for (size_t i = 0; i < snaps.size(); ++i) {
    r = image.snap_remove(snaps[i].name.c_str());
    if (r < 0) {
      pc.fail();
      return r;
    }
    pc.update_progress(i + 1, snaps.size());
  }

  pc.finish();
  return 0;
}

static int do_protect_snap(librbd::Image& image, const char *snapname)
{
  int r = image.snap_protect(snapname);
  if (r < 0)
    return r;

  return 0;
}

static int do_unprotect_snap(librbd::Image& image, const char *snapname)
{
  int r = image.snap_unprotect(snapname);
  if (r < 0)
    return r;

  return 0;
}

static int do_list_children(librbd::Image &image, Formatter *f)
{
  set<pair<string, string> > children;
  int r;

  r = image.list_children(&children);
  if (r < 0)
    return r;

  if (f)
    f->open_array_section("children");

  for (set<pair<string, string> >::const_iterator child_it = children.begin();
       child_it != children.end(); child_it++) {
    if (f) {
      f->open_object_section("child");
      f->dump_string("pool", child_it->first);
      f->dump_string("image", child_it->second);
      f->close_section();
    } else {
      cout << child_it->first << "/" << child_it->second << std::endl;
    }
  }

  if (f) {
    f->close_section();
    f->flush(cout);
  }

  return 0;
}

static int do_lock_list(librbd::Image& image, Formatter *f)
{
  list<librbd::locker_t> lockers;
  bool exclusive;
  string tag;
  TextTable tbl;
  int r;

  r = image.list_lockers(&lockers, &exclusive, &tag);
  if (r < 0)
    return r;

  if (f) {
    f->open_object_section("locks");
  } else {
    tbl.define_column("Locker", TextTable::LEFT, TextTable::LEFT);
    tbl.define_column("ID", TextTable::LEFT, TextTable::LEFT);
    tbl.define_column("Address", TextTable::LEFT, TextTable::LEFT);
  }

  if (lockers.size()) {
    bool one = (lockers.size() == 1);

    if (!f) {
      cout << "There " << (one ? "is " : "are ") << lockers.size()
	   << (exclusive ? " exclusive" : " shared")
	   << " lock" << (one ? "" : "s") << " on this image.\n";
      if (!exclusive)
	cout << "Lock tag: " << tag << "\n";
    }

    for (list<librbd::locker_t>::const_iterator it = lockers.begin();
	 it != lockers.end(); ++it) {
      if (f) {
	f->open_object_section(it->cookie.c_str());
	f->dump_string("locker", it->client);
	f->dump_string("address", it->address);
	f->close_section();
      } else {
	tbl << it->client << it->cookie << it->address << TextTable::endrow;
      }
    }
    if (!f)
      cout << tbl;
  }

  if (f) {
    f->close_section();
    f->flush(cout);
  }
  return 0;
}

static int do_lock_add(librbd::Image& image, const char *cookie,
		       const char *tag)
{
  if (tag)
    return image.lock_shared(cookie, tag);
  else
    return image.lock_exclusive(cookie);
}

static int do_lock_remove(librbd::Image& image, const char *client,
			  const char *cookie)
{
  return image.break_lock(client, cookie);
}

static void rbd_bencher_completion(void *c, void *pc);

struct rbd_bencher;

struct rbd_bencher {
  librbd::Image *image;
  Mutex lock;
  Cond cond;
  int in_flight;

  rbd_bencher(librbd::Image *i)
    : image(i),
      lock("rbd_bencher::lock"),
      in_flight(0)
  { }

  bool start_write(int max, uint64_t off, uint64_t len, bufferlist& bl,
		   int op_flags)
  {
    {
      Mutex::Locker l(lock);
      if (in_flight >= max)
	return false;
      in_flight++;
    }
    librbd::RBD::AioCompletion *c =
      new librbd::RBD::AioCompletion((void *)this, rbd_bencher_completion);
    image->aio_write2(off, len, bl, c, op_flags);
    //cout << "start " << c << " at " << off << "~" << len << std::endl;
    return true;
  }

  void wait_for(int max) {
    Mutex::Locker l(lock);
    while (in_flight > max) {
      utime_t dur;
      dur.set_from_double(.2);
      cond.WaitInterval(g_ceph_context, lock, dur);
    }
  }

};

void rbd_bencher_completion(void *vc, void *pc)
{
  librbd::RBD::AioCompletion *c = (librbd::RBD::AioCompletion *)vc;
  rbd_bencher *b = static_cast<rbd_bencher *>(pc);
  //cout << "complete " << c << std::endl;
  int ret = c->get_return_value();
  if (ret != 0) {
    cout << "write error: " << cpp_strerror(ret) << std::endl;
    assert(0 == ret);
  }
  b->lock.Lock();
  b->in_flight--;
  b->cond.Signal();
  b->lock.Unlock();
  c->release();
}

static int do_bench_write(librbd::Image& image, uint64_t io_size,
			  uint64_t io_threads, uint64_t io_bytes,
			  string pattern)
{
  rbd_bencher b(&image);

  cout << "bench-write "
       << " io_size " << io_size
       << " io_threads " << io_threads
       << " bytes " << io_bytes
       << " pattern " << pattern
       << std::endl;

  if (pattern != "rand" && pattern != "seq")
    return -EINVAL;

  srand(time(NULL) % (unsigned long) -1);

  bufferptr bp(io_size);
  memset(bp.c_str(), rand() & 0xff, io_size);
  bufferlist bl;
  bl.push_back(bp);

  utime_t start = ceph_clock_now(NULL);
  utime_t last;
  unsigned ios = 0;

  uint64_t size = 0;
  image.size(&size);

  vector<uint64_t> thread_offset;
  uint64_t i;
  uint64_t start_pos;

  // disturb all thread's offset, used by seq write
  for (i = 0; i < io_threads; i++) {
    start_pos = (rand() % (size / io_size)) * io_size;
    thread_offset.push_back(start_pos);
  }

  const int WINDOW_SIZE = 5;
  typedef boost::accumulators::accumulator_set<
    double, boost::accumulators::stats<
      boost::accumulators::tag::rolling_sum> > RollingSum;

  RollingSum time_acc(
    boost::accumulators::tag::rolling_window::window_size = WINDOW_SIZE);
  RollingSum ios_acc(
    boost::accumulators::tag::rolling_window::window_size = WINDOW_SIZE);
  RollingSum off_acc(
    boost::accumulators::tag::rolling_window::window_size = WINDOW_SIZE);
  uint64_t cur_ios = 0;
  uint64_t cur_off = 0;

  int op_flags;
  if  (pattern == "rand") {
    op_flags = LIBRADOS_OP_FLAG_FADVISE_RANDOM;
  } else {
    op_flags = LIBRADOS_OP_FLAG_FADVISE_SEQUENTIAL;
  }

  printf("  SEC       OPS   OPS/SEC   BYTES/SEC\n");
  uint64_t off;
  for (off = 0; off < io_bytes; ) {
    b.wait_for(io_threads - 1);
    i = 0;
    while (i < io_threads && off < io_bytes &&
	   b.start_write(io_threads, thread_offset[i], io_size, bl, op_flags)) {
      ++i;
      ++ios;
      off += io_size;

      ++cur_ios;
      cur_off += io_size;

      if (pattern == "rand") {
        thread_offset[i] = (rand() % (size / io_size)) * io_size;
      } else {
        thread_offset[i] += io_size;
        if (thread_offset[i] + io_size > size)
          thread_offset[i] = 0;
      }
    }

    utime_t now = ceph_clock_now(NULL);
    utime_t elapsed = now - start;
    if (last.is_zero()) {
      last = elapsed;
    } else if (elapsed.sec() != last.sec()) {
      time_acc(elapsed - last);
      ios_acc(static_cast<double>(cur_ios));
      off_acc(static_cast<double>(cur_off));
      cur_ios = 0;
      cur_off = 0;

      double time_sum = boost::accumulators::rolling_sum(time_acc);
      printf("%5d  %8d  %8.2lf  %8.2lf\n",
             (int)elapsed,
             (int)(ios - io_threads),
             boost::accumulators::rolling_sum(ios_acc) / time_sum,
             boost::accumulators::rolling_sum(off_acc) / time_sum);
      last = elapsed;
    }
  }
  b.wait_for(0);
  int r = image.flush();
  if (r < 0) {
    cerr << "Error flushing data at the end: " << cpp_strerror(r) << std::endl;
  }

  utime_t now = ceph_clock_now(NULL);
  double elapsed = now - start;

  printf("elapsed: %5d  ops: %8d  ops/sec: %8.2lf  bytes/sec: %8.2lf\n",
	 (int)elapsed, ios, (double)ios / elapsed, (double)off / elapsed);

  return 0;
}

struct ExportContext {
  librbd::Image *image;
  int fd;
  uint64_t totalsize;
  MyProgressContext pc;

  ExportContext(librbd::Image *i, int f, uint64_t t) :
    image(i),
    fd(f),
    totalsize(t),
    pc("Exporting image")
  {}
};

class AioExportContext : public Context
{
public:
  AioExportContext(SimpleThrottle &simple_throttle, librbd::Image &image,
                   uint64_t offset, uint64_t length, int fd)
    : m_aio_completion(
        new librbd::RBD::AioCompletion(this, &AioExportContext::aio_callback)),
      m_throttle(simple_throttle),
      m_offset(offset),
      m_fd(fd)
  {
    m_throttle.start_op();

    int op_flags = LIBRADOS_OP_FLAG_FADVISE_SEQUENTIAL |
		   LIBRADOS_OP_FLAG_FADVISE_NOCACHE;
    int r = image.aio_read2(offset, length, m_bufferlist, m_aio_completion,
			    op_flags);
    if (r < 0) {
      cerr << "rbd: error requesting read from source image" << std::endl;
      m_throttle.end_op(r);
    }
  }

  virtual ~AioExportContext()
  {
    m_aio_completion->release();
  }

  virtual void finish(int r)
  {
    BOOST_SCOPE_EXIT((&m_throttle) (&r))
    {
      m_throttle.end_op(r);
    } BOOST_SCOPE_EXIT_END

    if (r < 0) {
      cerr << "rbd: error reading from source image at offset "
           << m_offset << ": " << cpp_strerror(r) << std::endl;
      return;
    }

    assert(m_bufferlist.length() == static_cast<size_t>(r));
    if (m_fd != STDOUT_FILENO) {
      if (m_bufferlist.is_zero()) {
        return;
      }

      uint64_t chkret = lseek64(m_fd, m_offset, SEEK_SET);
      if (chkret != m_offset) {
        cerr << "rbd: error seeking destination image to offset "
             << m_offset << std::endl;
        r = -errno;
        return;
      }
    }

    r = m_bufferlist.write_fd(m_fd);
    if (r < 0) {
      cerr << "rbd: error writing to destination image at offset "
           << m_offset << std::endl;
    }
  }

  static void aio_callback(librbd::completion_t completion, void *arg)
  {
    librbd::RBD::AioCompletion *aio_completion =
      reinterpret_cast<librbd::RBD::AioCompletion*>(completion);
    AioExportContext *export_context = reinterpret_cast<AioExportContext*>(arg);
    export_context->complete(aio_completion->get_return_value());
  }

private:
  librbd::RBD::AioCompletion *m_aio_completion;
  SimpleThrottle &m_throttle;
  bufferlist m_bufferlist;
  uint64_t m_offset;
  int m_fd;
};

static int do_export(librbd::Image& image, const char *path)
{
  librbd::image_info_t info;
  int64_t r = image.stat(info, sizeof(info));
  if (r < 0)
    return r;

  int fd;
  int max_concurrent_ops;
  bool to_stdout = (strcmp(path, "-") == 0);
  if (to_stdout) {
    fd = STDOUT_FILENO;
    max_concurrent_ops = 1;
  } else {
    max_concurrent_ops = max(g_conf->rbd_concurrent_management_ops, 1);
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
      return -errno;
    }
    posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
  }

  MyProgressContext pc("Exporting image");

  SimpleThrottle throttle(max_concurrent_ops, false);
  uint64_t period = image.get_stripe_count() * (1ull << info.order);
  for (uint64_t offset = 0; offset < info.size; offset += period) {
    uint64_t length = min(period, info.size - offset);
    new AioExportContext(throttle, image, offset, length, fd);
    pc.update_progress(offset, info.size);
  }

  r = throttle.wait_for_ret();
  if (!to_stdout) {
    if (r >= 0) {
      r = ftruncate(fd, info.size);
    }
    close(fd);
  }

  if (r < 0) {
    pc.fail();
  } else {
    pc.finish();
  }
  return r;
}

static int export_diff_cb(uint64_t ofs, size_t _len, int exists, void *arg)
{
  ExportContext *ec = static_cast<ExportContext *>(arg);
  int r;

  // extent
  bufferlist bl;
  __u8 tag = exists ? 'w' : 'z';
  ::encode(tag, bl);
  ::encode(ofs, bl);
  uint64_t len = _len;
  ::encode(len, bl);
  r = bl.write_fd(ec->fd);
  if (r < 0)
    return r;

  if (exists) {
    // read block
    bl.clear();
    r = ec->image->read2(ofs, len, bl, LIBRADOS_OP_FLAG_FADVISE_NOCACHE);
    if (r < 0)
      return r;
    r = bl.write_fd(ec->fd);
    if (r < 0)
      return r;
  }

  ec->pc.update_progress(ofs, ec->totalsize);

  return 0;
}

static int do_export_diff(librbd::Image& image, const char *fromsnapname,
			  const char *endsnapname,
			  const char *path)
{
  int r;
  librbd::image_info_t info;
  int fd;

  r = image.stat(info, sizeof(info));
  if (r < 0)
    return r;

  if (strcmp(path, "-") == 0)
    fd = 1;
  else
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
  if (fd < 0)
    return -errno;

  {
    // header
    bufferlist bl;
    bl.append(RBD_DIFF_BANNER, strlen(RBD_DIFF_BANNER));

    __u8 tag;
    if (fromsnapname) {
      tag = 'f';
      ::encode(tag, bl);
      string from(fromsnapname);
      ::encode(from, bl);
    }

    if (endsnapname) {
      tag = 't';
      ::encode(tag, bl);
      string to(endsnapname);
      ::encode(to, bl);
    }

    tag = 's';
    ::encode(tag, bl);
    uint64_t endsize = info.size;
    ::encode(endsize, bl);

    r = bl.write_fd(fd);
    if (r < 0) {
      close(fd);
      return r;
    }
  }

  ExportContext ec(&image, fd, info.size);
  r = image.diff_iterate(fromsnapname, 0, info.size, export_diff_cb, (void *)&ec);
  if (r < 0)
    goto out;

  {
    __u8 tag = 'e';
    bufferlist bl;
    ::encode(tag, bl);
    r = bl.write_fd(fd);
  }

 out:
  close(fd);
  if (r < 0)
    ec.pc.fail();
  else
    ec.pc.finish();
  return r;
}

struct output_method {
  output_method() : f(NULL), t(NULL), empty(true) {}
  Formatter *f;
  TextTable *t;
  bool empty;
};

static int diff_cb(uint64_t ofs, size_t len, int exists, void *arg)
{
  output_method *om = static_cast<output_method *>(arg);
  om->empty = false;
  if (om->f) {
    om->f->open_object_section("extent");
    om->f->dump_unsigned("offset", ofs);
    om->f->dump_unsigned("length", len);
    om->f->dump_string("exists", exists ? "true" : "false");
    om->f->close_section();
  } else {
    assert(om->t);
    *(om->t) << ofs << len << (exists ? "data" : "zero") << TextTable::endrow;
  }
  return 0;
}

static int do_diff(librbd::Image& image, const char *fromsnapname,
		   Formatter *f)
{
  int r;
  librbd::image_info_t info;

  r = image.stat(info, sizeof(info));
  if (r < 0)
    return r;

  output_method om;
  if (f) {
    om.f = f;
    f->open_array_section("extents");
  } else {
    om.t = new TextTable();
    om.t->define_column("Offset", TextTable::LEFT, TextTable::LEFT);
    om.t->define_column("Length", TextTable::LEFT, TextTable::LEFT);
    om.t->define_column("Type", TextTable::LEFT, TextTable::LEFT);
  }

  r = image.diff_iterate(fromsnapname, 0, info.size, diff_cb, &om);
  if (f) {
    f->close_section();
    f->flush(cout);
  } else {
    if (!om.empty)
      cout << *om.t;
    delete om.t;
  }
  return r;
}

static const char *imgname_from_path(const char *path)
{
  const char *imgname;

  imgname = strrchr(path, '/');
  if (imgname)
    imgname++;
  else
    imgname = path;

  return imgname;
}

static void update_snap_name(char *imgname, char **snap)
{
  char *s;

  s = strrchr(imgname, '@');
  if (!s)
    return;

  *s = '\0';

  if (!snap)
    return;

  s++;
  if (*s)
    *snap = s;
}

static void set_pool_image_name(const char *orig_img, char **new_pool, 
				char **new_img, char **snap)
{
  const char *sep;

  if (!orig_img)
    return;

  sep = strchr(orig_img, '/');
  if (!sep) {
    *new_img = strdup(orig_img);
    goto done_img;
  }

  *new_pool =  strdup(orig_img);
  sep = strchr(*new_pool, '/');
  assert (sep);

  *(char *)sep = '\0';
  *new_img = strdup(sep + 1);

done_img:
  update_snap_name(*new_img, snap);
}

class AioImportContext : public Context
{
public:
  AioImportContext(SimpleThrottle &simple_throttle, librbd::Image &image,
                   bufferlist &bl, uint64_t offset)
    : m_throttle(simple_throttle),
      m_aio_completion(
        new librbd::RBD::AioCompletion(this, &AioImportContext::aio_callback)),
      m_bufferlist(bl), m_offset(offset)
  {
    m_throttle.start_op();

    int op_flags = LIBRADOS_OP_FLAG_FADVISE_SEQUENTIAL |
		   LIBRADOS_OP_FLAG_FADVISE_NOCACHE;
    int r = image.aio_write2(m_offset, m_bufferlist.length(), m_bufferlist,
			     m_aio_completion, op_flags);
    if (r < 0) {
      cerr << "rbd: error requesting write to destination image" << std::endl;
      m_throttle.end_op(r);
    }
  }

  virtual ~AioImportContext()
  {
    m_aio_completion->release();
  }

  virtual void finish(int r)
  {
    if (r < 0) {
      cerr << "rbd: error writing to destination image at offset "
           << m_offset << ": " << cpp_strerror(r) << std::endl;
    }
    m_throttle.end_op(r);
  }

  static void aio_callback(librbd::completion_t completion, void *arg)
  {
    librbd::RBD::AioCompletion *aio_completion =
      reinterpret_cast<librbd::RBD::AioCompletion*>(completion);
    AioImportContext *import_context = reinterpret_cast<AioImportContext*>(arg);
    import_context->complete(aio_completion->get_return_value());
  }

private:
  SimpleThrottle &m_throttle;
  librbd::RBD::AioCompletion *m_aio_completion;
  bufferlist m_bufferlist;
  uint64_t m_offset;
};

static int do_import(librbd::RBD &rbd, librados::IoCtx& io_ctx,
		     const char *imgname, int *order, const char *path,
		     int format, uint64_t features, uint64_t size)
{
  int fd, r;
  struct stat stat_buf;
  MyProgressContext pc("Importing image");

  assert(imgname);

  // default order as usual
  if (*order == 0)
    *order = 22;

  // try to fill whole imgblklen blocks for sparsification
  uint64_t image_pos = 0;
  size_t imgblklen = 1 << *order;
  char *p = new char[imgblklen];
  size_t reqlen = imgblklen;	// amount requested from read
  ssize_t readlen;		// amount received from one read
  size_t blklen = 0;		// amount accumulated from reads to fill blk
  librbd::Image image;

  boost::scoped_ptr<SimpleThrottle> throttle;
  bool from_stdin = !strcmp(path, "-");
  if (from_stdin) {
    throttle.reset(new SimpleThrottle(1, false));
    fd = 0;
    size = 1ULL << *order;
  } else {
    throttle.reset(new SimpleThrottle(
      max(g_conf->rbd_concurrent_management_ops, 1), false));
    if ((fd = open(path, O_RDONLY)) < 0) {
      r = -errno;
      cerr << "rbd: error opening " << path << std::endl;
      goto done2;
    }

    if ((fstat(fd, &stat_buf)) < 0) {
      r = -errno;
      cerr << "rbd: stat error " << path << std::endl;
      goto done;
    }
    if (S_ISDIR(stat_buf.st_mode)) {
      r = -EISDIR;
      cerr << "rbd: cannot import a directory" << std::endl;
      goto done;
    }
    if (stat_buf.st_size)
      size = (uint64_t)stat_buf.st_size;

    if (!size) {
      int64_t bdev_size = 0;
      r = get_block_device_size(fd, &bdev_size);
      if (r < 0) {
	cerr << "rbd: unable to get size of file/block device" << std::endl;
	goto done;
      }
      assert(bdev_size >= 0);
      size = (uint64_t) bdev_size;
    }

    posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
  }
  r = do_create(rbd, io_ctx, imgname, size, order, format, features, 0, 0);
  if (r < 0) {
    cerr << "rbd: image creation failed" << std::endl;
    goto done;
  }
  r = rbd.open(io_ctx, image, imgname);
  if (r < 0) {
    cerr << "rbd: failed to open image" << std::endl;
    goto done;
  }

  // loop body handles 0 return, as we may have a block to flush
  while ((readlen = ::read(fd, p + blklen, reqlen)) >= 0) {
    blklen += readlen;
    // if read was short, try again to fill the block before writing
    if (readlen && ((size_t)readlen < reqlen)) {
      reqlen -= readlen;
      continue;
    }
    if (!from_stdin)
      pc.update_progress(image_pos, size);

    bufferlist bl(blklen);
    bl.append(p, blklen);
    // resize output image by binary expansion as we go for stdin
    if (from_stdin && (image_pos + (size_t)blklen) > size) {
      size *= 2;
      r = image.resize(size);
      if (r < 0) {
	cerr << "rbd: can't resize image during import" << std::endl;
	goto done;
      }
    }

    // write as much as we got; perhaps less than imgblklen
    // but skip writing zeros to create sparse images
    if (!bl.is_zero()) {
      new AioImportContext(*throttle, image, bl, image_pos);
    }

    // done with whole block, whether written or not
    image_pos += blklen;
    // if read had returned 0, we're at EOF and should quit
    if (readlen == 0)
      break;
    blklen = 0;
    reqlen = imgblklen;
  }
  r = throttle->wait_for_ret();
  if (r < 0) {
    goto done;
  }

  if (from_stdin) {
    r = image.resize(image_pos);
    if (r < 0) {
      cerr << "rbd: final image resize failed" << std::endl;
      goto done;
    }
  }

  r = 0;

 done:
  if (!from_stdin) {
    if (r < 0)
      pc.fail();
    else
      pc.finish();
    close(fd);
  }
 done2:
  delete[] p;
  return r;
}

static int read_string(int fd, unsigned max, string *out)
{
  char buf[4];

  int r = safe_read_exact(fd, buf, 4);
  if (r < 0)
    return r;

  bufferlist bl;
  bl.append(buf, 4);
  bufferlist::iterator p = bl.begin();
  uint32_t len;
  ::decode(len, p);
  if (len > max)
    return -EINVAL;

  char sbuf[len];
  r = safe_read_exact(fd, sbuf, len);
  if (r < 0)
    return r;
  out->assign(sbuf, len);
  return len;
}

static int do_import_diff(librbd::Image &image, const char *path)
{
  int fd, r;
  struct stat stat_buf;
  MyProgressContext pc("Importing image diff");
  uint64_t size = 0;
  uint64_t off = 0;
  string from, to;

  bool from_stdin = !strcmp(path, "-");
  if (from_stdin) {
    fd = 0;
  } else {
    fd = open(path, O_RDONLY);
    if (fd < 0) {
      r = -errno;
      cerr << "rbd: error opening " << path << std::endl;
      return r;
    }
    r = ::fstat(fd, &stat_buf);
    if (r < 0)
      goto done;
    size = (uint64_t)stat_buf.st_size;
  }

  char buf[strlen(RBD_DIFF_BANNER) + 1];
  r = safe_read_exact(fd, buf, strlen(RBD_DIFF_BANNER));
  if (r < 0)
    goto done;
  buf[strlen(RBD_DIFF_BANNER)] = '\0';
  if (strcmp(buf, RBD_DIFF_BANNER)) {
    cerr << "invalid banner '" << buf << "', expected '" << RBD_DIFF_BANNER << "'" << std::endl;
    r = -EINVAL;
    goto done;
  }

  while (true) {
    __u8 tag;
    r = safe_read_exact(fd, &tag, 1);
    if (r < 0) {
      goto done;
    }

    if (tag == 'e') {
      dout(2) << " end diff" << dendl;
      break;
    } else if (tag == 'f') {
      r = read_string(fd, 4096, &from);   // 4k limit to make sure we don't get a garbage string
      if (r < 0)
	goto done;
      dout(2) << " from snap " << from << dendl;

      if (!image.snap_exists(from.c_str())) {
	cerr << "start snapshot '" << from << "' does not exist in the image, aborting" << std::endl;
	r = -EINVAL;
	goto done;
      }
    }
    else if (tag == 't') {
      r = read_string(fd, 4096, &to);   // 4k limit to make sure we don't get a garbage string
      if (r < 0)
	goto done;
      dout(2) << "   to snap " << to << dendl;

      // verify this snap isn't already present
      if (image.snap_exists(to.c_str())) {
	cerr << "end snapshot '" << to << "' already exists, aborting" << std::endl;
	r = -EEXIST;
	goto done;
      }
    } else if (tag == 's') {
      uint64_t end_size;
      char buf[8];
      r = safe_read_exact(fd, buf, 8);
      if (r < 0)
	goto done;
      bufferlist bl;
      bl.append(buf, 8);
      bufferlist::iterator p = bl.begin();
      ::decode(end_size, p);
      uint64_t cur_size;
      image.size(&cur_size);
      if (cur_size != end_size) {
	dout(2) << "resize " << cur_size << " -> " << end_size << dendl;
	image.resize(end_size);
      } else {
	dout(2) << "size " << end_size << " (no change)" << dendl;
      }
      if (from_stdin)
	size = end_size;
    } else if (tag == 'w' || tag == 'z') {
      uint64_t len;
      char buf[16];
      r = safe_read_exact(fd, buf, 16);
      if (r < 0)
	goto done;
      bufferlist bl;
      bl.append(buf, 16);
      bufferlist::iterator p = bl.begin();
      ::decode(off, p);
      ::decode(len, p);

      if (tag == 'w') {
	bufferptr bp = buffer::create(len);
	r = safe_read_exact(fd, bp.c_str(), len);
	if (r < 0)
	  goto done;
	bufferlist data;
	data.append(bp);
	dout(2) << " write " << off << "~" << len << dendl;
	image.write2(off, len, data, LIBRADOS_OP_FLAG_FADVISE_NOCACHE);
      } else {
	dout(2) << " zero " << off << "~" << len << dendl;
	image.discard(off, len);
      }
    } else {
      cerr << "unrecognized tag byte " << (int)tag << " in stream; aborting" << std::endl;
      r = -EINVAL;
      goto done;
    }
    if (!from_stdin) {
      // progress through input
      uint64_t off = lseek64(fd, 0, SEEK_CUR);
      pc.update_progress(off, size);
    } else if (size) {
      // progress through image offsets.  this may jitter if blocks
      // aren't in order, but it is better than nothing.
      pc.update_progress(off, size);
    }
  }

  // take final snap
  if (to.length()) {
    dout(2) << " create end snap " << to << dendl;
    r = image.snap_create(to.c_str());
  }

 done:
  if (r < 0)
    pc.fail();
  else
    pc.finish();
  if (!from_stdin)
    close(fd);
  return r;
}

static int parse_diff_header(int fd, __u8 *tag, string *from, string *to, uint64_t *size)
{
  int r;

  {//header
    char buf[strlen(RBD_DIFF_BANNER) + 1];
    r = safe_read_exact(fd, buf, strlen(RBD_DIFF_BANNER));
    if (r < 0)
      return r;

    buf[strlen(RBD_DIFF_BANNER)] = '\0';
    if (strcmp(buf, RBD_DIFF_BANNER)) {
      cerr << "invalid banner '" << buf << "', expected '" << RBD_DIFF_BANNER << "'" << std::endl;
      return -EINVAL;
    }
  }

  while (true) {
    r = safe_read_exact(fd, tag, 1);
    if (r < 0)
      return r;

    if (*tag == 'f') {
      r = read_string(fd, 4096, from);   // 4k limit to make sure we don't get a garbage string
      if (r < 0)
        return r;
      dout(2) << " from snap " << *from << dendl;
    } else if (*tag == 't') {
      r = read_string(fd, 4096, to);   // 4k limit to make sure we don't get a garbage string
      if (r < 0)
        return r;
      dout(2) << " to snap " << *to << dendl;
    } else if (*tag == 's') {
      char buf[8];
      r = safe_read_exact(fd, buf, 8);
      if (r < 0)
        return r;

      bufferlist bl;
      bl.append(buf, 8);
      bufferlist::iterator p = bl.begin();
      ::decode(*size, p);
    } else {
      break;
    }
  }

  return 0;
}

static int parse_diff_body(int fd, __u8 *tag, uint64_t *offset, uint64_t *length)
{
  int r;

  if (!(*tag)) {
    r = safe_read_exact(fd, tag, 1);
    if (r < 0)
      return r;
  }

  if (*tag == 'e') {
    offset = 0;
    length = 0;
    return 0;
  }

  if (*tag != 'w' && *tag != 'z')
    return -ENOTSUP;

  char buf[16];
  r = safe_read_exact(fd, buf, 16);
  if (r < 0)
    return r;

  bufferlist bl;
  bl.append(buf, 16);
  bufferlist::iterator p = bl.begin();
  ::decode(*offset, p);
  ::decode(*length, p);

  if (!(*length))
    return -ENOTSUP;

  return 0;
}

/*
 * fd: the diff file to read from
 * pd: the diff file to be written into
 */
static int accept_diff_body(int fd, int pd, __u8 tag, uint64_t offset, uint64_t length)
{
  if (tag == 'e')
    return 0;

  bufferlist bl;
  ::encode(tag, bl);
  ::encode(offset, bl);
  ::encode(length, bl);
  int r;
  r = bl.write_fd(pd);
  if (r < 0)
    return r;

  if (tag == 'w') {
    bufferptr bp = buffer::create(length);
    r = safe_read_exact(fd, bp.c_str(), length);
    if (r < 0)
      return r;
    bufferlist data;
    data.append(bp);
    r = data.write_fd(pd);
    if (r < 0)
      return r;
  }

  return 0;
}

/*
 * Merge two diff files into one single file
 * Note: It does not do the merging work if
 * either of the source diff files is stripped,
 * since which complicates the process and is
 * rarely used
 */
static int do_merge_diff(const char *first, const char *second, const char *path)
{
  MyProgressContext pc("Merging image diff");
  int fd = -1, sd = -1, pd = -1, r;

  string f_from, f_to;
  string s_from, s_to;
  uint64_t f_size, s_size, pc_size;

  __u8 f_tag = 0, s_tag = 0;
  uint64_t f_off = 0, f_len = 0;
  uint64_t s_off = 0, s_len = 0;
  bool f_end = false, s_end = false;

  bool first_stdin = !strcmp(first, "-");
  if (first_stdin) {
    fd = 0;
  } else {
    fd = open(first, O_RDONLY);
    if (fd < 0) {
      r = -errno;
      cerr << "rbd: error opening " << first << std::endl;
      goto done;
    }
  }

  sd = open(second, O_RDONLY);
  if (sd < 0) {
    r = -errno;
    cerr << "rbd: error opening " << second << std::endl;
    goto done;
  }

  if (strcmp(path, "-") == 0) {
    pd = 1;
  } else {
    pd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (pd < 0) {
      r = -errno;
      cerr << "rbd: error create " << path << std::endl;
      goto done;
    }
  }

  //We just handle the case like 'banner, [ftag], [ttag], stag, [wztag]*,etag',
  // and the (offset,length) in wztag must be ascending order.

  r = parse_diff_header(fd, &f_tag, &f_from, &f_to, &f_size);
  if (r < 0)
    goto done;

  r = parse_diff_header(sd, &s_tag, &s_from, &s_to, &s_size);
  if (r < 0)
    goto done;

  if (f_to != s_from) {
    r = -EINVAL;
    cerr << "The first TO snapshot must be equal with the second FROM snapshot, aborting" << std::endl;
    goto done;
  }

  {
    // header
    bufferlist bl;
    bl.append(RBD_DIFF_BANNER, strlen(RBD_DIFF_BANNER));

    __u8 tag;
    if (f_from.size()) {
      tag = 'f';
      ::encode(tag, bl);
      ::encode(f_from, bl);
    }

    if (s_to.size()) {
      tag = 't';
      ::encode(tag, bl);
      ::encode(s_to, bl);
    }

    tag = 's';
    ::encode(tag, bl);
    ::encode(s_size, bl);

    r = bl.write_fd(pd);
    if (r < 0)
      goto done;
  }

  if (f_size > s_size)
    pc_size = f_size << 1;
  else
    pc_size = s_size << 1;

  //data block
  while (!f_end || !s_end) {
    // progress through input
    pc.update_progress(f_off + s_off, pc_size);

    if (!f_end && !f_len) {
      uint64_t last_off = f_off;

      r = parse_diff_body(fd, &f_tag, &f_off, &f_len);
      if (r < 0)
        goto done;

      if (f_tag == 'e') {
        f_end = true;
        f_tag = 'z';
        f_off = f_size;
        if (f_size < s_size)
          f_len = s_size - f_size;
        else
          f_len = 0;
      }

      if (last_off > f_off) {
        r = -ENOTSUP;
        goto done;
      }
    }

    if (!s_end && !s_len) {
      uint64_t last_off = s_off;

      r = parse_diff_body(sd, &s_tag, &s_off, &s_len);
      if (r < 0)
        goto done;

      if (s_tag == 'e') {
        s_end = true;
        s_off = s_size;
        if (s_size < f_size)
          s_len = f_size - s_size;
        else
          s_len = 0;
      }

      if (last_off > s_off) {
        r = -ENOTSUP;
        goto done;
      }
    }

    if (f_off < s_off && f_len) {
      uint64_t delta = s_off - f_off;
      if (delta > f_len)
        delta = f_len;
      r = accept_diff_body(fd, pd, f_tag, f_off, delta);
      f_off += delta;
      f_len -= delta;

      if (!f_len) {
        f_tag = 0;
        continue;
      }
    }
    assert(f_off >= s_off);

    if (f_off < s_off + s_len && f_len) {
      uint64_t delta = s_off + s_len - f_off;
      if (delta > f_len)
        delta = f_len;
      if (f_tag == 'w') {
        if (first_stdin) {
          bufferptr bp = buffer::create(delta);
          r = safe_read_exact(fd, bp.c_str(), delta);
          if (r < 0)
            goto done;
        } else {
          r = lseek(fd, delta, SEEK_CUR);
          if(r < 0)
            goto done;
        }
      }
      f_off += delta;
      f_len -= delta;

      if (!f_len) {
        f_tag = 0;
        continue;
      }
    }
    assert(f_off >= s_off + s_len);

    if (s_len) {
      r = accept_diff_body(sd, pd, s_tag, s_off, s_len);
      s_off += s_len;
      s_len = 0;
      s_tag = 0;
    } else
      assert(f_end && s_end);
    continue;
  }

  {//tail
    __u8 tag = 'e';
    bufferlist bl;
    ::encode(tag, bl);
    r = bl.write_fd(pd);
  }

done:
  if (pd > 2)
    close(pd);
  if (sd > 2)
    close(sd);
  if (fd > 2)
    close(fd);

  if(r < 0) {
    pc.fail();
    if (pd > 2)
      unlink(path);
  } else
    pc.finish();

  return r;
}

static int do_copy(librbd::Image &src, librados::IoCtx& dest_pp,
		   const char *destname)
{
  MyProgressContext pc("Image copy");
  int r = src.copy_with_progress(dest_pp, destname, pc);
  if (r < 0){
    pc.fail();
    return r;
  }
  pc.finish();
  return 0;
}

class RbdWatchCtx : public librados::WatchCtx2 {
public:
  RbdWatchCtx(librados::IoCtx& io_ctx, const char *image_name,
	      std::string header_oid)
    : m_io_ctx(io_ctx), m_image_name(image_name), m_header_oid(header_oid)
  {
  }

  virtual ~RbdWatchCtx() {}

  virtual void handle_notify(uint64_t notify_id,
                             uint64_t cookie,
                             uint64_t notifier_id,
                             bufferlist& bl) {
    cout << m_image_name << " received notification: notify_id=" << notify_id
	 << ", cookie=" << cookie << ", notifier_id=" << notifier_id
	 << ", bl.length=" << bl.length() << std::endl;
    bufferlist reply;
    m_io_ctx.notify_ack(m_header_oid, notify_id, cookie, reply);
  }
  
  virtual void handle_error(uint64_t cookie, int err) {
    cerr << m_image_name << " received error: cookie=" << cookie << ", err="
	 << cpp_strerror(err) << std::endl;
  }
private:
  librados::IoCtx m_io_ctx;
  const char *m_image_name;
  string m_header_oid;
};

static int do_watch(librados::IoCtx& pp, librbd::Image &image,
		    const char *imgname)
{
  uint8_t old_format;
  int r = image.old_format(&old_format);
  if (r < 0) {
    cerr << "failed to query format" << std::endl;
    return r;
  }

  string header_oid;
  if (old_format != 0) {
    header_oid = string(imgname) + RBD_SUFFIX;
  } else {
    librbd::image_info_t info;
    r = image.stat(info, sizeof(info));
    if (r < 0) {
      cerr << "failed to stat image" << std::endl;
      return r;
    }

    char prefix[RBD_MAX_BLOCK_NAME_SIZE + 1];
    strncpy(prefix, info.block_name_prefix, RBD_MAX_BLOCK_NAME_SIZE);
    prefix[RBD_MAX_BLOCK_NAME_SIZE] = '\0';

    string image_id(prefix + strlen(RBD_DATA_PREFIX));
    header_oid = RBD_HEADER_PREFIX + image_id;
  }

  uint64_t cookie;
  RbdWatchCtx ctx(pp, imgname, header_oid);
  r = pp.watch2(header_oid, &cookie, &ctx);
  if (r < 0) {
    cerr << "rbd: watch failed" << std::endl;
    return r;
  }

  cout << "press enter to exit..." << std::endl;
  getchar();

  r = pp.unwatch2(cookie);
  if (r < 0) {
    cerr << "rbd: unwatch failed" << std::endl;
    return r;
  }
  return 0;
}

static int do_show_status(librados::IoCtx &io_ctx, librbd::Image &image,
                          const char *imgname, Formatter *f)
{
  librbd::image_info_t info;
  uint8_t old_format;
  int r;
  string header_oid;
  std::list<obj_watch_t> watchers;

  r = image.old_format(&old_format);
  if (r < 0)
    return r;

  if (old_format) {
    header_oid = imgname;
    header_oid += RBD_SUFFIX;
  } else {
    r = image.stat(info, sizeof(info));
    if (r < 0)
      return r;

    char prefix[RBD_MAX_BLOCK_NAME_SIZE + 1];
    strncpy(prefix, info.block_name_prefix, RBD_MAX_BLOCK_NAME_SIZE);
    prefix[RBD_MAX_BLOCK_NAME_SIZE] = '\0';

    header_oid = RBD_HEADER_PREFIX;
    header_oid.append(prefix + strlen(RBD_DATA_PREFIX));
  }

  r = io_ctx.list_watchers(header_oid, &watchers);
  if (r < 0)
    return r;

  if (f)
    f->open_object_section("status");

  if (f) {
    f->open_object_section("watchers");
    for (std::list<obj_watch_t>::iterator i = watchers.begin(); i != watchers.end(); ++i) {
      f->open_object_section("watcher");
      f->dump_string("address", i->addr);
      f->dump_unsigned("client", i->watcher_id);
      f->dump_unsigned("cookie", i->cookie);
      f->close_section();
    }
    f->close_section();
  } else {
    if (watchers.size()) {
      cout << "Watchers:" << std::endl;
      for (std::list<obj_watch_t>::iterator i = watchers.begin(); i != watchers.end(); ++i) {
        cout << "\twatcher=" << i->addr << " client." << i->watcher_id << " cookie=" << i->cookie << std::endl;
      }
    } else {
      cout << "Watchers: none" << std::endl;
    }
  }

  if (f) {
    f->close_section();
    f->flush(cout);
  }

  return 0;
}

static int do_kernel_map(const char *poolname, const char *imgname,
			 const char *snapname)
{
  struct krbd_ctx *krbd;
  ostringstream oss;
  char *devnode;
  int r;

  r = krbd_create_from_context(g_ceph_context, &krbd);
  if (r < 0)
    return r;

  for (map<string, string>::const_iterator it = map_options.begin();
       it != map_options.end();
       ++it) {
    // for compatibility with < 3.7 kernels, assume that rw is on by
    // default and omit it even if it was specified by the user
    // (see ceph.git commit fb0f1986449b)
    if (it->first == "rw" && it->second == "rw")
      continue;

    if (it != map_options.begin())
      oss << ",";
    oss << it->second;
  }

  r = krbd_map(krbd, poolname, imgname, snapname, oss.str().c_str(), &devnode);
  if (r < 0)
    goto out;

  cout << devnode << std::endl;

  free(devnode);
out:
  krbd_destroy(krbd);
  return r;
}

static int do_kernel_showmapped(Formatter *f)
{
  struct krbd_ctx *krbd;
  int r;

  r = krbd_create_from_context(g_ceph_context, &krbd);
  if (r < 0)
    return r;

  r = krbd_showmapped(krbd, f);

  krbd_destroy(krbd);
  return r;
}

static int do_kernel_unmap(const char *dev)
{
  struct krbd_ctx *krbd;
  int r;

  r = krbd_create_from_context(g_ceph_context, &krbd);
  if (r < 0)
    return r;

  r = krbd_unmap(krbd, dev);

  krbd_destroy(krbd);
  return r;
}

static string map_option_uuid_cb(const char *value_char)
{
  uuid_d u;
  if (!u.parse(value_char))
    return "";

  return stringify(u);
}

static string map_option_ip_cb(const char *value_char)
{
  entity_addr_t a;
  const char *endptr;
  if (!a.parse(value_char, &endptr) ||
      endptr != value_char + strlen(value_char)) {
    return "";
  }

  return stringify(a.addr);
}

static string map_option_int_cb(const char *value_char)
{
  string err;
  int d = strict_strtol(value_char, 10, &err);
  if (!err.empty() || d < 0)
    return "";

  return stringify(d);
}

static void put_map_option(const string key, string val)
{
  map<string, string>::const_iterator it = map_options.find(key);
  if (it != map_options.end()) {
    cerr << "rbd: warning: redefining map option " << key << ": '"
         << it->second << "' -> '" << val << "'" << std::endl;
  }
  map_options[key] = val;
}

static int put_map_option_value(const string opt, const char *value_char,
                                string (*parse_cb)(const char *))
{
  if (!value_char || *value_char == '\0') {
    cerr << "rbd: " << opt << " option requires a value" << std::endl;
    return 1;
  }

  string value = parse_cb(value_char);
  if (value.empty()) {
    cerr << "rbd: invalid " << opt << " value '" << value_char << "'"
         << std::endl;
    return 1;
  }

  put_map_option(opt, opt + "=" + value);
  return 0;
}

static int parse_map_options(char *options)
{
  for (char *this_char = strtok(options, ", ");
       this_char != NULL;
       this_char = strtok(NULL, ",")) {
    char *value_char;

    if ((value_char = strchr(this_char, '=')) != NULL)
      *value_char++ = '\0';

    if (!strcmp(this_char, "fsid")) {
      if (put_map_option_value("fsid", value_char, map_option_uuid_cb))
        return 1;
    } else if (!strcmp(this_char, "ip")) {
      if (put_map_option_value("ip", value_char, map_option_ip_cb))
        return 1;
    } else if (!strcmp(this_char, "share") || !strcmp(this_char, "noshare")) {
      put_map_option("share", this_char);
    } else if (!strcmp(this_char, "crc") || !strcmp(this_char, "nocrc")) {
      put_map_option("crc", this_char);
    } else if (!strcmp(this_char, "mount_timeout")) {
      if (put_map_option_value("mount_timeout", value_char, map_option_int_cb))
        return 1;
    } else if (!strcmp(this_char, "osdkeepalive")) {
      if (put_map_option_value("osdkeepalive", value_char, map_option_int_cb))
        return 1;
    } else if (!strcmp(this_char, "osd_idle_ttl")) {
      if (put_map_option_value("osd_idle_ttl", value_char, map_option_int_cb))
        return 1;
    } else if (!strcmp(this_char, "rw") || !strcmp(this_char, "ro")) {
      put_map_option("rw", this_char);
    } else {
      cerr << "rbd: unknown map option '" << this_char << "'" << std::endl;
      return 1;
    }
  }

  return 0;
}

enum {
  OPT_NO_CMD = 0,
  OPT_LIST,
  OPT_INFO,
  OPT_CREATE,
  OPT_CLONE,
  OPT_FLATTEN,
  OPT_CHILDREN,
  OPT_RESIZE,
  OPT_RM,
  OPT_EXPORT,
  OPT_EXPORT_DIFF,
  OPT_DIFF,
  OPT_IMPORT,
  OPT_IMPORT_DIFF,
  OPT_COPY,
  OPT_RENAME,
  OPT_SNAP_CREATE,
  OPT_SNAP_ROLLBACK,
  OPT_SNAP_REMOVE,
  OPT_SNAP_LIST,
  OPT_SNAP_PURGE,
  OPT_SNAP_PROTECT,
  OPT_SNAP_UNPROTECT,
  OPT_WATCH,
  OPT_STATUS,
  OPT_MAP,
  OPT_UNMAP,
  OPT_SHOWMAPPED,
  OPT_LOCK_LIST,
  OPT_LOCK_ADD,
  OPT_LOCK_REMOVE,
  OPT_BENCH_WRITE,
  OPT_MERGE_DIFF,
};

static int get_cmd(const char *cmd, bool snapcmd, bool lockcmd)
{
  if (!snapcmd && !lockcmd) {
    if (strcmp(cmd, "ls") == 0 ||
        strcmp(cmd, "list") == 0)
      return OPT_LIST;
    if (strcmp(cmd, "info") == 0)
      return OPT_INFO;
    if (strcmp(cmd, "create") == 0)
      return OPT_CREATE;
    if (strcmp(cmd, "clone") == 0)
      return OPT_CLONE;
    if (strcmp(cmd, "flatten") == 0)
      return OPT_FLATTEN;
    if (strcmp(cmd, "children") == 0)
      return OPT_CHILDREN;
    if (strcmp(cmd, "resize") == 0)
      return OPT_RESIZE;
    if (strcmp(cmd, "rm") == 0)
      return OPT_RM;
    if (strcmp(cmd, "export") == 0)
      return OPT_EXPORT;
    if (strcmp(cmd, "export-diff") == 0)
      return OPT_EXPORT_DIFF;
    if (strcmp(cmd, "merge-diff") == 0)
      return OPT_MERGE_DIFF;
    if (strcmp(cmd, "diff") == 0)
      return OPT_DIFF;
    if (strcmp(cmd, "import") == 0)
      return OPT_IMPORT;
    if (strcmp(cmd, "import-diff") == 0)
      return OPT_IMPORT_DIFF;
    if (strcmp(cmd, "copy") == 0 ||
        strcmp(cmd, "cp") == 0)
      return OPT_COPY;
    if (strcmp(cmd, "rename") == 0 ||
        strcmp(cmd, "mv") == 0)
      return OPT_RENAME;
    if (strcmp(cmd, "watch") == 0)
      return OPT_WATCH;
    if (strcmp(cmd, "status") == 0)
      return OPT_STATUS;
    if (strcmp(cmd, "map") == 0)
      return OPT_MAP;
    if (strcmp(cmd, "showmapped") == 0)
      return OPT_SHOWMAPPED;
    if (strcmp(cmd, "unmap") == 0)
      return OPT_UNMAP;
    if (strcmp(cmd, "bench-write") == 0)
      return OPT_BENCH_WRITE;
  } else if (snapcmd) {
    if (strcmp(cmd, "create") == 0 ||
        strcmp(cmd, "add") == 0)
      return OPT_SNAP_CREATE;
    if (strcmp(cmd, "rollback") == 0 ||
        strcmp(cmd, "revert") == 0)
      return OPT_SNAP_ROLLBACK;
    if (strcmp(cmd, "remove") == 0 ||
        strcmp(cmd, "rm") == 0)
      return OPT_SNAP_REMOVE;
    if (strcmp(cmd, "ls") == 0 ||
        strcmp(cmd, "list") == 0)
      return OPT_SNAP_LIST;
    if (strcmp(cmd, "purge") == 0)
      return OPT_SNAP_PURGE;
    if (strcmp(cmd, "protect") == 0)
      return OPT_SNAP_PROTECT;
    if (strcmp(cmd, "unprotect") == 0)
      return OPT_SNAP_UNPROTECT;
  } else {
    if (strcmp(cmd, "ls") == 0 ||
        strcmp(cmd, "list") == 0)
      return OPT_LOCK_LIST;
    if (strcmp(cmd, "add") == 0)
      return OPT_LOCK_ADD;
    if (strcmp(cmd, "remove") == 0 ||
	strcmp(cmd, "rm") == 0)
      return OPT_LOCK_REMOVE;
  }

  return OPT_NO_CMD;
}

/*
 * Called 1-N times depending on how many args the command needs.  If
 * the positional varN is already set, set the next one; this handles
 * both --args above and unadorned args below.  Calling with all args
 * filled is an error.
 */
static bool set_conf_param(const char *param, const char **var1,
			   const char **var2, const char **var3)
{
  if (!*var1)
    *var1 = param;
  else if (var2 && !*var2)
    *var2 = param;
  else if (var3 && !*var3)
    *var3 = param;
  else
    return false;
  return true;
}

bool size_set;

int main(int argc, const char **argv)
{
  librados::Rados rados;
  librbd::RBD rbd;
  librados::IoCtx io_ctx, dest_io_ctx;
  librbd::Image image;

  vector<const char*> args;

  argv_to_vec(argc, argv, args);
  env_to_vec(args);

  int opt_cmd = OPT_NO_CMD;
  global_init(NULL, args, CEPH_ENTITY_TYPE_CLIENT, CODE_ENVIRONMENT_UTILITY, 0);

  const char *poolname = NULL;
  uint64_t size = 0;  // in bytes
  int order = 0;
  bool format_specified = false,
    output_format_specified = false;
  int format = 1;
  uint64_t features = RBD_FEATURE_LAYERING | RBD_FEATURE_EXCLUSIVE_LOCK;
  const char *imgname = NULL, *snapname = NULL, *destname = NULL,
    *dest_poolname = NULL, *dest_snapname = NULL, *path = NULL,
    *devpath = NULL, *lock_cookie = NULL, *lock_client = NULL,
    *lock_tag = NULL, *output_format = "plain",
    *fromsnapname = NULL,
    *first_diff = NULL, *second_diff = NULL;
  bool lflag = false;
  int pretty_format = 0;
  long long stripe_unit = 0, stripe_count = 0;
  long long bench_io_size = 4096, bench_io_threads = 16, bench_bytes = 1 << 30;
  string bench_pattern = "seq";

  std::string val, parse_err;
  std::ostringstream err;
  long long sizell = 0;
  std::vector<const char*>::iterator i;
  for (i = args.begin(); i != args.end(); ) {
    if (ceph_argparse_double_dash(args, i)) {
      break;
    } else if (ceph_argparse_witharg(args, i, &val, "--secret", (char*)NULL)) {
      int r = g_conf->set_val("keyfile", val.c_str());
      assert(r == 0);
    } else if (ceph_argparse_flag(args, i, "-h", "--help", (char*)NULL)) {
      usage();
      return 0;
    } else if (ceph_argparse_flag(args, i, "--new-format", (char*)NULL)) {
      format = 2;
      format_specified = true;
    } else if (ceph_argparse_witharg(args, i, &val, "--image-format",
				     (char*)NULL)) {
      format = strict_strtol(val.c_str(), 10, &parse_err);
      if (!parse_err.empty()) {
	cerr << "rbd: error parsing --image-format: " << parse_err << std::endl;
	return EXIT_FAILURE;
      }
      format_specified = true;
      g_conf->set_val_or_die("rbd_default_format", val.c_str());
    } else if (ceph_argparse_witharg(args, i, &val, "-p", "--pool", (char*)NULL)) {
      poolname = strdup(val.c_str());
    } else if (ceph_argparse_witharg(args, i, &val, "--dest-pool", (char*)NULL)) {
      dest_poolname = strdup(val.c_str());
    } else if (ceph_argparse_witharg(args, i, &val, "--snap", (char*)NULL)) {
      snapname = strdup(val.c_str());
    } else if (ceph_argparse_witharg(args, i, &val, "--from-snap", (char*)NULL)) {
      fromsnapname = strdup(val.c_str());
    } else if (ceph_argparse_witharg(args, i, &val, "-i", "--image", (char*)NULL)) {
      imgname = strdup(val.c_str());
    } else if (ceph_argparse_withlonglong(args, i, &sizell, &err, "-s", "--size", (char*)NULL)) {
      if (!err.str().empty()) {
	cerr << "rbd: " << err.str() << std::endl;
	return EXIT_FAILURE;
      }
      if (sizell < 0) {
	cerr << "rbd: size must be >= 0" << std::endl;
	return EXIT_FAILURE;
      }
      size = sizell << 20;   // bytes to MB
      size_set = true;
    } else if (ceph_argparse_flag(args, i, "-l", "--long", (char*)NULL)) {
      lflag = true;
    } else if (ceph_argparse_withlonglong(args, i, &stripe_unit, &err, "--stripe-unit", (char*)NULL)) {
    } else if (ceph_argparse_withlonglong(args, i, &stripe_count, &err, "--stripe-count", (char*)NULL)) {
    } else if (ceph_argparse_withint(args, i, &order, &err, "--order", (char*)NULL)) {
      if (!err.str().empty()) {
	cerr << "rbd: " << err.str() << std::endl;
	return EXIT_FAILURE;
      }
    } else if (ceph_argparse_withlonglong(args, i, &bench_io_size, &err, "--io-size", (char*)NULL)) {
      if (!err.str().empty()) {
	cerr << "rbd: " << err.str() << std::endl;
	return EXIT_FAILURE;
      }
      if (bench_io_size == 0) {
	cerr << "rbd: io-size must be > 0" << std::endl;
	return EXIT_FAILURE;
      }
    } else if (ceph_argparse_withlonglong(args, i, &bench_io_threads, &err, "--io-threads", (char*)NULL)) {
    } else if (ceph_argparse_withlonglong(args, i, &bench_bytes, &err, "--io-total", (char*)NULL)) {
    } else if (ceph_argparse_witharg(args, i, &bench_pattern, &err, "--io-pattern", (char*)NULL)) {
    } else if (ceph_argparse_witharg(args, i, &val, "--path", (char*)NULL)) {
      path = strdup(val.c_str());
    } else if (ceph_argparse_witharg(args, i, &val, "--dest", (char*)NULL)) {
      destname = strdup(val.c_str());
    } else if (ceph_argparse_witharg(args, i, &val, "--parent", (char *)NULL)) {
      imgname = strdup(val.c_str());
    } else if (ceph_argparse_witharg(args, i, &val, "--shared", (char *)NULL)) {
      lock_tag = strdup(val.c_str());
    } else if (ceph_argparse_flag(args, i, "--no-settle", (char *)NULL)) {
      cerr << "rbd: --no-settle is deprecated" << std::endl;
    } else if (ceph_argparse_witharg(args, i, &val, "-o", "--options", (char*)NULL)) {
      char *map_options = strdup(val.c_str());
      if (parse_map_options(map_options)) {
        cerr << "rbd: couldn't parse map options" << std::endl;
        return EXIT_FAILURE;
      }
    } else if (ceph_argparse_flag(args, i, "--read-only", (char *)NULL)) {
      // --read-only is equivalent to -o ro
      put_map_option("rw", "ro");
    } else if (ceph_argparse_flag(args, i, "--no-progress", (char *)NULL)) {
      progress = false;
    } else if (ceph_argparse_flag(args, i , "--allow-shrink", (char *)NULL)) {
      resize_allow_shrink = true;
    } else if (ceph_argparse_flag(args, i, "--image-shared", (char *)NULL)) {
      features &= ~(RBD_FEATURE_EXCLUSIVE_LOCK | RBD_FEATURE_OBJECT_MAP);
    } else if (ceph_argparse_witharg(args, i, &val, "--format", (char *) NULL)) {
      long long ret = strict_strtoll(val.c_str(), 10, &parse_err);
      if (parse_err.empty()) {
	g_conf->set_val_or_die("rbd_default_format", val.c_str());
	format = ret;
	format_specified = true;
	cerr << "rbd: using --format for specifying the rbd image format is"
	     << " deprecated, use --image-format instead"
	     << std::endl;
      } else {
	output_format = strdup(val.c_str());
	output_format_specified = true;
      }
    } else if (ceph_argparse_binary_flag(args, i, &pretty_format, NULL, "--pretty-format", (char*)NULL)) {
    } else {
      ++i;
    }
  }

  common_init_finish(g_ceph_context);

  i = args.begin();
  if (i == args.end()) {
    cerr << "rbd: you must specify a command." << std::endl;
    return EXIT_FAILURE;
  } else if (strcmp(*i, "snap") == 0) {
    i = args.erase(i);
    if (i == args.end()) {
      cerr << "rbd: which snap command do you want?" << std::endl;
      return EXIT_FAILURE;
    }
    opt_cmd = get_cmd(*i, true, false);
  } else if (strcmp(*i, "lock") == 0) {
    i = args.erase(i);
    if (i == args.end()) {
      cerr << "rbd: which lock command do you want?" << std::endl;
      return EXIT_FAILURE;
    }
    opt_cmd = get_cmd(*i, false, true);
  } else {
    opt_cmd = get_cmd(*i, false, false);
  }
  if (opt_cmd == OPT_NO_CMD) {
    cerr << "rbd: error parsing command '" << *i << "'; -h or --help for usage" << std::endl;
    return EXIT_FAILURE;
  }

  // loop across all remaining arguments; by command, accumulate any
  // that are still missing into the appropriate variables, one at a
  // time (i.e. SET_CONF_PARAM will be called N times for N remaining
  // arguments).

#define SET_CONF_PARAM(v, p1, p2, p3) \
if (!set_conf_param(v, p1, p2, p3)) { \
  cerr << "rbd: extraneous parameter " << v << std::endl; \
  return EXIT_FAILURE; \
}

  for (i = args.erase(i); i != args.end(); ++i) {
    const char *v = *i;
    switch (opt_cmd) {
      case OPT_LIST:
	SET_CONF_PARAM(v, &poolname, NULL, NULL);
	break;
      case OPT_INFO:
      case OPT_CREATE:
      case OPT_FLATTEN:
      case OPT_RESIZE:
      case OPT_RM:
      case OPT_SNAP_CREATE:
      case OPT_SNAP_ROLLBACK:
      case OPT_SNAP_REMOVE:
      case OPT_SNAP_LIST:
      case OPT_SNAP_PURGE:
      case OPT_SNAP_PROTECT:
      case OPT_SNAP_UNPROTECT:
      case OPT_WATCH:
      case OPT_STATUS:
      case OPT_MAP:
      case OPT_BENCH_WRITE:
      case OPT_LOCK_LIST:
      case OPT_DIFF:
	SET_CONF_PARAM(v, &imgname, NULL, NULL);
	break;
      case OPT_UNMAP:
	SET_CONF_PARAM(v, &devpath, NULL, NULL);
	break;
      case OPT_EXPORT:
      case OPT_EXPORT_DIFF:
	SET_CONF_PARAM(v, &imgname, &path, NULL);
	break;
      case OPT_MERGE_DIFF:
        SET_CONF_PARAM(v, &first_diff, &second_diff, &path);
        break;
      case OPT_IMPORT:
      case OPT_IMPORT_DIFF:
	SET_CONF_PARAM(v, &path, &imgname, NULL);
	break;
      case OPT_COPY:
      case OPT_RENAME:
      case OPT_CLONE:
	SET_CONF_PARAM(v, &imgname, &destname, NULL);
	break;
      case OPT_SHOWMAPPED:
	cerr << "rbd: showmapped takes no parameters" << std::endl;
	return EXIT_FAILURE;
      case OPT_CHILDREN:
	SET_CONF_PARAM(v, &imgname, NULL, NULL);
	break;
      case OPT_LOCK_ADD:
	SET_CONF_PARAM(v, &imgname, &lock_cookie, NULL);
	break;
      case OPT_LOCK_REMOVE:
	SET_CONF_PARAM(v, &imgname, &lock_client, &lock_cookie);
	break;
    default:
	assert(0);
	break;
    }
  }

  g_conf->set_val_or_die("rbd_cache_writethrough_until_flush", "false");

  /* get defaults from rbd_default_* options to keep behavior consistent with
     manual short-form options */
  if (!format_specified)
    format = g_conf->rbd_default_format;
  if (!order)
    order = g_conf->rbd_default_order;
  if (!stripe_unit)
    stripe_unit = g_conf->rbd_default_stripe_unit;
  if (!stripe_count)
    stripe_count = g_conf->rbd_default_stripe_count;

  if (format_specified && opt_cmd != OPT_IMPORT && opt_cmd != OPT_CREATE) {
    cerr << "rbd: image format can only be set when "
	 << "creating or importing an image" << std::endl;
    return EXIT_FAILURE;
  }

  if (pretty_format && !strcmp(output_format, "plain")) {
    cerr << "rbd: --pretty-format only works when --format is json or xml"
	 << std::endl;
    return EXIT_FAILURE;
  }

  boost::scoped_ptr<Formatter> formatter;
  if (output_format_specified && opt_cmd != OPT_SHOWMAPPED &&
      opt_cmd != OPT_INFO && opt_cmd != OPT_LIST &&
      opt_cmd != OPT_SNAP_LIST && opt_cmd != OPT_LOCK_LIST &&
      opt_cmd != OPT_CHILDREN && opt_cmd != OPT_DIFF &&
      opt_cmd != OPT_STATUS) {
    cerr << "rbd: command doesn't use output formatting"
	 << std::endl;
    return EXIT_FAILURE;
  } else if (get_outfmt(output_format, pretty_format, &formatter) < 0) {
    return EXIT_FAILURE;
  }

  if (format_specified) {
    if (format < 1 || format > 2) {
      cerr << "rbd: image format must be 1 or 2" << std::endl;
      return EXIT_FAILURE;
    }
  }

  if (opt_cmd == OPT_EXPORT && !imgname) {
    cerr << "rbd: image name was not specified" << std::endl;
    return EXIT_FAILURE;
  }

  if ((opt_cmd == OPT_IMPORT || opt_cmd == OPT_IMPORT_DIFF) && !path) {
    cerr << "rbd: path was not specified" << std::endl;
    return EXIT_FAILURE;
  }

  if (opt_cmd == OPT_IMPORT && !destname) {
    destname = imgname;
    if (!destname)
      destname = imgname_from_path(path);
    imgname = NULL;
  }

  if (opt_cmd != OPT_LOCK_ADD && lock_tag) {
    cerr << "rbd: only the lock add command uses the --shared option"
	 << std::endl;
    return EXIT_FAILURE;
  }

  if ((opt_cmd == OPT_LOCK_ADD || opt_cmd == OPT_LOCK_REMOVE) &&
      !lock_cookie) {
    cerr << "rbd: lock id was not specified" << std::endl;
    return EXIT_FAILURE;
  }

  if (opt_cmd != OPT_LIST &&
      opt_cmd != OPT_IMPORT &&
      opt_cmd != OPT_IMPORT_DIFF &&
      opt_cmd != OPT_UNMAP &&
      opt_cmd != OPT_SHOWMAPPED &&
      opt_cmd != OPT_MERGE_DIFF && !imgname) {
    cerr << "rbd: image name was not specified" << std::endl;
    return EXIT_FAILURE;
  }

  if (opt_cmd == OPT_UNMAP && !devpath) {
    cerr << "rbd: device path was not specified" << std::endl;
    return EXIT_FAILURE;
  }

  // do this unconditionally so we can parse pool/image@snapshot into
  // the relevant parts
  set_pool_image_name(imgname, (char **)&poolname,
		      (char **)&imgname, (char **)&snapname);
  if (snapname && opt_cmd != OPT_SNAP_CREATE && opt_cmd != OPT_SNAP_ROLLBACK &&
      opt_cmd != OPT_SNAP_REMOVE && opt_cmd != OPT_INFO &&
      opt_cmd != OPT_EXPORT && opt_cmd != OPT_EXPORT_DIFF && opt_cmd != OPT_DIFF && opt_cmd != OPT_COPY &&
      opt_cmd != OPT_MAP && opt_cmd != OPT_CLONE &&
      opt_cmd != OPT_SNAP_PROTECT && opt_cmd != OPT_SNAP_UNPROTECT &&
      opt_cmd != OPT_CHILDREN) {
    cerr << "rbd: snapname specified for a command that doesn't use it"
	 << std::endl;
    return EXIT_FAILURE;
  }
  if ((opt_cmd == OPT_SNAP_CREATE || opt_cmd == OPT_SNAP_ROLLBACK ||
       opt_cmd == OPT_SNAP_REMOVE || opt_cmd == OPT_CLONE ||
       opt_cmd == OPT_SNAP_PROTECT || opt_cmd == OPT_SNAP_UNPROTECT ||
       opt_cmd == OPT_CHILDREN) && !snapname) {
    cerr << "rbd: snap name was not specified" << std::endl;
    return EXIT_FAILURE;
  }

  set_pool_image_name(destname, (char **)&dest_poolname,
		      (char **)&destname, (char **)&dest_snapname);

  if (opt_cmd == OPT_IMPORT) {
    if (poolname && dest_poolname) {
      cerr << "rbd: source and destination pool both specified" << std::endl;
      return EXIT_FAILURE;
    }
    if (imgname && destname) {
      cerr << "rbd: source and destination image both specified" << std::endl;
      return EXIT_FAILURE;
    }
    if (poolname)
      dest_poolname = poolname;
  }

  if (!poolname)
    poolname = "rbd";

  if (!dest_poolname)
    dest_poolname = "rbd";

  if (opt_cmd == OPT_MERGE_DIFF) {
    if (!first_diff) {
      cerr << "rbd: first diff was not specified" << std::endl;
      return EXIT_FAILURE;
    }
    if (!second_diff) {
      cerr << "rbd: second diff was not specified" << std::endl;
      return EXIT_FAILURE;
    }
    if (!path) {
      cerr << "rbd: path was not specified" << std::endl;
      return EXIT_FAILURE;
    }
  }
  if (opt_cmd == OPT_EXPORT && !path)
    path = imgname;

  if ((opt_cmd == OPT_COPY || opt_cmd == OPT_CLONE || opt_cmd == OPT_RENAME) &&
      !destname ) {
    cerr << "rbd: destination image name was not specified" << std::endl;
    return EXIT_FAILURE;
  }

  if ((opt_cmd == OPT_CLONE) && dest_snapname) {
    cerr << "rbd: cannot clone to a snapshot" << std::endl;
    return EXIT_FAILURE;
  }

  if ((opt_cmd == OPT_CLONE) && size) {
    cerr << "rbd: clone must begin at size of parent" << std::endl;
    return EXIT_FAILURE;
  }

  if ((opt_cmd == OPT_RENAME) && (strcmp(poolname, dest_poolname) != 0)) {
    cerr << "rbd: mv/rename across pools not supported" << std::endl;
    cerr << "source pool: " << poolname << " dest pool: " << dest_poolname
      << std::endl;
    return EXIT_FAILURE;
  }

  bool talk_to_cluster = (opt_cmd != OPT_MAP &&
			  opt_cmd != OPT_UNMAP &&
			  opt_cmd != OPT_SHOWMAPPED &&
                          opt_cmd != OPT_MERGE_DIFF);
  if (talk_to_cluster && rados.init_with_context(g_ceph_context) < 0) {
    cerr << "rbd: couldn't initialize rados!" << std::endl;
    return EXIT_FAILURE;
  }

  if (talk_to_cluster && rados.connect() < 0) {
    cerr << "rbd: couldn't connect to the cluster!" << std::endl;
    return EXIT_FAILURE;
  }

  int r;
  if (talk_to_cluster && opt_cmd != OPT_IMPORT) {
    r = rados.ioctx_create(poolname, io_ctx);
    if (r < 0) {
      cerr << "rbd: error opening pool " << poolname << ": "
	   << cpp_strerror(-r) << std::endl;
      return -r;
    }
  }

  if (imgname && talk_to_cluster &&
      (opt_cmd == OPT_RESIZE || opt_cmd == OPT_SNAP_CREATE ||
       opt_cmd == OPT_SNAP_ROLLBACK || opt_cmd == OPT_SNAP_REMOVE ||
       opt_cmd == OPT_SNAP_PURGE || opt_cmd == OPT_SNAP_PROTECT ||
       opt_cmd == OPT_SNAP_UNPROTECT || opt_cmd == OPT_WATCH ||
       opt_cmd == OPT_FLATTEN || opt_cmd == OPT_LOCK_ADD ||
       opt_cmd == OPT_LOCK_REMOVE || opt_cmd == OPT_BENCH_WRITE ||
       opt_cmd == OPT_INFO || opt_cmd == OPT_SNAP_LIST ||
       opt_cmd == OPT_IMPORT_DIFF ||
       opt_cmd == OPT_EXPORT || opt_cmd == OPT_EXPORT_DIFF || opt_cmd == OPT_COPY ||
       opt_cmd == OPT_DIFF ||
       opt_cmd == OPT_CHILDREN || opt_cmd == OPT_LOCK_LIST ||
       opt_cmd == OPT_STATUS)) {

    if (opt_cmd == OPT_INFO || opt_cmd == OPT_SNAP_LIST ||
	opt_cmd == OPT_EXPORT || opt_cmd == OPT_EXPORT || opt_cmd == OPT_COPY ||
	opt_cmd == OPT_CHILDREN || opt_cmd == OPT_LOCK_LIST ||
        opt_cmd == OPT_STATUS || opt_cmd == OPT_WATCH) {
      r = rbd.open_read_only(io_ctx, image, imgname, NULL);
    } else {
      r = rbd.open(io_ctx, image, imgname);
    }
    if (r < 0) {
      cerr << "rbd: error opening image " << imgname << ": "
	   << cpp_strerror(-r) << std::endl;
      return -r;
    }
  }

  if (snapname && talk_to_cluster &&
      (opt_cmd == OPT_INFO ||
       opt_cmd == OPT_EXPORT ||
       opt_cmd == OPT_EXPORT_DIFF ||
       opt_cmd == OPT_DIFF ||
       opt_cmd == OPT_COPY ||
       opt_cmd == OPT_CHILDREN)) {
    r = image.snap_set(snapname);
    if (r < 0) {
      cerr << "rbd: error setting snapshot context: " << cpp_strerror(-r)
	   << std::endl;
      return -r;
    }
  }

  if (opt_cmd == OPT_COPY || opt_cmd == OPT_IMPORT || opt_cmd == OPT_CLONE) {
    r = rados.ioctx_create(dest_poolname, dest_io_ctx);
    if (r < 0) {
      cerr << "rbd: error opening pool " << dest_poolname << ": "
	   << cpp_strerror(-r) << std::endl;
      return -r;
    }
  }

  if (opt_cmd == OPT_CREATE || opt_cmd == OPT_RESIZE) {
    if (!size_set) {
      cerr << "rbd: must specify --size <MB>" << std::endl;
      return EINVAL;
    }
  }

  switch (opt_cmd) {
  case OPT_LIST:
    r = do_list(rbd, io_ctx, lflag, formatter.get());
    if (r < 0) {
      cerr << "rbd: list: " << cpp_strerror(-r) << std::endl;
      return -r;
    }
    break;

  case OPT_CREATE:
    if (order && (order < 12 || order > 25)) {
      cerr << "rbd: order must be between 12 (4 KB) and 25 (32 MB)"
	   << std::endl;
      return EINVAL;
    }
    if ((stripe_unit && !stripe_count) || (!stripe_unit && stripe_count)) {
      cerr << "must specify both (or neither) of stripe-unit and stripe-count"
	   << std::endl;
      usage();
      return EINVAL;
    }
    r = do_create(rbd, io_ctx, imgname, size, &order, format, features,
		  stripe_unit, stripe_count);
    if (r < 0) {
      cerr << "rbd: create error: " << cpp_strerror(-r) << std::endl;
      return -r;
    }
    break;

  case OPT_CLONE:
    if (order && (order < 12 || order > 25)) {
      cerr << "rbd: order must be between 12 (4 KB) and 25 (32 MB)"
	   << std::endl;
      return EINVAL;
    }

    r = do_clone(rbd, io_ctx, imgname, snapname, dest_io_ctx, destname,
		 features, &order);
    if (r < 0) {
      cerr << "rbd: clone error: " << cpp_strerror(-r) << std::endl;
      return -r;
    }
    break;

  case OPT_FLATTEN:
    r = do_flatten(image);
    if (r < 0) {
      cerr << "rbd: flatten error: " << cpp_strerror(-r) << std::endl;
      return -r;
    }
    break;

  case OPT_RENAME:
    r = do_rename(rbd, io_ctx, imgname, destname);
    if (r < 0) {
      cerr << "rbd: rename error: " << cpp_strerror(-r) << std::endl;
      return -r;
    }
    break;

  case OPT_INFO:
    r = do_show_info(imgname, image, snapname, formatter.get());
    if (r < 0) {
      cerr << "rbd: info: " << cpp_strerror(-r) << std::endl;
      return -r;
    }
    break;

  case OPT_RM:
    r = do_delete(rbd, io_ctx, imgname);
    if (r < 0) {
      if (r == -ENOTEMPTY) {
	cerr << "rbd: image has snapshots - these must be deleted"
	     << " with 'rbd snap purge' before the image can be removed."
	     << std::endl;
      } else if (r == -EBUSY) {
	cerr << "rbd: error: image still has watchers"
	     << std::endl
	     << "This means the image is still open or the client using "
	     << "it crashed. Try again after closing/unmapping it or "
	     << "waiting 30s for the crashed client to timeout."
	     << std::endl;
      } else {
	cerr << "rbd: delete error: " << cpp_strerror(-r) << std::endl;
      }
      return -r ;
    }
    break;

  case OPT_RESIZE:
    librbd::image_info_t info;
    r = image.stat(info, sizeof(info));
    if (r < 0) {
      cerr << "rbd: resize error: " << cpp_strerror(-r) << std::endl;
      return -r;
    }

    if (info.size > size && !resize_allow_shrink) {
      cerr << "rbd: shrinking an image is only allowed with the --allow-shrink flag" << std::endl;
      return EINVAL;
    }

    r = do_resize(image, size);
    if (r < 0) {
      cerr << "rbd: resize error: " << cpp_strerror(-r) << std::endl;
      return -r;
    }
    break;

  case OPT_SNAP_LIST:
    if (!imgname) {
      cerr << "rbd: snap list requires an image parameter" << std::endl;
      return EXIT_FAILURE;
    }
    r = do_list_snaps(image, formatter.get());
    if (r < 0) {
      cerr << "rbd: failed to list snapshots: " << cpp_strerror(-r)
	   << std::endl;
      return -r;
    }
    break;

  case OPT_SNAP_CREATE:
    if (!imgname || !snapname) {
      cerr << "rbd: snap create requires image and snapname" << std::endl;
      return EINVAL;
    }
    r = do_add_snap(image, snapname);
    if (r < 0) {
      cerr << "rbd: failed to create snapshot: " << cpp_strerror(-r)
	   << std::endl;
      return -r;
    }
    break;

  case OPT_SNAP_ROLLBACK:
    if (!imgname) {
      cerr << "rbd: snap rollback requires image name" << std::endl;
      return EINVAL;
    }
    r = do_rollback_snap(image, snapname);
    if (r < 0) {
      cerr << "rbd: rollback failed: " << cpp_strerror(-r) << std::endl;
      return -r;
    }
    break;

  case OPT_SNAP_REMOVE:
    if (!imgname) {
      cerr << "rbd: snap remove requires image name" << std::endl;
      return EINVAL;
    }
    r = do_remove_snap(image, snapname);
    if (r < 0) {
      if (r == -EBUSY) {
        cerr << "rbd: snapshot '" << snapname << "' is protected from removal."
             << std::endl;
      } else {
        cerr << "rbd: failed to remove snapshot: " << cpp_strerror(-r)
             << std::endl;
      }
      return -r;
    }
    break;

  case OPT_SNAP_PURGE:
    if (!imgname) {
      cerr << "rbd: snap purge requires image name" << std::endl;
      return EINVAL;
    }
    r = do_purge_snaps(image);
    if (r < 0) {
      cerr << "rbd: removing snaps failed: " << cpp_strerror(-r) << std::endl;
      return -r;
    }
    break;

  case OPT_SNAP_PROTECT:
    if (!imgname) {
      cerr << "rbd: snap protect requires image name" << std::endl;
      return EINVAL;
    }
    r = do_protect_snap(image, snapname);
    if (r < 0) {
      cerr << "rbd: protecting snap failed: " << cpp_strerror(-r) << std::endl;
      return -r;
    }
    break;

  case OPT_SNAP_UNPROTECT:
    if (!imgname) {
      cerr << "rbd: snap unprotect requires image name" << std::endl;
      return EINVAL;
    }
    r = do_unprotect_snap(image, snapname);
    if (r < 0) {
      cerr << "rbd: unprotecting snap failed: " << cpp_strerror(-r)
	   << std::endl;
      return -r;
    }
    break;

  case OPT_CHILDREN:
    r = do_list_children(image, formatter.get());
    if (r < 0) {
      cerr << "rbd: listing children failed: " << cpp_strerror(-r) << std::endl;
      return -r;
    }
    break;

  case OPT_EXPORT:
    if (!path) {
      cerr << "rbd: export requires pathname" << std::endl;
      return EINVAL;
    }
    r = do_export(image, path);
    if (r < 0) {
      cerr << "rbd: export error: " << cpp_strerror(-r) << std::endl;
      return -r;
    }
    break;

  case OPT_DIFF:
    r = do_diff(image, fromsnapname, formatter.get());
    if (r < 0) {
      cerr << "rbd: diff error: " << cpp_strerror(-r) << std::endl;
      return -r;
    }
    break;

  case OPT_EXPORT_DIFF:
    if (!path) {
      cerr << "rbd: export-diff requires pathname" << std::endl;
      return EINVAL;
    }
    r = do_export_diff(image, fromsnapname, snapname, path);
    if (r < 0) {
      cerr << "rbd: export-diff error: " << cpp_strerror(-r) << std::endl;
      return -r;
    }
    break;

  case OPT_MERGE_DIFF:
    r = do_merge_diff(first_diff, second_diff, path);
    if (r < 0) {
      cerr << "rbd: merge-diff error" << std::endl;
      return -r;
    }
    break;

  case OPT_IMPORT:
    if (!path) {
      cerr << "rbd: import requires pathname" << std::endl;
      return EINVAL;
    }
    r = do_import(rbd, dest_io_ctx, destname, &order, path,
		  format, features, size);
    if (r < 0) {
      cerr << "rbd: import failed: " << cpp_strerror(-r) << std::endl;
      return -r;
    }
    break;

  case OPT_IMPORT_DIFF:
    assert(path);
    r = do_import_diff(image, path);
    if (r < 0) {
      cerr << "rbd: import-diff failed: " << cpp_strerror(-r) << std::endl;
      return -r;
    }
    break;

  case OPT_COPY:
    r = do_copy(image, dest_io_ctx, destname);
    if (r < 0) {
      cerr << "rbd: copy failed: " << cpp_strerror(-r) << std::endl;
      return -r;
    }
    break;

  case OPT_WATCH:
    r = do_watch(io_ctx, image, imgname);
    if (r < 0) {
      cerr << "rbd: watch failed: " << cpp_strerror(-r) << std::endl;
      return -r;
    }
    break;

  case OPT_STATUS:
    r = do_show_status(io_ctx, image, imgname, formatter.get());
    if (r < 0) {
      cerr << "rbd: show status failed: " << cpp_strerror(-r) << std::endl;
      return -r;
    }
    break;

  case OPT_MAP:
    r = do_kernel_map(poolname, imgname, snapname);
    if (r < 0) {
      cerr << "rbd: map failed: " << cpp_strerror(-r) << std::endl;
      return -r;
    }
    break;

  case OPT_UNMAP:
    r = do_kernel_unmap(devpath);
    if (r < 0) {
      cerr << "rbd: unmap failed: " << cpp_strerror(-r) << std::endl;
      return -r;
    }
    break;

  case OPT_SHOWMAPPED:
    r = do_kernel_showmapped(formatter.get());
    if (r < 0) {
      cerr << "rbd: showmapped failed: " << cpp_strerror(-r) << std::endl;
      return -r;
    }
    break;

  case OPT_LOCK_LIST:
    r = do_lock_list(image, formatter.get());
    if (r < 0) {
      cerr << "rbd: listing locks failed: " << cpp_strerror(r) << std::endl;
      return -r;
    }
    break;

  case OPT_LOCK_ADD:
    r = do_lock_add(image, lock_cookie, lock_tag);
    if (r < 0) {
      if (r == -EBUSY || r == -EEXIST) {
	if (lock_tag) {
	  cerr << "rbd: lock is alrady held by someone else"
	       << " with a different tag" << std::endl;
	} else {
	  cerr << "rbd: lock is already held by someone else" << std::endl;
	}
      } else {
	cerr << "rbd: taking lock failed: " << cpp_strerror(r) << std::endl;
      }
      return -r;
    }
    break;

  case OPT_LOCK_REMOVE:
    r = do_lock_remove(image, lock_cookie, lock_client);
    if (r < 0) {
      cerr << "rbd: releasing lock failed: " << cpp_strerror(r) << std::endl;
      return -r;
    }
    break;

  case OPT_BENCH_WRITE:
    r = do_bench_write(image, bench_io_size, bench_io_threads, bench_bytes, bench_pattern);
    if (r < 0) {
      cerr << "bench-write failed: " << cpp_strerror(-r) << std::endl;
      return -r;
    }
    break;
  }

  return 0;
}

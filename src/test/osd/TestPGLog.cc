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
#include "osd/PGLog.h"
#include "common/ceph_argparse.h"
#include "global/global_init.h"
#include <gtest/gtest.h>

class PGLogTest : public ::testing::Test, protected PGLog {
public:
  virtual void SetUp() { }

  virtual void TearDown() {
    clear();
  }

  static hobject_t mk_obj(unsigned id) {
    hobject_t hoid;
    stringstream ss;
    ss << "obj_" << id;
    hoid.oid = ss.str();
    hoid.set_hash(id);
    return hoid;
  }
  static eversion_t mk_evt(unsigned ep, unsigned v) {
    return eversion_t(ep, v);
  }
  static pg_log_entry_t mk_ple_mod(
    const hobject_t &hoid, eversion_t v, eversion_t pv) {
    pg_log_entry_t e;
    e.mod_desc.mark_unrollbackable();
    e.op = pg_log_entry_t::MODIFY;
    e.soid = hoid;
    e.version = v;
    e.prior_version = pv;
    return e;
  }
  static pg_log_entry_t mk_ple_dt(
    const hobject_t &hoid, eversion_t v, eversion_t pv) {
    pg_log_entry_t e;
    e.mod_desc.mark_unrollbackable();
    e.op = pg_log_entry_t::DELETE;
    e.soid = hoid;
    e.version = v;
    e.prior_version = pv;
    return e;
  }
  static pg_log_entry_t mk_ple_mod_rb(
    const hobject_t &hoid, eversion_t v, eversion_t pv) {
    pg_log_entry_t e;
    e.op = pg_log_entry_t::MODIFY;
    e.soid = hoid;
    e.version = v;
    e.prior_version = pv;
    return e;
  }
  static pg_log_entry_t mk_ple_dt_rb(
    const hobject_t &hoid, eversion_t v, eversion_t pv) {
    pg_log_entry_t e;
    e.op = pg_log_entry_t::DELETE;
    e.soid = hoid;
    e.version = v;
    e.prior_version = pv;
    return e;
  }

  struct TestCase {
    list<pg_log_entry_t> base;
    list<pg_log_entry_t> auth;
    list<pg_log_entry_t> div;

    pg_missing_t init;
    pg_missing_t final;

    set<hobject_t> toremove;
    list<pg_log_entry_t> torollback;

  private:
    IndexedLog fullauth;
    IndexedLog fulldiv;
    pg_info_t authinfo;
    pg_info_t divinfo;
  public:
    void setup() {
      fullauth.log.insert(fullauth.log.end(), base.begin(), base.end());
      fullauth.log.insert(fullauth.log.end(), auth.begin(), auth.end());
      fulldiv.log.insert(fulldiv.log.end(), base.begin(), base.end());
      fulldiv.log.insert(fulldiv.log.end(), div.begin(), div.end());

      fullauth.head = authinfo.last_update = fullauth.log.rbegin()->version;
      authinfo.last_complete = fullauth.log.rbegin()->version;
      authinfo.log_tail = fullauth.log.begin()->version;
      authinfo.log_tail.version--;
      fullauth.tail = authinfo.log_tail;
      authinfo.last_backfill = hobject_t::get_max();

      fulldiv.head = divinfo.last_update = fulldiv.log.rbegin()->version;
      divinfo.last_complete = eversion_t();
      divinfo.log_tail = fulldiv.log.begin()->version;
      divinfo.log_tail.version--;
      fulldiv.tail = divinfo.log_tail;
      divinfo.last_backfill = hobject_t::get_max();

      if (init.missing.empty()) {
	divinfo.last_complete = divinfo.last_update;
      } else {
	eversion_t fmissing = init.missing[init.rmissing.begin()->second].need;
	for (list<pg_log_entry_t>::const_iterator i = fulldiv.log.begin();
	     i != fulldiv.log.end();
	     ++i) {
	  if (i->version < fmissing)
	    divinfo.last_complete = i->version;
	  else
	    break;
	}
      }

      fullauth.index();
      fulldiv.index();
    }
    const IndexedLog &get_fullauth() const { return fullauth; }
    const IndexedLog &get_fulldiv() const { return fulldiv; }
    const pg_info_t &get_authinfo() const { return authinfo; }
    const pg_info_t &get_divinfo() const { return divinfo; }
  };

  struct LogHandler : public PGLog::LogEntryHandler {
    set<hobject_t> removed;
    list<pg_log_entry_t> rolledback;
    
    void rollback(
      const pg_log_entry_t &entry) {
      rolledback.push_back(entry);
    }
    void remove(
      const hobject_t &hoid) {
      removed.insert(hoid);
    }
    void trim(
      const pg_log_entry_t &entry) {}
  };

  void verify_missing(
    const TestCase &tcase,
    const pg_missing_t &missing) {
    ASSERT_EQ(tcase.final.missing.size(), missing.missing.size());
    for (map<hobject_t, pg_missing_t::item>::const_iterator i =
	   missing.missing.begin();
	 i != missing.missing.end();
	 ++i) {
      EXPECT_TRUE(tcase.final.missing.count(i->first));
      EXPECT_EQ(tcase.final.missing.find(i->first)->second.need, i->second.need);
      EXPECT_EQ(tcase.final.missing.find(i->first)->second.have, i->second.have);
    }
  }

  void verify_sideeffects(
    const TestCase &tcase,
    const LogHandler &handler) {
    ASSERT_EQ(tcase.toremove.size(), handler.removed.size());
    ASSERT_EQ(tcase.torollback.size(), handler.rolledback.size());

    {
      list<pg_log_entry_t>::const_iterator titer = tcase.torollback.begin();
      list<pg_log_entry_t>::const_iterator hiter = handler.rolledback.begin();
      for (; titer != tcase.torollback.end(); ++titer, ++hiter) {
	EXPECT_EQ(titer->version, hiter->version);
      }
    }

    {
      set<hobject_t>::const_iterator titer = tcase.toremove.begin();
      set<hobject_t>::const_iterator hiter = handler.removed.begin();
      for (; titer != tcase.toremove.end(); ++titer, ++hiter) {
	EXPECT_EQ(*titer, *hiter);
      }
    }
  }

  void test_merge_log(const TestCase &tcase) {
    clear();
    ObjectStore::Transaction t;
    log = tcase.get_fulldiv();
    pg_info_t info = tcase.get_divinfo();

    missing = tcase.init;

    IndexedLog olog;
    olog = tcase.get_fullauth();
    pg_info_t oinfo = tcase.get_authinfo();

    LogHandler h;
    bool dirty_info = false;
    bool dirty_big_info = false;
    merge_log(
      t, oinfo, olog, pg_shard_t(1, shard_id_t(0)), info,
      &h, dirty_info, dirty_big_info);

    ASSERT_EQ(info.last_update, oinfo.last_update);
    verify_missing(tcase, missing);
    verify_sideeffects(tcase, h);
  };
  void test_proc_replica_log(const TestCase &tcase) {
    clear();
    ObjectStore::Transaction t;
    log = tcase.get_fullauth();
    pg_info_t info = tcase.get_authinfo();

    pg_missing_t omissing = tcase.init;

    IndexedLog olog;
    olog = tcase.get_fulldiv();
    pg_info_t oinfo = tcase.get_divinfo();

    proc_replica_log(
       t, oinfo, olog, omissing, pg_shard_t(1, shard_id_t(0)));

    if (!tcase.base.empty()) {
      ASSERT_EQ(tcase.base.rbegin()->version, oinfo.last_update);
    }

    for (list<pg_log_entry_t>::const_iterator i = tcase.auth.begin();
	 i != tcase.auth.end();
	 ++i) {
      omissing.add_next_event(*i);
    }
    verify_missing(tcase, omissing);
  }
  void run_test_case(const TestCase &tcase) {
    test_merge_log(tcase);
    test_proc_replica_log(tcase);
  }
};

struct TestHandler : public PGLog::LogEntryHandler {
  list<hobject_t> &removed;
  TestHandler(list<hobject_t> &removed) : removed(removed) {}

  void rollback(
    const pg_log_entry_t &entry) {}
  void remove(
    const hobject_t &hoid) {
    removed.push_back(hoid);
  }
  void cant_rollback(const pg_log_entry_t &entry) {}
  void trim(
    const pg_log_entry_t &entry) {}
};

TEST_F(PGLogTest, rewind_divergent_log) {
  // newhead > log.tail : throw an assert
  {
    clear();

    ObjectStore::Transaction t;
    pg_info_t info;
    list<hobject_t> remove_snap;
    bool dirty_info = false;
    bool dirty_big_info = false;

    log.tail = eversion_t(2, 1);
    TestHandler h(remove_snap);
    EXPECT_THROW(rewind_divergent_log(t, eversion_t(1, 1), info, &h,
				      dirty_info, dirty_big_info),
		 FailedAssertion);
  }

  /*        +----------------+
            |  log           |
            +--------+-------+
            |        |object |
            |version | hash  |
            |        |       |
       tail > (1,1)  |  x5   |
            |        |       |
            |        |       |
            | (1,4)  |  x9   < newhead
            | MODIFY |       |
            |        |       |
       head > (1,5)  |  x9   |
            | DELETE |       |
            |        |       |
            +--------+-------+

  */
  {
    clear();

    ObjectStore::Transaction t;
    pg_info_t info;
    list<hobject_t> remove_snap;
    bool dirty_info = false;
    bool dirty_big_info = false;

    hobject_t divergent_object;
    eversion_t divergent_version;
    eversion_t newhead;

    hobject_t divergent;
    divergent.set_hash(0x9);

    {
      pg_log_entry_t e;
      e.mod_desc.mark_unrollbackable();

      e.version = eversion_t(1, 1);
      e.soid.set_hash(0x5);
      log.tail = e.version;
      log.log.push_back(e);
      e.version = newhead = eversion_t(1, 4);
      e.soid = divergent;
      e.op = pg_log_entry_t::MODIFY;
      log.log.push_back(e);
      e.version = divergent_version = eversion_t(1, 5);
      e.prior_version = eversion_t(1, 4);
      e.soid = divergent;
      divergent_object = e.soid;
      e.op = pg_log_entry_t::DELETE;
      log.log.push_back(e);
      log.head = e.version;
      log.index();

      info.last_update = log.head;
      info.last_complete = log.head;
    }

    EXPECT_FALSE(missing.have_missing());
    EXPECT_EQ(3U, log.log.size());
    EXPECT_TRUE(remove_snap.empty());
    EXPECT_TRUE(t.empty());
    EXPECT_EQ(log.head, info.last_update);
    EXPECT_EQ(log.head, info.last_complete);
    EXPECT_FALSE(is_dirty());
    EXPECT_FALSE(dirty_info);
    EXPECT_FALSE(dirty_big_info);

    TestHandler h(remove_snap);
    rewind_divergent_log(t, newhead, info, &h,
			 dirty_info, dirty_big_info);

    EXPECT_TRUE(log.objects.count(divergent));
    EXPECT_TRUE(missing.is_missing(divergent_object));
    EXPECT_EQ(1U, log.objects.count(divergent_object));
    EXPECT_EQ(2U, log.log.size());
    EXPECT_TRUE(remove_snap.empty());
    EXPECT_TRUE(t.empty());
    EXPECT_EQ(newhead, info.last_update);
    EXPECT_EQ(newhead, info.last_complete);
    EXPECT_TRUE(is_dirty());
    EXPECT_TRUE(dirty_info);
    EXPECT_TRUE(dirty_big_info);
  }

  /*        +----------------+
            |  log           |
            +--------+-------+
            |        |object |
            |version | hash  |
            |        |       |
       tail > (1,1)  | NULL  |
            |        |       |
            | (1,4)  | NULL  < newhead
            |        |       |
       head > (1,5)  |  x9   |
            |        |       |
            +--------+-------+

  */
  {
    clear();

    ObjectStore::Transaction t;
    pg_info_t info;
    list<hobject_t> remove_snap;
    bool dirty_info = false;
    bool dirty_big_info = false;

    hobject_t divergent_object;
    eversion_t divergent_version;
    eversion_t prior_version;
    eversion_t newhead;
    {
      pg_log_entry_t e;
      e.mod_desc.mark_unrollbackable();

      info.log_tail = log.tail = eversion_t(1, 1);
      newhead = eversion_t(1, 3);
      e.version = divergent_version = eversion_t(1, 5);
      e.soid.set_hash(0x9);
      divergent_object = e.soid;
      e.op = pg_log_entry_t::DELETE;
      e.prior_version = prior_version = eversion_t(0, 2);
      log.log.push_back(e);
      log.head = e.version;
    }

    EXPECT_FALSE(missing.have_missing());
    EXPECT_EQ(1U, log.log.size());
    EXPECT_TRUE(remove_snap.empty());
    EXPECT_TRUE(t.empty());
    EXPECT_FALSE(is_dirty());
    EXPECT_FALSE(dirty_info);
    EXPECT_FALSE(dirty_big_info);

    TestHandler h(remove_snap);
    rewind_divergent_log(t, newhead, info, &h,
			 dirty_info, dirty_big_info);

    EXPECT_TRUE(missing.is_missing(divergent_object));
    EXPECT_EQ(0U, log.objects.count(divergent_object));
    EXPECT_TRUE(log.empty());
    EXPECT_TRUE(remove_snap.empty());
    EXPECT_TRUE(t.empty());
    EXPECT_TRUE(is_dirty());
    EXPECT_TRUE(dirty_info);
    EXPECT_TRUE(dirty_big_info);
  }
}

TEST_F(PGLogTest, merge_old_entry) {
  // entries > last_backfill are silently ignored
  {
    clear();

    ObjectStore::Transaction t;
    pg_log_entry_t oe;
    oe.mod_desc.mark_unrollbackable();
    pg_info_t info;
    list<hobject_t> remove_snap;

    info.last_backfill = hobject_t();
    info.last_backfill.set_hash(1);
    oe.soid.set_hash(2);

    EXPECT_FALSE(is_dirty());
    EXPECT_TRUE(remove_snap.empty());
    EXPECT_TRUE(t.empty());
    EXPECT_FALSE(missing.have_missing());
    EXPECT_TRUE(log.empty());

    TestHandler h(remove_snap);
    merge_old_entry(t, oe, info, &h);

    EXPECT_FALSE(is_dirty());
    EXPECT_TRUE(remove_snap.empty());
    EXPECT_TRUE(t.empty());
    EXPECT_FALSE(missing.have_missing());
    EXPECT_TRUE(log.empty());
  }

  // the new entry (from the logs) has a version that is higher than
  // the old entry (from the log entry given in argument) : do
  // nothing and return false
  {
    clear();

    ObjectStore::Transaction t;
    pg_info_t info;
    list<hobject_t> remove_snap;

    pg_log_entry_t ne;
    ne.mod_desc.mark_unrollbackable();
    ne.version = eversion_t(2,1);
    log.add(ne);

    EXPECT_FALSE(is_dirty());
    EXPECT_TRUE(remove_snap.empty());
    EXPECT_TRUE(t.empty());
    EXPECT_FALSE(missing.have_missing());
    EXPECT_EQ(1U, log.log.size());
    EXPECT_EQ(ne.version, log.log.front().version);

    // the newer entry ( from the logs ) can be DELETE
    {
      log.log.front().op = pg_log_entry_t::DELETE;
      pg_log_entry_t oe;
      oe.mod_desc.mark_unrollbackable();
      oe.version = eversion_t(1,1);

      TestHandler h(remove_snap);
      merge_old_entry(t, oe, info, &h);
    }

    // if the newer entry is not DELETE, the object must be in missing
    {
      pg_log_entry_t &ne = log.log.front();
      ne.op = pg_log_entry_t::MODIFY;
      missing.add_next_event(ne);
      pg_log_entry_t oe;
      oe.mod_desc.mark_unrollbackable();
      oe.version = eversion_t(1,1);

      TestHandler h(remove_snap);
      merge_old_entry(t, oe, info, &h);

      missing.rm(ne.soid, ne.version);
    }

    EXPECT_FALSE(is_dirty());
    EXPECT_FALSE(remove_snap.empty());
    EXPECT_TRUE(t.empty());
    EXPECT_FALSE(missing.have_missing());
    EXPECT_EQ(1U, log.log.size());
    EXPECT_EQ(ne.version, log.log.front().version);

  }

  // the new entry (from the logs) has a version that is lower than
  // the old entry (from the log entry given in argument) and
  // old and new are delete : do nothing and return false
  {
    clear();

    ObjectStore::Transaction t;
    pg_log_entry_t oe;
    oe.mod_desc.mark_unrollbackable();
    pg_info_t info;
    list<hobject_t> remove_snap;

    pg_log_entry_t ne;
    ne.mod_desc.mark_unrollbackable();
    ne.version = eversion_t(1,1);
    ne.op = pg_log_entry_t::DELETE;
    log.add(ne);

    oe.version = eversion_t(2,1);
    oe.op = pg_log_entry_t::DELETE;

    EXPECT_FALSE(is_dirty());
    EXPECT_TRUE(remove_snap.empty());
    EXPECT_TRUE(t.empty());
    EXPECT_FALSE(missing.have_missing());
    EXPECT_EQ(1U, log.log.size());

    TestHandler h(remove_snap);
    merge_old_entry(t, oe, info, &h);

    EXPECT_FALSE(is_dirty());
    EXPECT_TRUE(remove_snap.empty());
    EXPECT_TRUE(t.empty());
    EXPECT_FALSE(missing.have_missing());
    EXPECT_EQ(1U, log.log.size());
  }

  // the new entry (from the logs) has a version that is lower than
  // the old entry (from the log entry given in argument) and
  // old is update and new is DELETE :
  // if the object is in missing, it is removed
  {
    clear();

    ObjectStore::Transaction t;
    pg_log_entry_t oe;
    oe.mod_desc.mark_unrollbackable();
    pg_info_t info;
    list<hobject_t> remove_snap;

    pg_log_entry_t ne;
    ne.mod_desc.mark_unrollbackable();
    ne.version = eversion_t(1,1);
    ne.op = pg_log_entry_t::DELETE;
    log.add(ne);

    oe.version = eversion_t(2,1);
    oe.op = pg_log_entry_t::MODIFY;
    missing.add_next_event(oe);

    EXPECT_FALSE(is_dirty());
    EXPECT_TRUE(remove_snap.empty());
    EXPECT_TRUE(t.empty());
    EXPECT_TRUE(missing.is_missing(oe.soid));
    EXPECT_EQ(1U, log.log.size());

    TestHandler h(remove_snap);
    merge_old_entry(t, oe, info, &h);

    EXPECT_FALSE(is_dirty());
    EXPECT_TRUE(remove_snap.size() > 0);
    EXPECT_TRUE(t.empty());
    EXPECT_FALSE(missing.have_missing());
    EXPECT_EQ(1U, log.log.size());
  }

  // there is no new entry (from the logs) and
  // the old entry (from the log entry given in argument) is not a CLONE and
  // the old entry prior_version is greater than the tail of the log :
  // do nothing and return false
  {
    clear();

    ObjectStore::Transaction t;
    pg_log_entry_t oe;
    oe.mod_desc.mark_unrollbackable();
    pg_info_t info;
    list<hobject_t> remove_snap;

    info.log_tail = eversion_t(1,1);
    oe.op = pg_log_entry_t::MODIFY;
    oe.prior_version = eversion_t(2,1);
    missing_add(oe.soid, oe.prior_version, eversion_t());

    EXPECT_FALSE(is_dirty());
    EXPECT_TRUE(remove_snap.empty());
    EXPECT_TRUE(t.empty());
    EXPECT_TRUE(log.empty());

    TestHandler h(remove_snap);
    merge_old_entry(t, oe, info, &h);

    EXPECT_FALSE(is_dirty());
    EXPECT_TRUE(remove_snap.empty());
    EXPECT_TRUE(t.empty());
    EXPECT_TRUE(log.empty());
  }

  // there is no new entry (from the logs) and
  // the old entry (from the log entry given in argument) is not a CLONE and
  // the old entry (from the log entry given in argument) is not a DELETE and
  // the old entry prior_version is lower than the tail of the log :
  //   add the old object to the remove_snap list and 
  //   add the old object to divergent priors and
  //   add or update the prior_version of the object to missing and
  //   return false
  {
    clear();

    ObjectStore::Transaction t;
    pg_log_entry_t oe;
    oe.mod_desc.mark_unrollbackable();
    pg_info_t info;
    list<hobject_t> remove_snap;

    info.log_tail = eversion_t(2,1);
    oe.soid.set_hash(1);
    oe.op = pg_log_entry_t::MODIFY;
    oe.prior_version = eversion_t(1,1);

    EXPECT_FALSE(is_dirty());
    EXPECT_TRUE(remove_snap.empty());
    EXPECT_TRUE(t.empty());
    EXPECT_FALSE(missing.have_missing());
    EXPECT_TRUE(log.empty());

    TestHandler h(remove_snap);
    merge_old_entry(t, oe, info, &h);

    EXPECT_TRUE(is_dirty());
    EXPECT_EQ(oe.soid, remove_snap.front());
    EXPECT_TRUE(t.empty());
    EXPECT_TRUE(missing.is_missing(oe.soid));
    EXPECT_TRUE(log.empty());
    EXPECT_EQ(oe.soid, divergent_priors[oe.prior_version]);
  }

  // there is no new entry (from the logs) and
  // the old entry (from the log entry given in argument) is not a CLONE and
  // the old entry (from the log entry given in argument) is a DELETE and
  // the old entry prior_version is lower than the tail of the log :
  //   add the old object to divergent priors and
  //   add or update the prior_version of the object to missing and
  //   return false
  {
    clear();

    ObjectStore::Transaction t;
    pg_log_entry_t oe;
    oe.mod_desc.mark_unrollbackable();
    pg_info_t info;
    list<hobject_t> remove_snap;

    info.log_tail = eversion_t(2,1);
    oe.soid.set_hash(1);
    oe.op = pg_log_entry_t::DELETE;
    oe.prior_version = eversion_t(1,1);

    EXPECT_FALSE(is_dirty());
    EXPECT_TRUE(remove_snap.empty());
    EXPECT_TRUE(t.empty());
    EXPECT_FALSE(missing.have_missing());
    EXPECT_TRUE(log.empty());

    TestHandler h(remove_snap);
    merge_old_entry(t, oe, info, &h);

    EXPECT_TRUE(is_dirty());
    EXPECT_TRUE(remove_snap.empty());
    EXPECT_TRUE(t.empty());
    EXPECT_TRUE(missing.is_missing(oe.soid));
    EXPECT_TRUE(log.empty());
    EXPECT_EQ(oe.soid, divergent_priors[oe.prior_version]);
  }


  // there is no new entry (from the logs) and
  // the old entry (from the log entry given in argument) is not a CLONE and
  // the old entry (from the log entry given in argument) is not a DELETE and
  // the old entry prior_version is eversion_t() :
  //   add the old object to the remove_snap list and
  //   remove the prior_version of the object from missing, if any and
  //   return false
  {
    clear();

    ObjectStore::Transaction t;
    pg_log_entry_t oe;
    oe.mod_desc.mark_unrollbackable();
    pg_info_t info;
    list<hobject_t> remove_snap;

    info.log_tail = eversion_t(10,1);
    oe.soid.set_hash(1);
    oe.op = pg_log_entry_t::MODIFY;
    oe.prior_version = eversion_t();

    missing.add(oe.soid, eversion_t(1,1), eversion_t());

    EXPECT_FALSE(is_dirty());
    EXPECT_TRUE(remove_snap.empty());
    EXPECT_TRUE(t.empty());
    EXPECT_TRUE(missing.is_missing(oe.soid));
    EXPECT_TRUE(log.empty());

    TestHandler h(remove_snap);
    merge_old_entry(t, oe, info, &h);

    EXPECT_FALSE(is_dirty());
    EXPECT_EQ(oe.soid, remove_snap.front());
    EXPECT_TRUE(t.empty());
    EXPECT_FALSE(missing.have_missing());
    EXPECT_TRUE(log.empty());
  }

}

TEST_F(PGLogTest, merge_log) {
  // head and tail match, last_backfill is set:
  // noop
  {
    clear();

    ObjectStore::Transaction t;
    pg_log_t olog;
    pg_info_t oinfo;
    pg_shard_t fromosd;
    pg_info_t info;
    list<hobject_t> remove_snap;
    bool dirty_info = false;
    bool dirty_big_info = false;

    hobject_t last_backfill(object_t("oname"), string("key"), 1, 234, 1, "");
    info.last_backfill = last_backfill;
    eversion_t stat_version(10, 1);
    info.stats.version = stat_version;
    log.tail = olog.tail = eversion_t(1, 1);
    log.head = olog.head = eversion_t(2, 1);

    EXPECT_FALSE(missing.have_missing());
    EXPECT_EQ(0U, log.log.size());
    EXPECT_EQ(stat_version, info.stats.version);
    EXPECT_TRUE(remove_snap.empty());
    EXPECT_TRUE(t.empty());
    EXPECT_EQ(last_backfill, info.last_backfill);
    EXPECT_TRUE(info.purged_snaps.empty());
    EXPECT_FALSE(is_dirty());
    EXPECT_FALSE(dirty_info);
    EXPECT_FALSE(dirty_big_info);

    TestHandler h(remove_snap);
    merge_log(t, oinfo, olog, fromosd, info, &h,
              dirty_info, dirty_big_info);

    EXPECT_FALSE(missing.have_missing());
    EXPECT_EQ(0U, log.log.size());
    EXPECT_EQ(stat_version, info.stats.version);
    EXPECT_TRUE(remove_snap.empty());
    EXPECT_TRUE(t.empty());
    EXPECT_TRUE(info.purged_snaps.empty());
    EXPECT_FALSE(is_dirty());
    EXPECT_FALSE(dirty_info);
    EXPECT_FALSE(dirty_big_info);
  }

  // head and tail match, last_backfill is not set: info.stats is
  // copied from oinfo.stats but info.stats.reported_* is guaranteed to
  // never be replaced by a lower version
  {
    clear();

    ObjectStore::Transaction t;
    pg_log_t olog;
    pg_info_t oinfo;
    pg_shard_t fromosd;
    pg_info_t info;
    list<hobject_t> remove_snap;
    bool dirty_info = false;
    bool dirty_big_info = false;

    eversion_t stat_version(10, 1);
    oinfo.stats.version = stat_version;
    info.stats.reported_seq = 1;
    info.stats.reported_epoch = 10;
    oinfo.stats.reported_seq = 1;
    oinfo.stats.reported_epoch = 1;
    log.tail = olog.tail = eversion_t(1, 1);
    log.head = olog.head = eversion_t(2, 1);

    EXPECT_FALSE(missing.have_missing());
    EXPECT_EQ(0U, log.log.size());
    EXPECT_EQ(eversion_t(), info.stats.version);
    EXPECT_EQ(1ull, info.stats.reported_seq);
    EXPECT_EQ(10u, info.stats.reported_epoch);
    EXPECT_TRUE(remove_snap.empty());
    EXPECT_TRUE(t.empty());
    EXPECT_TRUE(info.last_backfill.is_max());
    EXPECT_TRUE(info.purged_snaps.empty());
    EXPECT_FALSE(is_dirty());
    EXPECT_FALSE(dirty_info);
    EXPECT_FALSE(dirty_big_info);

    TestHandler h(remove_snap);
    merge_log(t, oinfo, olog, fromosd, info, &h,
              dirty_info, dirty_big_info);

    EXPECT_FALSE(missing.have_missing());
    EXPECT_EQ(0U, log.log.size());
    EXPECT_EQ(stat_version, info.stats.version);
    EXPECT_EQ(1ull, info.stats.reported_seq);
    EXPECT_EQ(10u, info.stats.reported_epoch);
    EXPECT_TRUE(remove_snap.empty());
    EXPECT_TRUE(t.empty());
    EXPECT_TRUE(info.purged_snaps.empty());
    EXPECT_FALSE(is_dirty());
    EXPECT_FALSE(dirty_info);
    EXPECT_FALSE(dirty_big_info);
  }

  /*         Before
            +--------------------------+
            |  log              olog   |
            +--------+-------+---------+
            |        |object |         |
            |version | hash  | version |
            |        |       |         |
            |        |  x5   |  (1,1)  < tail
            |        |       |         |
            |        |       |         |
       tail > (1,4)  |  x7   |         |
            |        |       |         |
            |        |       |         |
       head > (1,5)  |  x9   |  (1,5)  < head
            |        |       |         |
            |        |       |         |
            +--------+-------+---------+

             After
            +-----------------
            |  log           |
            +--------+-------+
            |        |object |
            |version | hash  |
            |        |       |
       tail > (1,1)  |  x5   |
            |        |       |
            |        |       |
            | (1,4)  |  x7   |
            |        |       |
            |        |       |
       head > (1,5)  |  x9   |
            |        |       |
            |        |       |
            +--------+-------+
  */
  {
    clear();

    ObjectStore::Transaction t;
    pg_log_t olog;
    pg_info_t oinfo;
    pg_shard_t fromosd;
    pg_info_t info;
    list<hobject_t> remove_snap;
    bool dirty_info = false;
    bool dirty_big_info = false;

    {
      pg_log_entry_t e;
      e.mod_desc.mark_unrollbackable();

      e.version = eversion_t(1, 4);
      e.soid.set_hash(0x5);
      log.tail = e.version;
      log.log.push_back(e);
      e.version = eversion_t(1, 5);
      e.soid.set_hash(0x9);
      log.log.push_back(e);
      log.head = e.version;
      log.index();

      info.last_update = log.head;

      e.version = eversion_t(1, 1);
      e.soid.set_hash(0x5);
      olog.tail = e.version;
      olog.log.push_back(e);
      e.version = eversion_t(1, 5);
      e.soid.set_hash(0x9);
      olog.log.push_back(e);
      olog.head = e.version;
    }

    hobject_t last_backfill(object_t("oname"), string("key"), 1, 234, 1, "");
    info.last_backfill = last_backfill;
    eversion_t stat_version(10, 1);
    info.stats.version = stat_version;

    EXPECT_FALSE(missing.have_missing());
    EXPECT_EQ(2U, log.log.size());
    EXPECT_EQ(stat_version, info.stats.version);
    EXPECT_TRUE(remove_snap.empty());
    EXPECT_TRUE(t.empty());
    EXPECT_EQ(last_backfill, info.last_backfill);
    EXPECT_TRUE(info.purged_snaps.empty());
    EXPECT_FALSE(is_dirty());
    EXPECT_FALSE(dirty_info);
    EXPECT_FALSE(dirty_big_info);

    TestHandler h(remove_snap);
    merge_log(t, oinfo, olog, fromosd, info, &h,
              dirty_info, dirty_big_info);

    EXPECT_FALSE(missing.have_missing());
    EXPECT_EQ(3U, log.log.size());
    EXPECT_EQ(stat_version, info.stats.version);
    EXPECT_TRUE(remove_snap.empty());
    EXPECT_TRUE(t.empty());
    EXPECT_TRUE(info.purged_snaps.empty());
    EXPECT_TRUE(is_dirty());
    EXPECT_TRUE(dirty_info);
    EXPECT_TRUE(dirty_big_info);
  }

  /*        +--------------------------+
            |  log              olog   |
            +--------+-------+---------+
            |        |object |         |
            |version | hash  | version |
            |        |       |         |
       tail > (1,1)  |  x5   |  (1,1)  < tail
            |        |       |         |
            |        |       |         |
            | (1,2)  |  x3   |  (1,2)  < lower_bound
            |        |       |         |
            |        |       |         |
       head > (1,3)  |  x9   |         |
            | DELETE |       |         |
            |        |       |         |
            |        |  x9   |  (2,3)  |
            |        |       |  MODIFY |
            |        |       |         |
            |        |  x7   |  (2,4)  < head
            |        |       |  DELETE |
            +--------+-------+---------+

      The log entry (1,3) deletes the object x9 but the olog entry (2,3) modifies
      it and is authoritative : the log entry (1,3) is divergent.

  */
  {
    clear();

    ObjectStore::Transaction t;
    pg_log_t olog;
    pg_info_t oinfo;
    pg_shard_t fromosd;
    pg_info_t info;
    list<hobject_t> remove_snap;
    bool dirty_info = false;
    bool dirty_big_info = false;

    hobject_t divergent_object;

    {
      pg_log_entry_t e;
      e.mod_desc.mark_unrollbackable();

      e.version = eversion_t(1, 1);
      e.soid.set_hash(0x5);
      log.tail = e.version;
      log.log.push_back(e);
      e.version = eversion_t(1, 2);
      e.soid.set_hash(0x3);
      log.log.push_back(e);
      e.version = eversion_t(1,3);
      e.soid.set_hash(0x9);
      divergent_object = e.soid;
      e.op = pg_log_entry_t::DELETE;
      log.log.push_back(e);
      log.head = e.version;
      log.index();

      info.last_update = log.head;

      e.version = eversion_t(1, 1);
      e.soid.set_hash(0x5);
      olog.tail = e.version;
      olog.log.push_back(e);
      e.version = eversion_t(1, 2);
      e.soid.set_hash(0x3);
      olog.log.push_back(e);
      e.version = eversion_t(2, 3);
      e.soid.set_hash(0x9);
      e.op = pg_log_entry_t::MODIFY;
      olog.log.push_back(e);
      e.version = eversion_t(2, 4);
      e.soid.set_hash(0x7);
      e.op = pg_log_entry_t::DELETE;
      olog.log.push_back(e);
      olog.head = e.version;
    }

    snapid_t purged_snap(1);
    {
      oinfo.last_update = olog.head;
      oinfo.purged_snaps.insert(purged_snap);
    }

    EXPECT_FALSE(missing.have_missing());
    EXPECT_EQ(1U, log.objects.count(divergent_object));
    EXPECT_EQ(3U, log.log.size());
    EXPECT_TRUE(remove_snap.empty());
    EXPECT_TRUE(t.empty());
    EXPECT_EQ(log.head, info.last_update);
    EXPECT_TRUE(info.purged_snaps.empty());
    EXPECT_FALSE(is_dirty());
    EXPECT_FALSE(dirty_info);
    EXPECT_FALSE(dirty_big_info);

    TestHandler h(remove_snap);
    merge_log(t, oinfo, olog, fromosd, info, &h,
              dirty_info, dirty_big_info);

    /* When the divergent entry is a DELETE and the authoritative
       entry is a MODIFY, the object will be added to missing : it is
       a verifiable side effect proving the entry was identified
       to be divergent.
    */
    EXPECT_TRUE(missing.is_missing(divergent_object));
    EXPECT_EQ(1U, log.objects.count(divergent_object));
    EXPECT_EQ(4U, log.log.size());
    /* DELETE entries from olog that are appended to the hed of the
       log are also added to remove_snap.
     */
    EXPECT_EQ(0x7U, remove_snap.front().get_hash());
    EXPECT_TRUE(t.empty());
    EXPECT_EQ(log.head, info.last_update);
    EXPECT_TRUE(info.purged_snaps.contains(purged_snap));
    EXPECT_TRUE(is_dirty());
    EXPECT_TRUE(dirty_info);
    EXPECT_TRUE(dirty_big_info);
  }

  /*        +--------------------------+
            |  log              olog   |
            +--------+-------+---------+
            |        |object |         |
            |version | hash  | version |
            |        |       |         |
       tail > (1,1)  |  x5   |  (1,1)  < tail
            |        |       |         |
            |        |       |         |
            | (1,4)  |  x7   |  (1,4)  < head
            |        |       |         |
            |        |       |         |
       head > (1,5)  |  x9   |         |
            |        |       |         |
            |        |       |         |
            +--------+-------+---------+

      The head of the log entry (1,5) is divergent because it is greater than the
      head of olog.

  */
  {
    clear();

    ObjectStore::Transaction t;
    pg_log_t olog;
    pg_info_t oinfo;
    pg_shard_t fromosd;
    pg_info_t info;
    list<hobject_t> remove_snap;
    bool dirty_info = false;
    bool dirty_big_info = false;

    {
      pg_log_entry_t e;
      e.mod_desc.mark_unrollbackable();

      e.version = eversion_t(1, 1);
      e.soid.set_hash(0x5);
      log.tail = e.version;
      log.log.push_back(e);
      e.version = eversion_t(1, 4);
      e.soid.set_hash(0x7);
      log.log.push_back(e);
      e.version = eversion_t(1, 5);
      e.soid.set_hash(0x9);
      log.log.push_back(e);
      log.head = e.version;
      log.index();

      info.last_update = log.head;

      e.version = eversion_t(1, 1);
      e.soid.set_hash(0x5);
      olog.tail = e.version;
      olog.log.push_back(e);
      e.version = eversion_t(1, 4);
      e.soid.set_hash(0x7);
      olog.log.push_back(e);
      olog.head = e.version;
    }

    hobject_t last_backfill(object_t("oname"), string("key"), 1, 234, 1, "");
    info.last_backfill = last_backfill;
    eversion_t stat_version(10, 1);
    info.stats.version = stat_version;

    EXPECT_FALSE(missing.have_missing());
    EXPECT_EQ(3U, log.log.size());
    EXPECT_EQ(stat_version, info.stats.version);
    EXPECT_TRUE(remove_snap.empty());
    EXPECT_TRUE(t.empty());
    EXPECT_EQ(last_backfill, info.last_backfill);
    EXPECT_TRUE(info.purged_snaps.empty());
    EXPECT_FALSE(is_dirty());
    EXPECT_FALSE(dirty_info);
    EXPECT_FALSE(dirty_big_info);

    TestHandler h(remove_snap);
    merge_log(t, oinfo, olog, fromosd, info, &h,
              dirty_info, dirty_big_info);

    EXPECT_FALSE(missing.have_missing());
    EXPECT_EQ(2U, log.log.size());
    EXPECT_EQ(stat_version, info.stats.version);
    EXPECT_EQ(0x9U, remove_snap.front().get_hash());
    EXPECT_TRUE(t.empty());
    EXPECT_TRUE(info.purged_snaps.empty());
    EXPECT_TRUE(is_dirty());
    EXPECT_TRUE(dirty_info);
    EXPECT_TRUE(dirty_big_info);
  }

  // If our log is empty, the incoming log needs to have not been trimmed.
  {
    clear();

    ObjectStore::Transaction t;
    pg_log_t olog;
    pg_info_t oinfo;
    pg_shard_t fromosd;
    pg_info_t info;
    list<hobject_t> remove_snap;
    bool dirty_info = false;
    bool dirty_big_info = false;

    // olog has been trimmed
    olog.tail = eversion_t(1, 1);

    TestHandler h(remove_snap);
    ASSERT_THROW(merge_log(t, oinfo, olog, fromosd, info, &h,
                           dirty_info, dirty_big_info), FailedAssertion);
  }

  /*        +--------------------------+
            |  log              olog   |
            +--------+-------+---------+
            |        |object |         |
            |version | hash  | version |
       tail > (0,0)  |       |         |
            | (1,1)  |  x5   |         |
            |        |       |         |
            |        |       |         |
       head > (1,2)  |  x3   |         |
            |        |       |         |
            |        |       |  (2,3)  < tail
            |        |  x9   |  (2,4)  |
            |        |       |         |
            |        |  x5   |  (2,5)  < head
            |        |       |         |
            +--------+-------+---------+

      If the logs do not overlap, throw an exception.

  */
  {
    clear();

    ObjectStore::Transaction t;
    pg_log_t olog;
    pg_info_t oinfo;
    pg_shard_t fromosd;
    pg_info_t info;
    list<hobject_t> remove_snap;
    bool dirty_info = false;
    bool dirty_big_info = false;

    {
      pg_log_entry_t e;
      e.mod_desc.mark_unrollbackable();

      log.tail = eversion_t();
      e.version = eversion_t(1, 1);
      e.soid.set_hash(0x5);
      log.log.push_back(e);
      e.version = eversion_t(1, 2);
      e.soid.set_hash(0x3);
      log.log.push_back(e);
      log.head = e.version;
      log.index();

      info.last_update = log.head;

      olog.tail = eversion_t(2, 3);
      e.version = eversion_t(2, 4);
      e.soid.set_hash(0x9);
      olog.log.push_back(e);
      e.version = eversion_t(2, 5);
      e.soid.set_hash(0x5);
      olog.log.push_back(e);
      olog.head = e.version;
    }

    TestHandler h(remove_snap);
    EXPECT_THROW(merge_log(t, oinfo, olog, fromosd, info, &h,
                           dirty_info, dirty_big_info), FailedAssertion);
  }
}

TEST_F(PGLogTest, proc_replica_log) {
  // empty log : no side effect
  {
    clear();

    ObjectStore::Transaction t;
    pg_log_t olog;
    pg_info_t oinfo;
    pg_missing_t omissing;
    pg_shard_t from;

    eversion_t last_update(1, 1);
    oinfo.last_update = last_update;
    eversion_t last_complete(2, 1);
    oinfo.last_complete = last_complete;

    EXPECT_TRUE(t.empty());
    EXPECT_FALSE(omissing.have_missing());
    EXPECT_EQ(last_update, oinfo.last_update);
    EXPECT_EQ(last_complete, oinfo.last_complete);

    proc_replica_log(t, oinfo, olog, omissing, from);

    EXPECT_TRUE(t.empty());
    EXPECT_FALSE(omissing.have_missing());
    EXPECT_EQ(last_update, oinfo.last_update);
    EXPECT_EQ(last_update, oinfo.last_complete);
  }

  /*        +--------------------------+
            |  log              olog   |
            +--------+-------+---------+
            |        |object |         |
            |version | hash  | version |
            |        |       |         |
            |        |  x3   |  (1,1)  < tail
            |        |       |         |
            |        |       |         |
       tail > (1,2)  |  x5   |         |
            |        |       |         |
            |        |       |         |
       head > (1,3)  |  x9   |         |
            | DELETE |       |         |
            |        |       |         |
            |        |  x9   |  (2,3)  < head
            |        |       |  DELETE |
            |        |       |         |
            +--------+-------+---------+
	    
      The log entry (1,3) deletes the object x9 and the olog entry
      (2,3) also deletes it : do nothing. The olog tail is ignored
      because it is before the log tail.
      
  */
  {
    clear();

    ObjectStore::Transaction t;
    pg_log_t olog;
    pg_info_t oinfo;
    pg_missing_t omissing;
    pg_shard_t from;

    {
      pg_log_entry_t e;
      e.mod_desc.mark_unrollbackable();

      e.version = eversion_t(1, 2);
      e.soid.set_hash(0x5);
      log.tail = e.version;
      log.log.push_back(e);
      e.version = eversion_t(1, 3);
      e.soid.set_hash(0x9);
      e.op = pg_log_entry_t::DELETE;
      log.log.push_back(e);
      log.head = e.version;
      log.index();

      e.version = eversion_t(1, 1);
      e.soid.set_hash(0x3);
      olog.tail = e.version;
      olog.log.push_back(e);
      e.version = eversion_t(2, 3);
      e.soid.set_hash(0x9);
      e.op = pg_log_entry_t::DELETE;
      olog.log.push_back(e);
      olog.head = e.version;

      oinfo.last_update = olog.head;
      oinfo.last_complete = olog.head;
    }

    EXPECT_TRUE(t.empty());
    EXPECT_FALSE(omissing.have_missing());
    EXPECT_EQ(olog.head, oinfo.last_update);
    EXPECT_EQ(olog.head, oinfo.last_complete);

    proc_replica_log(t, oinfo, olog, omissing, from);

    EXPECT_TRUE(t.empty());
    EXPECT_FALSE(omissing.have_missing());
  }

 {
    clear();

    ObjectStore::Transaction t;
    pg_log_t olog;
    pg_info_t oinfo;
    pg_missing_t omissing;
    pg_shard_t from;

    hobject_t divergent_object;

    {
      pg_log_entry_t e;
      e.mod_desc.mark_unrollbackable();

      {
	e.soid = divergent_object;
	e.soid.set_hash(0x1);
	e.version = eversion_t(1, 1);
	log.tail = e.version;
	log.log.push_back(e);

	e.soid = divergent_object;
	e.prior_version = eversion_t(1, 1);
	e.version = eversion_t(1, 2);
	log.tail = e.version;
	log.log.push_back(e);

	e.soid.set_hash(0x3);
	e.version = eversion_t(1, 4);
	log.log.push_back(e);

	e.soid.set_hash(0x7);
	e.version = eversion_t(1, 5);
	log.log.push_back(e);

	e.soid.set_hash(0x8);
	e.version = eversion_t(1, 6);
	log.log.push_back(e);

	e.soid.set_hash(0x9);
	e.op = pg_log_entry_t::DELETE;
	e.version = eversion_t(2, 7);
	log.log.push_back(e);

	e.soid.set_hash(0xa);
	e.version = eversion_t(2, 8);
	log.head = e.version;
	log.log.push_back(e);
      }
      log.index();

      {
	e.soid = divergent_object;
	e.soid.set_hash(0x1);
	e.version = eversion_t(1, 1);
	olog.tail = e.version;
	olog.log.push_back(e);

	e.soid = divergent_object;
	e.prior_version = eversion_t(1, 1);
	e.version = eversion_t(1, 2);
	olog.log.push_back(e);

	e.prior_version = eversion_t(0, 0);
	e.soid.set_hash(0x3);
	e.version = eversion_t(1, 4);
	olog.log.push_back(e);

	e.soid.set_hash(0x7);
	e.version = eversion_t(1, 5);
	olog.log.push_back(e);

	e.soid.set_hash(0x8);
	e.version = eversion_t(1, 6);
	olog.log.push_back(e);

	e.soid.set_hash(0x9); // should not be added to missing, create
	e.op = pg_log_entry_t::MODIFY;
	e.version = eversion_t(1, 7);
	olog.log.push_back(e);

	e.soid = divergent_object; // should be added to missing at 1,2
	e.op = pg_log_entry_t::MODIFY;
	e.version = eversion_t(1, 8);
	e.prior_version = eversion_t(1, 2);
	olog.log.push_back(e);
	olog.head = e.version;
      }
      oinfo.last_update = olog.head;
      oinfo.last_complete = olog.head;
    }

    EXPECT_TRUE(t.empty());
    EXPECT_FALSE(omissing.have_missing());
    EXPECT_EQ(olog.head, oinfo.last_update);
    EXPECT_EQ(olog.head, oinfo.last_complete);

    proc_replica_log(t, oinfo, olog, omissing, from);

    EXPECT_TRUE(t.empty());
    EXPECT_TRUE(omissing.have_missing());
    EXPECT_TRUE(omissing.is_missing(divergent_object));
    EXPECT_EQ(eversion_t(1, 2), omissing.missing[divergent_object].need);
    EXPECT_EQ(eversion_t(1, 6), oinfo.last_update);
    EXPECT_EQ(eversion_t(1, 1), oinfo.last_complete);
  }

  /*        +--------------------------+
            |  log              olog   |
            +--------+-------+---------+
            |        |object |         |
            |version | hash  | version |
            |        |       |         |
       tail > (1,1)  |  x5   |  (1,1)  < tail
            |        |       |         |
            |        |       |         |
            | (1,2)  |  x3   |  (1,2)  |
            |        |       |         |
            |        |       |         |
       head > (1,3)  |  x9   |         |
            | DELETE |       |         |
            |        |       |         |
            |        |  x9   |  (2,3)  < head
            |        |       |  DELETE |
            |        |       |         |
            +--------+-------+---------+

      The log entry (1,3) deletes the object x9 and the olog entry
      (2,3) also deletes it : do nothing.

  */
  {
    clear();

    ObjectStore::Transaction t;
    pg_log_t olog;
    pg_info_t oinfo;
    pg_missing_t omissing;
    pg_shard_t from;

    eversion_t last_update(1, 2);

    {
      pg_log_entry_t e;
      e.mod_desc.mark_unrollbackable();

      e.version = eversion_t(1, 1);
      e.soid.set_hash(0x5);
      log.tail = e.version;
      log.log.push_back(e);
      e.version = last_update;
      e.soid.set_hash(0x3);
      log.log.push_back(e);
      e.version = eversion_t(1,3);
      e.soid.set_hash(0x9);
      e.op = pg_log_entry_t::DELETE;
      log.log.push_back(e);
      log.head = e.version;
      log.index();

      e.version = eversion_t(1, 1);
      e.soid.set_hash(0x5);
      olog.tail = e.version;
      olog.log.push_back(e);
      e.version = last_update;
      e.soid.set_hash(0x3);
      olog.log.push_back(e);
      e.version = eversion_t(2, 3);
      e.soid.set_hash(0x9);
      e.op = pg_log_entry_t::DELETE;
      olog.log.push_back(e);
      olog.head = e.version;

      oinfo.last_update = olog.head;
      oinfo.last_complete = olog.head;
    }

    EXPECT_TRUE(t.empty());
    EXPECT_FALSE(omissing.have_missing());
    EXPECT_EQ(olog.head, oinfo.last_update);
    EXPECT_EQ(olog.head, oinfo.last_complete);

    proc_replica_log(t, oinfo, olog, omissing, from);

    EXPECT_TRUE(t.empty());
    EXPECT_FALSE(omissing.have_missing());
    EXPECT_EQ(last_update, oinfo.last_update);
    EXPECT_EQ(last_update, oinfo.last_complete);
  }

  /*        +--------------------------+
            |  log              olog   |
            +--------+-------+---------+
            |        |object |         |
            |version | hash  | version |
            |        |       |         |
       tail > (1,1)  |  x5   |  (1,1)  < tail
            |        |       |         |
            |        |       |         |
            | (1,2)  |  x3   |  (1,2)  |
            |        |       |         |
            |        |       |         |
       head > (1,3)  |  x9   |         |
            | DELETE |       |         |
            |        |       |         |
            |        |  x9   |  (2,3)  < head
            |        |       |  MODIFY |
            |        |       |         |
            +--------+-------+---------+

      The log entry (1,3) deletes the object x9 but the olog entry
      (2,3) modifies it : remove it from omissing.

  */
  {
    clear();

    ObjectStore::Transaction t;
    pg_log_t olog;
    pg_info_t oinfo;
    pg_missing_t omissing;
    pg_shard_t from;

    eversion_t last_update(1, 2);
    hobject_t divergent_object;

    {
      pg_log_entry_t e;
      e.mod_desc.mark_unrollbackable();

      e.version = eversion_t(1, 1);
      e.soid.set_hash(0x5);
      log.tail = e.version;
      log.log.push_back(e);
      e.version = last_update;
      e.soid.set_hash(0x3);
      log.log.push_back(e);
      e.version = eversion_t(1, 3);
      e.soid.set_hash(0x9);
      e.op = pg_log_entry_t::DELETE;
      log.log.push_back(e);
      log.head = e.version;
      log.index();

      e.version = eversion_t(1, 1);
      e.soid.set_hash(0x5);
      olog.tail = e.version;
      olog.log.push_back(e);
      e.version = last_update;
      e.soid.set_hash(0x3);
      olog.log.push_back(e);
      e.version = eversion_t(2, 3);
      e.soid.set_hash(0x9);
      divergent_object = e.soid;
      omissing.add(divergent_object, e.version, eversion_t());
      e.op = pg_log_entry_t::MODIFY;
      olog.log.push_back(e);
      olog.head = e.version;

      oinfo.last_update = olog.head;
      oinfo.last_complete = olog.head;
    }

    EXPECT_TRUE(t.empty());
    EXPECT_TRUE(omissing.have_missing());
    EXPECT_TRUE(omissing.is_missing(divergent_object));
    EXPECT_EQ(eversion_t(2, 3), omissing.missing[divergent_object].need);
    EXPECT_EQ(olog.head, oinfo.last_update);
    EXPECT_EQ(olog.head, oinfo.last_complete);

    proc_replica_log(t, oinfo, olog, omissing, from);

    EXPECT_TRUE(t.empty());
    EXPECT_FALSE(omissing.have_missing());
    EXPECT_EQ(last_update, oinfo.last_update);
    EXPECT_EQ(last_update, oinfo.last_complete);
  }

  /*        +--------------------------+
            |  log              olog   |
            +--------+-------+---------+
            |        |object |         |
            |version | hash  | version |
            |        |       |         |
       tail > (1,1)  |  x9   |  (1,1)  < tail
            |        |       |         |
            |        |       |         |
            | (1,2)  |  x3   |  (1,2)  |
            |        |       |         |
            |        |       |         |
            |        |  x9   |  (1,3)  < head
            |        |       |  MODIFY |
            |        |       |         |
       head > (2,3)  |  x9   |         |
            | DELETE |       |         |
            |        |       |         |
            +--------+-------+---------+

      The log entry (2,3) deletes the object x9 but the olog entry
      (1,3) modifies it : proc_replica_log should adjust missing to
      1,1 for that object until add_next_event in PG::activate processes
      the delete.
  */
  {
    clear();

    ObjectStore::Transaction t;
    pg_log_t olog;
    pg_info_t oinfo;
    pg_missing_t omissing;
    pg_shard_t from;

    eversion_t last_update(1, 2);
    hobject_t divergent_object;
    eversion_t new_version(2, 3);
    eversion_t divergent_version(1, 3);

    {
      pg_log_entry_t e;
      e.mod_desc.mark_unrollbackable();

      e.version = eversion_t(1, 1);
      e.soid.set_hash(0x9);
      log.tail = e.version;
      log.log.push_back(e);
      e.version = last_update;
      e.soid.set_hash(0x3);
      log.log.push_back(e);
      e.version = new_version;
      e.prior_version = eversion_t(1, 1);
      e.soid.set_hash(0x9);
      e.op = pg_log_entry_t::DELETE;
      log.log.push_back(e);
      log.head = e.version;
      log.index();

      e.op = pg_log_entry_t::MODIFY;
      e.version = eversion_t(1, 1);
      e.soid.set_hash(0x9);
      olog.tail = e.version;
      olog.log.push_back(e);
      e.version = last_update;
      e.soid.set_hash(0x3);
      olog.log.push_back(e);
      e.version = divergent_version;
      e.prior_version = eversion_t(1, 1);
      e.soid.set_hash(0x9);
      divergent_object = e.soid;
      omissing.add(divergent_object, e.version, eversion_t());
      e.op = pg_log_entry_t::MODIFY;
      olog.log.push_back(e);
      olog.head = e.version;

      oinfo.last_update = olog.head;
      oinfo.last_complete = olog.head;
    }

    EXPECT_TRUE(t.empty());
    EXPECT_TRUE(omissing.have_missing());
    EXPECT_TRUE(omissing.is_missing(divergent_object));
    EXPECT_EQ(divergent_version, omissing.missing[divergent_object].need);
    EXPECT_EQ(olog.head, oinfo.last_update);
    EXPECT_EQ(olog.head, oinfo.last_complete);

    proc_replica_log(t, oinfo, olog, omissing, from);

    EXPECT_TRUE(t.empty());
    EXPECT_TRUE(omissing.have_missing());
    EXPECT_TRUE(omissing.missing.begin()->second.need == eversion_t(1, 1));
    EXPECT_EQ(last_update, oinfo.last_update);
    EXPECT_EQ(eversion_t(0, 0), oinfo.last_complete);
  }

}

TEST_F(PGLogTest, merge_log_1) {
  TestCase t;
  t.base.push_back(mk_ple_mod(mk_obj(1), mk_evt(10, 100), mk_evt(8, 80)));

  t.div.push_back(mk_ple_mod(mk_obj(1), mk_evt(10, 101), mk_evt(10, 100)));

  t.final.add(mk_obj(1), mk_evt(10, 100), mk_evt(0, 0));

  t.toremove.insert(mk_obj(1));

  t.setup();
  run_test_case(t);
}

TEST_F(PGLogTest, merge_log_2) {
  TestCase t;
  t.base.push_back(mk_ple_mod_rb(mk_obj(1), mk_evt(10, 100), mk_evt(8, 80)));

  t.div.push_back(mk_ple_mod_rb(mk_obj(1), mk_evt(10, 101), mk_evt(10, 100)));
  t.div.push_back(mk_ple_mod_rb(mk_obj(1), mk_evt(10, 102), mk_evt(10, 101)));

  t.torollback.insert(
    t.torollback.begin(), t.div.rbegin(), t.div.rend());

  t.setup();
  run_test_case(t);
}

TEST_F(PGLogTest, merge_log_3) {
  TestCase t;
  t.base.push_back(mk_ple_mod_rb(mk_obj(1), mk_evt(10, 100), mk_evt(8, 80)));

  t.div.push_back(mk_ple_mod(mk_obj(1), mk_evt(10, 101), mk_evt(10, 100)));
  t.div.push_back(mk_ple_mod_rb(mk_obj(1), mk_evt(10, 102), mk_evt(10, 101)));

  t.final.add(mk_obj(1), mk_evt(10, 100), mk_evt(0, 0));

  t.toremove.insert(mk_obj(1));

  t.setup();
  run_test_case(t);
}

TEST_F(PGLogTest, merge_log_4) {
  TestCase t;
  t.base.push_back(mk_ple_mod_rb(mk_obj(1), mk_evt(10, 100), mk_evt(8, 80)));

  t.div.push_back(mk_ple_mod_rb(mk_obj(1), mk_evt(10, 101), mk_evt(10, 100)));
  t.div.push_back(mk_ple_mod_rb(mk_obj(1), mk_evt(10, 102), mk_evt(10, 101)));

  t.init.add(mk_obj(1), mk_evt(10, 102), mk_evt(0, 0));
  t.final.add(mk_obj(1), mk_evt(10, 100), mk_evt(0, 0));

  t.setup();
  run_test_case(t);
}

TEST_F(PGLogTest, merge_log_5) {
  TestCase t;
  t.base.push_back(mk_ple_mod_rb(mk_obj(1), mk_evt(10, 100), mk_evt(8, 80)));

  t.div.push_back(mk_ple_mod(mk_obj(1), mk_evt(10, 101), mk_evt(10, 100)));
  t.div.push_back(mk_ple_mod_rb(mk_obj(1), mk_evt(10, 102), mk_evt(10, 101)));

  t.auth.push_back(mk_ple_mod(mk_obj(1), mk_evt(11, 101), mk_evt(10, 100)));

  t.final.add(mk_obj(1), mk_evt(11, 101), mk_evt(0, 0));

  t.toremove.insert(mk_obj(1));

  t.setup();
  run_test_case(t);
}

TEST_F(PGLogTest, merge_log_6) {
  TestCase t;
  t.base.push_back(mk_ple_mod_rb(mk_obj(1), mk_evt(10, 100), mk_evt(8, 80)));

  t.auth.push_back(mk_ple_mod(mk_obj(1), mk_evt(11, 101), mk_evt(10, 100)));

  t.final.add(mk_obj(1), mk_evt(11, 101), mk_evt(10, 100));

  t.setup();
  run_test_case(t);
}

TEST_F(PGLogTest, merge_log_7) {
  TestCase t;
  t.base.push_back(mk_ple_mod_rb(mk_obj(1), mk_evt(10, 100), mk_evt(8, 80)));

  t.auth.push_back(mk_ple_mod(mk_obj(1), mk_evt(11, 101), mk_evt(10, 100)));

  t.init.add(mk_obj(1), mk_evt(10, 100), mk_evt(8, 80));
  t.final.add(mk_obj(1), mk_evt(11, 101), mk_evt(8, 80));

  t.setup();
  run_test_case(t);
}

TEST_F(PGLogTest, merge_log_8) {
  TestCase t;
  t.base.push_back(mk_ple_mod_rb(mk_obj(1), mk_evt(10, 100), mk_evt(8, 80)));

  t.auth.push_back(mk_ple_dt(mk_obj(1), mk_evt(11, 101), mk_evt(10, 100)));

  t.init.add(mk_obj(1), mk_evt(10, 100), mk_evt(8, 80));

  t.toremove.insert(mk_obj(1));

  t.setup();
  run_test_case(t);
}

TEST_F(PGLogTest, merge_log_prior_version_have) {
  TestCase t;
  t.base.push_back(mk_ple_mod_rb(mk_obj(1), mk_evt(10, 100), mk_evt(8, 80)));

  t.div.push_back(mk_ple_mod(mk_obj(1), mk_evt(10, 101), mk_evt(10, 100)));

  t.init.add(mk_obj(1), mk_evt(10, 101), mk_evt(10, 100));

  t.setup();
  run_test_case(t);
}

int main(int argc, char **argv) {
  vector<const char*> args;
  argv_to_vec(argc, (const char **)argv, args);

  global_init(NULL, args, CEPH_ENTITY_TYPE_CLIENT, CODE_ENVIRONMENT_UTILITY, 0);
  common_init_finish(g_ceph_context);

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

// Local Variables:
// compile-command: "cd ../.. ; make unittest_pglog ; ./unittest_pglog --log-to-stderr=true  --debug-osd=20 # --gtest_filter=*.* "
// End:

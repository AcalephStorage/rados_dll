// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
#include "include/encoding.h"
#include "KeyValueDBMemory.h"
#include <map>
#include <set>
#include "include/memory.h"
#include <iostream>

using namespace std;

/**
 * Iterate over the whole key space of the in-memory store
 *
 * @note Removing keys from the store while iterating over the store key-space
 *	 may result in unspecified behavior.
 *	 If one wants to safely iterate over the store while updating the
 *	 store, one should instead use a snapshot iterator, which provides
 *	 strong read-consistency.
 */
class WholeSpaceMemIterator : public KeyValueDB::WholeSpaceIteratorImpl {
protected:
  KeyValueDBMemory *db;
  bool ready;

  map<pair<string,string>, bufferlist>::iterator it;

public:
  WholeSpaceMemIterator(KeyValueDBMemory *db) : db(db), ready(false) { }
  virtual ~WholeSpaceMemIterator() { }

  int seek_to_first() {
    if (db->db.empty()) {
      it = db->db.end();
      ready = false;
      return 0;
    }
    it = db->db.begin();
    ready = true;
    return 0;
  }

  int seek_to_first(const string &prefix) {
    it = db->db.lower_bound(make_pair(prefix, ""));
    if (db->db.empty() || (it == db->db.end())) {
      it = db->db.end();
      ready = false;
      return 0;
    }
    ready = true;
    return 0;
  }

  int seek_to_last() {
    it = db->db.end();
    if (db->db.empty()) {
      ready = false;
      return 0;
    }
    --it;
    assert(it != db->db.end());
    ready = true;
    return 0;
  }

  int seek_to_last(const string &prefix) {
    string tmp(prefix);
    tmp.append(1, (char) 0);
    it = db->db.upper_bound(make_pair(tmp,""));

    if (db->db.empty() || (it == db->db.end())) {
      seek_to_last();
    }
    else {
      ready = true;
      prev();
    }
    return 0;
  }

  int lower_bound(const string &prefix, const string &to) {
    it = db->db.lower_bound(make_pair(prefix,to));
    if ((db->db.empty()) || (it == db->db.end())) {
      it = db->db.end();
      ready = false;
      return 0;
    }

    assert(it != db->db.end());

    ready = true;
    return 0;
  }

  int upper_bound(const string &prefix, const string &after) {
    it = db->db.upper_bound(make_pair(prefix,after));
    if ((db->db.empty()) || (it == db->db.end())) {
      it = db->db.end();
      ready = false;
      return 0;
    }
    assert(it != db->db.end());
    ready = true;
    return 0;
  }

  bool valid() {
    return ready && (it != db->db.end());
  }

  bool begin() {
    return ready && (it == db->db.begin());
  }

  int prev() {
    if (!begin() && ready)
      --it;
    else
      it = db->db.end();
    return 0;
  }

  int next() {
    if (valid())
      ++it;
    return 0;
  }

  string key() {
    if (valid())
      return (*it).first.second;
    else
      return "";
  }

  pair<string,string> raw_key() {
    if (valid())
      return (*it).first;
    else
      return make_pair("", "");
  }

  bufferlist value() {
    if (valid())
      return (*it).second;
    else
      return bufferlist();
  }

  int status() {
    return 0;
  }
};

int KeyValueDBMemory::get(const string &prefix,
			  const std::set<string> &key,
			  map<string, bufferlist> *out) {
  if (!exists_prefix(prefix))
    return 0;

  for (std::set<string>::const_iterator i = key.begin();
       i != key.end();
       ++i) {
    pair<string,string> k(prefix, *i);
    if (db.count(k))
      (*out)[*i] = db[k];
  }
  return 0;
}

int KeyValueDBMemory::get_keys(const string &prefix,
			       const std::set<string> &key,
			       std::set<string> *out) {
  if (!exists_prefix(prefix))
    return 0;

  for (std::set<string>::const_iterator i = key.begin();
       i != key.end();
       ++i) {
    if (db.count(make_pair(prefix, *i)))
      out->insert(*i);
  }
  return 0;
}

int KeyValueDBMemory::set(const string &prefix,
			  const string &key,
			  const bufferlist &bl) {
  db[make_pair(prefix,key)] = bl;
  return 0;
}

int KeyValueDBMemory::rmkey(const string &prefix,
			    const string &key) {
  db.erase(make_pair(prefix,key));
  return 0;
}

int KeyValueDBMemory::rmkeys_by_prefix(const string &prefix) {
  map<std::pair<string,string>,bufferlist>::iterator i;
  i = db.lower_bound(make_pair(prefix, ""));
  if (i == db.end())
    return 0;

  while (i != db.end()) {
    std::pair<string,string> key = (*i).first;
    if (key.first != prefix)
      break;

    ++i;
    rmkey(key.first, key.second);
  }
  return 0;
}

KeyValueDB::WholeSpaceIterator KeyValueDBMemory::_get_iterator() {
  return ceph::shared_ptr<KeyValueDB::WholeSpaceIteratorImpl>(
    new WholeSpaceMemIterator(this)
  );
}

class WholeSpaceSnapshotMemIterator : public WholeSpaceMemIterator {
public:

  /**
   * @note
   * We perform a copy of the db map, which is populated by bufferlists.
   *
   * These are designed as shallow containers, thus there is a chance that
   * changing the underlying memory pages will lead to the iterator seeing
   * erroneous states.
   *
   * Although we haven't verified this yet, there is this chance, so we should
   * keep it in mind.
   */

  WholeSpaceSnapshotMemIterator(KeyValueDBMemory *db) :
    WholeSpaceMemIterator(db) { }
  ~WholeSpaceSnapshotMemIterator() {
    delete db;
  }
};

KeyValueDB::WholeSpaceIterator KeyValueDBMemory::_get_snapshot_iterator() {
  KeyValueDBMemory *snap_db = new KeyValueDBMemory(this);
  return ceph::shared_ptr<KeyValueDB::WholeSpaceIteratorImpl>(
    new WholeSpaceSnapshotMemIterator(snap_db)
  );
}

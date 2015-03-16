// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2012 Red Hat
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */


#ifndef MDS_CONTEXT_H
#define MDS_CONTEXT_H

#include "include/Context.h"

class MDS;


/**
 * Completion which has access to a reference to the global MDS instance.
 *
 * This class exists so that Context subclasses can provide the MDS pointer
 * from a pointer they already had, e.g. MDCache or Locker, rather than
 * necessarily having to carry around an extra MDS* pointer. 
 */
class MDSContext : public Context
{
protected:
  virtual MDS *get_mds() = 0;
};


/**
 * A context which must be called with the big MDS lock held.  Subclass
 * this with a get_mds implementation.
 */
class MDSInternalContextBase : public MDSContext
{
public:
    void complete(int r);
};

/**
 * General purpose, lets you pass in an MDS pointer.
 */
class MDSInternalContext : public MDSInternalContextBase
{
protected:
  MDS *mds;
  virtual MDS* get_mds();

public:
  MDSInternalContext(MDS *mds_) : mds(mds_) {
    assert(mds != NULL);
  }
};

/**
 * Wrap a regular Context up as an Internal context. Useful
 * if you're trying to work with one of our more generic frameworks.
 */
class MDSInternalContextWrapper : public MDSInternalContextBase
{
protected:
  MDS *mds;
  Context *fin;
  MDS *get_mds();
public:
  MDSInternalContextWrapper(MDS *m, Context *c) : mds(m), fin(c) {}
  void finish(int r);
};

class MDSIOContextBase : public MDSContext
{
    void complete(int r);
};

/**
 * Completion for an I/O operation, takes big MDS lock
 * before executing finish function.
 */
class MDSIOContext : public MDSIOContextBase
{
protected:
  MDS *mds;
  virtual MDS* get_mds();

public:
  MDSIOContext(MDS *mds_) : mds(mds_) {
    assert(mds != NULL);
  }
};

/**
 * Wrap a regular Context up as an IO Context. Useful
 * if you're trying to work with one of our more generic frameworks.
 */
class MDSIOContextWrapper : public MDSIOContextBase
{
protected:
  MDS *mds;
  Context *fin;
  MDS *get_mds();
public:
  MDSIOContextWrapper(MDS *m, Context *c) : mds(m), fin(c) {}
  void finish(int r);
};

/**
 * No-op for callers expecting MDSInternalContextBase
 */
class C_MDSInternalNoop : public MDSInternalContextBase
{
  virtual MDS* get_mds() {assert(0);}
public:
  void finish(int r) {}
  void complete(int r) {}
};


/**
 * This class is used where you have an MDSInternalContextBase but
 * you sometimes want to call it back from an I/O completion.
 */
class C_IO_Wrapper : public MDSIOContext
{
private:
  MDSInternalContextBase *wrapped;
public:
  C_IO_Wrapper(MDS *mds_, MDSInternalContextBase *wrapped_) : MDSIOContext(mds_), wrapped(wrapped_) {
    assert(wrapped != NULL);
  }
  virtual void finish(int r) {
    wrapped->complete(r);
  }
};


/**
 * Gather needs a default-constructable class
 */
class MDSInternalContextGather : public MDSInternalContextBase
{
protected:
  MDS *get_mds();
};


class MDSGather : public C_GatherBase<MDSInternalContextBase, MDSInternalContextGather>
{
public:
  MDSGather(CephContext *cct, MDSInternalContextBase *onfinish) : C_GatherBase<MDSInternalContextBase, MDSInternalContextGather>(cct, onfinish) {}
protected:
  virtual MDS *get_mds() {return NULL;}
};


typedef C_GatherBuilderBase<MDSInternalContextBase, MDSGather> MDSGatherBuilder;

#endif  // MDS_CONTEXT_H

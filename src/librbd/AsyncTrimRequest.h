// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
#ifndef CEPH_LIBRBD_ASYNC_TRIM_REQUEST_H
#define CEPH_LIBRBD_ASYNC_TRIM_REQUEST_H

#include "librbd/AsyncRequest.h"

namespace librbd
{

class ImageCtx;
class ProgressContext;

class AsyncTrimRequest : public AsyncRequest
{
public:
  AsyncTrimRequest(ImageCtx &image_ctx, Context *on_finish,
		   uint64_t original_size, uint64_t new_size,
		   ProgressContext &prog_ctx);

  virtual void send();

protected:
  /**
   * Trim goes through the following state machine to remove whole objects,
   * clean partially trimmed objects, and update the object map:
   *
   *     <start> . . . . > STATE_FINISHED . . . . . . . . .
   *      |   .                                           .
   *      |   . . . . . . . . . . . .                     .
   *      |                         .                     .
   *      v                         v                     .
   * STATE_PRE_REMOVE ---> STATE_REMOVE_OBJECTS           .
   *                                |   .   .             .
   *        /-----------------------/   .   . . . . . .   .
   *        |                           .             .   .
   *        v                           v             v   v
   * STATE_POST_REMOVE --> STATE_CLEAN_BOUNDARY ---> <finish>
   *        .                                           ^
   *        .                                           .
   *        . . . . . . . . . . . . . . . . . . . . . . .
   *
   * The _PRE_REMOVE/_POST_REMOVE states are skipped if the object map
   * isn't enabled. The _REMOVE_OBJECTS state is skipped if no whole objects
   * are removed.  The _CLEAN_BOUNDARY state is skipped if no boundary
   * objects are cleaned.  The state machine will immediately transition
   * to _FINISHED state if there are no bytes to trim.
   */ 

  enum State {
    STATE_PRE_REMOVE,
    STATE_REMOVE_OBJECTS,
    STATE_POST_REMOVE,
    STATE_CLEAN_BOUNDARY,
    STATE_FINISHED
  };

  virtual bool should_complete(int r);

  State m_state;

private:
  uint64_t m_delete_start;
  uint64_t m_num_objects;
  uint64_t m_delete_off;
  uint64_t m_new_size;
  ProgressContext &m_prog_ctx;

  void send_remove_objects();
  void send_pre_remove();
  bool send_post_remove();
  bool send_clean_boundary();
};

} // namespace librbd

#endif // CEPH_LIBRBD_ASYNC_TRIM_REQUEST_H

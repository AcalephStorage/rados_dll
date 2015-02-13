// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
#ifndef CEPH_LIBRBD_ASYNC_RESIZE_REQUEST_H
#define CEPH_LIBRBD_ASYNC_RESIZE_REQUEST_H

#include "librbd/AsyncRequest.h"

namespace librbd
{

class ImageCtx;
class ProgressContext;

class AsyncResizeRequest : public AsyncRequest
{
public:
  AsyncResizeRequest(ImageCtx &image_ctx, Context *on_finish,
		     uint64_t original_size, uint64_t new_size,
		     ProgressContext &prog_ctx)
    : AsyncRequest(image_ctx, on_finish),
      m_original_size(original_size), m_new_size(new_size),
      m_prog_ctx(prog_ctx), m_original_parent_overlap(0),
      m_new_parent_overlap(0)
  {
  }

  virtual void send();

protected:
  /**
   * Resize goes through the following state machine to resize the image
   * and update the object map:
   *
   * <start> ----> STATE_FINISHED --------------------------------\
   *  |  .                                                        |
   *  |  . . . . . . . . . . . . . . . . . .                      |
   *  |                                    .                      |
   *  |                                    v                      |
   *  |---> STATE_GROW_OBJECT_MAP ---> STATE_UPDATE_HEADER -------|
   *  |                                                           |
   *  |                                                           |
   *  \---> STATE_TRIM_IMAGE --------> STATE_UPDATE_HEADER . . .  |
   *                                       |                   .  |
   *                                       |                   .  |
   *                                       v                   v  v
   *                            STATE_SHRINK_OBJECT_MAP ---> <finish>
   *
   * The _OBJECT_MAP states are skipped if the object map isn't enabled.
   * The state machine will immediately transition to _FINISHED if there
   * are no objects to trim.
   */ 
  enum State {
    STATE_TRIM_IMAGE,
    STATE_GROW_OBJECT_MAP,
    STATE_UPDATE_HEADER,
    STATE_SHRINK_OBJECT_MAP,
    STATE_FINISHED
  };

  State m_state;
  uint64_t m_original_size;
  uint64_t m_new_size;
  ProgressContext &m_prog_ctx;
  uint64_t m_original_parent_overlap;
  uint64_t m_new_parent_overlap;

  virtual bool should_complete(int r);

  void send_trim_image();
  void send_grow_object_map();
  bool send_shrink_object_map();
  void send_update_header();

};

} // namespace librbd

#endif // CEPH_LIBRBD_ASYNC_RESIZE_REQUEST_H

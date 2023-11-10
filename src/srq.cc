#include "rdmapp/srq.h"

#include <cstring>
#include <memory>

#include <infiniband/verbs.h>

#include "rdmapp/device.h"
#include "rdmapp/error.h"


namespace rdmapp {

srq::srq(pd* pd, size_t max_wr) : srq_(nullptr), pd_(pd) {
  struct ibv_srq_init_attr srq_init_attr;
  srq_init_attr.srq_context = this;
  srq_init_attr.attr.max_sge = 1;
  srq_init_attr.attr.max_wr = max_wr;
  srq_init_attr.attr.srq_limit = max_wr;

  srq_ = ::ibv_create_srq(pd_->pd_, &srq_init_attr);
  check_ptr(srq_, "failed to create srq");
  
}

srq::~srq() {
  if (srq_ == nullptr) [[unlikely]] {
    return;
  }

  if (auto rc = ::ibv_destroy_srq(srq_); rc != 0) [[unlikely]] {
    
  } else {
    
  }
}

} // namespace rdmapp
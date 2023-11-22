#pragma once

#include <infiniband/verbs.h>

namespace rdmapp {

void process_wc(struct ibv_wc const &wc);

}
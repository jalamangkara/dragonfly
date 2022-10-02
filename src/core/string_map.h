// Copyright 2022, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//

#pragma once

#include "core/dense_set.h"

extern "C" {
#include "redis/sds.h"
}

namespace dfly {

class StringMap : public DenseSet {
 public:
  StringMap(std::pmr::memory_resource* res = std::pmr::get_default_resource()) : DenseSet(res) {
  }

  ~StringMap() {
    Clear();
  }

  bool AddOrSet(std::string_view field, std::string_view value, uint32_t ttl_sec = UINT32_MAX);

  bool Erase(std::string_view s1);

  bool Contains(std::string_view s1) const;

  void Clear();
};

}  // namespace dfly

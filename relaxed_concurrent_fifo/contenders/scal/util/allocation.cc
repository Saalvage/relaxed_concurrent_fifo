// Copyright (c) 2014, the Scal Project Authors.  All rights reserved.
// Please see the AUTHORS file for details.  Use of this source code is governed
// by a BSD license that can be found in the LICENSE file.

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "allocation.h"
#pragma GCC diagnostic pop

#include <stdio.h>
#include <string.h>

namespace scal {

pthread_once_t tla_key_once = PTHREAD_ONCE_INIT;

pthread_key_t ThreadLocalAllocator::tla_key;


size_t HumanSizeToPages(const char* hsize, size_t len) {
  if (hsize[len] != 0) {
    fprintf(stderr, "%s: epected valid c string\n", __func__);
    abort();
  }
  size_t multiplier;
  switch (hsize[len-1]) {
  case 'k':
  case 'K':
    multiplier = 1024;
    break;
  case 'm':
  case 'M':
    multiplier = 1024 * 1024;
    break;
  case 'g':
  case 'G':
    multiplier = 1024 * 1024 * 1024;
    break;
  default:
    multiplier = 1;
  }
  const size_t val = strtol(hsize, NULL, 10);
  if (val == 0) {
    fprintf(stderr, "%s: strtol() returned 0; do not try to set the "
                    "prealloc_size to 0\n", __func__);
    abort();
  }
  const size_t bytes = val * multiplier;
  return ((bytes % kPageSize) == 0) ? bytes/kPageSize : bytes/kPageSize + 1;
}

}  // namespace scal


void* tla_malloc(size_t size) {
  return scal::ThreadLocalAllocator::Get().Malloc(size);
}


void* tla_malloc_aligned(size_t size, size_t alignment) {
  return scal::ThreadLocalAllocator::Get().MallocAligned(size, alignment);
}

// -*- mode: c++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// Copyright 2017 University of Massachusetts, Amherst

#include <sched.h>
#include <stdint.h>
#include <stdlib.h>

#include <atomic>
#include <mutex>
#include <thread>

#include "gtest/gtest.h"

#include "internal.h"
#include "runtime.h"

using namespace std;
using namespace mesh;

static constexpr size_t StrLen = 128;
static constexpr size_t ObjCount = 32;

static char *s1;
static char *s2;

static atomic<int> ShouldExit;
static atomic<int> ShouldContinueTest;

// we need to wrap pthread_create so that we can safely implement a
// stop-the-world quiescent period for the copy/mremap phase of
// meshing -- copied from libmesh.cc
int pthread_create(pthread_t *thread, const pthread_attr_t *attr, mesh::PthreadFn startRoutine, void *arg) {
  return mesh::runtime().createThread(thread, attr, startRoutine, arg);
}

static void writerThread() {
  ShouldContinueTest = 1;

  for (size_t i = 1; i < numeric_limits<uint64_t>::max(); i++) {
    if (i % 1000000 == 0 && ShouldExit)
      return;

    s1[0] = 'A';
    s2[0] = 'Z';
  }

  debug("loop ended before ShouldExit\n");
}

// shows up in strace logs, but otherwise does nothing
static inline void note(const char *note) {
  (void)write(-1, note, strlen(note));
}

static void meshTestConcurrentWrite(bool invert) {
  MWC prng(internal::seed(), internal::seed());
  GlobalHeap &gheap = runtime().heap();

  ASSERT_EQ(gheap.getAllocatedMiniheapCount(), 0);

  Freelist f1;
  Freelist f2;

  // allocate two miniheaps for the same object size from our global heap
  MiniHeap *mh1 = gheap.allocMiniheap(StrLen);
  MiniHeap *mh2 = gheap.allocMiniheap(StrLen);
  mh1->reattach(f1, prng);
  mh2->reattach(f2, prng);

  ASSERT_EQ(gheap.getAllocatedMiniheapCount(), 2);

  // sanity checks
  ASSERT_TRUE(mh1 != mh2);
  ASSERT_EQ(mh1->maxCount(), mh2->maxCount());
  ASSERT_EQ(mh1->maxCount(), ObjCount);

  // allocate two c strings, one from each miniheap at different offsets
  s1 = reinterpret_cast<char *>(mh1->mallocAt(0));
  s2 = reinterpret_cast<char *>(mh2->mallocAt(ObjCount - 1));

  ASSERT_TRUE(s1 != nullptr);
  ASSERT_TRUE(s2 != nullptr);

  mh1->freeEntireFreelistExcept(f1, s1);
  mh2->freeEntireFreelistExcept(f2, s2);

  // fill in the strings, set the trailing null byte
  memset(s1, 'A', StrLen);
  memset(s2, 'Z', StrLen);
  s1[StrLen - 1] = 0;
  s2[StrLen - 1] = 0;

  // copy these strings so we can check the contents after meshing
  char *v1 = strdup(s1);
  char *v2 = strdup(s2);
  ASSERT_TRUE(strcmp(s1, v1) == 0);
  ASSERT_TRUE(strcmp(s2, v2) == 0);

  ASSERT_EQ(mh1->inUseCount(), 1);
  ASSERT_EQ(mh2->inUseCount(), 1);

  if (invert) {
    MiniHeap *tmp = mh1;
    mh1 = mh2;
    mh2 = tmp;
  }

  thread writer(writerThread);

  while (ShouldContinueTest != 1)
    sched_yield();

  const auto bitmap1 = mh1->bitmap().bitmap();
  const auto bitmap2 = mh2->bitmap().bitmap();
  const auto len = mh1->bitmap().byteCount();
  ASSERT_EQ(len, mh2->bitmap().byteCount());

  ASSERT_TRUE(mesh::bitmapsMeshable(bitmap1, bitmap2, len));

  note("ABOUT TO MESH");
  // mesh the two miniheaps together
  gheap.meshLocked(mh1, mh2);
  note("DONE MESHING");

  // mh2 is consumed by mesh call, ensure it is now a null pointer
  ASSERT_EQ(mh2, nullptr);

  // ensure the count of set bits looks right
  ASSERT_EQ(mh1->inUseCount(), 2);

  // check that our two allocated objects still look right
  ASSERT_TRUE(strcmp(s1, v1) == 0);
  ASSERT_TRUE(strcmp(s2, v2) == 0);

  // get an aliased pointer to the second string by pointer arithmatic
  // on the first string
  char *s3 = s1 + (ObjCount - 1) * StrLen;
  ASSERT_TRUE(strcmp(s2, s3) == 0);

  ShouldExit = 1;
  writer.join();

  // modify the second string, ensure the modification shows up on
  // string 3 (would fail if the two miniheaps weren't meshed)
  s2[0] = 'b';
  ASSERT_EQ(s3[0], 'b');

  // now free the objects by going through the global heap -- it
  // should redirect both objects to the same miniheap
  gheap.free(s1);
  ASSERT_TRUE(!mh1->isEmpty());
  gheap.free(s2);
  ASSERT_TRUE(mh1->isEmpty());  // safe because mh1 isn't "done"

  note("ABOUT TO FREE");
  gheap.freeMiniheap(mh1);

  ASSERT_EQ(gheap.getAllocatedMiniheapCount(), 0);
}

TEST(ConcurrentMeshTest, TryMesh) {
  meshTestConcurrentWrite(false);
}

TEST(ConcurrentMeshTest, TryMeshInverse) {
  meshTestConcurrentWrite(true);
}

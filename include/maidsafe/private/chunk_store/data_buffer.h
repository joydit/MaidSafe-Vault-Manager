/* Copyright (c) 2012 maidsafe.net limited
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
    this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice,
    this list of conditions and the following disclaimer in the documentation
    and/or other materials provided with the distribution.
    * Neither the name of the maidsafe.net limited nor the names of its
    contributors may be used to endorse or promote products derived from this
    software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef MAIDSAFE_PRIVATE_CHUNK_STORE_DATA_BUFFER_H_
#define MAIDSAFE_PRIVATE_CHUNK_STORE_DATA_BUFFER_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <utility>
#include <deque>

#include "boost/filesystem/path.hpp"
#include "boost/variant.hpp"

#include "maidsafe/common/tagged_value.h"
#include "maidsafe/common/types.h"
#include "maidsafe/passport/detail/config.h"


namespace maidsafe {

namespace priv {

namespace chunk_store {

namespace test { class DataBufferTest; }

class DataBuffer {
 public:
  typedef boost::variant<TaggedValue<Identity, passport::detail::AnmidTag>,
                         TaggedValue<Identity, passport::detail::AnsmidTag>,
                         TaggedValue<Identity, passport::detail::AntmidTag>,
                         TaggedValue<Identity, passport::detail::AnmaidTag>,
                         TaggedValue<Identity, passport::detail::MaidTag>,
                         TaggedValue<Identity, passport::detail::PmidTag>,
                         TaggedValue<Identity, passport::detail::MidTag>,
                         TaggedValue<Identity, passport::detail::SmidTag>,
                         TaggedValue<Identity, passport::detail::TmidTag>,
                         TaggedValue<Identity, passport::detail::AnmpidTag>,
                         TaggedValue<Identity, passport::detail::MpidTag> > VariantType;

  typedef std::function<void(const Identity&, const NonEmptyString&)> PopFunctor;
  // Throws if max_memory_usage >= max_disk_usage.  Throws if a writable folder can't be created in
  // temp_directory_path().  Starts a background worker thread which copies values from memory to
  // disk.  If pop_functor is valid, the disk cache will pop excess items when it is full,
  // otherwise Store will block until there is space made via Delete calls.
  DataBuffer(MemoryUsage max_memory_usage, DiskUsage max_disk_usage, PopFunctor pop_functor);
  // Throws if max_memory_usage >= max_disk_usage.  Throws if a writable folder can't be created in
  // "disk_buffer".  Starts a background worker thread which copies values from memory to disk.  If
  // pop_functor is valid, the disk cache will pop excess items when it is full, otherwise Store
  // will block until there is space made via Delete calls.
  DataBuffer(MemoryUsage max_memory_usage,
                 DiskUsage max_disk_usage,
                 PopFunctor pop_functor,
                 const boost::filesystem::path& disk_buffer);
  ~DataBuffer();
  // Throws if the background worker has thrown (e.g. the disk has become inaccessible).  Throws if
  // the size of value is greater than the current specified maximum disk usage, or if the value
  // can't be written to disk (e.g. value is not initialised).  If there is not enough space to
  // store to memory, blocks until there is enough space to store to disk.  Space will be made
  // available via external calls to Delete, and also automatically if pop_functor_ is not NULL.
  void Store(const Identity& key, const NonEmptyString& value);
  // Throws if the background worker has thrown (e.g. the disk has become inaccessible).  Throws if
  // the value can't be read from disk.  If the value isn't in memory and has started to be stored
  // to disk, blocks while waiting for the storing to complete.
  NonEmptyString Get(const Identity& key);
  // Throws if the background worker has thrown (e.g. the disk has become inaccessible).  Throws if
  // the value was written to disk and can't be removed.
  void Delete(const Identity& key);
  // Throws if max_memory_usage > max_disk_usage_.
  void SetMaxMemoryUsage(MemoryUsage max_memory_usage);
  // Throws if max_memory_usage_ > max_disk_usage.
  void SetMaxDiskUsage(DiskUsage max_disk_usage);

  friend class test::DataBufferTest;

 private:
  DataBuffer(const DataBuffer&);
  DataBuffer& operator=(const DataBuffer&);

  template<typename UsageType, typename IndexType>
  struct Storage {
    typedef IndexType index_type;
    explicit Storage(UsageType max_in) : max(max_in), current(0), index(), mutex(), cond_var() {}  // NOLINT (Fraser)
    UsageType max, current;
    IndexType index;
    std::mutex mutex;
    std::condition_variable cond_var;
  };

  enum class StoringState { kNotStarted, kStarted, kCancelled, kCompleted };

  template<typename DataType>
  struct MemoryElement {
    MemoryElement(const DataType& key_in, const NonEmptyString& value_in)
        : key(key_in), value(value_in), also_on_disk(StoringState::kNotStarted) {}
    DataType key;
    NonEmptyString value;
    StoringState also_on_disk;
  };

  typedef std::deque<MemoryElement<VariantType> > MemoryIndex;

  template<typename DataType>
  struct DiskElement {
    explicit DiskElement(const DataType& key_in) : key(key_in), state(StoringState::kStarted) {}
    DataType key;
    StoringState state;
  };
  typedef std::deque<DiskElement<VariantType> > DiskIndex;

  void Init();
  bool StoreInMemory(const Identity& key, const NonEmptyString& value);
  void WaitForSpaceInMemory(const uint64_t& required_space,
                            std::unique_lock<std::mutex>& memory_store_lock);
  void StoreOnDisk(const Identity& key, const NonEmptyString& value);
  void WaitForSpaceOnDisk(const Identity& key,
                          const uint64_t& required_space,
                          std::unique_lock<std::mutex>& disk_store_lock,
                          bool& cancelled);
  void DeleteFromMemory(const Identity& key, StoringState& also_on_disk);
  void DeleteFromDisk(const Identity& key);
  void RemoveFile(const Identity& key, NonEmptyString* value);
  void CopyQueueToDisk();
  void CheckWorkerIsStillRunning();
  void StopRunning();
  boost::filesystem::path GetFilename(const Identity& key) const;
  template<typename T>
  bool HasSpace(const T& store, const uint64_t& required_space);
  template<typename T>
  typename T::index_type::iterator Find(T& store, const Identity& key);
  MemoryIndex::iterator FindOldestInMemoryOnly();
  MemoryIndex::iterator FindMemoryRemovalCandidate(const uint64_t& required_space,
                                                   std::unique_lock<std::mutex>& memory_store_lock);
  DiskIndex::iterator FindStartedToStoreOnDisk(const Identity& key);
  DiskIndex::iterator FindOldestOnDisk();
  DiskIndex::iterator FindAndThrowIfCancelled(const Identity& key);

  Storage<MemoryUsage, MemoryIndex> memory_store_;
  Storage<DiskUsage, DiskIndex> disk_store_;
  const PopFunctor kPopFunctor_;
  const boost::filesystem::path kDiskBuffer_;
  const bool kShouldRemoveRoot_;
  std::atomic<bool> running_;
  std::future<void> worker_;
};

}  // namespace chunks_store

}  // namespace priv

}  // namespace maidsafe


#endif  // MAIDSAFE_PRIVATE_CHUNK_STORE_DATA_BUFFER_H_
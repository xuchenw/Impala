// Copyright 2012 Cloudera Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "runtime/runtime-state.h"
#include "runtime/mem-tracker.h"
#include "runtime/mem-pool.h"
#include "runtime/buffered-block-mgr.h"
#include "runtime/tmp-file-mgr.h"
#include "util/runtime-profile.h"
#include "util/disk-info.h"
#include "util/filesystem-util.h"
#include "util/impalad-metrics.h"
#include "util/uid-util.h"

#include <openssl/rand.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/err.h>

#include <gutil/strings/substitute.h>

using namespace boost;
using namespace strings;   // for Substitute

DEFINE_bool(disk_spill_encryption, false, "Set this to encrypt and perform an integrity "
    "check on all data spilled to disk during a query");

using namespace std;
using namespace strings;

namespace impala {

BufferedBlockMgr::BlockMgrsMap BufferedBlockMgr::query_to_block_mgrs_;
SpinLock BufferedBlockMgr::static_block_mgrs_lock_;

struct BufferedBlockMgr::Client {
  Client(BufferedBlockMgr* mgr, int num_reserved_buffers, MemTracker* tracker,
      RuntimeState* state)
    : mgr_(mgr),
      state_(state),
      tracker_(tracker),
      query_tracker_(mgr_->mem_tracker_->parent()),
      num_reserved_buffers_(num_reserved_buffers),
      num_tmp_reserved_buffers_(0),
      num_pinned_buffers_(0) {
  }

  // Unowned.
  BufferedBlockMgr* mgr_;

  // Unowned.
  RuntimeState* state_;

  // Tracker for this client. Can be NULL. Unowned.
  // If this is set, when the client gets a buffer, we update the consumption on this
  // tracker. However, we don't want to transfer the buffer from the block mgr to the
  // client since (i.e. release from the block mgr), since the block mgr is where the
  // block mem usage limit is enforced. Even when we give a buffer to a client, the
  // buffer is still owned and counts against the block mgr tracker (i.e. there is a
  // fixed pool of buffers regardless of if they are in the block mgr or the clients).
  MemTracker* tracker_;

  // This is the common ancestor between the block mgr tracker and the client tracker.
  // When memory is transferred to the client, we want it to stop at this tracker.
  MemTracker* query_tracker_;

  // Number of buffers reserved by this client.
  int num_reserved_buffers_;

  // Number of buffers temporarily reserved.
  int num_tmp_reserved_buffers_;

  // Number of buffers pinned by this client.
  int num_pinned_buffers_;

  void PinBuffer(BufferDescriptor* buffer) {
    DCHECK_NOTNULL(buffer);
    if (buffer->len == mgr_->max_block_size()) {
      ++num_pinned_buffers_;
      if (tracker_ != NULL) tracker_->ConsumeLocal(buffer->len, query_tracker_);
    }
  }

  void UnpinBuffer(BufferDescriptor* buffer) {
    DCHECK_NOTNULL(buffer);
    if (buffer->len == mgr_->max_block_size()) {
      DCHECK_GT(num_pinned_buffers_, 0);
      --num_pinned_buffers_;
      if (tracker_ != NULL) tracker_->ReleaseLocal(buffer->len, query_tracker_);
    }
  }

  string DebugString() const {
    stringstream ss;
    ss << "Client " << this << endl
       << "  num_reserved_buffers=" << num_reserved_buffers_ << endl
       << "  num_tmp_reserved_buffers=" << num_tmp_reserved_buffers_ << endl
       << "  num_pinned_buffers=" << num_pinned_buffers_;
    return ss.str();
  }
};

// BufferedBlockMgr::Block methods.
BufferedBlockMgr::Block::Block(BufferedBlockMgr* block_mgr)
  : buffer_desc_(NULL),
    block_mgr_(block_mgr),
    client_(NULL),
    write_range_(NULL),
    valid_data_len_(0),
    num_rows_(0) {
}

Status BufferedBlockMgr::Block::Pin(bool* pinned, Block* release_block, bool unpin) {
  RETURN_IF_ERROR(block_mgr_->PinBlock(this, pinned, release_block, unpin));
  DCHECK(Validate()) << endl << DebugString();
  return Status::OK;
}

Status BufferedBlockMgr::Block::Unpin() {
  RETURN_IF_ERROR(block_mgr_->UnpinBlock(this));
  DCHECK(Validate()) << endl << DebugString();
  return Status::OK;
}

Status BufferedBlockMgr::Block::Delete() {
  RETURN_IF_ERROR(block_mgr_->DeleteBlock(this));
  DCHECK(Validate()) << endl << DebugString();
  return Status::OK;
}

void BufferedBlockMgr::Block::Init() {
  // No locks are taken because the block is new or has previously been deleted.
  is_pinned_ = false;
  in_write_ = false;
  is_deleted_ = false;
  valid_data_len_ = 0;
  client_ = NULL;
  num_rows_ = 0;
}

bool BufferedBlockMgr::Block::Validate() const {
  if (is_deleted_ && (is_pinned_ || (!in_write_ && buffer_desc_ != NULL))) {
    LOG(ERROR) << "Deleted block in use - " << DebugString();
    return false;
  }

  if (buffer_desc_ == NULL && (is_pinned_ || in_write_)) {
    LOG(ERROR) << "Block without buffer in use - " << DebugString();
    return false;
  }

  if (buffer_desc_ == NULL && block_mgr_->unpinned_blocks_.Contains(this)) {
    LOG(ERROR) << "Unpersisted block without buffer - " << DebugString();
    return false;
  }

  if (buffer_desc_ != NULL && (buffer_desc_->block != this)) {
    LOG(ERROR) << "Block buffer inconsistency - " << DebugString();
    return false;
  }

  return true;
}

string BufferedBlockMgr::Block::DebugString() const {
  stringstream ss;
  ss << "Block: " << this << endl
     << "  Buffer Desc: " << buffer_desc_ << endl
     << "  Data Len: " << valid_data_len_ << endl
     << "  Num Rows: " << num_rows_ << endl;
  if (is_pinned_) ss << "  Buffer Len: " << buffer_len() << endl;
  ss << "  Deleted: " << is_deleted_ << endl
     << "  Pinned: " << is_pinned_ << endl
     << "  Write Issued: " << in_write_ << endl
     << "  Client Local: " << client_local_;
  return ss.str();
}

BufferedBlockMgr::BufferedBlockMgr(RuntimeState* state, int64_t block_size)
  : max_block_size_(block_size),
    // Keep two writes in flight per scratch disk so the disks can stay busy.
    block_write_threshold_(TmpFileMgr::num_tmp_devices() * 2),
    disable_spill_(state->query_ctx().disable_spilling),
    query_id_(state->query_id()),
    initialized_(false),
    unfullfilled_reserved_buffers_(0),
    total_pinned_buffers_(0),
    num_outstanding_writes_(0),
    io_mgr_(state->io_mgr()),
    is_cancelled_(0),
    writes_issued_(0),
    encryption_(FLAGS_disk_spill_encryption),
    check_integrity_(FLAGS_disk_spill_encryption) {
}

Status BufferedBlockMgr::Create(RuntimeState* state, MemTracker* parent,
    RuntimeProfile* profile, int64_t mem_limit, int64_t block_size,
    shared_ptr<BufferedBlockMgr>* block_mgr) {
  DCHECK_NOTNULL(parent);
  block_mgr->reset();
  {
    ScopedSpinLock lock(&static_block_mgrs_lock_);
    BlockMgrsMap::iterator it = query_to_block_mgrs_.find(state->query_id());
    if (it != query_to_block_mgrs_.end()) *block_mgr = it->second.lock();
    if (*block_mgr == NULL) {
      // weak_ptr::lock returns NULL if the weak_ptr is expired. This means
      // all shared_ptr references have gone to 0 and it is in the process of
      // being deleted. This can happen if the last shared reference is released
      // but before the weak ptr is removed from the map.
      block_mgr->reset(new BufferedBlockMgr(state, block_size));
      query_to_block_mgrs_[state->query_id()] = *block_mgr;
    }
  }
  (*block_mgr)->Init(state->io_mgr(), profile, parent, mem_limit);
  return Status::OK;
}

int64_t BufferedBlockMgr::available_buffers(Client* client) const {
  int64_t unused_reserved = client->num_reserved_buffers_ +
      client->num_tmp_reserved_buffers_ - client->num_pinned_buffers_;
  return max(0L, remaining_unreserved_buffers()) + max(0L, unused_reserved);
}

int64_t BufferedBlockMgr::remaining_unreserved_buffers() const {
  int64_t num_buffers = free_io_buffers_.size() +
      unpinned_blocks_.size() + num_outstanding_writes_;
  num_buffers += mem_tracker_->SpareCapacity() / max_block_size();
  num_buffers -= unfullfilled_reserved_buffers_;
  return num_buffers;
}

Status BufferedBlockMgr::RegisterClient(int num_reserved_buffers, MemTracker* tracker,
    RuntimeState* state, Client** client) {
  lock_guard<mutex> lock(lock_);
  DCHECK_GE(num_reserved_buffers, 0);
  *client = obj_pool_.Add(new Client(this, num_reserved_buffers, tracker, state));
  unfullfilled_reserved_buffers_ += num_reserved_buffers;
  return Status::OK;
}

void BufferedBlockMgr::ClearReservations(Client* client) {
  lock_guard<mutex> lock(lock_);
  if (client->num_pinned_buffers_ < client->num_reserved_buffers_) {
    unfullfilled_reserved_buffers_ -=
        client->num_reserved_buffers_ - client->num_pinned_buffers_;
  }
  client->num_reserved_buffers_ = 0;

  unfullfilled_reserved_buffers_ -= client->num_tmp_reserved_buffers_;
  client->num_tmp_reserved_buffers_ = 0;
}

bool BufferedBlockMgr::TryAcquireTmpReservation(Client* client, int num_buffers) {
  lock_guard<mutex> lock(lock_);
  DCHECK_EQ(client->num_tmp_reserved_buffers_, 0);
  if (client->num_pinned_buffers_ < client->num_reserved_buffers_) {
    // If client has unused reserved buffers, we use those first.
    num_buffers -= client->num_reserved_buffers_ - client->num_pinned_buffers_;
  }
  if (num_buffers < 0) return true;
  if (available_buffers(client) < num_buffers) return false;

  client->num_tmp_reserved_buffers_ = num_buffers;
  unfullfilled_reserved_buffers_ += num_buffers;
  return true;
}

void BufferedBlockMgr::ClearTmpReservation(Client* client) {
  lock_guard<mutex> lock(lock_);
  unfullfilled_reserved_buffers_ -= client->num_tmp_reserved_buffers_;
  client->num_tmp_reserved_buffers_ = 0;
}

bool BufferedBlockMgr::ConsumeMemory(Client* client, int64_t size) {
  int buffers_needed = BitUtil::Ceil(size, max_block_size());

  unique_lock<mutex> lock(lock_);

  if (size < max_block_size() && mem_tracker_->TryConsume(size)) {
    // For small allocations (less than a block size), just let the allocation through.
    client->tracker_->ConsumeLocal(size, client->query_tracker_);
    return true;
  }

  if (max(0L, remaining_unreserved_buffers()) + client->num_tmp_reserved_buffers_ <
        buffers_needed) {
    return false;
  }

  if (mem_tracker_->TryConsume(size)) {
    // There was still unallocated memory, don't need to recycle allocated blocks.
    client->tracker_->ConsumeLocal(size, client->query_tracker_);
    return true;
  }

  // Bump up client->num_tmp_reserved_buffers_ to satisfy this request. We don't want
  // another client to grab the buffer.
  int additional_tmp_reservations = 0;
  if (client->num_tmp_reserved_buffers_ < buffers_needed) {
    additional_tmp_reservations = buffers_needed - client->num_tmp_reserved_buffers_;
    client->num_tmp_reserved_buffers_ += additional_tmp_reservations;
    unfullfilled_reserved_buffers_ += additional_tmp_reservations;
  }

  // Loop until we have freed enough memory.
  // We free all the memory at the end. We don't want another component to steal the
  // memory.
  int buffers_acquired = 0;
  while (buffers_acquired != buffers_needed) {
    BufferDescriptor* buffer_desc = NULL;
    FindBuffer(lock, &buffer_desc); // This waits on the lock.
    if (buffer_desc == NULL) {
      // We couldn't get a buffer. This can happen if another query came and
      // allocated memory. Undo the reservation.
      if (buffers_acquired < additional_tmp_reservations) {
        client->num_tmp_reserved_buffers_ -=
            (additional_tmp_reservations - buffers_acquired);
        unfullfilled_reserved_buffers_ -=
            (additional_tmp_reservations - buffers_acquired);
      }
      mem_tracker_->Release(buffers_acquired * max_block_size());
      return false;
    }
    all_io_buffers_.erase(buffer_desc->all_buffers_it);
    if (buffer_desc->block != NULL) buffer_desc->block->buffer_desc_ = NULL;
    delete[] buffer_desc->buffer;
    ++buffers_acquired;
  }
  WriteUnpinnedBlocks();

  client->num_tmp_reserved_buffers_ -= buffers_acquired;
  unfullfilled_reserved_buffers_ -= buffers_acquired;

  DCHECK_GE(buffers_acquired * max_block_size(), size);
  mem_tracker_->Release(buffers_acquired * max_block_size());
  if (!mem_tracker_->TryConsume(size)) return false;
  client->tracker_->ConsumeLocal(size, client->query_tracker_);
  DCHECK(Validate()) << endl << DebugInternal();
  return true;
}

void BufferedBlockMgr::ReleaseMemory(Client* client, int64_t size) {
  mem_tracker_->Release(size);
  client->tracker_->ReleaseLocal(size, client->query_tracker_);
}

void BufferedBlockMgr::Cancel() {
  if (is_cancelled_.Swap(1) == 1) return;
  // Cancel to the underlying io mgr to unblock any waiting threads.
  io_mgr_->CancelContext(io_request_context_);
}

Status BufferedBlockMgr::MemLimitTooLowError(Client* client) {
  // TODO: what to print here. We can't know the value of the entire query here.
  Status status = Status::MEM_LIMIT_EXCEEDED;
  status.AddErrorMsg(Substitute("The memory limit is set too low initialize the"
      " spilling operator. The minimum required memory to spill this operator is $0.",
      PrettyPrinter::Print(client->num_reserved_buffers_ * max_block_size(),
          TUnit::BYTES)));
  VLOG_QUERY << "Query: " << query_id_ << " ran out of memory: " << endl
             << DebugInternal() << endl << client->DebugString() << endl
             << GetStackTrace();
  return status;
}

Status BufferedBlockMgr::GetNewBlock(Client* client, Block* unpin_block, Block** block,
    int64_t len) {
  DCHECK_LE(len, max_block_size_) << "Cannot request blocks bigger than max_len";

  *block = NULL;
  if (is_cancelled_.Read() == 1) return Status::CANCELLED;
  Block* new_block = NULL;
  {
    lock_guard<mutex> lock(lock_);
    new_block = GetUnusedBlock(client);

    DCHECK_NOTNULL(new_block);
    DCHECK(new_block->client_ == client);

    if (len >= 0 && len < max_block_size_) {
      DCHECK(unpin_block == NULL);
      if (client->tracker_->TryConsume(len)) {
        uint8_t* buffer = new uint8_t[len];
        new_block->buffer_desc_ = obj_pool_.Add(new BufferDescriptor(buffer, len));
        new_block->buffer_desc_->block = new_block;
        new_block->is_pinned_ = true;
        client->PinBuffer(new_block->buffer_desc_);
        ++total_pinned_buffers_;
        *block = new_block;
        return Status::OK;
      } else {
        new_block->is_deleted_ = true;
        ReturnUnusedBlock(new_block);
        return Status::OK;
      }
    }
  }

  bool in_mem;
  RETURN_IF_ERROR(FindBufferForBlock(new_block, &in_mem));
  DCHECK(!in_mem) << "A new block cannot start in mem.";

  if (!new_block->is_pinned()) {
    if (unpin_block == NULL) {
      // We couldn't get a new block and no unpin block was provided. Can't return
      // a block.
      new_block->is_deleted_ = true;
      ReturnUnusedBlock(new_block);
      new_block = NULL;
    } else {
      // We need to transfer the buffer from unpin_block to new_block.
      RETURN_IF_ERROR(TransferBuffer(new_block, unpin_block, true));
    }
  } else if (unpin_block != NULL) {
    // Got a new block without needing to transfer. Just unpin this block.
    RETURN_IF_ERROR(unpin_block->Unpin());
  }

  if (new_block != NULL) DCHECK(new_block->is_pinned());
  *block = new_block;
  return Status::OK;
}

Status BufferedBlockMgr::TransferBuffer(Block* dst, Block* src, bool unpin) {
  // First write out the src block.
  DCHECK(src->is_pinned_);
  DCHECK(!dst->is_pinned_);
  DCHECK(dst->buffer_desc_ == NULL);
  DCHECK_EQ(src->buffer_desc_->len, max_block_size_);
  src->is_pinned_ = false;

  if (unpin) {
    unique_lock<mutex> lock(lock_);
    src->client_local_ = true;
    RETURN_IF_ERROR(WriteUnpinnedBlock(src));
    // Wait for the write to complete.
    while (src->in_write_ && is_cancelled_.Read() == 0) {
      src->write_complete_cv_.wait(lock);
    }
    if (is_cancelled_.Read() == 1) return Status::CANCELLED;
    DCHECK(!src->in_write_);
  }

  // Assign the buffer to the new block.
  dst->buffer_desc_ = src->buffer_desc_;
  dst->buffer_desc_->block = dst;
  src->buffer_desc_ = NULL;
  dst->is_pinned_ = true;
  if (!unpin) {
    src->is_deleted_ = true;
    ReturnUnusedBlock(src);
  }

  return Status::OK;
}

BufferedBlockMgr::~BufferedBlockMgr() {
  {
    ScopedSpinLock lock(&static_block_mgrs_lock_);
    DCHECK(query_to_block_mgrs_.find(query_id_) != query_to_block_mgrs_.end());
    query_to_block_mgrs_.erase(query_id_);
  }

  if (io_request_context_ != NULL) {
    io_mgr_->UnregisterContext(io_request_context_);
  }

  // Grab this lock to synchronize with io threads in WriteComplete(). We need those
  // to finish to ensure that memory buffers remain valid for any in-progress writes.
  lock_guard<mutex> lock(lock_);
  // Delete tmp files.
  BOOST_FOREACH(TmpFileMgr::File& file, tmp_files_) {
    file.Remove();
  }
  tmp_files_.clear();

  // Free memory resources.
  BOOST_FOREACH(BufferDescriptor* buffer, all_io_buffers_) {
    mem_tracker_->Release(buffer->len);
    delete[] buffer->buffer;
  }
  DCHECK_EQ(mem_tracker_->consumption(), 0);
  mem_tracker_->UnregisterFromParent();
  mem_tracker_.reset();
}

int64_t BufferedBlockMgr::bytes_allocated() const {
  return mem_tracker_->consumption();
}

int BufferedBlockMgr::num_pinned_buffers(Client* client) const {
  return client->num_pinned_buffers_;
}

int BufferedBlockMgr::num_reserved_buffers_remaining(Client* client) const {
  return max(client->num_reserved_buffers_ - client->num_pinned_buffers_, 0);
}

MemTracker* BufferedBlockMgr::get_tracker(Client* client) const {
  return client->tracker_;
}

static Status DeleteOrUnpin(BufferedBlockMgr::Block* block, bool unpin) {
  if (block == NULL) return Status::OK;
  return unpin ? block->Unpin() : block->Delete();
}

Status BufferedBlockMgr::PinBlock(Block* block, bool* pinned, Block* release_block,
    bool unpin) {
  DCHECK(!block->is_deleted_);
  *pinned = false;
  if (is_cancelled_.Read() == 1) return Status::CANCELLED;

  if (block->is_pinned_) {
    *pinned = true;
    return DeleteOrUnpin(release_block, unpin);
  }

  bool in_mem = false;
  RETURN_IF_ERROR(FindBufferForBlock(block, &in_mem));
  *pinned = block->is_pinned_;

  // Block was not evicted or had no data, nothing left to do.
  if (in_mem || block->valid_data_len_ == 0) return DeleteOrUnpin(release_block, unpin);

  if (!block->is_pinned_) {
    if (release_block == NULL) return Status::OK;

    if (block->buffer_desc_ != NULL) {
      if (free_io_buffers_.Contains(block->buffer_desc_)) {
        free_io_buffers_.Remove(block->buffer_desc_);
      } else if (unpinned_blocks_.Contains(block)) {
        unpinned_blocks_.Remove(block);
      } else {
        DCHECK(block->in_write_);
      }
      block->is_pinned_ = true;
      *pinned = true;
      block->client_->PinBuffer(block->buffer_desc_);
      ++total_pinned_buffers_;
      RETURN_IF_ERROR(WriteUnpinnedBlocks());
      return DeleteOrUnpin(release_block, unpin);
    }

    RETURN_IF_ERROR(TransferBuffer(block, release_block, unpin));
    DCHECK(!release_block->is_pinned_);
    release_block = NULL; // Handled by transfer.
    DCHECK(block->is_pinned_);
    *pinned = true;
  }

  // Read the block from disk if it was not in memory.
  DCHECK(block->write_range_ != NULL) << block->DebugString() << endl << release_block;
  SCOPED_TIMER(disk_read_timer_);
  // Create a ScanRange to perform the read.
  DiskIoMgr::ScanRange* scan_range =
      obj_pool_.Add(new DiskIoMgr::ScanRange());
  scan_range->Reset(NULL, block->write_range_->file(), block->write_range_->len(),
      block->write_range_->offset(), block->write_range_->disk_id(), false, block);
  vector<DiskIoMgr::ScanRange*> ranges(1, scan_range);
  RETURN_IF_ERROR(io_mgr_->AddScanRanges(io_request_context_, ranges, true));

  // Read from the io mgr buffer into the block's assigned buffer.
  int64_t offset = 0;
  DiskIoMgr::BufferDescriptor* io_mgr_buffer;
  do {
    RETURN_IF_ERROR(scan_range->GetNext(&io_mgr_buffer));
    memcpy(block->buffer() + offset, io_mgr_buffer->buffer(), io_mgr_buffer->len());
    offset += io_mgr_buffer->len();
    io_mgr_buffer->Return();
  } while (!io_mgr_buffer->eosr());
  DCHECK_EQ(offset, block->write_range_->len());

  // Verify integrity first, because the hash was generated from encrypted data
  if (check_integrity_) RETURN_IF_ERROR(VerifyHash(block));

  // Decryption is done in-place, since the buffer can't be accessed by anyone else
  if (encryption_) RETURN_IF_ERROR(Decrypt(block));

  return DeleteOrUnpin(release_block, unpin);
}

Status BufferedBlockMgr::UnpinBlock(Block* block) {
  if (is_cancelled_.Read() == 1) return Status::CANCELLED;
  DCHECK(!block->is_deleted_) << "Unpin for deleted block.";

  lock_guard<mutex> unpinned_lock(lock_);
  if (!block->is_pinned_) return Status::OK;
  DCHECK(Validate()) << endl << DebugInternal();
  DCHECK_EQ(block->buffer_desc_->len, max_block_size_) << "Can only unpin io blocks.";
  // Add 'block' to the list of unpinned blocks and set is_pinned_ to false.
  // Cache its position in the list for later removal.
  block->is_pinned_ = false;
  DCHECK(!unpinned_blocks_.Contains(block)) << " Unpin for block in unpinned list";
  if (!block->in_write_) unpinned_blocks_.Enqueue(block);
  block->client_->UnpinBuffer(block->buffer_desc_);
  if (block->client_->num_pinned_buffers_ < block->client_->num_reserved_buffers_) {
    ++unfullfilled_reserved_buffers_;
  }
  --total_pinned_buffers_;
  RETURN_IF_ERROR(WriteUnpinnedBlocks());
  DCHECK(Validate()) << endl << DebugInternal();
  return Status::OK;
}

Status BufferedBlockMgr::WriteUnpinnedBlocks() {
  if (disable_spill_) return Status::OK;

  // Assumes block manager lock is already taken.
  while (num_outstanding_writes_ + free_io_buffers_.size() < block_write_threshold_) {
    if (unpinned_blocks_.empty()) break;
    // Pop a block from the back of the list (LIFO)
    Block* write_block = unpinned_blocks_.PopBack();
    write_block->client_local_ = false;
    RETURN_IF_ERROR(WriteUnpinnedBlock(write_block));
    ++num_outstanding_writes_;
  }
  DCHECK(Validate()) << endl << DebugInternal();
  return Status::OK;
}

Status BufferedBlockMgr::WriteUnpinnedBlock(Block* block) {
  // Assumes block manager lock is already taken.
  DCHECK(!block->is_pinned_) << block->DebugString();
  DCHECK(!block->in_write_) << block->DebugString();

  if (block->write_range_ == NULL) {
    if (tmp_files_.empty()) RETURN_IF_ERROR(InitTmpFiles());

    // First time the block is being persisted. Find the next physical file in
    // round-robin order and create a write range for it.
    TmpFileMgr::File& tmp_file = tmp_files_[next_block_index_];
    next_block_index_ = (next_block_index_ + 1) % tmp_files_.size();
    int64_t file_offset;
    RETURN_IF_ERROR(tmp_file.AllocateSpace(max_block_size_, &file_offset));
    int disk_id = tmp_file.disk_id();
    if (disk_id < 0) {
      // Assign a valid disk id to the write range if the tmp file was not assigned one.
      static unsigned int next_disk_id = 0;
      disk_id = (++next_disk_id) % io_mgr_->num_disks();
    }
    disk_id %= io_mgr_->num_disks();
    DiskIoMgr::WriteRange::WriteDoneCallback callback =
        bind(mem_fn(&BufferedBlockMgr::WriteComplete), this, block, _1);
    block->write_range_ = obj_pool_.Add(new DiskIoMgr::WriteRange(
        tmp_file.path(), file_offset, disk_id, callback));
  }

  uint8_t* outbuf = NULL;
  if (encryption_) {
    // The block->buffer() could be accessed during the write path, so we have to
    // make a copy of it while writing.
    RETURN_IF_ERROR(Encrypt(block, &outbuf));
  } else {
    outbuf = block->buffer();
  }

  if (check_integrity_) SetHash(block);

  block->write_range_->SetData(outbuf, block->valid_data_len_);

  // Issue write through DiskIoMgr.
  RETURN_IF_ERROR(io_mgr_->AddWriteRange(io_request_context_, block->write_range_));
  block->in_write_ = true;
  DCHECK(block->Validate()) << endl << block->DebugString();
  outstanding_writes_counter_->Add(1);
  bytes_written_counter_->Add(block->valid_data_len_);
  ++writes_issued_;
  if (writes_issued_ == 1) {
    if (ImpaladMetrics::NUM_QUERIES_SPILLED != NULL) {
      ImpaladMetrics::NUM_QUERIES_SPILLED->Increment(1);
    }
  }
  return Status::OK;
}

void BufferedBlockMgr::WriteComplete(Block* block, const Status& write_status) {
  outstanding_writes_counter_->Add(-1);
  lock_guard<mutex> lock(lock_);
  DCHECK(Validate()) << endl << DebugInternal();
  DCHECK(block->in_write_) << "WriteComplete() for block not in write."
                           << endl << block->DebugString();
  if (!block->client_local_) {
    DCHECK_GT(num_outstanding_writes_, 0) << block->DebugString();
    --num_outstanding_writes_;
  }
  block->in_write_ = false;
  if (encryption_) {
    // Explicitly release our temporarily allocated buffer here so that it doesn't
    // hang around needlessly.
    EncryptDone(block);
  }
  if (is_cancelled_.Read() == 1) return;
  // Check for an error. Set cancelled and wake up waiting threads if an error occurred.
  if (!write_status.ok()) {
    block->client_->state_->LogError(write_status);
    is_cancelled_.Swap(1);
    if (block->client_local_) {
      block->write_complete_cv_.notify_one();
    } else {
      buffer_available_cv_.notify_all();
    }
    return;
  }

  // If the block was re-pinned when it was in the IOMgr queue, don't free it.
  if (block->is_pinned_) {
    // The number of outstanding writes has decreased but the number of free buffers
    // hasn't.
    DCHECK(!block->client_local_)
        << "Client should be waiting, No one should have pinned this block.";
    WriteUnpinnedBlocks();
    DCHECK(Validate()) << endl << DebugInternal();
    return;
  }

  if (block->client_local_) {
    DCHECK(!block->is_deleted_)
        << "Client should be waiting. No one should have deleted this block.";
    block->write_complete_cv_.notify_one();
    return;
  }

  DCHECK_EQ(block->buffer_desc_->len, max_block_size_)
      << "Only io sized buffers should spill";
  free_io_buffers_.Enqueue(block->buffer_desc_);
  if (block->is_deleted_) {
    block->buffer_desc_->block = NULL;
    block->buffer_desc_ = NULL;
    ReturnUnusedBlock(block);
  }
  DCHECK(Validate()) << endl << DebugInternal();
  buffer_available_cv_.notify_one();
}

Status BufferedBlockMgr::DeleteBlock(Block* block) {
  DCHECK(!block->is_deleted_);
  if (is_cancelled_.Read() == 1) return Status::CANCELLED;

  lock_guard<mutex> lock(lock_);
  DCHECK(block->Validate()) << endl << DebugInternal();
  block->is_deleted_ = true;

  if (block->is_pinned_) {
    if (block->is_max_size()) --total_pinned_buffers_;
    block->is_pinned_ = false;
    block->client_->UnpinBuffer(block->buffer_desc_);
    if (block->client_->num_pinned_buffers_ < block->client_->num_reserved_buffers_) {
      ++unfullfilled_reserved_buffers_;
    }
  } else if (unpinned_blocks_.Contains(block)) {
    // Remove block from unpinned list.
    unpinned_blocks_.Remove(block);
  }

  if (block->in_write_) {
    // If a write is still pending, return. Cleanup will be done in WriteComplete().
    return Status::OK;
  }

  if (block->buffer_desc_ != NULL) {
    if (block->buffer_desc_->len != max_block_size_) {
      // Just delete the block for now.
      delete[] block->buffer_desc_->buffer;
      block->client_->tracker_->Release(block->buffer_desc_->len);
    } else if (!free_io_buffers_.Contains(block->buffer_desc_)) {
      free_io_buffers_.Enqueue(block->buffer_desc_);
      buffer_available_cv_.notify_one();
    }
    block->buffer_desc_->block = NULL;
    block->buffer_desc_ = NULL;
  }
  ReturnUnusedBlock(block);
  DCHECK(Validate()) << endl << DebugInternal();
  return Status::OK;
}

void BufferedBlockMgr::ReturnUnusedBlock(Block* block) {
  DCHECK(block->is_deleted_) << block->DebugString();
  DCHECK(!block->is_pinned_) << block->DebugString();;
  DCHECK(block->buffer_desc_ == NULL);
  block->Init();
  unused_blocks_.Enqueue(block);
}

Status BufferedBlockMgr::FindBufferForBlock(Block* block, bool* in_mem) {
  DCHECK_NOTNULL(block);
  Client* client = block->client_;
  DCHECK_NOTNULL(client);
  DCHECK(!block->is_pinned_);

  *in_mem = false;
  unique_lock<mutex> l(lock_);

  DCHECK(!block->is_pinned_ && !block->is_deleted_)
      << "FindBufferForBlock() " << endl << block->DebugString();
  DCHECK(Validate()) << endl << DebugInternal();

  // First check if there is enough memory to satisfy this request.
  bool is_reserved_request = false;
  if (client->num_pinned_buffers_ < client->num_reserved_buffers_) {
    is_reserved_request = true;
  } else  if (client->num_tmp_reserved_buffers_ > 0) {
    is_reserved_request = true;
    --client->num_tmp_reserved_buffers_;
  }
  if (is_reserved_request) --unfullfilled_reserved_buffers_;

  if (!is_reserved_request && remaining_unreserved_buffers() < 1) {
    // The client already has its quota and there are no unreserved blocks left.
    // Note that even if this passes, it is still possible for the path below to
    // see OOM because another query consumed memory from the process tracker. This
    // only happens if the buffer has not already been allocated by the block mgr.
    // This check should ensure that the memory cannot be consumed by another client
    // of the block mgr.
    return Status::OK;
  }

  if (block->buffer_desc_ != NULL) {
    // The block is in memory. It may be in 3 states
    // 1) In the unpinned list. The buffer will not be in the free list.
    // 2) Or, in_write_ = true. The buffer will not be in the free list.
    // 3) Or, the buffer is free, but hasn't yet been reassigned to a different block.
    DCHECK(unpinned_blocks_.Contains(block) ||
           block->in_write_ ||
           free_io_buffers_.Contains(block->buffer_desc_));
    if (unpinned_blocks_.Contains(block)) {
      unpinned_blocks_.Remove(block);
      DCHECK(!free_io_buffers_.Contains(block->buffer_desc_));
    } else if (block->in_write_) {
      DCHECK(block->in_write_ && !free_io_buffers_.Contains(block->buffer_desc_));
    } else {
      free_io_buffers_.Remove(block->buffer_desc_);
    }
    buffered_pin_counter_->Add(1);
    *in_mem = true;
  } else {
    BufferDescriptor* buffer_desc = NULL;
    RETURN_IF_ERROR(FindBuffer(l, &buffer_desc));

    if (buffer_desc == NULL) {
      // There are no free buffers or blocks we can evict. We need to fail this request.
      // If this is an optional request, return OK. If it is required, return OOM.
      if (!is_reserved_request) return Status::OK;
      if (VLOG_QUERY_IS_ON) {
        stringstream ss;
        ss << "Query id=" << query_id_ << " was unable to get minimum required buffers."
           << endl << DebugInternal() << endl << client->DebugString()
           << endl << GetStackTrace();
        VLOG_QUERY << ss.str();
      }
      Status status = Status::MEM_LIMIT_EXCEEDED;
      status.AddErrorMsg("Query did not have enough memory to get the minimum required "
          "buffers in the block manager.");
      return status;
    }

    DCHECK(buffer_desc != NULL);
    if (buffer_desc->block != NULL) {
      // This buffer was assigned to a block but now we are reusing it. Reset the
      // previous block->buffer link.
      DCHECK(buffer_desc->block->Validate()) << endl << buffer_desc->block->DebugString();
      buffer_desc->block->buffer_desc_ = NULL;
    }
    buffer_desc->block = block;
    block->buffer_desc_ = buffer_desc;
  }
  DCHECK_NOTNULL(block->buffer_desc_);
  block->is_pinned_ = true;
  client->PinBuffer(block->buffer_desc_);
  ++total_pinned_buffers_;

  DCHECK(block->Validate()) << endl << block->DebugString();
  // The number of free buffers has decreased. Write unpinned blocks if the number
  // of free buffers below the threshold is reached.
  RETURN_IF_ERROR(WriteUnpinnedBlocks());
  DCHECK(Validate()) << endl << DebugInternal();
  return Status::OK;
}

// We need to find a new buffer. We prefer getting this buffer in this order:
//  1. Allocate a new block if the number of free blocks is less than the write
//     threshold, until we run out of memory.
//  2. Pick a buffer from the free list.
//  3. Wait and evict an unpinned buffer.
Status BufferedBlockMgr::FindBuffer(unique_lock<mutex>& lock,
    BufferDescriptor** buffer_desc) {
  *buffer_desc = NULL;

  // First try to allocate a new buffer.
  if (free_io_buffers_.size() < block_write_threshold_ &&
      mem_tracker_->TryConsume(max_block_size_)) {
    uint8_t* new_buffer = new uint8_t[max_block_size_];
    *buffer_desc = obj_pool_.Add(new BufferDescriptor(new_buffer, max_block_size_));
    (*buffer_desc)->all_buffers_it = all_io_buffers_.insert(
        all_io_buffers_.end(), *buffer_desc);
    return Status::OK;
  }

  if (free_io_buffers_.empty()) {
    // No free buffers. If spills are disabled or there no unpinned blocks we can write,
    // return. We can't get a buffer.
    if (disable_spill_) {
      return Status("Spilling has been disabled for plans that do not have stats and "
        "are not hinted to prevent potentially bad plans from using too many cluster "
        "resources. Compute stats on these tables, hint the plan or disable this "
        "behavior via query options to enable spilling.");
    }
    if (unpinned_blocks_.empty() && num_outstanding_writes_ == 0) return Status::OK;
  }

  // At this point, this block needs to use a buffer that was unpinned from another
  // block. Get a free buffer from the front of the queue and assign it to the block.
  while (free_io_buffers_.empty()) {
    SCOPED_TIMER(buffer_wait_timer_);
    // Try to evict unpinned blocks before waiting.
    RETURN_IF_ERROR(WriteUnpinnedBlocks());
    buffer_available_cv_.wait(lock);
    if (is_cancelled_.Read() == 1) return Status::CANCELLED;
  }
  *buffer_desc = free_io_buffers_.Dequeue();

  return Status::OK;
}

BufferedBlockMgr::Block* BufferedBlockMgr::GetUnusedBlock(Client* client) {
  DCHECK_NOTNULL(client);
  Block* new_block;
  if (unused_blocks_.empty()) {
    new_block = obj_pool_.Add(new Block(this));
    new_block->Init();
    created_block_counter_->Add(1);
  } else {
    new_block = unused_blocks_.Dequeue();
    recycled_blocks_counter_->Add(1);
  }
  new_block->client_ = client;

  DCHECK_NOTNULL(new_block);
  DCHECK(new_block->Validate()) << endl << new_block->DebugString();
  return new_block;
}

bool BufferedBlockMgr::Validate() {
  int num_free_io_buffers = 0;

  if (total_pinned_buffers_ < 0) {
    LOG(ERROR) << "total_pinned_buffers_ < 0: " << total_pinned_buffers_;
    return false;
  }

  BOOST_FOREACH(BufferDescriptor* buffer, all_io_buffers_) {
    bool is_free = free_io_buffers_.Contains(buffer);
    num_free_io_buffers += is_free;

    if (*buffer->all_buffers_it != buffer) {
      LOG(ERROR) << "All buffers list is corrupt. Buffer iterator is not valid.";
      return false;
    }

    if (buffer->block == NULL && !is_free) {
      LOG(ERROR) << "Buffer with no block not in free list." << endl << DebugInternal();
      return false;
    }

    if (buffer->len != max_block_size_) {
      LOG(ERROR) << "Non-io sized buffers should not end up on free list.";
      return false;
    }

    if (buffer->block != NULL) {
      if (!buffer->block->Validate()) {
        LOG(ERROR) << "buffer->block inconsistent."
          << endl << buffer->block->DebugString();
        return false;
      }

      if (is_free && (buffer->block->is_pinned_ || buffer->block->in_write_ ||
            unpinned_blocks_.Contains(buffer->block))) {
        LOG(ERROR) << "Block with buffer in free list and"
          << " is_pinned_ = " << buffer->block->is_pinned_
          << " in_write_ = " << buffer->block->in_write_
          << " Unpinned_blocks_.Contains = "
          << unpinned_blocks_.Contains(buffer->block)
          << endl << buffer->block->DebugString();
        return false;
      }
    }
  }

  if (free_io_buffers_.size() != num_free_io_buffers) {
    LOG(ERROR) << "free_buffer_list_ inconsistency."
      << " num_free_io_buffers = " << num_free_io_buffers
      << " free_io_buffers_.size() = " << free_io_buffers_.size()
      << endl << DebugInternal();
    return false;
  }

  Block* block = unpinned_blocks_.head();
  while (block != NULL) {
    if (!block->Validate()) {
      LOG(ERROR) << "Block inconsistent in unpinned list."
        << endl << block->DebugString();
      return false;
    }

    if (block->in_write_ || free_io_buffers_.Contains(block->buffer_desc_)) {
      LOG(ERROR) << "Block in unpinned list with"
        << " in_write_ = " << block->in_write_
        << " free_io_buffers_.Contains = "
        << free_io_buffers_.Contains(block->buffer_desc_)
        << endl << block->DebugString();
      return false;
    }
    block = block->Next();
  }

  // Check if we're writing blocks when the number of free buffers falls below
  // threshold. We don't write blocks after cancellation.
  if ((is_cancelled_.Read() == 0) && !unpinned_blocks_.empty() && !disable_spill_ &&
      (free_io_buffers_.size() + num_outstanding_writes_ < block_write_threshold_)) {
    LOG(ERROR) << "Missed writing unpinned blocks";
    return false;
  }
  return true;
}

string BufferedBlockMgr::DebugString(Client* client) {
  unique_lock<mutex> l(lock_);
  stringstream ss;
  ss <<  DebugInternal();
  if (client != NULL) ss << endl << client->DebugString();
  return ss.str();
}

string BufferedBlockMgr::DebugInternal() const {
  stringstream ss;
  ss << "Buffered block mgr" << endl
     << "  Num writes outstanding: " << outstanding_writes_counter_->value() << endl
     << "  Num free io buffers: " << free_io_buffers_.size() << endl
     << "  Num unpinned blocks: " << unpinned_blocks_.size() << endl
     << "  Num available buffers: " << remaining_unreserved_buffers() << endl
     << "  Total pinned buffers: " << total_pinned_buffers_ << endl
     << "  Unfullfilled reserved buffers: " << unfullfilled_reserved_buffers_ << endl
     << "  Remaining memory: " << mem_tracker_->SpareCapacity()
     << " (#blocks=" << (mem_tracker_->SpareCapacity() / max_block_size_) << ")" << endl
     << "  Block write threshold: " << block_write_threshold_;
  return ss.str();
}

void BufferedBlockMgr::Init(DiskIoMgr* io_mgr, RuntimeProfile* parent_profile,
    MemTracker* parent_tracker, int64_t mem_limit) {
  unique_lock<mutex> l(lock_);
  if (initialized_) return;

  io_mgr->RegisterContext(&io_request_context_);
  if (encryption_) {
    static bool openssl_loaded = false;
    if (!openssl_loaded) {
      // These are idempotent, so no threading worries.
      OpenSSL_add_all_algorithms();
      ERR_load_crypto_strings();
      openssl_loaded = true;
    }
    // Seed the random number generator
    // TODO: try non-blocking read from /dev/random and add that, too.
    RAND_load_file("/dev/urandom", 4096);
  }

  profile_.reset(new RuntimeProfile(&obj_pool_, "BlockMgr"));
  parent_profile->AddChild(profile_.get());

  mem_limit_counter_ = ADD_COUNTER(profile_.get(), "MemoryLimit", TUnit::BYTES);
  mem_limit_counter_->Set(mem_limit);
  block_size_counter_ = ADD_COUNTER(profile_.get(), "MaxBlockSize", TUnit::BYTES);
  block_size_counter_->Set(max_block_size_);
  created_block_counter_ = ADD_COUNTER(
      profile_.get(), "BlocksCreated", TUnit::UNIT);
  recycled_blocks_counter_ = ADD_COUNTER(
      profile_.get(), "BlocksRecycled", TUnit::UNIT);
  bytes_written_counter_ = ADD_COUNTER(
      profile_.get(), "BytesWritten", TUnit::BYTES);
  outstanding_writes_counter_ =
      ADD_COUNTER(profile_.get(), "BlockWritesOutstanding", TUnit::UNIT);
  buffered_pin_counter_ = ADD_COUNTER(profile_.get(), "BufferedPins", TUnit::UNIT);
  disk_read_timer_ = ADD_TIMER(profile_.get(), "TotalReadBlockTime");
  buffer_wait_timer_ = ADD_TIMER(profile_.get(), "TotalBufferWaitTime");
  encryption_timer_ = ADD_TIMER(profile_.get(), "TotalEncryptionTime");
  integrity_check_timer_ = ADD_TIMER(profile_.get(), "TotalIntegrityCheckTime");

  // Create a new mem_tracker and allocate buffers.
  mem_tracker_.reset(new MemTracker(
      profile(), mem_limit, -1, "Block Manager", parent_tracker));

  initialized_ = true;
}

Status BufferedBlockMgr::InitTmpFiles() {
  DCHECK(tmp_files_.empty());

  // Initialize the tmp files and the initial file to use.
  int num_tmp_devices = TmpFileMgr::num_tmp_devices();
  if (num_tmp_devices == 0) {
    return Status(
        "No spilling directories configured. Cannot spill. Set --scratch_dirs.");
  }

  tmp_files_.reserve(num_tmp_devices);
  for (int i = 0; i < num_tmp_devices; ++i) {
    TmpFileMgr::File* tmp_file;
    RETURN_IF_ERROR(TmpFileMgr::GetFile(i, query_id_, &tmp_file));
    tmp_files_.push_back(tmp_file);
  }
  next_block_index_ = rand() % num_tmp_devices;
  return Status::OK;
}

// Callback used by OpenSSLErr() - write the error given to us through buf to the
// stringstream that's passed in through ctx.
static int OpenSSLErrCallback(const char *buf, size_t len, void* ctx) {
  stringstream* errstream = static_cast<stringstream*>(ctx);
  *errstream << buf;
  return 1;
}

// Called upon OpenSSL errors; returns a non-OK status with an error message.
static Status OpenSSLErr(const string& msg) {
  stringstream errstream;
  errstream << msg << ": ";
  ERR_print_errors_cb (OpenSSLErrCallback, &errstream);
  return Status(Substitute("Openssl Error: $0", errstream.str()));
}

Status BufferedBlockMgr::Encrypt(Block* block, uint8_t** outbuf) {
  DCHECK(encryption_);
  DCHECK(block->buffer());
  DCHECK(!block->is_pinned_);
  DCHECK(!block->in_write_);
  DCHECK(outbuf);
  SCOPED_TIMER(encryption_timer_);

  // Since we're using AES-CFB mode, we must take care not to reuse a key/iv pair.
  // Regenerate a new key and iv for every block of data we write, including between
  // writes of the same Block.
  RAND_bytes(block->key_, sizeof(block->key_));
  RAND_bytes(block->iv_, sizeof(block->iv_));
  block->encrypted_write_buffer_.reset(new uint8_t[block->valid_data_len_]);

  EVP_CIPHER_CTX ctx;
  int len = static_cast<int>(block->valid_data_len_);

  // Create and initialize the context for encryption
  EVP_CIPHER_CTX_init(&ctx);
  EVP_CIPHER_CTX_set_padding(&ctx, 0);

  // Start encryption.  We use a 256-bit AES key, and the cipher block mode
  // is CFB because this gives us a stream cipher, which supports arbitrary
  // length ciphertexts - it doesn't have to be a multiple of 16 bytes.
  if (EVP_EncryptInit_ex(&ctx, EVP_aes_256_cfb(), NULL, block->key_, block->iv_) != 1) {
    return OpenSSLErr("EVP_EncryptInit_ex failure");
  }

  // Encrypt block->buffer() into the new encrypted_write_buffer_
  if (EVP_EncryptUpdate(&ctx, block->encrypted_write_buffer_.get(), &len,
        block->buffer(), len) != 1) {
    return OpenSSLErr("EVP_EncryptUpdate failure");
  }

  // This is safe because we're using CFB mode without padding.
  DCHECK_EQ(len, block->valid_data_len_);

  // Finalize encryption.
  if (1 != EVP_EncryptFinal_ex(&ctx, block->encrypted_write_buffer_.get() + len, &len)) {
    return OpenSSLErr("EVP_EncryptFinal failure");
  }

  // Again safe due to CFB with no padding
  DCHECK_EQ(len, 0);

  *outbuf = block->encrypted_write_buffer_.get();
  return Status::OK;
}

void BufferedBlockMgr::EncryptDone(Block* block) {
  DCHECK(encryption_);
  DCHECK(block->encrypted_write_buffer_.get());
  block->encrypted_write_buffer_.reset();
}

Status BufferedBlockMgr::Decrypt(Block* block) {
  DCHECK(encryption_);
  DCHECK(block->buffer());
  SCOPED_TIMER(encryption_timer_);

  EVP_CIPHER_CTX ctx;
  int len = static_cast<int>(block->valid_data_len_);

  // Create and initialize the context for encryption
  EVP_CIPHER_CTX_init(&ctx);
  EVP_CIPHER_CTX_set_padding(&ctx, 0);

  // Start decryption; same parameters as encryption for obvious reasons
  if (EVP_DecryptInit_ex(&ctx, EVP_aes_256_cfb(), NULL, block->key_, block->iv_) != 1) {
    return OpenSSLErr("EVP_DecryptInit_ex failure");
  }

  // Decrypt block->buffer() in-place.  Safe because no one is accessing it.
  if (EVP_DecryptUpdate(&ctx, block->buffer(), &len, block->buffer(), len) != 1) {
    return OpenSSLErr("EVP_DecryptUpdate failure");
  }

  // This is safe because we're using CFB mode without padding.
  DCHECK_EQ(len, block->valid_data_len_);

  // Finalize decryption.
  if (1 != EVP_DecryptFinal_ex(&ctx, block->buffer() + len, &len)) {
    return OpenSSLErr("EVP_DecryptFinal failure");
  }

  // Again safe due to CFB with no padding
  DCHECK_EQ(len, 0);

  return Status::OK;
}

void BufferedBlockMgr::SetHash(Block* block) {
  DCHECK(check_integrity_);
  SCOPED_TIMER(integrity_check_timer_);
  uint8_t* data = NULL;
  if (encryption_) {
    DCHECK(block->encrypted_write_buffer_.get());
    data = block->encrypted_write_buffer_.get();
  } else {
    DCHECK(block->buffer());
    data = block->buffer();
  }
  // Explicitly ignore the return value from SHA256(); it can't fail.
  (void) SHA256(data, block->valid_data_len_, block->hash_);
}

Status BufferedBlockMgr::VerifyHash(Block* block) {
  DCHECK(check_integrity_);
  DCHECK(block->buffer());
  SCOPED_TIMER(integrity_check_timer_);
  uint8_t test_hash[SHA256_DIGEST_LENGTH];
  (void) SHA256(block->buffer(), block->valid_data_len_, test_hash);
  if (memcmp(test_hash, block->hash_, SHA256_DIGEST_LENGTH) != 0) {
    return Status("Block verification failure");
  }
  return Status::OK;
}

} // namespace impala

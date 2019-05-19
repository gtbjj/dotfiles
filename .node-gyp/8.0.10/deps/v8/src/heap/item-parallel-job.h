// Copyright 2017 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_HEAP_ITEM_PARALLEL_JOB_H_
#define V8_HEAP_ITEM_PARALLEL_JOB_H_

#include <memory>
#include <vector>

#include "src/base/atomic-utils.h"
#include "src/base/logging.h"
#include "src/base/macros.h"
#include "src/base/optional.h"
#include "src/cancelable-task.h"
#include "src/counters.h"
#include "src/globals.h"

namespace v8 {

namespace base {
class Semaphore;
}

namespace internal {

class Counters;
class Isolate;

// This class manages background tasks that process a set of items in parallel.
// The first task added is executed on the same thread as |job.Run()| is called.
// All other tasks are scheduled in the background.
//
// - Items need to inherit from ItemParallelJob::Item.
// - Tasks need to inherit from ItemParallelJob::Task.
//
// Items need to be marked as finished after processing them. Task and Item
// ownership is transferred to the job.
//
// Each parallel (non-main thread) task will report the time between the job
// being created and it being scheduled to |gc_parallel_task_latency_histogram|.
class V8_EXPORT_PRIVATE ItemParallelJob {
 public:
  class Task;

  class V8_EXPORT_PRIVATE Item {
   public:
    Item() = default;
    virtual ~Item() = default;

    // Marks an item as being finished.
    void MarkFinished() { CHECK(state_.TrySetValue(kProcessing, kFinished)); }

   private:
    enum ProcessingState { kAvailable, kProcessing, kFinished };

    bool TryMarkingAsProcessing() {
      return state_.TrySetValue(kAvailable, kProcessing);
    }
    bool IsFinished() { return state_.Value() == kFinished; }

    base::AtomicValue<ProcessingState> state_{kAvailable};

    friend class ItemParallelJob;
    friend class ItemParallelJob::Task;

    DISALLOW_COPY_AND_ASSIGN(Item);
  };

  class V8_EXPORT_PRIVATE Task : public CancelableTask {
   public:
    explicit Task(Isolate* isolate);
    virtual ~Task();

    virtual void RunInParallel() = 0;

   protected:
    // Retrieves a new item that needs to be processed. Returns |nullptr| if
    // all items are processed. Upon returning an item, the task is required
    // to process the item and mark the item as finished after doing so.
    template <class ItemType>
    ItemType* GetItem() {
      while (items_considered_++ != items_->size()) {
        // Wrap around.
        if (cur_index_ == items_->size()) {
          cur_index_ = 0;
        }
        Item* item = (*items_)[cur_index_++];
        if (item->TryMarkingAsProcessing()) {
          return static_cast<ItemType*>(item);
        }
      }
      return nullptr;
    }

   private:
    friend class ItemParallelJob;
    friend class Item;

    // Sets up state required before invoking Run(). If
    // |start_index is >= items_.size()|, this task will not process work items
    // (some jobs have more tasks than work items in order to parallelize post-
    // processing, e.g. scavenging). If |gc_parallel_task_latency_histogram| is
    // provided, it will be used to report histograms on the latency between
    // posting the task and it being scheduled.
    void SetupInternal(
        base::Semaphore* on_finish, std::vector<Item*>* items,
        size_t start_index,
        base::Optional<AsyncTimedHistogram> gc_parallel_task_latency_histogram);

    // We don't allow overriding this method any further.
    void RunInternal() final;

    std::vector<Item*>* items_ = nullptr;
    size_t cur_index_ = 0;
    size_t items_considered_ = 0;
    base::Semaphore* on_finish_ = nullptr;
    base::Optional<AsyncTimedHistogram> gc_parallel_task_latency_histogram_;

    DISALLOW_COPY_AND_ASSIGN(Task);
  };

  ItemParallelJob(CancelableTaskManager* cancelable_task_manager,
                  base::Semaphore* pending_tasks);

  ~ItemParallelJob();

  // Adds a task to the job. Transfers ownership to the job.
  void AddTask(Task* task) { tasks_.push_back(std::unique_ptr<Task>(task)); }

  // Adds an item to the job. Transfers ownership to the job.
  void AddItem(Item* item) { items_.push_back(item); }

  int NumberOfItems() const { return static_cast<int>(items_.size()); }
  int NumberOfTasks() const { return static_cast<int>(tasks_.size()); }

  // Runs this job. Reporting metrics in a thread-safe manner to
  // |async_counters|.
  void Run(std::shared_ptr<Counters> async_counters);

 private:
  std::vector<Item*> items_;
  std::vector<std::unique_ptr<Task>> tasks_;
  CancelableTaskManager* cancelable_task_manager_;
  base::Semaphore* pending_tasks_;
  DISALLOW_COPY_AND_ASSIGN(ItemParallelJob);
};

}  // namespace internal
}  // namespace v8

#endif  // V8_HEAP_ITEM_PARALLEL_JOB_H_

/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RuntimeScheduler_Legacy.h"
#include "SchedulerPriorityUtils.h"

#include <react/renderer/debug/SystraceSection.h>
#include <utility>
#include "ErrorUtils.h"

namespace facebook::react {

#pragma mark - Public

RuntimeScheduler_Legacy::RuntimeScheduler_Legacy(
    RuntimeExecutor runtimeExecutor,
    std::function<RuntimeSchedulerTimePoint()> now)
    : runtimeExecutor_(std::move(runtimeExecutor)), now_(std::move(now)) {}

void RuntimeScheduler_Legacy::scheduleWork(RawCallback&& callback) noexcept {
  SystraceSection s("RuntimeScheduler::scheduleWork");

  runtimeAccessRequests_ += 1;

  runtimeExecutor_(
      [this, callback = std::move(callback)](jsi::Runtime& runtime) {
        SystraceSection s2("RuntimeScheduler::scheduleWork callback");
        runtimeAccessRequests_ -= 1;
        callback(runtime);
        startWorkLoop(runtime);
      });
}

std::shared_ptr<Task> RuntimeScheduler_Legacy::scheduleTask(
    SchedulerPriority priority,
    jsi::Function&& callback) noexcept {
  SystraceSection s(
      "RuntimeScheduler::scheduleTask",
      "priority",
      serialize(priority),
      "callbackType",
      "jsi::Function");

  auto expirationTime = now_() + timeoutForSchedulerPriority(priority);
  auto task =
      std::make_shared<Task>(priority, std::move(callback), expirationTime);
  taskQueue_.push(task);

  scheduleWorkLoopIfNecessary();

  return task;
}

std::shared_ptr<Task> RuntimeScheduler_Legacy::scheduleTask(
    SchedulerPriority priority,
    RawCallback&& callback) noexcept {
  SystraceSection s(
      "RuntimeScheduler::scheduleTask",
      "priority",
      serialize(priority),
      "callbackType",
      "RawCallback");

  auto expirationTime = now_() + timeoutForSchedulerPriority(priority);
  auto task =
      std::make_shared<Task>(priority, std::move(callback), expirationTime);
  taskQueue_.push(task);

  scheduleWorkLoopIfNecessary();

  return task;
}

bool RuntimeScheduler_Legacy::getShouldYield() const noexcept {
  return runtimeAccessRequests_ > 0;
}

bool RuntimeScheduler_Legacy::getIsSynchronous() const noexcept {
  return isSynchronous_;
}

void RuntimeScheduler_Legacy::cancelTask(Task& task) noexcept {
  task.callback.reset();
}

SchedulerPriority RuntimeScheduler_Legacy::getCurrentPriorityLevel()
    const noexcept {
  return currentPriority_;
}

RuntimeSchedulerTimePoint RuntimeScheduler_Legacy::now() const noexcept {
  return now_();
}

void RuntimeScheduler_Legacy::executeNowOnTheSameThread(
    RawCallback&& callback) {
  SystraceSection s("RuntimeScheduler::executeNowOnTheSameThread");

  runtimeAccessRequests_ += 1;
  executeSynchronouslyOnSameThread_CAN_DEADLOCK(
      runtimeExecutor_,
      [this, callback = std::move(callback)](jsi::Runtime& runtime) {
        SystraceSection s2(
            "RuntimeScheduler::executeNowOnTheSameThread callback");

        runtimeAccessRequests_ -= 1;
        isSynchronous_ = true;
        callback(runtime);
        isSynchronous_ = false;
      });

  // Resume work loop if needed. In synchronous mode
  // only expired tasks are executed. Tasks with lower priority
  // might be still in the queue.
  scheduleWorkLoopIfNecessary();
}

void RuntimeScheduler_Legacy::callExpiredTasks(jsi::Runtime& runtime) {
  SystraceSection s("RuntimeScheduler::callExpiredTasks");

  auto previousPriority = currentPriority_;
  try {
    while (!taskQueue_.empty()) {
      auto topPriorityTask = taskQueue_.top();
      auto now = now_();
      auto didUserCallbackTimeout = topPriorityTask->expirationTime <= now;

      if (!didUserCallbackTimeout) {
        break;
      }

      executeTask(runtime, topPriorityTask, didUserCallbackTimeout);
    }
  } catch (jsi::JSError& error) {
    handleFatalError(runtime, error);
  }

  currentPriority_ = previousPriority;
}

#pragma mark - Private

void RuntimeScheduler_Legacy::scheduleWorkLoopIfNecessary() {
  if (!isWorkLoopScheduled_ && !isPerformingWork_) {
    isWorkLoopScheduled_ = true;
    runtimeExecutor_([this](jsi::Runtime& runtime) {
      isWorkLoopScheduled_ = false;
      startWorkLoop(runtime);
    });
  }
}

void RuntimeScheduler_Legacy::startWorkLoop(jsi::Runtime& runtime) {
  SystraceSection s("RuntimeScheduler::startWorkLoop");

  auto previousPriority = currentPriority_;
  isPerformingWork_ = true;
  try {
    while (!taskQueue_.empty()) {
      auto topPriorityTask = taskQueue_.top();
      auto now = now_();
      auto didUserCallbackTimeout = topPriorityTask->expirationTime <= now;

      if (!didUserCallbackTimeout && getShouldYield()) {
        // This currentTask hasn't expired, and we need to yield.
        break;
      }

      executeTask(runtime, topPriorityTask, didUserCallbackTimeout);
    }
  } catch (jsi::JSError& error) {
    handleFatalError(runtime, error);
  }

  currentPriority_ = previousPriority;
  isPerformingWork_ = false;
}

void RuntimeScheduler_Legacy::executeTask(
    jsi::Runtime& runtime,
    const std::shared_ptr<Task>& task,
    bool didUserCallbackTimeout) {
  SystraceSection s(
      "RuntimeScheduler::executeTask",
      "priority",
      serialize(task->priority),
      "didUserCallbackTimeout",
      didUserCallbackTimeout);

  currentPriority_ = task->priority;
  auto result = task->execute(runtime, didUserCallbackTimeout);

  if (result.isObject() && result.getObject(runtime).isFunction(runtime)) {
    task->callback = result.getObject(runtime).getFunction(runtime);
  } else {
    if (taskQueue_.top() == task) {
      taskQueue_.pop();
    }
  }
}

} // namespace facebook::react

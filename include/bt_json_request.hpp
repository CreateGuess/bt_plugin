#pragma once

#include <behaviortree_cpp/action_node.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>

#include "plugin_core/sdk/plugin_sdk.hpp"

class BtJsonRequest {
public:
  enum class State : int {
    Idle = 0,
    Pending,
    Success,
    Failure,
  };

  BtJsonRequest() = default;

  ~BtJsonRequest() { cancel(); }

  BtJsonRequest(const BtJsonRequest&) = delete;
  BtJsonRequest& operator=(const BtJsonRequest&) = delete;

  bool start(IReqRep* reqrep, const char* service, const nlohmann::json& payload,
             std::chrono::milliseconds timeout) {
    reset();

    if (!reqrep || !service) {
      fail("reqrep or service is null");
      return false;
    }

    deadline_ = std::chrono::steady_clock::now() + timeout;
    state_data_->state.store(State::Pending, std::memory_order_release);
    state_data_->req_status.store(static_cast<int>(ReqStatus::Pending), std::memory_order_release);

    const auto generation = state_data_->generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    const std::weak_ptr<SharedState> weak_state = state_data_;

    handle_ = ReqRepJson::call_async(
        reqrep, service, payload, [weak_state, generation](ReqJsonResult ret) {
          auto state = weak_state.lock();
          if (!state) {
            return;
          }

          std::lock_guard lk(state->mtx);
          if (generation != state->generation.load(std::memory_order_acquire)) return;

          state->req_status.store(static_cast<int>(ret.req_status), std::memory_order_release);
          state->message = ret.message;

          if (ret.ok) {
            state->state.store(State::Success, std::memory_order_release);
          } else {
            state->state.store(State::Failure, std::memory_order_release);
          }
        });

    if (!handle_) {
      fail("request create failed");
      return false;
    }

    return true;
  }

  BT::NodeStatus tick(const char* tag) {
    const auto state = state_data_->state.load(std::memory_order_acquire);

    switch (state) {
      case State::Pending: {
        if (std::chrono::steady_clock::now() >= deadline_) {
          LOG_WARN("[{}] request timeout", tag ? tag : "BtJsonRequest");

          cancel();
          return BT::NodeStatus::FAILURE;
        }

        return BT::NodeStatus::RUNNING;
      }

      case State::Success: {
        // LOG_DEBUG("[{}] success: {}", tag ? tag : "BtJsonRequest", message());

        reset();
        return BT::NodeStatus::SUCCESS;
      }

      case State::Failure: {
        LOG_ERROR("[{}] failed, req_status={}, message={}", tag ? tag : "BtJsonRequest",
                  state_data_->req_status.load(std::memory_order_acquire), message());

        reset();
        return BT::NodeStatus::FAILURE;
      }

      case State::Idle:
      default:
        return BT::NodeStatus::FAILURE;
    }
  }

  void cancel() {
    {
      std::lock_guard lk(state_data_->mtx);
      state_data_->generation.fetch_add(1, std::memory_order_acq_rel);
      reset_state_locked();
    }

    if (handle_) {
      handle_->cancel();
      handle_.reset();
    }
  }

  State state() const noexcept { return state_data_->state.load(std::memory_order_acquire); }

  std::string message() const {
    std::lock_guard lk(state_data_->mtx);
    return state_data_->message;
  }

private:
  struct SharedState {
    std::atomic<State> state{State::Idle};
    std::atomic<int> req_status{static_cast<int>(ReqStatus::Pending)};
    std::atomic<uint64_t> generation{0};
    mutable std::mutex mtx;
    std::string message;
  };

  void reset() {
    {
      std::lock_guard lk(state_data_->mtx);
      state_data_->generation.fetch_add(1, std::memory_order_acq_rel);
      reset_state_locked();
    }
    handle_.reset();
  }

  void reset_state_locked() {
    state_data_->state.store(State::Idle, std::memory_order_release);
    state_data_->req_status.store(static_cast<int>(ReqStatus::Pending), std::memory_order_release);
    state_data_->message.clear();
  }

  void fail(std::string msg) {
    std::lock_guard lk(state_data_->mtx);
    state_data_->message = std::move(msg);
    state_data_->state.store(State::Failure, std::memory_order_release);
  }

private:
  std::shared_ptr<ReqJsonAsyncHandle> handle_;
  std::shared_ptr<SharedState> state_data_{std::make_shared<SharedState>()};

  std::chrono::steady_clock::time_point deadline_{};
};

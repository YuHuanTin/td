//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2020
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "td/telegram/Client.h"

#include "td/telegram/Td.h"
#include "td/telegram/TdCallback.h"

#include "td/actor/actor.h"

#include "td/utils/common.h"
#include "td/utils/crypto.h"
#include "td/utils/logging.h"
#include "td/utils/misc.h"
#include "td/utils/MpscPollableQueue.h"
#include "td/utils/port/RwMutex.h"
#include "td/utils/port/thread.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <queue>
#include <unordered_map>

namespace td {

#if TD_THREAD_UNSUPPORTED || TD_EVENTFD_UNSUPPORTED
class TdReceiver {
 public:
  ClientManager::Response receive(double timeout) {
    if (!responses_.empty()) {
      auto result = std::move(responses_.front());
      responses_.pop();
      return result;
    }
    return {0, 0, nullptr};
  }

  unique_ptr<TdCallback> create_callback(ClientManager::ClientId client_id) {
    class Callback : public TdCallback {
     public:
      Callback(ClientManager::ClientId client_id, TdReceiver *impl) : client_id_(client_id), impl_(impl) {
      }
      void on_result(uint64 id, td_api::object_ptr<td_api::Object> result) override {
        impl_->responses_.push({client_id_, id, std::move(result)});
      }
      void on_error(uint64 id, td_api::object_ptr<td_api::error> error) override {
        impl_->responses_.push({client_id_, id, std::move(error)});
      }
      Callback(const Callback &) = delete;
      Callback &operator=(const Callback &) = delete;
      Callback(Callback &&) = delete;
      Callback &operator=(Callback &&) = delete;
      ~Callback() override {
        impl_->responses_.push({client_id_, 0, nullptr});
      }

     private:
      ClientManager::ClientId client_id_;
      TdReceiver *impl_;
    };
    return td::make_unique<Callback>(client_id, this);
  }

  void add_response(ClientManager::ClientId client_id, uint64 id, td_api::object_ptr<td_api::Object> result) {
    responses_.push({client_id, id, std::move(result)});
  }

 private:
  std::queue<ClientManager::Response> responses_;
};

class ClientManager::Impl final {
 public:
  Impl() {
    options_.net_query_stats = std::make_shared<NetQueryStats>();
    concurrent_scheduler_ = make_unique<ConcurrentScheduler>();
    concurrent_scheduler_->init(0);
    receiver_ = make_unique<TdReceiver>();
    concurrent_scheduler_->start();
  }

  ClientId create_client() {
    auto client_id = ++client_id_;
    tds_[client_id] =
        concurrent_scheduler_->create_actor_unsafe<Td>(0, "Td", receiver_->create_callback(client_id), options_);
    return client_id;
  }

  void send(ClientId client_id, RequestId request_id, td_api::object_ptr<td_api::Function> &&request) {
    Request request;
    request.client_id = client_id;
    request.id = request_id;
    request.request = std::move(request);
    requests_.push_back(std::move(request));
  }

  Response receive(double timeout) {
    if (!requests_.empty()) {
      auto guard = concurrent_scheduler_->get_main_guard();
      for (auto &request : requests_) {
        auto &td = tds_[request.client_id];
        CHECK(!td.empty());
        send_closure_later(td, &Td::request, request.id, std::move(request.request));
      }
      requests_.clear();
    }

    auto response = receiver_->receive(0);
    if (response.client_id == 0) {
      concurrent_scheduler_->run_main(0);
      response = receiver_->receive(0);
    } else {
      ConcurrentScheduler::emscripten_clear_main_timeout();
    }
    if (response.object == nullptr && response.client_id != 0 && response.request_id == 0) {
      auto guard = concurrent_scheduler_->get_main_guard();
      tds_.erase(response.client_id);
    }
    return response;
  }

  Impl(const Impl &) = delete;
  Impl &operator=(const Impl &) = delete;
  Impl(Impl &&) = delete;
  Impl &operator=(Impl &&) = delete;
  ~Impl() {
    {
      auto guard = concurrent_scheduler_->get_main_guard();
      for (auto &td : tds_) {
        td.second = {};
      }
    }
    while (!tds_.empty()) {
      receive(10);
    }
    concurrent_scheduler_->finish();
  }

 private:
  unique_ptr<TdReceiver> receiver_;
  struct Request {
    ClientId client_id;
    RequestId id;
    td_api::object_ptr<td_api::Function> request;
  };
  td::vector<Request> requests_;
  unique_ptr<ConcurrentScheduler> concurrent_scheduler_;
  ClientId client_id_{0};
  Td::Options options_;
  std::unordered_map<int32, ActorOwn<Td>> tds_;
};

class Client::Impl final {
 public:
  Impl() {
    client_id_ = impl_.create_client();
  }

  void send(Request request) {
    impl_.send(client_id_, request.id, std::move(request.request));
  }

  Response receive(double timeout) {
    auto response = impl_.receive(timeout);
    Response old_response;
    old_response.id = response.id;
    old_response.object = std::move(response.object);
    return old_response;
  }

 private:
  ClientManager::Impl impl_;
  ClientManager::ClientId client_id_;
};

#else

class MultiTd : public Actor {
 public:
  explicit MultiTd(Td::Options options) : options_(std::move(options)) {
  }
  void create(int32 td_id, unique_ptr<TdCallback> callback) {
    auto &td = tds_[td_id];
    CHECK(td.empty());

    string name = "Td";
    auto context = std::make_shared<td::ActorContext>();
    auto old_context = set_context(context);
    auto old_tag = set_tag(to_string(td_id));
    td = create_actor<Td>("Td", std::move(callback), options_);
    set_context(old_context);
    set_tag(old_tag);
  }

  void send(ClientManager::ClientId client_id, ClientManager::RequestId request_id,
            td_api::object_ptr<td_api::Function> &&request) {
    auto &td = tds_[client_id];
    CHECK(!td.empty());
    send_closure(td, &Td::request, request_id, std::move(request));
  }

  void close(int32 td_id) {
    // no check that td_id hasn't been deleted before
    tds_.erase(td_id);
  }

 private:
  Td::Options options_;
  std::unordered_map<int32, ActorOwn<Td>> tds_;
};

class TdReceiver {
 public:
  TdReceiver() {
    output_queue_ = std::make_shared<OutputQueue>();
    output_queue_->init();
  }

  ClientManager::Response receive(double timeout) {
    VLOG(td_requests) << "Begin to wait for updates with timeout " << timeout;
    auto is_locked = receive_lock_.exchange(true);
    CHECK(!is_locked);
    auto response = receive_unlocked(timeout);
    is_locked = receive_lock_.exchange(false);
    CHECK(is_locked);
    VLOG(td_requests) << "End to wait for updates, returning object " << response.request_id << ' '
                      << response.object.get();
    return response;
  }

  unique_ptr<TdCallback> create_callback(ClientManager::ClientId client_id) {
    class Callback : public TdCallback {
     public:
      explicit Callback(ClientManager::ClientId client_id, std::shared_ptr<OutputQueue> output_queue)
          : client_id_(client_id), output_queue_(std::move(output_queue)) {
      }
      void on_result(uint64 id, td_api::object_ptr<td_api::Object> result) override {
        output_queue_->writer_put({client_id_, id, std::move(result)});
      }
      void on_error(uint64 id, td_api::object_ptr<td_api::error> error) override {
        output_queue_->writer_put({client_id_, id, std::move(error)});
      }
      Callback(const Callback &) = delete;
      Callback &operator=(const Callback &) = delete;
      Callback(Callback &&) = delete;
      Callback &operator=(Callback &&) = delete;
      ~Callback() override {
        output_queue_->writer_put({client_id_, 0, nullptr});
      }

     private:
      ClientManager::ClientId client_id_;
      std::shared_ptr<OutputQueue> output_queue_;
    };
    return td::make_unique<Callback>(client_id, output_queue_);
  }

  void add_response(ClientManager::ClientId client_id, uint64 id, td_api::object_ptr<td_api::Object> result) {
    output_queue_->writer_put({client_id, id, std::move(result)});
  }

 private:
  using OutputQueue = MpscPollableQueue<ClientManager::Response>;
  std::shared_ptr<OutputQueue> output_queue_;
  int output_queue_ready_cnt_{0};
  std::atomic<bool> receive_lock_{false};

  ClientManager::Response receive_unlocked(double timeout) {
    if (output_queue_ready_cnt_ == 0) {
      output_queue_ready_cnt_ = output_queue_->reader_wait_nonblock();
    }
    if (output_queue_ready_cnt_ > 0) {
      output_queue_ready_cnt_--;
      return output_queue_->reader_get_unsafe();
    }
    if (timeout != 0) {
      output_queue_->reader_get_event_fd().wait(static_cast<int>(timeout * 1000));
      return receive_unlocked(0);
    }
    return {0, 0, nullptr};
  }
};

class MultiImpl {
 public:
  explicit MultiImpl(std::shared_ptr<NetQueryStats> net_query_stats) {
    concurrent_scheduler_ = std::make_shared<ConcurrentScheduler>();
    concurrent_scheduler_->init(3);
    concurrent_scheduler_->start();

    {
      auto guard = concurrent_scheduler_->get_main_guard();
      Td::Options options;
      options.net_query_stats = std::move(net_query_stats);
      multi_td_ = create_actor<MultiTd>("MultiTd", std::move(options));
    }

    scheduler_thread_ = thread([concurrent_scheduler = concurrent_scheduler_] {
      while (concurrent_scheduler->run_main(10)) {
      }
    });
  }
  MultiImpl(const MultiImpl &) = delete;
  MultiImpl &operator=(const MultiImpl &) = delete;
  MultiImpl(MultiImpl &&) = delete;
  MultiImpl &operator=(MultiImpl &&) = delete;

  int32 create(TdReceiver &receiver) {
    auto id = create_id();
    create(id, receiver.create_callback(id));
    return id;
  }

  void send(ClientManager::ClientId client_id, ClientManager::RequestId request_id,
            td_api::object_ptr<td_api::Function> &&request) {
    auto guard = concurrent_scheduler_->get_send_guard();
    send_closure(multi_td_, &MultiTd::send, client_id, request_id, std::move(request));
  }

  void close(ClientManager::ClientId client_id) {
    auto guard = concurrent_scheduler_->get_send_guard();
    send_closure(multi_td_, &MultiTd::close, client_id);
  }

  ~MultiImpl() {
    {
      auto guard = concurrent_scheduler_->get_send_guard();
      multi_td_.reset();
      Scheduler::instance()->finish();
    }
    scheduler_thread_.join();
    concurrent_scheduler_->finish();
  }

 private:
  std::shared_ptr<ConcurrentScheduler> concurrent_scheduler_;
  thread scheduler_thread_;
  ActorOwn<MultiTd> multi_td_;

  static int32 create_id() {
    static std::atomic<int32> current_id{1};
    return current_id.fetch_add(1);
  }

  void create(int32 td_id, unique_ptr<TdCallback> callback) {
    auto guard = concurrent_scheduler_->get_send_guard();
    send_closure(multi_td_, &MultiTd::create, td_id, std::move(callback));
  }
};

class MultiImplPool {
 public:
  std::shared_ptr<MultiImpl> get() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (impls_.empty()) {
      init_openssl_threads();

      impls_.resize(clamp(thread::hardware_concurrency(), 8u, 1000u) * 5 / 4);
    }
    auto &impl = *std::min_element(impls_.begin(), impls_.end(),
                                   [](auto &a, auto &b) { return a.lock().use_count() < b.lock().use_count(); });
    auto res = impl.lock();
    if (!res) {
      res = std::make_shared<MultiImpl>(net_query_stats_);
      impl = res;
    }
    return res;
  }

 private:
  std::mutex mutex_;
  std::vector<std::weak_ptr<MultiImpl>> impls_;
  std::shared_ptr<NetQueryStats> net_query_stats_ = std::make_shared<NetQueryStats>();
};

class ClientManager::Impl final {
 public:
  ClientId create_client() {
    auto impl = pool_.get();
    auto client_id = impl->create(*receiver_);
    {
      auto lock = impls_mutex_.lock_write().move_as_ok();
      impls_[client_id] = std::move(impl);
    }
    return client_id;
  }

  void send(ClientId client_id, RequestId request_id, td_api::object_ptr<td_api::Function> &&request) {
    auto lock = impls_mutex_.lock_read().move_as_ok();
    auto it = impls_.find(client_id);
    if (it == impls_.end()) {
      receiver_->add_response(client_id, request_id,
                              td_api::make_object<td_api::error>(400, "Invalid TDLib instance specified"));
      return;
    }
    it->second->send(client_id, request_id, std::move(request));
  }

  Response receive(double timeout) {
    auto response = receiver_->receive(timeout);
    if (response.object == nullptr && response.client_id != 0 && response.request_id == 0) {
      auto lock = impls_mutex_.lock_write().move_as_ok();
      impls_.erase(response.client_id);
    }
    return response;
  }

  Impl() = default;
  Impl(const Impl &) = delete;
  Impl &operator=(const Impl &) = delete;
  Impl(Impl &&) = delete;
  Impl &operator=(Impl &&) = delete;
  ~Impl() {
    for (auto &it : impls_) {
      it.second->close(it.first);
    }
    while (!impls_.empty()) {
      receive(10);
    }
  }

 private:
  MultiImplPool pool_;
  RwMutex impls_mutex_;
  std::unordered_map<ClientId, std::shared_ptr<MultiImpl>> impls_;
  unique_ptr<TdReceiver> receiver_{make_unique<TdReceiver>()};
};

class Client::Impl final {
 public:
  Impl() {
    static MultiImplPool pool;
    multi_impl_ = pool.get();
    receiver_ = make_unique<TdReceiver>();
    td_id_ = multi_impl_->create(*receiver_);
  }

  void send(Client::Request request) {
    if (request.id == 0 || request.function == nullptr) {
      LOG(ERROR) << "Drop wrong request " << request.id;
      return;
    }

    multi_impl_->send(td_id_, request.id, std::move(request.function));
  }

  Client::Response receive(double timeout) {
    auto res = receiver_->receive(timeout);

    Client::Response old_res;
    old_res.id = res.request_id;
    old_res.object = std::move(res.object);
    return old_res;
  }

  Impl(const Impl &) = delete;
  Impl &operator=(const Impl &) = delete;
  Impl(Impl &&) = delete;
  Impl &operator=(Impl &&) = delete;
  ~Impl() {
    multi_impl_->close(td_id_);
    while (true) {
      auto response = receiver_->receive(10.0);
      if (response.object == nullptr && response.client_id != 0 && response.request_id == 0) {
        break;
      }
    }
  }

 private:
  std::shared_ptr<MultiImpl> multi_impl_;
  unique_ptr<TdReceiver> receiver_;

  int32 td_id_;
};
#endif

Client::Client() : impl_(std::make_unique<Impl>()) {
}

void Client::send(Request &&request) {
  impl_->send(std::move(request));
}

Client::Response Client::receive(double timeout) {
  return impl_->receive(timeout);
}

Client::Response Client::execute(Request &&request) {
  Response response;
  response.id = request.id;
  response.object = Td::static_request(std::move(request.function));
  return response;
}

Client::~Client() = default;
Client::Client(Client &&other) = default;
Client &Client::operator=(Client &&other) = default;

ClientManager::ClientManager() : impl_(std::make_unique<Impl>()) {
}

ClientManager::ClientId ClientManager::create_client() {
  return impl_->create_client();
}

void ClientManager::send(ClientId client_id, RequestId request_id, td_api::object_ptr<td_api::Function> &&request) {
  impl_->send(client_id, request_id, std::move(request));
}

ClientManager::Response ClientManager::receive(double timeout) {
  return impl_->receive(timeout);
}

td_api::object_ptr<td_api::Object> ClientManager::execute(td_api::object_ptr<td_api::Function> &&request) {
  return Td::static_request(std::move(request));
}

ClientManager::~ClientManager() = default;
ClientManager::ClientManager(ClientManager &&other) = default;
ClientManager &ClientManager::operator=(ClientManager &&other) = default;

}  // namespace td

// Copyright 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "google/cloud/spanner/internal/transaction_impl.h"
#include "google/cloud/spanner/testing/mock_spanner_stub.h"
#include "google/cloud/spanner/timestamp.h"
#include "google/cloud/spanner/transaction.h"
#include "google/cloud/internal/port_platform.h"
#include <gmock/gmock.h>
#include <array>
#include <chrono>
#include <ctime>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace google {
namespace cloud {
namespace spanner_internal {
GOOGLE_CLOUD_CPP_INLINE_NAMESPACE_BEGIN
namespace {

using ::google::spanner::v1::TransactionSelector;
using ::testing::IsEmpty;
using ::testing::IsNull;
using ::testing::NotNull;

class KeySet {};
class ResultSet {};

// A fake `google::cloud::spanner::Client`, supporting a single `Read()`
// operation that does nothing but track the expected transaction callbacks.
class Client {
 public:
  enum class Mode {
    kReadSucceeds,  // and assigns a transaction ID
    kReadFailsAndTxnRemainsBegin,
    kReadFailsAndTxnInvalidated,
  };

  explicit Client(Mode mode) : mode_(mode) {
    for (std::size_t i = 0; i < stubs_.max_size(); ++i) {
      stubs_[i] =
          std::shared_ptr<SpannerStub>(new spanner_testing::MockSpannerStub());
    }
    next_stub_ = stubs_.begin();
  }

  // Set the `read_timestamp` we expect to see, and the `session_id` and
  // `txn_id` we want to use during the upcoming `Read()` calls.
  void Reset(spanner::Timestamp read_timestamp, std::string session_id,
             std::string txn_id,
             absl::optional<std::shared_ptr<SpannerStub>> stub) {
    read_timestamp_ = read_timestamp;
    session_id_ = std::move(session_id);
    txn_id_ = std::move(txn_id);
    expected_stub_ = std::move(stub);
    std::unique_lock<std::mutex> lock(mu_);
    valid_visits_ = 0;
  }

  // Return the number of valid visitations made to the transaction during a
  // completed set of `Read()` calls.
  int ValidVisits() {
    std::unique_lock<std::mutex> lock(mu_);
    return valid_visits_;
  }

  // User-visible read operation.
  ResultSet Read(spanner::Transaction txn, std::string const& table,
                 KeySet const& keys, std::vector<std::string> const& columns) {
    auto read = [this, &table, &keys, &columns](
                    SessionHolder& session,
                    StatusOr<TransactionSelector>& selector,
                    TransactionContext& ctx) {
      return this->Read(session, selector, ctx, table, keys, columns);
    };
#if GOOGLE_CLOUD_CPP_HAVE_EXCEPTIONS
    try {
#endif
      return Visit(std::move(txn), std::move(read));
#if GOOGLE_CLOUD_CPP_HAVE_EXCEPTIONS
    } catch (char const*) {
      return {};
    }
#endif
  }

  std::array<std::shared_ptr<SpannerStub>, 3> const& stubs() const {
    return stubs_;
  }

 private:
  ResultSet Read(SessionHolder& session,
                 StatusOr<TransactionSelector>& selector,
                 TransactionContext& ctx, std::string const& table,
                 KeySet const& keys, std::vector<std::string> const& columns);

  bool NoSelector(StatusOr<TransactionSelector>& selector);

  bool SelectorHasBegin(SessionHolder& session,
                        StatusOr<TransactionSelector>& selector,
                        TransactionContext& ctx);

  void SelectorHasId(SessionHolder& session,
                     StatusOr<TransactionSelector>& selector,
                     TransactionContext& ctx);

  std::shared_ptr<SpannerStub> GetStub() { return *next_stub_++; }

  Mode mode_;
  spanner::Timestamp read_timestamp_;
  std::string session_id_;
  std::string txn_id_;
  absl::optional<std::shared_ptr<SpannerStub>> expected_stub_;
  std::array<std::shared_ptr<SpannerStub>, 3>::iterator next_stub_;
  std::array<std::shared_ptr<SpannerStub>, 3> stubs_;
  std::mutex mu_;
  std::int64_t begin_seqno_{0};  // GUARDED_BY(mu_)
  int valid_visits_;             // GUARDED_BY(mu_)
};

bool Client::NoSelector(StatusOr<TransactionSelector>& selector) {
  // when we mark a transaction invalid, we use this Status.
  Status const failed_txn_status(StatusCode::kInternal, "Bad transaction");
  bool fail_with_throw = false;
  std::unique_lock<std::mutex> lock(mu_);
  switch (mode_) {
    case Mode::kReadSucceeds:  // visits never valid
    case Mode::kReadFailsAndTxnRemainsBegin:
      break;
    case Mode::kReadFailsAndTxnInvalidated:
      EXPECT_EQ(selector.status(), failed_txn_status);
      ++valid_visits_;
      fail_with_throw = (valid_visits_ % 2 == 0);
      break;
  }
  return fail_with_throw;
}

bool Client::SelectorHasBegin(SessionHolder& session,
                              StatusOr<TransactionSelector>& selector,
                              TransactionContext& ctx) {
  // when we mark a transaction invalid, we use this Status.
  Status const failed_txn_status(StatusCode::kInternal, "Bad transaction");
  bool fail_with_throw = false;
  EXPECT_THAT(session, IsNull());
  if (selector->begin().has_read_only() &&
      selector->begin().read_only().has_read_timestamp()) {
    auto const& proto = selector->begin().read_only().read_timestamp();
    if (spanner::MakeTimestamp(proto).value() == read_timestamp_ &&
        ctx.seqno > 0) {
      std::unique_lock<std::mutex> lock(mu_);
      switch (mode_) {
        case Mode::kReadSucceeds:  // first visit valid
          if (valid_visits_ == 0) ++valid_visits_;
          break;
        case Mode::kReadFailsAndTxnRemainsBegin:  // visits always valid
        case Mode::kReadFailsAndTxnInvalidated:
          ++valid_visits_;
          fail_with_throw = (valid_visits_ % 2 == 0);
          break;
      }
      if (valid_visits_ != 0) begin_seqno_ = ctx.seqno;
    }
  }
  std::shared_ptr<SpannerStub> stub;
  switch (mode_) {
    case Mode::kReadSucceeds:
      // `begin` -> `id`, calls now parallelized
      session = MakeDissociatedSessionHolder(session_id_);
      ctx.stub = GetStub();
      selector->set_id(txn_id_);
      break;
    case Mode::kReadFailsAndTxnRemainsBegin:
      // leave as `begin`, calls stay serialized
      break;
    case Mode::kReadFailsAndTxnInvalidated:
      // `begin` -> `error`, calls now parallelized
      selector = failed_txn_status;
      break;
  }
  return fail_with_throw;
}

void Client::SelectorHasId(SessionHolder& session,
                           StatusOr<TransactionSelector>& selector,
                           TransactionContext& ctx) {
  if (selector->id() == txn_id_) {
    EXPECT_THAT(session, NotNull());
    EXPECT_EQ(session_id_, session->session_name());
    EXPECT_THAT(expected_stub_, ::testing::Optional(ctx.stub));

    std::unique_lock<std::mutex> lock(mu_);
    switch (mode_) {
      case Mode::kReadSucceeds:  // non-initial visits valid
        if (valid_visits_ != 0 && ctx.seqno > begin_seqno_) ++valid_visits_;
        break;
      case Mode::kReadFailsAndTxnRemainsBegin:  // visits never valid
      case Mode::kReadFailsAndTxnInvalidated:
        break;
    }
  }
}

// Transaction callback.  Normally we would use the TransactionSelector
// to make a StreamingRead() RPC, and then, if the selector was a `begin`,
// switch the selector to use the allocated transaction ID.  Here we use
// the pre-assigned transaction ID after checking the read timestamp.
ResultSet Client::Read(SessionHolder& session,
                       StatusOr<TransactionSelector>& selector,
                       TransactionContext& ctx, std::string const&,
                       KeySet const&, std::vector<std::string> const&) {
  bool fail_with_throw = false;
  EXPECT_THAT(ctx.tag, IsEmpty());
  if (!selector) {
    fail_with_throw = NoSelector(selector);
  } else if (selector->has_begin()) {
    fail_with_throw = SelectorHasBegin(session, selector, ctx);
  } else {
    SelectorHasId(session, selector, ctx);
  }
  if (fail_with_throw) {
#if GOOGLE_CLOUD_CPP_HAVE_EXCEPTIONS
    throw "1202 Program Alarm - Executive Overflow - No VAC Areas.";
#endif
  }
  // kReadSucceeds -v- kReadFailsAnd* is all about whether we assign a
  // transaction ID, not about what we return here (which is never used).
  return {};
}

// Call `client->Read()` from multiple threads in the context of a single,
// read-only transaction with an exact-staleness timestamp, and return the
// number of valid visitations to that transaction (should be `n_threads`).
int MultiThreadedRead(int n_threads, Client* client, std::time_t read_time,
                      std::string const& session_id, std::string const& txn_id,
                      absl::optional<std::shared_ptr<SpannerStub>> stub) {
  auto read_timestamp =
      spanner::MakeTimestamp(std::chrono::system_clock::from_time_t(read_time))
          .value();
  client->Reset(read_timestamp, session_id, txn_id, std::move(stub));

  spanner::Transaction::ReadOnlyOptions opts(read_timestamp);
  spanner::Transaction txn(opts);

  // Unused Read() parameters.
  std::string const table{};
  KeySet const keys{};
  std::vector<std::string> const columns{};

  std::promise<void> ready_promise;
  std::shared_future<void> ready_future(ready_promise.get_future());
  auto read = [&](std::promise<void>* started) {
    started->set_value();
    ready_future.wait();                      // wait for go signal
    client->Read(txn, table, keys, columns);  // ignore ResultSet
  };

  std::vector<std::thread> threads;
  for (int i = 0; i < n_threads; ++i) {
    std::promise<void> started;
    threads.emplace_back(read, &started);
    started.get_future().wait();  // wait until thread running
  }
  ready_promise.set_value();  // go!
  for (auto& thread : threads) {
    thread.join();
  }

  return client->ValidVisits();  // should be n_threads
}

TEST(InternalTransaction, ReadSucceeds) {
  Client client(Client::Mode::kReadSucceeds);
  EXPECT_EQ(1, MultiThreadedRead(1, &client, 1562359982, "sess0", "txn0",
                                 client.stubs()[0]));
  EXPECT_EQ(64, MultiThreadedRead(64, &client, 1562360571, "sess1", "txn1",
                                  client.stubs()[1]));
  EXPECT_EQ(128, MultiThreadedRead(128, &client, 1562361252, "sess2", "txn2",
                                   client.stubs()[2]));
}

TEST(InternalTransaction, ReadFailsAndTxnRemainsBegin) {
  Client client(Client::Mode::kReadFailsAndTxnRemainsBegin);
  EXPECT_EQ(1, MultiThreadedRead(1, &client, 1562359982, "sess0", "txn0",
                                 absl::nullopt));
  EXPECT_EQ(64, MultiThreadedRead(64, &client, 1562360571, "sess1", "txn1",
                                  absl::nullopt));
  EXPECT_EQ(128, MultiThreadedRead(128, &client, 1562361252, "sess2", "txn2",
                                   absl::nullopt));
}

TEST(InternalTransaction, ReadFailsAndTxnInvalidated) {
  Client client(Client::Mode::kReadFailsAndTxnInvalidated);
  EXPECT_EQ(1, MultiThreadedRead(1, &client, 1562359982, "sess0", "txn0",
                                 absl::nullopt));
  EXPECT_EQ(64, MultiThreadedRead(64, &client, 1562360571, "sess1", "txn1",
                                  absl::nullopt));
  EXPECT_EQ(128, MultiThreadedRead(128, &client, 1562361252, "sess2", "txn2",
                                   absl::nullopt));
}

}  // namespace
GOOGLE_CLOUD_CPP_INLINE_NAMESPACE_END
}  // namespace spanner_internal
}  // namespace cloud
}  // namespace google

/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <proxygen/lib/http/session/HQUpstreamSession.h>

#include <folly/futures/Future.h>
#include <folly/io/async/EventBaseManager.h>
#include <folly/portability/GTest.h>
#include <limits>
#include <proxygen/lib/http/HTTPHeaderSize.h>
#include <proxygen/lib/http/codec/HQControlCodec.h>
#include <proxygen/lib/http/codec/HQStreamCodec.h>
#include <proxygen/lib/http/codec/HQUnidirectionalCodec.h>
#include <proxygen/lib/http/codec/HTTP1xCodec.h>
#include <proxygen/lib/http/codec/test/TestUtils.h>
#include <proxygen/lib/http/session/HQStreamLookup.h>
#include <proxygen/lib/http/session/test/HQSessionMocks.h>
#include <proxygen/lib/http/session/test/HQSessionTestCommon.h>
#include <proxygen/lib/http/session/test/HTTPSessionMocks.h>
#include <proxygen/lib/http/session/test/HTTPTransactionMocks.h>
#include <proxygen/lib/http/session/test/MockQuicSocketDriver.h>
#include <proxygen/lib/http/session/test/TestUtils.h>
#include <quic/api/test/MockQuicSocket.h>
#include <wangle/acceptor/ConnectionManager.h>

using namespace proxygen;
using namespace proxygen::hq;
using namespace quic;
using namespace folly;
using namespace testing;
using namespace std::chrono;

namespace {
constexpr quic::StreamId kQPACKEncoderIngressStreamId = 7;
constexpr quic::StreamId kQPACKDecoderEgressStreamId = 10;
} // namespace

class HQUpstreamSessionTest : public HQSessionTest {
 public:
  HQUpstreamSessionTest()
      : HQSessionTest(proxygen::TransportDirection::UPSTREAM) {
  }

 protected:
  std::pair<HTTPCodec::StreamID, std::unique_ptr<HTTPCodec>> makeCodec(
      HTTPCodec::StreamID id) {
    if (IS_HQ) {
      return {id,
              std::make_unique<hq::HQStreamCodec>(
                  id,
                  TransportDirection::DOWNSTREAM,
                  qpackCodec_,
                  encoderWriteBuf_,
                  decoderWriteBuf_,
                  [] { return std::numeric_limits<uint64_t>::max(); },
                  egressSettings_,
                  ingressSettings_,
                  GetParam().prParams.hasValue())};
    } else {
      auto codec =
          std::make_unique<HTTP1xCodec>(TransportDirection::DOWNSTREAM, true);
      // When the codec is created, need to fake the request
      FakeHTTPCodecCallback cb;
      codec->setCallback(&cb);
      codec->onIngress(*folly::IOBuf::copyBuffer("GET / HTTP/1.1\r\n\r\n"));
      return {1, std::move(codec)};
    }
  }

  void sendResponse(quic::StreamId id,
                    const HTTPMessage& resp,
                    std::unique_ptr<folly::IOBuf> body = nullptr,
                    bool eom = true) {
    auto c = makeCodec(id);
    auto res =
        streams_.emplace(std::piecewise_construct,
                         std::forward_as_tuple(id),
                         std::forward_as_tuple(c.first, std::move(c.second)));
    auto& stream = res.first->second;
    stream.readEOF = eom;
    stream.codec->generateHeader(
        stream.buf, stream.codecId, resp, body == nullptr ? eom : false);
    if (body && body->computeChainDataLength() > 0) {
      stream.codec->generateBody(
          stream.buf, stream.codecId, std::move(body), folly::none, eom);
    }
  }

  void startPartialResponse(quic::StreamId id,
                            const HTTPMessage& resp,
                            std::unique_ptr<folly::IOBuf> body = nullptr) {
    auto c = makeCodec(id);
    auto res =
        streams_.emplace(std::piecewise_construct,
                         std::forward_as_tuple(id),
                         std::forward_as_tuple(c.first, std::move(c.second)));
    auto& stream = res.first->second;
    stream.readEOF = false;

    const uint64_t frameHeaderSize = 2;
    HTTPHeaderSize headerSize;
    stream.codec->generateHeader(
        stream.buf, stream.codecId, resp, false, &headerSize);
    socketDriver_->streams_[id].writeBufOffset +=
        (2 * frameHeaderSize) + headerSize.compressed;

    if (body) {
      socketDriver_->streams_[id].writeBufOffset += stream.codec->generateBody(
          stream.buf, stream.codecId, std::move(body), folly::none, false);
    }
  }

  void sendPartialBody(quic::StreamId id,
                       std::unique_ptr<folly::IOBuf> body,
                       bool eom = true) {
    auto it = streams_.find(id);
    CHECK(it != streams_.end());
    auto& stream = it->second;

    stream.readEOF = eom;
    if (body) {
      socketDriver_->streams_[id].writeBufOffset += stream.codec->generateBody(
          stream.buf, stream.codecId, std::move(body), folly::none, eom);
    }
  }

  void peerSendDataExpired(quic::StreamId id, uint64_t streamOffset) {
    auto it = streams_.find(id);
    CHECK(it != streams_.end());
    auto& stream = it->second;

    HQStreamCodec* hqStreamCodec = (HQStreamCodec*)stream.codec.get();
    hqStreamCodec->onEgressBodySkip(streamOffset);
  }

  void peerReceiveDataRejected(quic::StreamId id, uint64_t streamOffset) {
    auto it = streams_.find(id);
    CHECK(it != streams_.end());
    auto& stream = it->second;

    HQStreamCodec* hqStreamCodec = (HQStreamCodec*)stream.codec.get();
    hqStreamCodec->onIngressDataRejected(streamOffset);
  }

  quic::StreamId nextUnidirectionalStreamId() {
    auto id = nextUnidirectionalStreamId_;
    nextUnidirectionalStreamId_ += 4;
    return id;
  }

  void SetUp() override {
    folly::EventBaseManager::get()->clearEventBase();
    localAddress_.setFromIpPort("0.0.0.0", 0);
    peerAddress_.setFromIpPort("127.0.0.0", 443);
    EXPECT_CALL(*socketDriver_->getSocket(), getLocalAddress())
        .WillRepeatedly(ReturnRef(localAddress_));

    EXPECT_CALL(*socketDriver_->getSocket(), getPeerAddress())
        .WillRepeatedly(ReturnRef(peerAddress_));
    EXPECT_CALL(*socketDriver_->getSocket(), getAppProtocol())
        .WillRepeatedly(Return(getProtocolString()));
    HTTPSession::setDefaultWriteBufferLimit(65536);
    HTTP2PriorityQueue::setNodeLifetime(std::chrono::milliseconds(2));
    dynamic_cast<HQUpstreamSession*>(hqSession_)
        ->setConnectCallback(&connectCb_);

    EXPECT_CALL(connectCb_, connectSuccess());

    hqSession_->onTransportReady();

    createControlStreams();

    flushAndLoop();
    if (IS_HQ) {
      EXPECT_EQ(httpCallbacks_.settings, 1);
    }
  }

  void TearDown() override {
    if (!IS_H1Q_FB_V1) {
      // With control streams we may need an extra loop for proper shutdown
      if (!socketDriver_->isClosed()) {
        // Send the first GOAWAY with MAX_STREAM_ID immediately
        sendGoaway(quic::kEightByteLimit);
        // Schedule the second GOAWAY with the last seen stream ID, after some
        // delay
        sendGoaway(socketDriver_->getMaxStreamId(), milliseconds(50));
      }
      eventBase_.loopOnce();
    }
  }

  void sendGoaway(quic::StreamId lastStreamId,
                  milliseconds delay = milliseconds(0)) {
    folly::IOBufQueue writeBuf{folly::IOBufQueue::cacheChainLength()};
    egressControlCodec_->generateGoaway(
        writeBuf, lastStreamId, ErrorCode::NO_ERROR);
    socketDriver_->addReadEvent(connControlStreamId_, writeBuf.move(), delay);
  }

  template <class HandlerType>
  std::unique_ptr<StrictMock<HandlerType>> openTransactionBase(
      bool expectStartPaused = false) {
    // Returns a mock handler with txn_ field set in it
    auto handler = std::make_unique<StrictMock<HandlerType>>();
    handler->expectTransaction();
    if (expectStartPaused) {
      handler->expectEgressPaused();
    }
    HTTPTransaction* txn = hqSession_->newTransaction(handler.get());
    EXPECT_EQ(txn, handler->txn_);
    return handler;
  }

  std::unique_ptr<StrictMock<MockHTTPHandler>> openTransaction() {
    return openTransactionBase<MockHTTPHandler>();
  }

  void flushAndLoop(
      bool eof = false,
      milliseconds eofDelay = milliseconds(0),
      milliseconds initialDelay = milliseconds(0),
      std::function<void()> extraEventsFn = std::function<void()>()) {
    flush(eof, eofDelay, initialDelay, extraEventsFn);
    CHECK(eventBase_.loop());
  }

  void flushAndLoopN(
      uint64_t n,
      bool eof = false,
      milliseconds eofDelay = milliseconds(0),
      milliseconds initialDelay = milliseconds(0),
      std::function<void()> extraEventsFn = std::function<void()>()) {
    flush(eof, eofDelay, initialDelay, extraEventsFn);
    for (uint64_t i = 0; i < n; i++) {
      eventBase_.loopOnce();
    }
  }

  bool flush(bool eof = false,
             milliseconds eofDelay = milliseconds(0),
             milliseconds initialDelay = milliseconds(0),
             std::function<void()> extraEventsFn = std::function<void()>()) {
    bool done = true;
    if (!encoderWriteBuf_.empty()) {
      socketDriver_->addReadEvent(kQPACKEncoderIngressStreamId,
                                  encoderWriteBuf_.move(),
                                  milliseconds(0));
    }
    for (auto& stream : streams_) {
      if (socketDriver_->isStreamIdle(stream.first)) {
        continue;
      }
      if (stream.second.buf.chainLength() > 0) {
        socketDriver_->addReadEvent(
            stream.first, stream.second.buf.move(), initialDelay);
        done = false;
      }
      // EOM -> stream EOF
      if (stream.second.readEOF) {
        socketDriver_->addReadEOF(stream.first, eofDelay);
        done = false;
      }
    }

    if (extraEventsFn) {
      extraEventsFn();
    }
    if (eof || eofDelay.count() > 0) {
      /*  wonkiness.  Should somehow close the connection?
       * socketDriver_->addReadEOF(1, eofDelay);
       */
    }
    return done;
  }

  StrictMock<MockController>& getMockController() {
    return controllerContainer_.mockController;
  }

  // Representation of stream data
  // If create with a push id, can be used
  // as a push stream (requires writing the stream preface
  // followed by unframed push id)
  struct ServerStream {
    ServerStream(HTTPCodec::StreamID cId,
                 std::unique_ptr<HTTPCodec> c,
                 folly::Optional<hq::PushId> pId = folly::none)
        : codecId(cId), codec(std::move(c)), pushId(pId) {
    }

    // Transport stream id
    HTTPCodec::StreamID id;

    IOBufQueue buf{IOBufQueue::cacheChainLength()};
    bool readEOF{false};
    HTTPCodec::StreamID codecId;

    std::unique_ptr<HTTPCodec> codec;

    folly::Optional<hq::PushId> pushId;
  };

  MockConnectCallback connectCb_;
  std::unordered_map<quic::StreamId, ServerStream> streams_;
  folly::IOBufQueue encoderWriteBuf_{folly::IOBufQueue::cacheChainLength()};
  folly::IOBufQueue decoderWriteBuf_{folly::IOBufQueue::cacheChainLength()};
};

// Use this test class for h1q-fb only tests
using HQUpstreamSessionTestH1q = HQUpstreamSessionTest;
// Use this test class for h1q-fb-v1 only tests
using HQUpstreamSessionTestH1qv1 = HQUpstreamSessionTest;
// Use this test class for h1q-fb-v2 only tests
using HQUpstreamSessionTestH1qv2 = HQUpstreamSessionTest;
// Use this test class for h1q-fb-v2 and hq tests
using HQUpstreamSessionTestH1qv2HQ = HQUpstreamSessionTest;
// Use this test class for hq only tests
using HQUpstreamSessionTestHQ = HQUpstreamSessionTest;

class HQUpstreamSessionPRTest : public HQUpstreamSessionTest {
 public:
  void SetUp() override {
    // propagate setup call
    HQUpstreamSessionTest::SetUp();
    // enable callbacks
    socketDriver_->enablePartialReliability();
  }

  void TearDown() override {
    // propagate tear down call
    HQUpstreamSessionTest::TearDown();
  }

  std::unique_ptr<StrictMock<MockHqPrUpstreamHTTPHandler>> openPrTransaction() {
    return openTransactionBase<MockHqPrUpstreamHTTPHandler>();
  }
};

// Use this test class for hq PR general tests
using HQUpstreamSessionTestHQPR = HQUpstreamSessionPRTest;
// Use this test class for hq PR scripted recv tests
using HQUpstreamSessionTestHQPRRecvBodyScripted = HQUpstreamSessionPRTest;
using HQUpstreamSessionTestHQPRDeliveryAck = HQUpstreamSessionPRTest;

TEST_P(HQUpstreamSessionTest, SimpleGet) {
  auto handler = openTransaction();
  handler->txn_->sendHeaders(getGetRequest());
  handler->txn_->sendEOM();
  handler->expectHeaders();
  handler->expectBody();
  handler->expectEOM();
  handler->expectDetachTransaction();
  auto resp = makeResponse(200, 100);
  sendResponse(handler->txn_->getID(),
               *std::get<0>(resp),
               std::move(std::get<1>(resp)),
               true);
  flushAndLoop();
  hqSession_->closeWhenIdle();
}

TEST_P(HQUpstreamSessionTest, NoNewTransactionIfSockIsNotGood) {
  socketDriver_->sockGood_ = false;
  EXPECT_EQ(hqSession_->newTransaction(nullptr), nullptr);
  hqSession_->closeWhenIdle();
}

TEST_P(HQUpstreamSessionTest, DropConnectionWithEarlyDataFailedError) {
  auto handler = openTransaction();
  handler->txn_->sendHeaders(getGetRequest());
  handler->txn_->sendEOM();

  EXPECT_CALL(*handler, onError(_))
      .WillOnce(Invoke([](const HTTPException& error) {
        EXPECT_EQ(error.getProxygenError(), kErrorEarlyDataFailed);
        EXPECT_TRUE(std::string(error.what()).find("quic loses race") !=
                    std::string::npos);
      }));
  handler->expectDetachTransaction();
  socketDriver_->deliverConnectionError(
      {HTTP3::ErrorCode::GIVEUP_ZERO_RTT, "quic loses race"});
}

TEST_P(HQUpstreamSessionTest, ResponseTermedByFin) {
  auto handler = openTransaction();
  handler->txn_->sendHeaders(getGetRequest());
  handler->txn_->sendEOM();
  handler->expectHeaders();
  handler->expectBody();
  handler->expectEOM();
  handler->expectDetachTransaction();
  HTTPMessage resp;
  resp.setStatusCode(200);
  resp.setHTTPVersion(1, 0);
  // HTTP/1.0 response with no content-length, termed by tranport FIN
  sendResponse(handler->txn_->getID(), resp, makeBuf(100), true);
  flushAndLoop();
  hqSession_->closeWhenIdle();
}

TEST_P(HQUpstreamSessionTest, WaitForReplaySafeCallback) {
  auto handler = openTransaction();
  StrictMock<folly::test::MockReplaySafetyCallback> cb1;
  StrictMock<folly::test::MockReplaySafetyCallback> cb2;
  StrictMock<folly::test::MockReplaySafetyCallback> cb3;

  auto sock = socketDriver_->getSocket();
  EXPECT_CALL(*sock, replaySafe()).WillRepeatedly(Return(false));
  handler->txn_->addWaitingForReplaySafety(&cb1);
  handler->txn_->addWaitingForReplaySafety(&cb2);
  handler->txn_->addWaitingForReplaySafety(&cb3);
  handler->txn_->removeWaitingForReplaySafety(&cb2);

  ON_CALL(*sock, replaySafe()).WillByDefault(Return(true));
  EXPECT_CALL(cb1, onReplaySafe_());
  EXPECT_CALL(cb3, onReplaySafe_());
  hqSession_->onReplaySafe();

  handler->expectDetachTransaction();
  handler->txn_->sendAbort();
  hqSession_->closeWhenIdle();
  eventBase_.loopOnce();
}

TEST_P(HQUpstreamSessionTest, AlreadyReplaySafe) {
  auto handler = openTransaction();

  StrictMock<folly::test::MockReplaySafetyCallback> cb;

  auto sock = socketDriver_->getSocket();
  EXPECT_CALL(*sock, replaySafe()).WillRepeatedly(Return(true));
  EXPECT_CALL(cb, onReplaySafe_());
  handler->txn_->addWaitingForReplaySafety(&cb);

  handler->expectDetachTransaction();
  handler->txn_->sendAbort();
  hqSession_->closeWhenIdle();
  eventBase_.loopOnce();
}

TEST_P(HQUpstreamSessionTest, Test100Continue) {
  InSequence enforceOrder;
  auto handler = openTransaction();
  auto req = getPostRequest(10);
  req.getHeaders().add(HTTP_HEADER_EXPECT, "100-continue");
  handler->txn_->sendHeaders(req);
  handler->txn_->sendEOM();
  handler->expectHeaders();
  handler->expectHeaders();
  handler->expectBody();
  handler->expectEOM();
  handler->expectDetachTransaction();
  sendResponse(handler->txn_->getID(), *makeResponse(100), nullptr, false);
  auto resp = makeResponse(200, 100);
  sendResponse(handler->txn_->getID(),
               *std::get<0>(resp),
               std::move(std::get<1>(resp)),
               true);
  flushAndLoop();
  hqSession_->closeWhenIdle();
}

TEST_P(HQUpstreamSessionTest, GetAddresses) {
  folly::SocketAddress localAddr("::", 65001);
  folly::SocketAddress remoteAddr("31.13.31.13", 3113);
  EXPECT_CALL(*socketDriver_->getSocket(), getLocalAddress())
      .WillRepeatedly(ReturnRef(localAddr));
  EXPECT_CALL(*socketDriver_->getSocket(), getPeerAddress())
      .WillRepeatedly(ReturnRef(remoteAddr));
  EXPECT_EQ(localAddr, hqSession_->getLocalAddress());
  EXPECT_EQ(remoteAddr, hqSession_->getPeerAddress());
  hqSession_->dropConnection();
}

TEST_P(HQUpstreamSessionTest, GetAddressesFromBase) {
  HTTPSessionBase* sessionBase = dynamic_cast<HTTPSessionBase*>(hqSession_);
  EXPECT_EQ(localAddress_, sessionBase->getLocalAddress());
  EXPECT_EQ(localAddress_, sessionBase->getLocalAddress());
  hqSession_->dropConnection();
}

TEST_P(HQUpstreamSessionTest, GetAddressesAfterDropConnection) {
  HQSession::DestructorGuard dg(hqSession_);
  hqSession_->dropConnection();
  EXPECT_EQ(localAddress_, hqSession_->getLocalAddress());
  EXPECT_EQ(peerAddress_, hqSession_->getPeerAddress());
}

TEST_P(HQUpstreamSessionTest, DropConnectionTwice) {
  HQSession::DestructorGuard dg(hqSession_);
  hqSession_->closeWhenIdle();
  hqSession_->dropConnection();
}

TEST_P(HQUpstreamSessionTest, DropConnectionTwiceWithPendingStreams) {
  folly::IOBufQueue writeBuf{folly::IOBufQueue::cacheChainLength()};
  socketDriver_->addReadEvent(15, writeBuf.move());
  flushAndLoopN(1);
  HQSession::DestructorGuard dg(hqSession_);
  hqSession_->dropConnection();
  eventBase_.loopOnce();
  hqSession_->closeWhenIdle();
}

TEST_P(HQUpstreamSessionTest, DropConnectionAfterCloseWhenIdle) {
  HQSession::DestructorGuard dg(hqSession_);
  hqSession_->closeWhenIdle();
  flushAndLoopN(1);
  hqSession_->dropConnection();
}

TEST_P(HQUpstreamSessionTest, DropConnectionWithStreamAfterCloseWhenIdle) {
  HQSession::DestructorGuard dg(hqSession_);
  auto handler = openTransaction();
  handler->txn_->sendHeaders(getGetRequest());
  hqSession_->closeWhenIdle();
  flushAndLoopN(1);
  handler->expectError();
  handler->expectDetachTransaction();
  hqSession_->dropConnection();
}

TEST_P(HQUpstreamSessionTest, NotifyConnectCallbackBeforeDestruct) {
  MockConnectCallback connectCb;
  dynamic_cast<HQUpstreamSession*>(hqSession_)->setConnectCallback(&connectCb);
  EXPECT_CALL(connectCb, connectError(_)).Times(1);
  socketDriver_->deliverConnectionError(
      {quic::LocalErrorCode::CONNECT_FAILED, "Peer closed"});
}

TEST_P(HQUpstreamSessionTest, DropFromConnectError) {
  MockConnectCallback connectCb;
  HQUpstreamSession* upstreamSess =
      dynamic_cast<HQUpstreamSession*>(hqSession_);
  upstreamSess->setConnectCallback(&connectCb);
  EXPECT_CALL(connectCb, connectError(_)).WillOnce(InvokeWithoutArgs([&] {
    hqSession_->dropConnection();
  }));
  socketDriver_->addOnConnectionEndEvent(0);
  eventBase_.loop();
}

TEST_P(HQUpstreamSessionTest, NotifyReplaySafeAfterTransportReady) {
  MockConnectCallback connectCb;
  HQUpstreamSession* upstreamSess =
      dynamic_cast<HQUpstreamSession*>(hqSession_);
  upstreamSess->setConnectCallback(&connectCb);

  // onTransportReady gets called in SetUp() already

  EXPECT_CALL(connectCb, onReplaySafe());
  upstreamSess->onReplaySafe();

  upstreamSess->closeWhenIdle();
  eventBase_.loopOnce();
}

TEST_P(HQUpstreamSessionTest, OnConnectionErrorWithOpenStreams) {
  HQSession::DestructorGuard dg(hqSession_);
  auto handler = openTransaction();
  handler->expectError();
  handler->expectDetachTransaction();
  hqSession_->onConnectionError(
      std::make_pair(quic::LocalErrorCode::CONNECT_FAILED,
                     "Connect Failure with Open streams"));
  eventBase_.loop();
  EXPECT_EQ(hqSession_->getConnectionCloseReason(),
            ConnectionCloseReason::SHUTDOWN);
}

TEST_P(HQUpstreamSessionTest, OnConnectionErrorWithOpenStreamsPause) {
  HQSession::DestructorGuard dg(hqSession_);
  auto handler1 = openTransaction();
  auto handler2 = openTransaction();
  handler1->txn_->sendHeaders(getGetRequest());
  handler1->txn_->sendEOM();
  handler2->txn_->sendHeaders(getGetRequest());
  handler2->txn_->sendEOM();
  auto resp = makeResponse(200, 100);
  sendResponse(handler1->txn_->getID(),
               *std::get<0>(resp),
               std::move(std::get<1>(resp)),
               true);
  resp = makeResponse(200, 100);
  sendResponse(handler2->txn_->getID(),
               *std::get<0>(resp),
               std::move(std::get<1>(resp)),
               true);
  flush();
  eventBase_.runInLoop([&] {
    hqSession_->onConnectionError(
        std::make_pair(quic::LocalErrorCode::CONNECT_FAILED,
                       "Connect Failure with Open streams"));
  });
  handler1->expectError(
      [&](const HTTPException&) { handler2->txn_->pauseIngress(); });
  handler1->expectDetachTransaction();
  handler2->expectError();
  handler2->expectDetachTransaction();
  eventBase_.loop();
  EXPECT_EQ(hqSession_->getConnectionCloseReason(),
            ConnectionCloseReason::SHUTDOWN);
}

TEST_P(HQUpstreamSessionTestH1qv2HQ, GoawayStreamsUnacknowledged) {
  std::vector<std::unique_ptr<StrictMock<MockHTTPHandler>>> handlers;
  auto numStreams = 4;
  quic::StreamId goawayId = (numStreams * 4) / 2;
  for (auto n = 1; n <= numStreams; n++) {
    handlers.emplace_back(openTransaction());
    auto handler = handlers.back().get();
    handler->txn_->sendHeaders(getGetRequest());
    handler->txn_->sendEOM();
    EXPECT_CALL(*handler, onGoaway(testing::_)).Times(2);
    if (handler->txn_->getID() > goawayId) {
      handler->expectError([hdlr = handler](const HTTPException& err) {
        EXPECT_TRUE(err.hasProxygenError());
        EXPECT_EQ(err.getProxygenError(), kErrorStreamUnacknowledged);
        ASSERT_EQ(
            folly::to<std::string>("StreamUnacknowledged on transaction id: ",
                                   hdlr->txn_->getID()),
            std::string(err.what()));
      });
    } else {
      handler->expectHeaders();
      handler->expectBody();
      handler->expectEOM();
    }

    if (n < numStreams) {
      handler->expectDetachTransaction();
    } else {
      handler->expectDetachTransaction([&] {
        // Make sure the session can't create any more transactions.
        MockHTTPHandler handler2;
        EXPECT_EQ(hqSession_->newTransaction(&handler2), nullptr);
        // Send the responses for the acknowledged streams
        for (auto& hdlr : handlers) {
          auto id = hdlr->txn_->getID();
          if (id <= goawayId) {
            auto resp = makeResponse(200, 100);
            sendResponse(
                id, *std::get<0>(resp), std::move(std::get<1>(resp)), true);
          }
        }
        flush();
      });
    }
  }

  sendGoaway(quic::kEightByteLimit, milliseconds(50));
  sendGoaway(goawayId, milliseconds(100));
  flushAndLoop();
}

TEST_P(HQUpstreamSessionTestHQ, DelayedQPACK) {
  InSequence enforceOrder;
  auto handler = openTransaction();
  handler->txn_->sendHeaders(getGetRequest());
  handler->txn_->sendEOM();
  handler->expectHeaders();
  handler->expectHeaders();
  handler->expectBody();
  handler->expectEOM();
  handler->expectDetachTransaction();
  auto cont = makeResponse(100);
  auto resp = makeResponse(200, 100);
  cont->getHeaders().add("X-FB-Debug", "jvrbfihvuvvclgvfkbkikjlcbruleekj");
  std::get<0>(resp)->getHeaders().add("X-FB-Debug",
                                      "egedljtrbullljdjjvtjkekebffefclj");
  sendResponse(handler->txn_->getID(), *cont, nullptr, false);
  sendResponse(handler->txn_->getID(),
               *std::get<0>(resp),
               std::move(std::get<1>(resp)),
               true);
  auto control = encoderWriteBuf_.move();
  flushAndLoopN(1);
  encoderWriteBuf_.append(std::move(control));
  flushAndLoop();
  hqSession_->closeWhenIdle();
}

TEST_P(HQUpstreamSessionTestHQ, DelayedQPACKTimeout) {
  InSequence enforceOrder;
  auto handler = openTransaction();
  handler->txn_->sendHeaders(getGetRequest());
  handler->txn_->sendEOM();
  handler->expectError();
  auto resp = makeResponse(200, 100);
  std::get<0>(resp)->getHeaders().add("X-FB-Debug",
                                      "egedljtrbullljdjjvtjkekebffefclj");
  sendResponse(handler->txn_->getID(),
               *std::get<0>(resp),
               std::move(std::get<1>(resp)),
               true);
  auto control = encoderWriteBuf_.move();
  handler->expectDetachTransaction([this, &control]() mutable {
    // have the header block arrive after destruction
    encoderWriteBuf_.append(std::move(control));
    eventBase_.runInLoop([this] { flush(); });
    eventBase_.runAfterDelay([this] { hqSession_->closeWhenIdle(); }, 100);
  });
  flushAndLoop();
}

TEST_P(HQUpstreamSessionTestHQ, QPACKDecoderStreamFlushed) {
  InSequence enforceOrder;
  auto handler = openTransaction();
  handler->txn_->sendHeadersWithOptionalEOM(getGetRequest(), true);
  flushAndLoopN(1);
  handler->expectDetachTransaction();
  handler->txn_->sendAbort();
  flushAndLoop();
  auto& decoderStream = socketDriver_->streams_[kQPACKDecoderEgressStreamId];
  // type byte plus cancel
  EXPECT_EQ(decoderStream.writeBuf.chainLength(), 2);

  handler = openTransaction();
  handler->txn_->sendHeadersWithOptionalEOM(getGetRequest(), true);
  handler->expectHeaders();
  handler->expectBody();
  handler->expectEOM();
  auto resp = makeResponse(200, 100);
  std::get<0>(resp)->getHeaders().add("Response", "Dynamic");
  sendResponse(handler->txn_->getID(),
               *std::get<0>(resp),
               std::move(std::get<1>(resp)),
               true);
  auto qpackData = encoderWriteBuf_.move();
  flushAndLoopN(1);
  encoderWriteBuf_.append(std::move(qpackData));
  handler->expectDetachTransaction();
  hqSession_->closeWhenIdle();
  flushAndLoop();
  // type byte plus cancel plus ack
  EXPECT_EQ(decoderStream.writeBuf.chainLength(), 3);
}

TEST_P(HQUpstreamSessionTestHQ, DelayedQPACKAfterReset) {
  // Stand on your head and spit wooden nickels
  // Ensure the session does not deliver input data to a transaction detached
  // earlier the same loop
  InSequence enforceOrder;
  // Send two requests
  auto handler1 = openTransaction();
  auto handler2 = openTransaction();
  handler1->txn_->sendHeadersWithOptionalEOM(getGetRequest(), true);
  handler2->txn_->sendHeadersWithOptionalEOM(getGetRequest(), true);
  // Send a response to txn1 that will block on QPACK data
  auto resp1 = makeResponse(302, 0);
  std::get<0>(resp1)->getHeaders().add("Response1", "Dynamic");
  sendResponse(handler1->txn_->getID(),
               *std::get<0>(resp1),
               std::move(std::get<1>(resp1)),
               true);
  // Save first QPACK data
  auto qpackData1 = encoderWriteBuf_.move();
  // Send response to txn2 that will block on *different* QPACK data
  auto resp2 = makeResponse(302, 0);
  std::get<0>(resp2)->getHeaders().add("Respnse2", "Dynamic");
  sendResponse(handler2->txn_->getID(),
               *std::get<0>(resp2),
               std::move(std::get<1>(resp2)),
               false);
  // Save second QPACK data
  auto qpackData2 = encoderWriteBuf_.move();

  // Abort *both* txns when txn1 gets headers.  This will leave txn2 detached
  // with pending input data in this loop.
  handler1->expectHeaders([&] {
    handler1->txn_->sendAbort();
    handler2->txn_->sendAbort();
  });

  auto streamIt1 = streams_.find(handler1->txn_->getID());
  CHECK(streamIt1 != streams_.end());
  auto streamIt2 = streams_.find(handler2->txn_->getID());
  CHECK(streamIt2 != streams_.end());
  // add all the events in the same callback, with the stream data coming
  // before the QPACK data
  std::vector<MockQuicSocketDriver::ReadEvent> events;
  events.emplace_back(handler2->txn_->getID(),
                      streamIt2->second.buf.move(),
                      streamIt2->second.readEOF,
                      folly::none,
                      false);
  events.emplace_back(handler1->txn_->getID(),
                      streamIt1->second.buf.move(),
                      streamIt1->second.readEOF,
                      folly::none,
                      false);
  events.emplace_back(kQPACKEncoderIngressStreamId,
                      std::move(qpackData1),
                      false,
                      folly::none,
                      false);
  socketDriver_->addReadEvents(std::move(events));
  handler2->expectDetachTransaction();
  handler1->expectDetachTransaction();
  eventBase_.loopOnce();
  // Add the QPACK data that would unblock txn2.  It's long gone and this
  // should be a no-op.
  socketDriver_->addReadEvent(kQPACKEncoderIngressStreamId,
                              std::move(qpackData2));
  eventBase_.loopOnce();
  hqSession_->closeWhenIdle();
}

TEST_P(HQUpstreamSessionTestHQ, TestDropConnectionSynchronously) {
  std::unique_ptr<testing::NiceMock<proxygen::MockHTTPSessionInfoCallback>>
      infoCb = std::make_unique<
          testing::NiceMock<proxygen::MockHTTPSessionInfoCallback>>();
  auto handler = openTransaction();
  handler->txn_->sendHeaders(getGetRequest());
  handler->expectError();
  handler->expectDetachTransaction();
  hqSession_->setInfoCallback(infoCb.get());
  // the session is destroyed synchronously, so the destroy callback gets
  // invoked
  EXPECT_CALL(*infoCb.get(), onDestroy(_)).Times(1);
  hqSession_->dropConnection();
  infoCb.reset();
  eventBase_.loopOnce();
}

TEST_P(HQUpstreamSessionTestHQ, TestOnStopSendingHTTPRequestRejected) {
  auto handler = openTransaction();
  auto streamId = handler->txn_->getID();
  handler->txn_->sendHeaders(getGetRequest());
  eventBase_.loopOnce();
  EXPECT_CALL(*socketDriver_->getSocket(),
              resetStream(streamId, HTTP3::ErrorCode::HTTP_REQUEST_CANCELLED))
      .Times(2) // See comment in HTTPSession::handleWriteError
      .WillRepeatedly(
          Invoke([&](quic::StreamId id, quic::ApplicationErrorCode) {
            // setWriteError will cancaleDeliveryCallbacks which will invoke
            // onCanceled to decrementPendingByteEvents on the txn.
            socketDriver_->setWriteError(id);
            return folly::unit;
          }));
  EXPECT_CALL(*handler, onError(_))
      .Times(1)
      .WillOnce(Invoke([](HTTPException ex) {
        EXPECT_EQ(kErrorStreamUnacknowledged, ex.getProxygenError());
      }));
  handler->expectDetachTransaction();
  hqSession_->onStopSending(streamId, HTTP3::ErrorCode::HTTP_REQUEST_REJECTED);
  hqSession_->closeWhenIdle();
}

// This test is checking two different scenarios for different protocol
//   - in HQ we already have sent SETTINGS in SetUp, so tests that multiple
//     setting frames are not allowed
//   - in h1q-fb-v2 tests that receiving even a single SETTINGS frame errors
//     out the connection
TEST_P(HQUpstreamSessionTestH1qv2HQ, ExtraSettings) {
  auto handler = openTransaction();
  handler->txn_->sendHeaders(getGetRequest());
  handler->txn_->sendEOM();
  handler->expectError();
  handler->expectDetachTransaction();

  // Need to use a new codec. Since generating settings twice is
  // forbidden
  HQControlCodec auxControlCodec_{nextUnidirectionalStreamId_,
                                  TransportDirection::DOWNSTREAM,
                                  StreamDirection::EGRESS,
                                  egressSettings_,
                                  UnidirectionalStreamType::H1Q_CONTROL};
  folly::IOBufQueue writeBuf{folly::IOBufQueue::cacheChainLength()};
  auxControlCodec_.generateSettings(writeBuf);
  socketDriver_->addReadEvent(
      connControlStreamId_, writeBuf.move(), milliseconds(0));

  flushAndLoop();

  EXPECT_EQ(*socketDriver_->streams_[kConnectionStreamId].error,
            HTTP3::ErrorCode::HTTP_UNEXPECTED_FRAME);
}

using HQUpstreamSessionDeathTestH1qv2HQ = HQUpstreamSessionTestH1qv2HQ;
TEST_P(HQUpstreamSessionDeathTestH1qv2HQ, WriteExtraSettings) {
  EXPECT_EXIT(sendSettings(),
              ::testing::KilledBySignal(SIGABRT),
              "Check failed: !sentSettings_");
}

// Test Cases for which Settings are not sent in the test SetUp
using HQUpstreamSessionTestHQNoSettings = HQUpstreamSessionTest;

INSTANTIATE_TEST_CASE_P(HQUpstreamSessionTest,
                        HQUpstreamSessionTestHQNoSettings,
                        Values([] {
                          TestParams tp;
                          tp.alpn_ = "h3";
                          tp.shouldSendSettings_ = false;
                          return tp;
                        }()),
                        paramsToTestName);
TEST_P(HQUpstreamSessionTestHQNoSettings, SimpleGet) {
  EXPECT_CALL(connectCb_, connectError(_)).Times(1);
  socketDriver_->deliverConnectionError(
      {quic::LocalErrorCode::CONNECT_FAILED, "Peer closed"});
}

TEST_P(HQUpstreamSessionTestHQNoSettings, GoawayBeforeSettings) {
  auto handler = openTransaction();
  handler->txn_->sendHeaders(getGetRequest());
  handler->txn_->sendEOM();
  handler->expectError();
  handler->expectDetachTransaction();

  sendGoaway(quic::kEightByteLimit);
  flushAndLoop();

  EXPECT_EQ(*socketDriver_->streams_[kConnectionStreamId].error,
            HTTP3::ErrorCode::HTTP_MISSING_SETTINGS);
}

TEST_P(HQUpstreamSessionTestH1qv1, TestConnectionClose) {
  hqSession_->drain();
  auto handler = openTransaction();
  handler->txn_->sendHeaders(getGetRequest());
  handler->txn_->sendEOM();
  handler->expectHeaders();
  handler->expectBody();
  handler->expectEOM();
  handler->expectDetachTransaction();
  auto resp = makeResponse(200, 100);
  std::get<0>(resp)->getHeaders().set(HTTP_HEADER_CONNECTION, "close");
  sendResponse(handler->txn_->getID(),
               *std::get<0>(resp),
               std::move(std::get<1>(resp)),
               true);
  hqSession_->closeWhenIdle();
  flushAndLoop();
}

/**
 * Push tests
 */

class HQUpstreamSessionTestHQPush : public HQUpstreamSessionTest {
 public:
  void SetUp() override {
    HQUpstreamSessionTest::SetUp();
    SetUpAssocHandler();
    nextPushId_ = kInitialPushId;
    lastPushPromiseHeadersSize_.compressed = 0;
    lastPushPromiseHeadersSize_.uncompressed = 0;
    ;
  }

  void SetUpAssocHandler() {
    // Create the primary request
    assocHandler_ = openTransaction();
    assocHandler_->txn_->sendHeaders(getGetRequest());
    assocHandler_->expectDetachTransaction();
  }

  void TearDown() override {
    HQUpstreamSessionTest::TearDown();
  }

  void SetUpServerPushLifecycleCallbacks() {
    if (!SLCcallback_) {
      SLCcallback_ = std::make_unique<MockServerPushLifecycleCallback>();
      hqSession_->setServerPushLifecycleCallback(SLCcallback_.get());
    }
  }

  hq::PushId nextPushId() {
    auto id = nextPushId_;
    nextPushId_ += kPushIdIncrement;
    return id | hq::kPushIdMask;
  }

  // NOTE: Using odd numbers for push ids, to allow detecting
  // subtle bugs where streamID and pushID are quietly misplaced
  bool isPushIdValid(hq::PushId pushId) {
    return (pushId % 2) == 1;
  }

  using WriteFunctor = std::function<folly::Optional<size_t>(IOBufQueue&)>;
  folly::Optional<size_t> writeUpTo(quic::StreamId id,
                                    size_t maxlen,
                                    WriteFunctor functor) {
    // Lookup the stream
    auto findRes = streams_.find(id);
    if (findRes == streams_.end()) {
      return folly::none;
    }

    IOBufQueue tmpbuf{IOBufQueue::cacheChainLength()};
    auto funcres = functor(tmpbuf);
    if (!funcres) {
      return folly::none;
    }

    auto eventbuf = tmpbuf.splitAtMost(maxlen);
    auto wlen = eventbuf->length();
    CHECK_LE(wlen, maxlen) << "The written len must not exceed the max len";
    socketDriver_->addReadEvent(id, std::move(eventbuf), milliseconds(0));
    return wlen;
  }

  // Use the common facilities to write the quic integer
  folly::Optional<size_t> writePushStreamPreface(quic::StreamId id,
                                                 size_t maxlen) {
    WriteFunctor f = [](IOBufQueue& outbuf) {
      return generateStreamPreface(outbuf, hq::UnidirectionalStreamType::PUSH);
    };

    auto res = writeUpTo(id, maxlen, f);
    return res;
  }

  folly::Optional<size_t> writeUnframedPushId(quic::StreamId id,
                                              size_t maxlen,
                                              hq::PushId pushId) {
    CHECK(hq::isInternalPushId(pushId))
        << "Expecting the push id to be in the internal representation";

    // Since this method does not use a codec, we have to clear
    // the internal push id bit ourselves
    pushId &= ~hq::kPushIdMask;

    WriteFunctor f = [=](IOBufQueue& outbuf) -> folly::Optional<size_t> {
      folly::io::QueueAppender appender(&outbuf, 8);
      uint8_t size = 1 << (folly::Random::rand32() % 4);
      auto wlen = encodeQuicIntegerWithAtLeast(pushId, size, appender);
      CHECK_GE(wlen, size);
      return wlen;
    };

    auto res = writeUpTo(id, maxlen, f);
    return res;
  }

  void expectPushPromiseBegin(
      std::function<void(HTTPCodec::StreamID, hq::PushId)> callback =
          std::function<void(HTTPCodec::StreamID, hq::PushId)>()) {
    SetUpServerPushLifecycleCallbacks();
    SLCcallback_->expectPushPromiseBegin(callback);
  }

  void expectPushPromise(
      std::function<void(HTTPCodec::StreamID, hq::PushId, HTTPMessage*)>
          callback = std::function<
              void(HTTPCodec::StreamID, hq::PushId, HTTPMessage*)>()) {
    SetUpServerPushLifecycleCallbacks();
    SLCcallback_->expectPushPromise(callback);
  }

  void expectNascentPushStreamBegin(
      std::function<void(HTTPCodec::StreamID, bool)> callback =
          std::function<void(HTTPCodec::StreamID, bool)>()) {
    SetUpServerPushLifecycleCallbacks();
    SLCcallback_->expectNascentPushStreamBegin(callback);
  }

  void expectNascentPushStream(
      std::function<void(HTTPCodec::StreamID, hq::PushId, bool)> callback =
          std::function<void(HTTPCodec::StreamID, hq::PushId, bool)>()) {
    SetUpServerPushLifecycleCallbacks();
    SLCcallback_->expectNascentPushStream(callback);
  }

  void expectNascentEof(
      std::function<void(HTTPCodec::StreamID, folly::Optional<hq::PushId>)>
          callback = std::function<void(HTTPCodec::StreamID,
                                        folly::Optional<hq::PushId>)>()) {
    SetUpServerPushLifecycleCallbacks();
    SLCcallback_->expectNascentEof(callback);
  }

  void expectOrphanedNascentStream(
      std::function<void(HTTPCodec::StreamID, folly::Optional<hq::PushId>)>
          callback = std::function<void(HTTPCodec::StreamID,
                                        folly::Optional<hq::PushId>)>()) {

    SetUpServerPushLifecycleCallbacks();
    SLCcallback_->expectOrphanedNascentStream(callback);
  }

  void expectHalfOpenPushedTxn(
      std::function<
          void(const HTTPTransaction*, hq::PushId, HTTPCodec::StreamID, bool)>
          callback = std::function<void(const HTTPTransaction*,
                                        hq::PushId,
                                        HTTPCodec::StreamID,
                                        bool)>()) {
    SetUpServerPushLifecycleCallbacks();
    SLCcallback_->expectHalfOpenPushedTxn(callback);
  }

  void expectPushedTxn(std::function<void(const HTTPTransaction*,
                                          HTTPCodec::StreamID,
                                          hq::PushId,
                                          HTTPCodec::StreamID,
                                          bool)> callback =
                           std::function<void(const HTTPTransaction*,
                                              HTTPCodec::StreamID,
                                              hq::PushId,
                                              HTTPCodec::StreamID,
                                              bool)>()) {
    SetUpServerPushLifecycleCallbacks();
    SLCcallback_->expectPushedTxn(callback);
  }

  void expectPushedTxnTimeout(
      std::function<void(const HTTPTransaction*)> callback =
          std::function<void(const HTTPTransaction*)>()) {
    SetUpServerPushLifecycleCallbacks();
    SLCcallback_->expectPushedTxnTimeout(callback);
  }

  void expectOrphanedHalfOpenPushedTxn(
      std::function<void(const HTTPTransaction*)> callback =
          std::function<void(const HTTPTransaction*)>()) {
    SetUpServerPushLifecycleCallbacks();
    SLCcallback_->expectOrphanedHalfOpenPushedTxn(callback);
  }

  void sendPushPromise(quic::StreamId streamId,
                       hq::PushId pushId = kUnknownPushId,
                       const std::string& url = "/",
                       proxygen::HTTPHeaderSize* outHeaderSize = nullptr,
                       bool eom = false) {
    auto promise = getGetRequest(url);
    promise.setURL(url);

    return sendPushPromise(streamId, promise, pushId, outHeaderSize, eom);
  }

  void sendPushPromise(quic::StreamId streamId,
                       const HTTPMessage& promiseHeadersBlock,
                       hq::PushId pushId = kUnknownPushId,
                       proxygen::HTTPHeaderSize* outHeaderSize = nullptr,
                       bool eom = false) {

    // In case the user is not interested in knowing the size
    // of headers, but just in the fact that the headers were
    // written, use a temporary size for checks
    if (outHeaderSize == nullptr) {
      outHeaderSize = &lastPushPromiseHeadersSize_;
    }

    if (pushId == kUnknownPushId) {
      pushId = nextPushId();
    }

    CHECK(hq::isInternalPushId(pushId))
        << "Expecting the push id to be in the internal representation";

    auto c = makeCodec(streamId);
    auto res =
        streams_.emplace(std::piecewise_construct,
                         std::forward_as_tuple(streamId),
                         std::forward_as_tuple(c.first, std::move(c.second)));

    auto& pushPromiseRequest = res.first->second;
    pushPromiseRequest.id = streamId;

    // Push promises should not have EOF set.
    pushPromiseRequest.readEOF = eom;

    // Write the push promise to the request buffer.
    // The push promise includes the headers
    pushPromiseRequest.codec->generatePushPromise(pushPromiseRequest.buf,
                                                  streamId,
                                                  promiseHeadersBlock,
                                                  pushId,
                                                  eom,
                                                  outHeaderSize);
  }

  // Shared implementation for different push stream
  // methods
  ServerStream& createPushStreamImpl(quic::StreamId streamId,
                                     folly::Optional<hq::PushId> pushId,
                                     std::size_t len = kUnlimited,
                                     bool eom = true) {

    if (pushId.hasValue()) {
      CHECK(hq::isInternalPushId(*pushId))
          << "Expecting the push id to be in the internal representation";
    }

    auto c = makeCodec(streamId);
    // Setting a push id allows us to send push preface
    auto res = streams_.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(streamId),
        std::forward_as_tuple(c.first, std::move(c.second), pushId));

    auto& stream = res.first->second;
    stream.id = stream.codec->createStream();
    stream.readEOF = eom;

    // Generate the push stream preface, and if there's enough headroom
    // the unframed push id that follows it
    auto prefaceRes = writePushStreamPreface(stream.id, len);
    if (pushId.hasValue()) {
      if (prefaceRes) {
        len -= *prefaceRes;
        writeUnframedPushId(stream.id, len, *pushId);
      }
    }

    return stream;
  }

  // Create a push stream with a header block and body
  void createPushStream(quic::StreamId streamId,
                        hq::PushId pushId,
                        const HTTPMessage& resp,
                        std::unique_ptr<folly::IOBuf> body = nullptr,
                        bool eom = true) {

    CHECK(hq::isInternalPushId(pushId))
        << "Expecting the push id to be in the internal representation";

    auto& stream = createPushStreamImpl(streamId, pushId, kUnlimited, eom);

    // Write the response
    stream.codec->generateHeader(
        stream.buf, stream.codecId, resp, body == nullptr ? eom : false);
    if (body) {
      stream.codec->generateBody(
          stream.buf, stream.codecId, std::move(body), folly::none, eom);
    }
  }

  // Convenience method for creating a push stream without the
  // need to allocate transport stream id
  void createPushStream(hq::PushId pushId,
                        const HTTPMessage& resp,
                        std::unique_ptr<folly::IOBuf> body = nullptr,
                        bool eom = true) {
    return createPushStream(
        nextUnidirectionalStreamId(), pushId, resp, std::move(body), eom);
  }

  // Create nascent stream (no body)
  void createNascentPushStream(quic::StreamId streamId,
                               folly::Optional<hq::PushId> pushId,
                               std::size_t len = kUnlimited,
                               bool eom = true) {
    createPushStreamImpl(streamId, pushId, len, eom);
  }

  bool lastPushPromiseHeadersSizeValid() {
    return ((lastPushPromiseHeadersSize_.uncompressed > 0) &&
            (lastPushPromiseHeadersSize_.compressed > 0));
  }

  void createNascentPushStream(hq::PushId pushId,
                               std::size_t prefaceBytes = kUnlimited,
                               bool eom = true) {
    return createNascentPushStream(
        nextUnidirectionalStreamId(), pushId, prefaceBytes, eom);
  }

  proxygen::HTTPHeaderSize lastPushPromiseHeadersSize_;
  hq::PushId nextPushId_;
  std::unique_ptr<StrictMock<MockHTTPHandler>> assocHandler_;

  std::unique_ptr<MockServerPushLifecycleCallback> SLCcallback_;
};

// Ingress push tests have different parameters
using HQUpstreamSessionTestIngressHQPush = HQUpstreamSessionTestHQPush;

TEST_P(HQUpstreamSessionTestHQPush, TestPushPromiseCallbacksInvoked) {
  // the push promise is not followed by a push stream, and the eof is not
  // set.
  // The transaction is supposed to stay open and to time out eventually.
  assocHandler_->expectError([&](const HTTPException& ex) {
    ASSERT_EQ(ex.getProxygenError(), kErrorTimeout);
  });
  assocHandler_->expectPushedTransaction();

  hq::PushId pushId = nextPushId();

  ASSERT_TRUE(hq::isInternalPushId(pushId))
      << "Expecting the push id to be in the internal representation";

  auto pushPromiseRequest = getGetRequest();

  expectPushPromiseBegin(
      [&](HTTPCodec::StreamID owningStreamId, hq::PushId promisedPushId) {
        EXPECT_EQ(promisedPushId, pushId);
        EXPECT_EQ(owningStreamId, assocHandler_->txn_->getID());
      });

  expectPushPromise([&](HTTPCodec::StreamID owningStreamId,
                        hq::PushId promisedPushId,
                        HTTPMessage* msg) {
    EXPECT_EQ(promisedPushId, pushId);
    EXPECT_EQ(owningStreamId, assocHandler_->txn_->getID());

    EXPECT_THAT(msg, NotNull());

    auto expectedHeaders = pushPromiseRequest.getHeaders();
    auto actualHeaders = msg->getHeaders();

    expectedHeaders.forEach(
        [&](const std::string& header, const std::string& /* val */) {
          EXPECT_TRUE(actualHeaders.exists(header));
          EXPECT_EQ(expectedHeaders.getNumberOfValues(header),
                    actualHeaders.getNumberOfValues(header));
        });
  });

  HTTPCodec::StreamID nascentStreamId;

  expectNascentPushStreamBegin([&](HTTPCodec::StreamID streamId, bool isEOF) {
    nascentStreamId = streamId;
    EXPECT_FALSE(isEOF);
  });

  expectNascentPushStream([&](HTTPCodec::StreamID pushStreamId,
                              hq::PushId pushStreamPushId,
                              bool /* isEOF */) {
    EXPECT_EQ(pushStreamPushId, pushId);
    EXPECT_EQ(pushStreamId, nascentStreamId);
  });

  sendPushPromise(assocHandler_->txn_->getID(), pushPromiseRequest, pushId);
  EXPECT_TRUE(lastPushPromiseHeadersSizeValid());

  HTTPMessage resp;
  resp.setStatusCode(200);
  createPushStream(pushId, resp, makeBuf(100), true);

  assocHandler_->txn_->sendEOM();

  hqSession_->closeWhenIdle();
  flushAndLoop();
}

TEST_P(HQUpstreamSessionTestHQPush, TestIngressPushStream) {

  hq::PushId pushId = nextPushId();

  auto pushPromiseRequest = getGetRequest();

  HTTPCodec::StreamID nascentStreamId;

  expectNascentPushStreamBegin([&](HTTPCodec::StreamID streamId, bool isEOF) {
    nascentStreamId = streamId;
    EXPECT_FALSE(isEOF);
  });

  expectNascentPushStream([&](HTTPCodec::StreamID streamId,
                              hq::PushId pushStreamPushId,
                              bool isEOF) {
    EXPECT_EQ(streamId, nascentStreamId);
    EXPECT_EQ(pushId, pushStreamPushId);
    EXPECT_EQ(isEOF, false);
  });

  // Since push promise is not sent, full ingress push stream
  // not going to be created
  /*
    expectOrphanedNascentStream([&](HTTPCodec::StreamID streamId,
                                    folly::Optional<hq::PushId> maybePushId) {
      ASSERT_EQ(streamId, nascentStreamId);
      EXPECT_EQ(maybePushId.has_value(), true);
      EXPECT_EQ(maybePushId.value(), pushId);
    });
  */
  HTTPMessage resp;
  resp.setStatusCode(200);
  createPushStream(pushId, resp, makeBuf(100), true);

  // Currently, the new transaction is not created corectly,
  // and an error is expected. to be extended in the following
  // diffs which add creation of pushed transaction
  assocHandler_->expectError();

  assocHandler_->txn_->sendEOM();
  hqSession_->closeWhenIdle();
  flushAndLoop(); // One read for the letter, one read for quic integer. Is
                  // enough?
}

TEST_P(HQUpstreamSessionTestHQPush, TestPushPromiseFollowedByPushStream) {
  // the transaction is expected to timeout, since the PushPromise does not have
  // EOF set, and it is not followed by a PushStream.
  assocHandler_->expectError();
  assocHandler_->expectPushedTransaction();

  hq::PushId pushId = nextPushId();

  auto pushPromiseRequest = getGetRequest();

  expectPushPromiseBegin(
      [&](HTTPCodec::StreamID owningStreamId, hq::PushId promisedPushId) {
        EXPECT_EQ(promisedPushId, pushId);
        EXPECT_EQ(owningStreamId, assocHandler_->txn_->getID());
      });

  expectPushPromise([&](HTTPCodec::StreamID owningStreamId,
                        hq::PushId promisedPushId,
                        HTTPMessage* msg) {
    EXPECT_EQ(promisedPushId, pushId);
    EXPECT_EQ(owningStreamId, assocHandler_->txn_->getID());

    EXPECT_THAT(msg, NotNull());

    auto expectedHeaders = pushPromiseRequest.getHeaders();
    auto actualHeaders = msg->getHeaders();

    expectedHeaders.forEach(
        [&](const std::string& header, const std::string& /* val */) {
          EXPECT_TRUE(actualHeaders.exists(header));
          EXPECT_EQ(expectedHeaders.getNumberOfValues(header),
                    actualHeaders.getNumberOfValues(header));
        });
  });

  HTTPCodec::StreamID nascentStreamId;

  expectNascentPushStreamBegin([&](HTTPCodec::StreamID streamId, bool isEOF) {
    nascentStreamId = streamId;
    EXPECT_FALSE(isEOF);
  });

  // since push stream arrives after the promise,
  // full ingress push stream has to be created
  expectNascentPushStream([&](HTTPCodec::StreamID pushStreamId,
                              hq::PushId pushStreamPushId,
                              bool /* isEOF */) {
    EXPECT_EQ(pushStreamPushId, pushId);
    EXPECT_EQ(pushStreamId, nascentStreamId);
  });

  proxygen::HTTPHeaderSize pushPromiseSize;

  sendPushPromise(assocHandler_->txn_->getID(),
                  pushPromiseRequest,
                  pushId,
                  &pushPromiseSize);
  HTTPMessage resp;
  resp.setStatusCode(200);
  createPushStream(pushId, resp, makeBuf(100), true);

  assocHandler_->txn_->sendEOM();

  hqSession_->closeWhenIdle();
  flushAndLoop();
}

TEST_P(HQUpstreamSessionTestHQPush, TestOnPushedTransaction) {
  // the transaction is expected to timeout, since the PushPromise does not have
  // EOF set, and it is not followed by a PushStream.
  assocHandler_->expectError();
  // assocHandler_->expectHeaders();

  hq::PushId pushId = nextPushId();

  auto pushPromiseRequest = getGetRequest();

  proxygen::HTTPHeaderSize pushPromiseSize;

  sendPushPromise(assocHandler_->txn_->getID(),
                  pushPromiseRequest,
                  pushId,
                  &pushPromiseSize);

  HTTPMessage resp;
  resp.setStatusCode(200);
  createPushStream(pushId, resp, makeBuf(100), true);

  // Once both push promise and push stream have been received, a push
  // transaction should be created
  assocHandler_->expectPushedTransaction();

  assocHandler_->txn_->sendEOM();

  hqSession_->closeWhenIdle();
  flushAndLoop();
}

TEST_P(HQUpstreamSessionTestHQPush, TestOnPushedTransactionOutOfOrder) {
  // the transaction is expected to timeout, since the PushPromise does not have
  // EOF set, and it is not followed by a PushStream.
  assocHandler_->expectError();
  // assocHandler_->expectHeaders();

  hq::PushId pushId = nextPushId();

  HTTPMessage resp;
  resp.setStatusCode(200);
  createPushStream(pushId, resp, makeBuf(100), true);

  auto pushPromiseRequest = getGetRequest();
  proxygen::HTTPHeaderSize pushPromiseSize;
  sendPushPromise(assocHandler_->txn_->getID(),
                  pushPromiseRequest,
                  pushId,
                  &pushPromiseSize);

  // Once both push promise and push stream have been received, a push
  // transaction should be created
  assocHandler_->expectPushedTransaction();

  assocHandler_->txn_->sendEOM();

  hqSession_->closeWhenIdle();
  flushAndLoop();
}

TEST_P(HQUpstreamSessionTestHQPush, TestCloseDroppedConnection) {
  HQSession::DestructorGuard dg(hqSession_);
  // Two "onError" calls are expected:
  // the first when MockQuicSocketDriver closes the socket
  // the second when the error is propagated to the stream
  EXPECT_CALL(*assocHandler_, onError(testing::_)).Times(2);

  // Create a nascent push stream with a preface only
  createNascentPushStream(1111 /* streamId */, folly::none /* pushId */);

  // Run the event loop to let the dispatcher register the nascent stream
  flushAndLoop();

  // Drop the connection
  hqSession_->dropConnection();
  flushAndLoop();
}

TEST_P(HQUpstreamSessionTestHQPush, TestOrphanedPushStream) {
  // the transaction is expected to timeout, since the PushPromise does not have
  // EOF set, and it is not followed by a PushStream.
  assocHandler_->expectError();

  hq::PushId pushId = nextPushId();

  HTTPMessage resp;
  resp.setStatusCode(200);
  createPushStream(pushId, resp, makeBuf(100), true);

  assocHandler_->txn_->sendEOM();

  hqSession_->closeWhenIdle();
  flushAndLoop();
}

/**
 * Instantiate the Parametrized test cases
 */

// Make sure all the tests keep working with all the supported protocol versions
INSTANTIATE_TEST_CASE_P(HQUpstreamSessionTest,
                        HQUpstreamSessionTest,
                        Values(TestParams({.alpn_ = "h1q-fb"}),
                               TestParams({.alpn_ = "h1q-fb-v2"}),
                               TestParams({.alpn_ = "h3"}),
                               [] {
                                 TestParams tp;
                                 tp.alpn_ = "h3";
                                 tp.prParams = PartiallyReliableTestParams{
                                     .bodyScript = std::vector<uint8_t>(),
                                 };
                                 return tp;
                               }()),
                        paramsToTestName);

// Instantiate h1 only tests
INSTANTIATE_TEST_CASE_P(HQUpstreamSessionTest,
                        HQUpstreamSessionTestH1q,
                        Values(TestParams({.alpn_ = "h1q-fb"}),
                               TestParams({.alpn_ = "h1q-fb-v2"})),
                        paramsToTestName);

// Instantiate h1q-fb-v2 and hq only tests (goaway tests)
INSTANTIATE_TEST_CASE_P(HQUpstreamSessionTest,
                        HQUpstreamSessionTestH1qv2HQ,
                        Values(TestParams({.alpn_ = "h1q-fb-v2"}),
                               TestParams({.alpn_ = "h3"})),
                        paramsToTestName);

// Instantiate h1q-fb-v1 only tests
INSTANTIATE_TEST_CASE_P(HQUpstreamSessionTest,
                        HQUpstreamSessionTestH1qv1,
                        Values(TestParams({.alpn_ = "h1q-fb"})),
                        paramsToTestName);

// Instantiate h1q-fb-v2 only tests
INSTANTIATE_TEST_CASE_P(HQUpstreamSessionTest,
                        HQUpstreamSessionTestH1qv2,
                        Values(TestParams({.alpn_ = "h1q-fb-v2"})),
                        paramsToTestName);

// Instantiate hq only tests
INSTANTIATE_TEST_CASE_P(HQUpstreamSessionTest,
                        HQUpstreamSessionTestHQ,
                        Values(TestParams({.alpn_ = "h3"}),
                               [] {
                                 TestParams tp;
                                 tp.alpn_ = "h3";
                                 tp.prParams = PartiallyReliableTestParams{
                                     .bodyScript = std::vector<uint8_t>(),
                                 };
                                 return tp;
                               }()),
                        paramsToTestName);

// Instantiate tests for H3 Push functionality (requires HQ)
INSTANTIATE_TEST_CASE_P(HQUpstreamSessionTest,
                        HQUpstreamSessionTestHQPush,
                        Values([] {
                          TestParams tp;
                          tp.alpn_ = "h3";
                          tp.unidirectionalStreamsCredit = 4;
                          return tp;
                        }()),
                        paramsToTestName);

INSTANTIATE_TEST_CASE_P(HQUpstreamSessionTest,
                        HQUpstreamSessionTestIngressHQPush,
                        Values(
                            [] {
                              TestParams tp;
                              tp.alpn_ = "h3";
                              tp.unidirectionalStreamsCredit = 4;
                              tp.numBytesOnPushStream = 8;
                              return tp;
                            }(),
                            [] {
                              TestParams tp;
                              tp.alpn_ = "h3";
                              tp.unidirectionalStreamsCredit = 4;
                              tp.numBytesOnPushStream = 15;
                              return tp;
                            }(),
                            [] {
                              TestParams tp;
                              tp.alpn_ = "h3";
                              tp.unidirectionalStreamsCredit = 4;
                              tp.numBytesOnPushStream = 16;
                              return tp;
                            }()),
                        paramsToTestName);

INSTANTIATE_TEST_CASE_P(
    HQUpstreamSessionTest,
    HQUpstreamSessionTestHQPRRecvBodyScripted,
    Values(
        [] {
          TestParams tp;
          tp.alpn_ = "h3";
          tp.prParams = PartiallyReliableTestParams{
              .bodyScript = std::vector<uint8_t>({PR_BODY}),
          };
          return tp;
        }(),
        [] {
          TestParams tp;
          tp.alpn_ = "h3";
          tp.prParams = PartiallyReliableTestParams{
              .bodyScript = std::vector<uint8_t>({PR_SKIP}),
          };
          return tp;
        }(),
        [] {
          TestParams tp;
          tp.alpn_ = "h3";
          tp.prParams = PartiallyReliableTestParams{
              .bodyScript = std::vector<uint8_t>({PR_BODY, PR_SKIP, PR_BODY}),
          };
          return tp;
        }(),
        [] {
          TestParams tp;
          tp.alpn_ = "h3";
          tp.prParams = PartiallyReliableTestParams{
              .bodyScript = std::vector<uint8_t>({PR_SKIP, PR_BODY, PR_SKIP}),
          };
          return tp;
        }(),
        [] {
          TestParams tp;
          tp.alpn_ = "h3";
          tp.prParams = PartiallyReliableTestParams{
              .bodyScript =
                  std::vector<uint8_t>({PR_BODY, PR_BODY, PR_SKIP, PR_BODY}),
          };
          return tp;
        }(),
        [] {
          TestParams tp;
          tp.alpn_ = "h3";
          tp.prParams = PartiallyReliableTestParams{
              .bodyScript =
                  std::vector<uint8_t>({PR_SKIP, PR_SKIP, PR_BODY, PR_SKIP}),
          };
          return tp;
        }(),
        [] {
          TestParams tp;
          tp.alpn_ = "h3";
          tp.prParams = PartiallyReliableTestParams{
              .bodyScript = std::vector<uint8_t>({PR_SKIP, PR_SKIP}),
          };
          return tp;
        }(),
        [] {
          TestParams tp;
          tp.alpn_ = "h3";
          tp.prParams = PartiallyReliableTestParams{
              .bodyScript = std::vector<uint8_t>({PR_BODY, PR_BODY}),
          };
          return tp;
        }()),
    paramsToTestName);

TEST_P(HQUpstreamSessionTestHQPRRecvBodyScripted, GetPrBodyScriptedExpire) {
  InSequence enforceOrder;

  const auto& bodyScript = GetParam().prParams->bodyScript;

  // Start a transaction and send headers only.
  auto handler = openPrTransaction();
  auto req = getGetRequest();
  req.setPartiallyReliable();
  handler->txn_->sendHeaders(req);
  handler->txn_->sendEOM();
  handler->expectHeaders();
  auto resp = makeResponse(200, 0);
  auto& response = std::get<0>(resp);
  response->setPartiallyReliable();

  uint64_t delta = 42;
  size_t responseLen = delta * bodyScript.size();

  response->getHeaders().set(HTTP_HEADER_CONTENT_LENGTH,
                             folly::to<std::string>(responseLen));

  auto streamId = handler->txn_->getID();
  startPartialResponse(streamId, *std::get<0>(resp));
  flushAndLoopN(1);

  uint64_t expectedStreamOffset = 0;
  uint64_t bodyBytesProcessed = 0;
  size_t c = 0;

  for (const auto& item : bodyScript) {
    bool eom = c == bodyScript.size() - 1;
    switch (item) {
      case PR_BODY:
        EXPECT_CALL(*handler, onBodyWithOffset(bodyBytesProcessed, testing::_));
        if (eom) {
          handler->expectEOM();
          handler->expectDetachTransaction();
        }
        sendPartialBody(streamId, makeBuf(delta), eom);
        break;
      case PR_SKIP:
        // Expected offset on the stream.
        expectedStreamOffset = socketDriver_->streams_[streamId].readOffset;

        // Skip <delta> bytes of the body.
        handler->expectBodySkipped([&](uint64_t offset) {
          EXPECT_EQ(offset, bodyBytesProcessed + delta);
        });
        socketDriver_->deliverDataExpired(streamId,
                                          expectedStreamOffset + delta);
        if (eom) {
          handler->expectEOM();
          handler->expectDetachTransaction();
        }

        // Pass data expire through server codec to keep state in tact.
        peerSendDataExpired(streamId, expectedStreamOffset + delta);

        if (eom) {
          sendPartialBody(streamId, nullptr, true);
        }
        break;
      default:
        CHECK(false) << "Unknown PR body script item: " << item;
    }

    if (eom) {
      flushAndLoop();
    } else {
      flushAndLoopN(1);
    }

    Mock::VerifyAndClearExpectations(handler.get());

    bodyBytesProcessed += delta;
    c++;
  }
  hqSession_->closeWhenIdle();
}

TEST_P(HQUpstreamSessionTestHQPRRecvBodyScripted, GetPrBodyScriptedReject) {
  InSequence enforceOrder;

  const auto& bodyScript = GetParam().prParams->bodyScript;

  // Start a transaction and send headers only.
  auto handler = openPrTransaction();
  auto req = getGetRequest();
  req.setPartiallyReliable();
  handler->txn_->sendHeaders(req);
  handler->txn_->sendEOM();
  handler->expectHeaders();
  auto resp = makeResponse(200, 0);
  auto& response = std::get<0>(resp);
  response->setPartiallyReliable();

  uint64_t delta = 42;
  size_t responseLen = delta * bodyScript.size();

  response->getHeaders().set(HTTP_HEADER_CONTENT_LENGTH,
                             folly::to<std::string>(responseLen));

  auto streamId = handler->txn_->getID();
  startPartialResponse(streamId, *std::get<0>(resp));
  flushAndLoopN(1);

  folly::Expected<folly::Optional<uint64_t>, ErrorCode> rejectRes;
  uint64_t bodyBytesProcessed = 0;
  uint64_t oldReadOffset = 0;
  size_t c = 0;

  for (const auto& item : bodyScript) {
    bool eom = c == bodyScript.size() - 1;
    switch (item) {
      case PR_BODY:
        EXPECT_CALL(*handler, onBodyWithOffset(bodyBytesProcessed, testing::_));
        if (eom) {
          handler->expectEOM();
          handler->expectDetachTransaction();
        }
        sendPartialBody(streamId, makeBuf(delta), eom);
        break;
      case PR_SKIP:
        // Reject first <delta> bytes.
        oldReadOffset = socketDriver_->streams_[streamId].readOffset;
        rejectRes = handler->txn_->rejectBodyTo(bodyBytesProcessed + delta);
        EXPECT_FALSE(rejectRes.hasError());
        EXPECT_EQ(socketDriver_->streams_[streamId].readOffset,
                  oldReadOffset + delta);

        // Pass data reject through server codec to keep state in tact.
        peerReceiveDataRejected(streamId, oldReadOffset + delta);

        if (eom) {
          handler->expectEOM();
          handler->expectDetachTransaction();
          sendPartialBody(streamId, nullptr, true);
        }
        break;
      default:
        CHECK(false) << "Unknown PR body script item: " << item;
    }

    if (eom) {
      flushAndLoop();
    } else {
      flushAndLoopN(1);
    }

    Mock::VerifyAndClearExpectations(handler.get());

    bodyBytesProcessed += delta;
    c++;
  }
  hqSession_->closeWhenIdle();
}

INSTANTIATE_TEST_CASE_P(HQUpstreamSessionTest,
                        HQUpstreamSessionTestHQPR,
                        Values([] {
                          TestParams tp;
                          tp.alpn_ = "h3";
                          tp.prParams = PartiallyReliableTestParams{
                              .bodyScript = std::vector<uint8_t>(),
                          };
                          return tp;
                        }()),
                        paramsToTestName);

TEST_P(HQUpstreamSessionTestHQPR, TestWrongOffsetErrorCleanup) {
  InSequence enforceOrder;

  // Start a transaction and send headers only.
  auto handler = openPrTransaction();
  auto req = getGetRequest();
  req.setPartiallyReliable();
  handler->txn_->sendHeaders(req);
  handler->txn_->sendEOM();
  handler->expectHeaders();
  auto resp = makeResponse(200, 0);
  auto& response = std::get<0>(resp);
  response->setPartiallyReliable();

  const size_t responseLen = 42;
  response->getHeaders().set(HTTP_HEADER_CONTENT_LENGTH,
                             folly::to<std::string>(responseLen));

  auto streamId = handler->txn_->getID();
  startPartialResponse(streamId, *std::get<0>(resp));
  flushAndLoopN(1);

  EXPECT_CALL(*handler, onBodyWithOffset(testing::_, testing::_));
  sendPartialBody(streamId, makeBuf(21), false);
  flushAndLoopN(1);

  // Give wrong offset to the session and expect transaction to finish properly.
  // Wrong offset is a soft error, error message is printed to the log.
  uint64_t wrongOffset = 1;
  EXPECT_CALL(*handler, onBodyWithOffset(testing::_, testing::_));
  EXPECT_CALL(*handler, onEOM());
  handler->expectDetachTransaction();
  hqSession_->getDispatcher()->onDataExpired(streamId, wrongOffset);
  sendPartialBody(streamId, makeBuf(21), true);

  flushAndLoop();

  hqSession_->closeWhenIdle();
}

TEST_P(HQUpstreamSessionTestHQPRDeliveryAck,
       DropConnectionWithDeliveryAckCbSetError) {
  auto handler = openPrTransaction();
  auto req = getGetRequest();
  req.setPartiallyReliable();
  auto streamId = handler->txn_->getID();
  auto sock = socketDriver_->getSocket();

  // This is a copy of the one in MockQuicSocketDriver, only hijacks data stream
  // and forces an error.
  EXPECT_CALL(*sock,
              registerDeliveryCallback(testing::_, testing::_, testing::_))
      .WillRepeatedly(
          testing::Invoke([streamId, &socketDriver = socketDriver_](
                              quic::StreamId id,
                              uint64_t offset,
                              MockQuicSocket::DeliveryCallback* cb)
                              -> folly::Expected<folly::Unit, LocalErrorCode> {
            if (id == streamId) {
              return folly::makeUnexpected(LocalErrorCode::INVALID_OPERATION);
            }

            socketDriver->checkNotReadOnlyStream(id);
            auto it = socketDriver->streams_.find(id);
            if (it == socketDriver->streams_.end() ||
                it->second.writeOffset >= offset) {
              return folly::makeUnexpected(LocalErrorCode::STREAM_NOT_EXISTS);
            }
            CHECK_NE(it->second.writeState,
                     MockQuicSocketDriver::StateEnum::CLOSED);
            it->second.deliveryCallbacks.push_back({offset, cb});
            return folly::unit;
          }));

  EXPECT_CALL(*handler, onError(_))
      .WillOnce(Invoke([](const HTTPException& error) {
        EXPECT_TRUE(std::string(error.what())
                        .find("failed to register delivery callback") !=
                    std::string::npos);
      }));
  handler->expectDetachTransaction();

  handler->txn_->sendHeaders(req);
  flushAndLoop();

  hqSession_->closeWhenIdle();
}

INSTANTIATE_TEST_CASE_P(HQUpstreamSessionTest,
                        HQUpstreamSessionTestHQPRDeliveryAck,
                        Values([] {
                          TestParams tp;
                          tp.alpn_ = "h3";
                          tp.prParams = PartiallyReliableTestParams{
                              .bodyScript = std::vector<uint8_t>(),
                          };
                          return tp;
                        }()),
                        paramsToTestName);

#include "http2/adapter/oghttp2_session.h"

#include "http2/adapter/mock_http2_visitor.h"
#include "http2/adapter/test_frame_sequence.h"
#include "http2/adapter/test_utils.h"
#include "common/platform/api/quiche_test.h"

namespace http2 {
namespace adapter {
namespace test {
namespace {

using spdy::SpdyFrameType;
using testing::_;

enum FrameType {
  DATA,
  HEADERS,
  PRIORITY,
  RST_STREAM,
  SETTINGS,
  PUSH_PROMISE,
  PING,
  GOAWAY,
  WINDOW_UPDATE,
};

}  // namespace

TEST(OgHttp2SessionTest, ClientConstruction) {
  testing::StrictMock<MockHttp2Visitor> visitor;
  OgHttp2Session session(
      visitor, OgHttp2Session::Options{.perspective = Perspective::kClient});
  EXPECT_TRUE(session.want_read());
  EXPECT_FALSE(session.want_write());
  EXPECT_EQ(session.GetRemoteWindowSize(), kDefaultInitialStreamWindowSize);
  EXPECT_FALSE(session.IsServerSession());
  EXPECT_EQ(0, session.GetHighestReceivedStreamId());
}

TEST(OgHttp2SessionTest, ClientHandlesFrames) {
  testing::StrictMock<MockHttp2Visitor> visitor;
  OgHttp2Session session(
      visitor, OgHttp2Session::Options{.perspective = Perspective::kClient});

  const std::string initial_frames = TestFrameSequence()
                                         .ServerPreface()
                                         .Ping(42)
                                         .WindowUpdate(0, 1000)
                                         .Serialize();
  testing::InSequence s;

  // Server preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(0, 8, PING, 0));
  EXPECT_CALL(visitor, OnPing(42, false));
  EXPECT_CALL(visitor, OnFrameHeader(0, 4, WINDOW_UPDATE, 0));
  EXPECT_CALL(visitor, OnWindowUpdate(0, 1000));

  const ssize_t initial_result = session.ProcessBytes(initial_frames);
  EXPECT_EQ(initial_frames.size(), initial_result);

  EXPECT_EQ(session.GetRemoteWindowSize(),
            kDefaultInitialStreamWindowSize + 1000);
  EXPECT_EQ(0, session.GetHighestReceivedStreamId());

  // Should OgHttp2Session require that streams 1 and 3 have been created?

  const std::string stream_frames =
      TestFrameSequence()
          .Headers(1,
                   {{":status", "200"},
                    {"server", "my-fake-server"},
                    {"date", "Tue, 6 Apr 2021 12:54:01 GMT"}},
                   /*fin=*/false)
          .Data(1, "This is the response body.")
          .RstStream(3, Http2ErrorCode::INTERNAL_ERROR)
          .GoAway(5, Http2ErrorCode::ENHANCE_YOUR_CALM, "calm down!!")
          .Serialize();

  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":status", "200"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, "server", "my-fake-server"));
  EXPECT_CALL(visitor,
              OnHeaderForStream(1, "date", "Tue, 6 Apr 2021 12:54:01 GMT"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnFrameHeader(1, 26, DATA, 0));
  EXPECT_CALL(visitor, OnBeginDataForStream(1, 26));
  EXPECT_CALL(visitor, OnDataForStream(1, "This is the response body."));
  EXPECT_CALL(visitor, OnFrameHeader(3, 4, RST_STREAM, 0));
  EXPECT_CALL(visitor, OnRstStream(3, Http2ErrorCode::INTERNAL_ERROR));
  EXPECT_CALL(visitor, OnCloseStream(3, Http2ErrorCode::INTERNAL_ERROR));
  EXPECT_CALL(visitor, OnFrameHeader(0, 19, GOAWAY, 0));
  EXPECT_CALL(visitor, OnGoAway(5, Http2ErrorCode::ENHANCE_YOUR_CALM, ""));
  const ssize_t stream_result = session.ProcessBytes(stream_frames);
  EXPECT_EQ(stream_frames.size(), stream_result);
  EXPECT_EQ(3, session.GetHighestReceivedStreamId());
}

// Verifies that a client session enqueues initial SETTINGS if Send() is called
// before any frames are explicitly queued.
TEST(OgHttp2SessionTest, ClientEnqueuesSettingsOnSend) {
  DataSavingVisitor visitor;
  OgHttp2Session session(
      visitor, OgHttp2Session::Options{.perspective = Perspective::kClient});
  EXPECT_FALSE(session.want_write());
  session.Send();
  absl::string_view serialized = visitor.data();
  EXPECT_THAT(serialized,
              testing::StartsWith(spdy::kHttp2ConnectionHeaderPrefix));
  serialized.remove_prefix(strlen(spdy::kHttp2ConnectionHeaderPrefix));
  EXPECT_THAT(serialized, EqualsFrames({SpdyFrameType::SETTINGS}));
}

// Verifies that a client session enqueues initial SETTINGS before whatever
// frame type is passed to the first invocation of EnqueueFrame().
TEST(OgHttp2SessionTest, ClientEnqueuesSettingsBeforeOtherFrame) {
  DataSavingVisitor visitor;
  OgHttp2Session session(
      visitor, OgHttp2Session::Options{.perspective = Perspective::kClient});
  EXPECT_FALSE(session.want_write());
  session.EnqueueFrame(absl::make_unique<spdy::SpdyPingIR>(42));
  EXPECT_TRUE(session.want_write());
  session.Send();
  absl::string_view serialized = visitor.data();
  EXPECT_THAT(serialized,
              testing::StartsWith(spdy::kHttp2ConnectionHeaderPrefix));
  serialized.remove_prefix(strlen(spdy::kHttp2ConnectionHeaderPrefix));
  EXPECT_THAT(serialized,
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::PING}));
}

// Verifies that if the first call to EnqueueFrame() passes a SETTINGS frame,
// the client session will not enqueue an additional SETTINGS frame.
TEST(OgHttp2SessionTest, ClientEnqueuesSettingsOnce) {
  DataSavingVisitor visitor;
  OgHttp2Session session(
      visitor, OgHttp2Session::Options{.perspective = Perspective::kClient});
  EXPECT_FALSE(session.want_write());
  session.EnqueueFrame(absl::make_unique<spdy::SpdySettingsIR>());
  EXPECT_TRUE(session.want_write());
  session.Send();
  absl::string_view serialized = visitor.data();
  EXPECT_THAT(serialized,
              testing::StartsWith(spdy::kHttp2ConnectionHeaderPrefix));
  serialized.remove_prefix(strlen(spdy::kHttp2ConnectionHeaderPrefix));
  EXPECT_THAT(serialized, EqualsFrames({SpdyFrameType::SETTINGS}));
}

TEST(OgHttp2SessionTest, ClientSubmitRequest) {
  DataSavingVisitor visitor;
  OgHttp2Session session(
      visitor, OgHttp2Session::Options{.perspective = Perspective::kClient});

  EXPECT_FALSE(session.want_write());

  // Even though the user has not queued any frames for the session, it should
  // still send the connection preface.
  session.Send();
  absl::string_view serialized = visitor.data();
  EXPECT_THAT(serialized,
              testing::StartsWith(spdy::kHttp2ConnectionHeaderPrefix));
  serialized.remove_prefix(strlen(spdy::kHttp2ConnectionHeaderPrefix));
  // Initial SETTINGS.
  EXPECT_THAT(serialized, EqualsFrames({SpdyFrameType::SETTINGS}));
  visitor.Clear();

  const std::string initial_frames =
      TestFrameSequence().ServerPreface().Serialize();
  testing::InSequence s;

  // Server preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  const ssize_t initial_result = session.ProcessBytes(initial_frames);
  EXPECT_EQ(initial_frames.size(), initial_result);

  // Session will want to write a SETTINGS ack.
  EXPECT_TRUE(session.want_write());
  session.Send();
  EXPECT_THAT(visitor.data(), EqualsFrames({SpdyFrameType::SETTINGS}));
  visitor.Clear();

  const char* kSentinel = "";
  TestDataFrameSource body1(visitor, "This is an example request body.");
  int stream_id =
      session.SubmitRequest(ToHeaders({{":method", "POST"},
                                       {":scheme", "http"},
                                       {":authority", "example.com"},
                                       {":path", "/this/is/request/one"}}),
                            &body1, const_cast<char*>(kSentinel));
  EXPECT_GT(stream_id, 0);
  EXPECT_TRUE(session.want_write());
  session.Send();
  EXPECT_THAT(visitor.data(), EqualsFrames({spdy::SpdyFrameType::HEADERS,
                                            spdy::SpdyFrameType::DATA}));
  EXPECT_FALSE(session.want_write());
}

TEST(OgHttp2SessionTest, ClientStartShutdown) {
  DataSavingVisitor visitor;
  OgHttp2Session session(
      visitor, OgHttp2Session::Options{.perspective = Perspective::kClient});

  EXPECT_FALSE(session.want_write());

  // No-op (except for logging) for a client implementation.
  session.StartGracefulShutdown();
  EXPECT_FALSE(session.want_write());

  session.Send();
  absl::string_view serialized = visitor.data();
  EXPECT_THAT(serialized,
              testing::StartsWith(spdy::kHttp2ConnectionHeaderPrefix));
  serialized.remove_prefix(strlen(spdy::kHttp2ConnectionHeaderPrefix));
  EXPECT_THAT(serialized, EqualsFrames({SpdyFrameType::SETTINGS}));
}

TEST(OgHttp2SessionTest, ServerConstruction) {
  testing::StrictMock<MockHttp2Visitor> visitor;
  OgHttp2Session session(
      visitor, OgHttp2Session::Options{.perspective = Perspective::kServer});
  EXPECT_TRUE(session.want_read());
  EXPECT_FALSE(session.want_write());
  EXPECT_EQ(session.GetRemoteWindowSize(), kDefaultInitialStreamWindowSize);
  EXPECT_TRUE(session.IsServerSession());
  EXPECT_EQ(0, session.GetHighestReceivedStreamId());
}

TEST(OgHttp2SessionTest, ServerHandlesFrames) {
  testing::StrictMock<MockHttp2Visitor> visitor;
  OgHttp2Session session(
      visitor, OgHttp2Session::Options{.perspective = Perspective::kServer});

  const std::string frames = TestFrameSequence()
                                 .ClientPreface()
                                 .Ping(42)
                                 .WindowUpdate(0, 1000)
                                 .Headers(1,
                                          {{":method", "POST"},
                                           {":scheme", "https"},
                                           {":authority", "example.com"},
                                           {":path", "/this/is/request/one"}},
                                          /*fin=*/false)
                                 .WindowUpdate(1, 2000)
                                 .Data(1, "This is the request body.")
                                 .Headers(3,
                                          {{":method", "GET"},
                                           {":scheme", "http"},
                                           {":authority", "example.com"},
                                           {":path", "/this/is/request/two"}},
                                          /*fin=*/true)
                                 .RstStream(3, Http2ErrorCode::CANCEL)
                                 .Ping(47)
                                 .Serialize();
  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(0, 8, PING, 0));
  EXPECT_CALL(visitor, OnPing(42, false));
  EXPECT_CALL(visitor, OnFrameHeader(0, 4, WINDOW_UPDATE, 0));
  EXPECT_CALL(visitor, OnWindowUpdate(0, 1000));
  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":method", "POST"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":scheme", "https"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":authority", "example.com"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":path", "/this/is/request/one"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnFrameHeader(1, 4, WINDOW_UPDATE, 0));
  EXPECT_CALL(visitor, OnWindowUpdate(1, 2000));
  EXPECT_CALL(visitor, OnFrameHeader(1, 25, DATA, 0));
  EXPECT_CALL(visitor, OnBeginDataForStream(1, 25));
  EXPECT_CALL(visitor, OnDataForStream(1, "This is the request body."));
  EXPECT_CALL(visitor, OnFrameHeader(3, _, HEADERS, 5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(3));
  EXPECT_CALL(visitor, OnHeaderForStream(3, ":method", "GET"));
  EXPECT_CALL(visitor, OnHeaderForStream(3, ":scheme", "http"));
  EXPECT_CALL(visitor, OnHeaderForStream(3, ":authority", "example.com"));
  EXPECT_CALL(visitor, OnHeaderForStream(3, ":path", "/this/is/request/two"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(3));
  EXPECT_CALL(visitor, OnEndStream(3));
  EXPECT_CALL(visitor, OnFrameHeader(3, 4, RST_STREAM, 0));
  EXPECT_CALL(visitor, OnRstStream(3, Http2ErrorCode::CANCEL));
  EXPECT_CALL(visitor, OnCloseStream(3, Http2ErrorCode::CANCEL));
  EXPECT_CALL(visitor, OnFrameHeader(0, 8, PING, 0));
  EXPECT_CALL(visitor, OnPing(47, false));

  const ssize_t result = session.ProcessBytes(frames);
  EXPECT_EQ(frames.size(), result);

  EXPECT_EQ(session.GetRemoteWindowSize(),
            kDefaultInitialStreamWindowSize + 1000);
  EXPECT_EQ(3, session.GetHighestReceivedStreamId());
}

// Verifies that a server session enqueues initial SETTINGS before whatever
// frame type is passed to the first invocation of EnqueueFrame().
TEST(OgHttp2SessionTest, ServerEnqueuesSettingsBeforeOtherFrame) {
  DataSavingVisitor visitor;
  OgHttp2Session session(
      visitor, OgHttp2Session::Options{.perspective = Perspective::kServer});
  EXPECT_FALSE(session.want_write());
  session.EnqueueFrame(absl::make_unique<spdy::SpdyPingIR>(42));
  EXPECT_TRUE(session.want_write());
  session.Send();
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::PING}));
}

// Verifies that if the first call to EnqueueFrame() passes a SETTINGS frame,
// the server session will not enqueue an additional SETTINGS frame.
TEST(OgHttp2SessionTest, ServerEnqueuesSettingsOnce) {
  DataSavingVisitor visitor;
  OgHttp2Session session(
      visitor, OgHttp2Session::Options{.perspective = Perspective::kServer});
  EXPECT_FALSE(session.want_write());
  session.EnqueueFrame(absl::make_unique<spdy::SpdySettingsIR>());
  EXPECT_TRUE(session.want_write());
  session.Send();
  EXPECT_THAT(visitor.data(), EqualsFrames({SpdyFrameType::SETTINGS}));
}

TEST(OgHttp2SessionTest, ServerSubmitResponse) {
  DataSavingVisitor visitor;
  OgHttp2Session session(
      visitor, OgHttp2Session::Options{.perspective = Perspective::kServer});

  EXPECT_FALSE(session.want_write());

  const std::string frames = TestFrameSequence()
                                 .ClientPreface()
                                 .Headers(1,
                                          {{":method", "GET"},
                                           {":scheme", "https"},
                                           {":authority", "example.com"},
                                           {":path", "/this/is/request/one"}},
                                          /*fin=*/true)
                                 .Serialize();
  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());
  // Stream 1
  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":method", "GET"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":scheme", "https"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":authority", "example.com"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":path", "/this/is/request/one"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnEndStream(1));

  const ssize_t result = session.ProcessBytes(frames);
  EXPECT_EQ(frames.size(), result);

  EXPECT_EQ(1, session.GetHighestReceivedStreamId());

  // Server will want to send initial SETTINGS, and a SETTINGS ack.
  EXPECT_TRUE(session.want_write());
  session.Send();
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::SETTINGS}));
  visitor.Clear();

  EXPECT_FALSE(session.want_write());
  TestDataFrameSource body1(visitor, "This is an example response body.");
  int submit_result = session.SubmitResponse(
      1,
      ToHeaders({{":status", "404"},
                 {"x-comment", "I have no idea what you're talking about."}}),
      &body1);
  EXPECT_EQ(submit_result, 0);
  EXPECT_TRUE(session.want_write());
  EXPECT_CALL(visitor, OnCloseStream(1, Http2ErrorCode::NO_ERROR));
  session.Send();
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::HEADERS, SpdyFrameType::DATA}));
  EXPECT_FALSE(session.want_write());
}

TEST(OgHttp2SessionTest, ServerStartShutdown) {
  DataSavingVisitor visitor;
  OgHttp2Session session(
      visitor, OgHttp2Session::Options{.perspective = Perspective::kServer});

  EXPECT_FALSE(session.want_write());

  session.StartGracefulShutdown();
  EXPECT_TRUE(session.want_write());

  session.Send();
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::GOAWAY}));
}

TEST(OgHttp2SessionTest, ServerStartShutdownAfterGoaway) {
  DataSavingVisitor visitor;
  OgHttp2Session session(
      visitor, OgHttp2Session::Options{.perspective = Perspective::kServer});

  EXPECT_FALSE(session.want_write());

  auto goaway = absl::make_unique<spdy::SpdyGoAwayIR>(
      1, spdy::ERROR_CODE_NO_ERROR, "and don't come back!");
  session.EnqueueFrame(std::move(goaway));
  EXPECT_TRUE(session.want_write());

  session.Send();
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::GOAWAY}));

  // No-op, since a GOAWAY has previously been enqueued.
  session.StartGracefulShutdown();
  EXPECT_FALSE(session.want_write());
}

// Tests the case where the server queues trailers after the data stream is
// exhausted.
TEST(OgHttp2SessionTest, ServerSendsTrailers) {
  DataSavingVisitor visitor;
  OgHttp2Session session(
      visitor, OgHttp2Session::Options{.perspective = Perspective::kServer});

  EXPECT_FALSE(session.want_write());

  const std::string frames = TestFrameSequence()
                                 .ClientPreface()
                                 .Headers(1,
                                          {{":method", "GET"},
                                           {":scheme", "https"},
                                           {":authority", "example.com"},
                                           {":path", "/this/is/request/one"}},
                                          /*fin=*/true)
                                 .Serialize();
  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());
  // Stream 1
  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":method", "GET"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":scheme", "https"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":authority", "example.com"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":path", "/this/is/request/one"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnEndStream(1));

  const ssize_t result = session.ProcessBytes(frames);
  EXPECT_EQ(frames.size(), result);

  // Server will want to send initial SETTINGS, and a SETTINGS ack.
  EXPECT_TRUE(session.want_write());
  session.Send();
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::SETTINGS}));
  visitor.Clear();

  EXPECT_FALSE(session.want_write());

  // The body source must indicate that the end of the body is not the end of
  // the stream.
  TestDataFrameSource body1(visitor, "This is an example response body.",
                            /*has_fin=*/false);
  int submit_result = session.SubmitResponse(
      1, ToHeaders({{":status", "200"}, {"x-comment", "Sure, sounds good."}}),
      &body1);
  EXPECT_EQ(submit_result, 0);
  EXPECT_TRUE(session.want_write());
  session.Send();
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::HEADERS, SpdyFrameType::DATA}));
  visitor.Clear();
  EXPECT_FALSE(session.want_write());

  EXPECT_CALL(visitor, OnCloseStream(1, Http2ErrorCode::NO_ERROR));
  // The body source has been exhausted by the call to Send() above.
  int trailer_result = session.SubmitTrailer(
      1, ToHeaders({{"final-status", "a-ok"},
                    {"x-comment", "trailers sure are cool"}}));
  ASSERT_EQ(trailer_result, 0);
  EXPECT_TRUE(session.want_write());

  session.Send();
  EXPECT_THAT(visitor.data(), EqualsFrames({SpdyFrameType::HEADERS}));
}

// Tests the case where the server queues trailers immediately after headers and
// data, and before any writes have taken place.
TEST(OgHttp2SessionTest, ServerQueuesTrailersWithResponse) {
  DataSavingVisitor visitor;
  OgHttp2Session session(
      visitor, OgHttp2Session::Options{.perspective = Perspective::kServer});

  EXPECT_FALSE(session.want_write());

  const std::string frames = TestFrameSequence()
                                 .ClientPreface()
                                 .Headers(1,
                                          {{":method", "GET"},
                                           {":scheme", "https"},
                                           {":authority", "example.com"},
                                           {":path", "/this/is/request/one"}},
                                          /*fin=*/true)
                                 .Serialize();
  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());
  // Stream 1
  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":method", "GET"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":scheme", "https"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":authority", "example.com"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":path", "/this/is/request/one"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnEndStream(1));

  const ssize_t result = session.ProcessBytes(frames);
  EXPECT_EQ(frames.size(), result);

  // Server will want to send initial SETTINGS, and a SETTINGS ack.
  EXPECT_TRUE(session.want_write());
  session.Send();
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::SETTINGS}));
  visitor.Clear();

  EXPECT_FALSE(session.want_write());

  // The body source must indicate that the end of the body is not the end of
  // the stream.
  TestDataFrameSource body1(visitor, "This is an example response body.",
                            /*has_fin=*/false);
  int submit_result = session.SubmitResponse(
      1, ToHeaders({{":status", "200"}, {"x-comment", "Sure, sounds good."}}),
      &body1);
  EXPECT_EQ(submit_result, 0);
  EXPECT_TRUE(session.want_write());
  // There has not been a call to Send() yet, so neither headers nor body have
  // been written.
  int trailer_result = session.SubmitTrailer(
      1, ToHeaders({{"final-status", "a-ok"},
                    {"x-comment", "trailers sure are cool"}}));
  ASSERT_EQ(trailer_result, 0);
  EXPECT_TRUE(session.want_write());

  EXPECT_CALL(visitor, OnCloseStream(1, Http2ErrorCode::NO_ERROR));
  session.Send();
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::HEADERS, SpdyFrameType::DATA,
                            SpdyFrameType::HEADERS}));
}

}  // namespace test
}  // namespace adapter
}  // namespace http2

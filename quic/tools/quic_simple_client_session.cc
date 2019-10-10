// Copyright (c) 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/third_party/quiche/src/quic/tools/quic_simple_client_session.h"

#include <utility>

namespace quic {

QuicSimpleClientSession::QuicSimpleClientSession(
    const QuicConfig& config,
    const ParsedQuicVersionVector& supported_versions,
    QuicConnection* connection,
    const QuicServerId& server_id,
    QuicCryptoClientConfig* crypto_config,
    QuicClientPushPromiseIndex* push_promise_index,
    bool drop_response_body)
    : QuicSpdyClientSession(config,
                            supported_versions,
                            connection,
                            server_id,
                            crypto_config,
                            push_promise_index),
      drop_response_body_(drop_response_body) {
  // Do not use the QPACK dynamic table in tests to avoid flakiness due to the
  // uncertain order of receiving the SETTINGS frame and sending headers.
  set_qpack_maximum_dynamic_table_capacity(0);
  set_qpack_maximum_blocked_streams(0);
}

std::unique_ptr<QuicSpdyClientStream>
QuicSimpleClientSession::CreateClientStream() {
  return std::make_unique<QuicSimpleClientStream>(
      GetNextOutgoingBidirectionalStreamId(), this, BIDIRECTIONAL,
      drop_response_body_);
}

}  // namespace quic

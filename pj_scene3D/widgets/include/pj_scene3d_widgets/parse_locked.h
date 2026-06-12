// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>
#include <mutex>

#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/SessionManager.h"

namespace pj::scene3d {

// MessageParser plugins keep stateful scratch (fastcdr et al.) and aren't
// thread-safe; when a topic's parser is shared across consumers (SessionManager
// hands back a singleton) the per-topic mutex serialises parseObject. Held for
// the decode call only — the returned object owns its bytes, so callers use it
// without the lock. Mirrors ImagePipelineSource's shared-parser_mutex discipline.
//
// `mutex` may be null (the topic has no registered parser, or an unguarded
// path): the decode then runs directly. Returns whatever
// MessageParserPluginBase::parseObject returns.
inline auto parseLocked(
    PJ::MessageParserPluginBase* parser, const std::shared_ptr<std::mutex>& mutex, PJ::Timestamp ts,
    const PJ::sdk::PayloadView& payload) {
  if (mutex) {
    std::lock_guard<std::mutex> lock(*mutex);
    return parser->parseObject(ts, payload);
  }
  return parser->parseObject(ts, payload);
}

// Binding-snapshot form: resolves lifetime as well as locking. Fetch the
// binding from SessionManager per use — a file reload re-registers the topic's
// parser slot, so a parser pointer cached across calls dangles. The binding's
// keepalive holds the parser instance (and its plugin DSO) alive for this call.
inline auto parseLocked(
    const PJ::SessionManager::ParserBinding& binding, PJ::Timestamp ts, const PJ::sdk::PayloadView& payload) {
  return parseLocked(binding.parser, binding.mutex, ts, payload);
}

}  // namespace pj::scene3d

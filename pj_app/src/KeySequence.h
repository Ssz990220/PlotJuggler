// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QObject>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace PJ {

// Maps a (position, key) pair to an opaque 32-bit value. The position acts as a
// salt, so the same key at different positions maps to different values (which
// keeps the configured step values from revealing repeats). constexpr so the
// comparison constants fold at compile time.
constexpr std::uint32_t stepHash(std::size_t pos, int key) {
  constexpr std::uint32_t kOffset = 0x811c9dc5u;
  constexpr std::uint32_t kPrime = 0x01000193u;
  constexpr std::uint32_t kSalt = 0x9e3779b9u;
  std::uint32_t h = kOffset;
  h = (h ^ (static_cast<std::uint32_t>(pos) + kSalt)) * kPrime;
  h = (h ^ static_cast<std::uint32_t>(key)) * kPrime;
  return h;
}

// A tiny fixed-sequence detector. It is configured with one step value per
// position (see stepHash). Feed key codes one at a time; feed() returns true
// exactly when the most recent keys complete the configured sequence, then
// resets so the next full run fires again. On mismatch it resets, re-testing the
// current key as a possible new start. Deliberately Qt-free so it unit-tests
// without a QApplication.
class KeySequenceMatcher {
 public:
  explicit KeySequenceMatcher(std::vector<std::uint32_t> steps);

  bool feed(int key);
  void reset() noexcept;
  [[nodiscard]] std::size_t progress() const noexcept {
    return index_;
  }

 private:
  std::vector<std::uint32_t> steps_;
  std::size_t index_ = 0;
};

// The precomputed per-position step values (see stepHash) that arm the watcher.
// Defined in one place so both the watcher and the install site share it.
std::vector<std::uint32_t> unlockSteps();

// Application-wide event filter. Install on qApp; it observes KeyPress events,
// feeds their key codes to a KeySequenceMatcher, and invokes on_match when the
// sequence completes. It NEVER consumes events (always returns false) so normal
// key handling (plot panning, text entry) is unaffected.
//
// QApplication re-delivers an unaccepted key event to application-level filters
// once per widget as it propagates up the focus parent chain, so a single
// physical press reaches this filter many times. Those re-deliveries all share
// one event timestamp, so we collapse them and advance the matcher only once
// per press. (Synthetic events with timestamp 0 are never collapsed, so unit
// tests that call eventFilter directly are unaffected.)
class KeySequenceWatcher : public QObject {
  Q_OBJECT
 public:
  KeySequenceWatcher(std::vector<std::uint32_t> steps, std::function<void()> on_match, QObject* parent = nullptr);

  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  KeySequenceMatcher matcher_;
  std::function<void()> on_match_;
  quint64 last_key_timestamp_ = 0;
};

}  // namespace PJ

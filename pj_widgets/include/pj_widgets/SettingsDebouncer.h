#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QAnyStringView>
#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariant>

namespace PJ {

// Coalesces QSettings writes for a high-frequency source (e.g. a slider/scrubber
// drag) into ONE settings flush per settle window. The default QSettings INI
// backend rewrites the whole file on each setValue(), so persisting on every
// valueChanged tick thrashes the disk on the GUI thread (a single drag = one
// rewrite per mouse-move sample). Route the PERSIST through queue(); keep
// applying the live value to your UI/view yourself every tick — this type only
// defers the write, never the apply.
//
// GUI-thread only (owns a QTimer and touches QSettings). Writes go to the
// default-constructed QSettings() (the app organization/application scope),
// under `group` when non-empty. The last value queued for a key wins. Pending
// writes are flushed on the debounce timeout, on flush(), on destruction, and
// on QCoreApplication::aboutToQuit — so a quit inside the settle window never
// drops the last change.
class SettingsDebouncer : public QObject {
  Q_OBJECT
 public:
  // group: QSettings beginGroup() prefix ("" writes keys at the root, letting
  // callers pass already-qualified keys like "ui/icon_size").
  // delay_ms: settle window — the write fires this long after the last queue().
  explicit SettingsDebouncer(QString group = {}, int delay_ms = 400, QObject* parent = nullptr);
  ~SettingsDebouncer() override;

  // Buffer value under key and (re)arm the timer. Coalescing: repeated calls for
  // the same key before the next flush keep only the latest value. key is a
  // QAnyStringView so call sites pass a string literal / const char* / QString /
  // QStringLiteral with no conversion; the owning copy is taken internally.
  void queue(QAnyStringView key, const QVariant& value);
  // Bulk form: buffer a whole group of key→value pairs in one shot (e.g. a
  // "reset to defaults" / preset apply) so the group persists as a single
  // rewrite rather than one per key. Arms the timer once; empty input is a no-op.
  void queue(const QHash<QString, QVariant>& values);
  // Write every pending key now (one QSettings rewrite) and clear the buffer.
  // No-op when nothing is pending. Invoked by the timer, the dtor, and aboutToQuit.
  void flush();
  // Whether any writes are buffered (mainly for tests, or a caller that wants to
  // know a flush would do work).
  [[nodiscard]] bool hasPending() const {
    return !pending_.isEmpty();
  }

 private:
  QString group_;
  QHash<QString, QVariant> pending_;
  QTimer timer_;
};

}  // namespace PJ

// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "WidgetClipboard.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>

namespace PJ::widget_clipboard {
namespace {

constexpr const char* kWidgetClipboardMimeType = "application/x-plotjuggler-widget+xml";

}  // namespace

QString xml() {
  const QClipboard* clipboard = QGuiApplication::clipboard();
  if (clipboard == nullptr) {
    return {};
  }
  const QMimeData* mime = clipboard->mimeData();
  if (mime != nullptr && mime->hasFormat(QLatin1String(kWidgetClipboardMimeType))) {
    return QString::fromUtf8(mime->data(QLatin1String(kWidgetClipboardMimeType)));
  }
  return clipboard->text();
}

bool hasWidgetXml() {
  const QClipboard* clipboard = QGuiApplication::clipboard();
  if (clipboard == nullptr) {
    return false;
  }
  const QMimeData* mime = clipboard->mimeData();
  return mime != nullptr && mime->hasFormat(QLatin1String(kWidgetClipboardMimeType));
}

void setXml(const QString& xml) {
  auto* clipboard = QGuiApplication::clipboard();
  if (clipboard == nullptr || xml.isEmpty()) {
    return;
  }
  auto* mime = new QMimeData;
  mime->setData(QLatin1String(kWidgetClipboardMimeType), xml.toUtf8());
  mime->setText(xml);
  clipboard->setMimeData(mime);
}

bool parse(QDomDocument& doc, const QString& expected_tag) {
  const QString payload = xml();
  if (payload.isEmpty() || !doc.setContent(payload)) {
    return false;
  }
  return doc.documentElement().tagName() == expected_tag;
}

}  // namespace PJ::widget_clipboard

#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDomDocument>
#include <QString>

namespace PJ::widget_clipboard {

// Returns widget XML from the custom MIME payload or clipboard text fallback.
[[nodiscard]] QString xml();
// True when the clipboard carries a PlotJuggler widget XML MIME payload.
[[nodiscard]] bool hasWidgetXml();
// Stores widget XML as both custom MIME data and plain text.
void setXml(const QString& xml);
// Parses clipboard XML and checks that its root tag matches the expected widget.
[[nodiscard]] bool parse(QDomDocument& doc, const QString& expected_tag);

}  // namespace PJ::widget_clipboard

#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QWidget>

namespace PJ {

// Styled-widget adapters: swap plain controls that come out of a plugin's .ui
// for PlotJuggler's own styled equivalents, while keeping the ORIGINAL control
// hidden and alive so plugin data/events keep flowing through it unchanged.
//
//   QRadioButton (exclusive pair) -> DualOptionsWidget (segmented control)
//   QCheckBox                     -> ToggleSwitch (inline label on the left)
//   QComboBox                     -> PJ::ComboBox styling (gradient popup)
//
// The originals stay in the widget tree (hidden) and remain the source of truth
// for plugin WidgetData and the event callbacks wired by connectWidgetSignals;
// the styled replacements just drive them. Adaptation is structural (depends on
// the widget tree, built once at load), so the engines call adaptStyledWidgets
// once after loading the .ui; widget_binding then keeps the replacements in sync
// per applied widget via the seam below.

/// Adapt every adaptable control under `root` (radios, checkboxes, comboboxes).
/// Call once after the .ui is loaded. Idempotent and safe to re-run.
void adaptStyledWidgets(QWidget* root);

/// Per-kind entry points (adaptStyledWidgets calls each). Exposed individually
/// so each adapter can be unit-tested in isolation.
void adaptRadioButtonPairs(QWidget* root);
void adaptCheckBoxes(QWidget* root);
void adaptComboBoxes(QWidget* root);
/// Attach a PJ::Scrollbar overlay (one horizontal + one vertical) to every
/// QAbstractScrollArea found under `root` that has not yet been adapted. The
/// native bars are hidden (policy forced to AlwaysOff); the pill overlays own
/// hover/fade/drag. Idempotent: a marker property prevents double-attachment.
///
/// Per-area config (set as dynamic properties BEFORE calling this function):
///   pjScrollbarAutoHide (bool, default true)  — fade-on-hover vs always-on
///   pjScrollbarFadeMs   (int,  default 150)   — fade animation duration
void adaptScrollAreas(QWidget* root);

/// Reactively adapt the single widget `w` if it just became adaptable (e.g.
/// plugin data selected one option of a previously-unselected radio pair, or
/// gave a checkbox its text). No-op if `w` is already adapted or not adaptable.
void tryAdaptStyledWidget(QWidget* w);

/// Push `w`'s freshly-applied state (checked / enabled) onto its styled
/// replacement. No-op if `w` is not an adapted original.
void syncStyledWidget(QWidget* w);

/// If `w` is an adapted original whose replacement owns the visible geometry,
/// stash the desired visibility on `w` (the replacement derives its own) and
/// return true. Returns false when `w` is not adapted — the caller should then
/// apply visibility to `w` directly.
[[nodiscard]] bool redirectAdaptedVisibility(QWidget* w, bool visible);

}  // namespace PJ

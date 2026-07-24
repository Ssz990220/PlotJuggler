#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QColor>
#include <QString>
#include <utility>

namespace PJ::theme {

// Typed C++ accessor over the semantic UI framework's palette. The single
// ground truth for the values is the QSS palette in
// resources/stylesheet_{dark,light}.qss (spec: resources/ui_framework.md);
// QSS-driven widgets reach those tokens as `${token}`. Widgets that paint
// themselves with QPainter can't reach the QSS engine, so they call these
// functions to read the SAME token values by name.
//
// This header holds NO color/size numbers. Each call parses the compiled
// stylesheet resource for the requested theme (cached; the .qss files are
// build-time qrc resources that never change at runtime) and returns the token's
// value. Edit a value in the QSS palette and both render paths follow — there is
// nothing to keep "in sync" because there is only one source.
//
// If the stylesheet resource is not linked (e.g. a headless unit test that
// doesn't pull in resources.qrc), values resolve to an invalid QColor / -1 and a
// warning is logged — link the qrc to fix, exactly as QSS-styled widgets need it.
//
// This is the single framework colour vocabulary — every colour in the app
// resolves from here (the legacy brand/overlay tokens and the old ThemeColors.h
// mirror have been retired). Beyond the base surface/interaction/outline/overlay
// grid it also covers the extended semantic families: status, destructive,
// progress, slider/scroll, syntax highlighting, contrast ink, and the dev-only
// diagnostic palette. Roles are catalogued in resources/ui_framework_roadmap.md.

// Which stylesheet a value is resolved for. Prefer themeFor(bool) at call sites
// that only have a light/dark flag (e.g. from QPalette::Window lightness).
enum class Theme { Dark, Light };

// The seven background surface roles (ui_framework.md § Surfaces). A region's
// fill is chosen by its role, never by a raw literal.
enum class Surface {
  Banner,        // titlebar / section header bands
  Backdrop,      // default window / dialog / popup background
  DataBackdrop,  // interactable data containers: plots, trees, tables, scrubbers
  Input,         // editable text entry (line/text edits, filter fields)
  Separation,    // dividers, splitter handles, borders
  BannerInput,   // an input embedded inside a Banner
  ScrollHandle,  // grabbable scroll handles (not timeline/playback sliders)
};

// The solid interaction variants (ui_framework.md § Interactions). These are
// accent colour families named by visual emphasis, not by state: Neutral (grey
// default), Accent (blue), Highlight (magenta), Emphasis (yellow). "Special" is
// not a variant here — it is a per-state Accent→Highlight gradient; build it
// via special().
enum class Variant { Neutral, Accent, Highlight, Emphasis };

// The interaction state ramp an enabled, interactive element walks through.
// Focus is drawn as an outline over the nominal fill, so interaction()/onFill()
// map State::Focused to Nominal; the visible focus color comes from
// outline(role, OutlineState::Focused, theme).
enum class State { Nominal, Hovered, Focused, Checked, Pressed, CheckedHovered, CheckedPressed, Disabled };

// Content-ink emphasis levels for surface foreground (ui_framework.md § Foreground).
enum class Emphasis { Default, Muted, Disabled };

// Stroke-ink roles for borders, dividers, gridlines, focus rings, and checked
// outlines (ui_framework.md § Outline). These are strokes, not fills;
// Surface::Separation remains the fill role for separator surfaces.
enum class OutlineRole { Default, Interactive, Divider, Gridline };

// The outline state ramp. Focus is a distinct ring color here (not collapsed
// into hover).
enum class OutlineState { Rest, Hovered, Focused, Checked, Disabled };

// Alpha-bearing overlay roles (ui_framework.md § Overlays and Selection). Hover,
// Pressed, and Selected are state layers; Scrim and Hud are content veils.
enum class Overlay { Hover, Pressed, Selected, Scrim, Hud };

// Selection fill/ink pairs, distinct from interaction Checked. Item is the solid
// view/list/tab selection; TextEdit is the translucent selection inside editors.
enum class Selection { Item, TextEdit };

// Raised/floating surface roles (ui_framework.md § Elevation Surfaces). Kept
// separate from Surface so these don't churn the base seven background surfaces.
enum class ElevatedSurface { Dialog, Card, CardHover, Toast, Disabled };

// Named two-stop framework gradients. QSS composes these endpoint tokens as
// qlineargradient(... stop:0 ${gradient_*_start}, stop:1 ${gradient_*_end}).
enum class Gradient { Accent, Brand };
enum class GradientEndpoint { Start, End };

// Extended semantic families that the base interaction grid cannot express.
// Status hues (semantic warning/success/error/info/neutral), destructive (close/
// remove) actions, progress-bar parts, editor syntax ink, theme-independent
// contrast ink, and the dev-only diagnostic categorical palette.
enum class Status { Success, Warning, Error, Info, Neutral };
enum class Destructive { Ink, Nominal, Hovered, Pressed };
enum class ProgressRole { Outline, Track, Indicator };
enum class SyntaxRole { Keyword, Number, String, Comment, Builtin };
enum class Contrast { Dark, Light };
enum class Diagnostic {
  Fallback,
  MainWindow,
  Dialog,
  Widget,
  Frame,
  Splitter,
  SplitterHandle,
  TabWidget,
  TabPane,
  Tab,
  MenuBar,
  MenuBarItem,
  Menu,
  MenuItem,
  ToolTip,
  LineEdit,
  PlainTextEdit,
  TextBrowser,
  ComboBox,
  SpinBox,
  CheckBox,
  RadioButton,
  GroupBox,
  Label,
  PushButton,
  ListView,
  TreeView,
  HeaderSection,
  ScrollBar,
  ScrollHandle,
  SliderGroove,
  SliderHandle,
  SliderSubPage,
  PlotWidget,
  QwtPlot,
  TitleBar,
  TitleBarButton
};

// Non-color axes (ui_framework.md § Size / Non-Color Axes). Theme-agnostic; the
// Theme parameter only selects which palette copy to read.
enum class Space { None, Tight, Snug, Comfortable, Section };
enum class Metric {
  InputMinHeight,
  InputOuterHeight,
};
enum class Radius { Square, Input, Card, Dialog, Pill };
enum class Stroke { Hairline, Emphasis };
enum class Motion { Fast, Base, Slow, RevealHold };
enum class TextRole { Caption, Body, Title, Heading };
enum class TypeFacet { Family, Size, Weight };
struct TypeSpec {
  int size;
  int weight;
  QString family;
};

// -------- Color: surfaces & interaction fills --------

// Framework color for a surface role in the given theme.
QColor surface(Surface s, Theme t);

// Framework color for a solid interaction variant in a given state and theme.
QColor interaction(Variant v, State st, Theme t);

// The "Special" variant for a state: the gradient endpoints {Accent, Highlight}
// of that same state. Callers build the QLinearGradient; QSS composes the
// identical gradient from the two ${accent_*}/${highlight_*} tokens.
std::pair<QColor, QColor> special(State st, Theme t);

// -------- Color: foreground / content ink --------

// Default foreground / text ink — an alias for onSurface(Backdrop, Default).
QColor text(Theme t);

// Content ink on a background surface role at the requested emphasis.
QColor onSurface(Surface s, Emphasis emphasis, Theme t);

// Content ink on a solid interaction fill for the variant/state cell. The
// legible ink flips by state (WCAG), so this is per-cell, not per-variant.
QColor onFill(Variant v, State st, Theme t);

// Content ink over the Special gradient for the given state.
QColor onSpecial(State st, Theme t);

// Icon ink. Nominal icons match the body-text ink; disabled icons drop to a
// muted mid-grey.
QColor iconInk(Theme t);
QColor iconInkDisabled(Theme t);

// -------- Color: outline / overlay / selection --------

// Framework stroke ink for an outline role in a given state and theme.
QColor outline(OutlineRole r, OutlineState st, Theme t);

// Framework alpha overlay for the given role and theme.
QColor overlay(Overlay o, Theme t);

// Selection fill and ink for the given selection role and theme.
QColor selectionFill(Selection s, Theme t);
QColor onSelection(Selection s, Theme t);

// -------- Color: elevated surfaces & gradients --------

// Framework color for a raised/floating surface role in the given theme.
QColor elevatedSurface(ElevatedSurface s, Theme t);

// Named gradient endpoints {start, end}; callers build the QLinearGradient.
std::pair<QColor, QColor> gradient(Gradient g, Theme t);

// -------- Extended semantic families --------

// Semantic status hue (theme-agnostic warning/success/error/info/neutral).
QColor status(Status s, Theme t);
// Hovered status hue. Only Warning and Info define a hover; the rest return the
// nominal status(s, t).
QColor statusHovered(Status s, Theme t);
// Ink over a saturated status fill — per status, because the legible ink flips
// with the hue's luminance (dark ink on the warm/light hues, white on error).
QColor onStatus(Status s, Theme t);
// Soft invalid-field error surface + its ink (validation backgrounds).
QColor statusErrorSurface(Theme t);
QColor onStatusErrorSurface(Theme t);

// Destructive (close / remove) fills and the ink over them — per fill state,
// because the pale Hovered fill needs dark ink while Nominal/Pressed need white.
QColor destructive(Destructive d, Theme t);
QColor onDestructive(Destructive d, Theme t);

// Progress-bar parts (outline / track / indicator) and the caption ink.
QColor progress(ProgressRole r, Theme t);
QColor onProgress(Theme t);

// Slider handle (nominal / hovered) and the scrollbar-handle hover tint.
QColor sliderHandle(bool hovered, Theme t);
QColor scrollHandleHovered(Theme t);

// Foreground ink over the Overlay::Hud veil (HUD / tracker / hint text).
QColor onOverlayHud(Theme t);

// Code-editor body ink (the editor background is Surface::DataBackdrop).
QColor onCodeEditor(Theme t);

// Editor syntax-highlighting ink for a token role.
QColor syntaxInk(SyntaxRole r, Theme t);

// Theme-independent guaranteed-contrast ink — for a cursor/ring drawn over an
// arbitrary user-picked colour, where normal theme ink can't guarantee contrast.
QColor contrastInk(Contrast c);

// Dev-only categorical diagnostic hue (DebugUi widget-class outlines). Distinct
// per widget class so the debug overlay stays legible; not for product chrome.
QColor diagnostic(Diagnostic d);

// Muted tree-branch line ink (also baked into the *_light/_dark.svg branch assets).
QColor iconInkMuted(Theme t);

// -------- Non-color axes --------

int space(Space s, Theme t = Theme::Dark);
int metric(Metric m, Theme t = Theme::Dark);
int radius(Radius r, Theme t = Theme::Dark);
int stroke(Stroke s, Theme t = Theme::Dark);
int duration(Motion m, Theme t = Theme::Dark);
TypeSpec type(TextRole role, Theme t = Theme::Dark);

// -------- QSS palette keys (enum -> token name) --------

QString key(Surface s);
QString key(Variant v, State st);
QString key(Emphasis emphasis);
QString key(OutlineRole r, OutlineState st);
QString key(Overlay o);
QString key(ElevatedSurface s);
QString key(Gradient g, GradientEndpoint endpoint);
QString key(Space s);
QString key(Metric m);
QString key(Radius r);
QString key(Stroke s);
QString key(Motion m);
QString key(TextRole role, TypeFacet facet);
QString onSurfaceKey(Surface s, Emphasis emphasis);
QString onFillKey(Variant v, State st);
QString onSpecialKey(State st);
QString selectionFillKey(Selection s);
QString onSelectionKey(Selection s);
QString iconInkKey();
QString iconInkDisabledKey();
QString textKey();

// Resolve a Theme from a plain light/dark flag (e.g. a widget's palette lightness).
inline Theme themeFor(bool light) {
  return light ? Theme::Light : Theme::Dark;
}

// The active theme, derived from the APPLICATION palette's Window lightness
// (Theme.cpp keeps QGuiApplication::palette() in lockstep with the loaded
// stylesheet). Always use this instead of deriving a theme from a
// widget-local palette: QStyleSheetStyle rewrites widget palettes under QSS
// (a styled ancestor can resolve Window to #000000), which mis-detects
// "dark" inside styled containers. Returns Light when no QGuiApplication
// exists (headless tests).
[[nodiscard]] Theme appTheme();

}  // namespace PJ::theme

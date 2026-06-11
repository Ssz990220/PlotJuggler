# Visual guidelines

This document is the single source of truth for PJ4's theme, chrome
layout, icon pipeline, and QSS authoring discipline. Every UI change
should be reviewed against this file. If you find yourself wanting to
introduce a new value that conflicts with what's here, update this
doc first and have the change reviewed.

The supporting QSS lives in `resources/stylesheet_light.qss` and
`resources/stylesheet_dark.qss`. Both share an identical token block
delimited by `// PALETTE START` and `// PALETTE END`; `Theme.cpp`
parses that block at load time and substitutes `${KEY}` placeholders
in the body of each stylesheet. The body must contain **only** token
references — no hard-coded colors, sizes, or paddings.

---

## 1. Palette

The token names below are stable; the values differ per theme.

### 1.1 Surfaces

| Token | Light | Dark | Used for |
|---|---|---|---|
| `widget_background` | `transparent` | `transparent` | Default widget fill — lets the parent paint show through. |
| `main_background` | `#eeeeee` | `#373743` | Generic content background (currently unused; reserve for non-chrome panels). |
| `dark_background` | `#F5F5F5` | `#3B3B47` | `QMainWindow` and `QDialog` body fill. |
| `titlebar_background` | `#e0e0e0` | `#1C1C22` | Every chrome row (TitleBar, dialog title bar, Sources / Datasets / Custom Series header bands). |
| `widget_background_disabled` | `#eeeeee` | `#404040` | Disabled-state fill for input widgets. |
| `input_background` | `#FFFFFF` | `#4D4D5A` | Fill for the input-chrome family (`QLineEdit`, `QAbstractSpinBox`, `QComboBox`, `DoubleScrubber`) — reads as a chart-surface inset rectangle. |

### 1.2 Text

| Token | Light | Dark | Used for |
|---|---|---|---|
| `default_text` | `#111111` | `#F0F0F0` | All body and chrome text. |
| `disabled_text` | `#666666` | `#777777` | Text inside disabled widgets. |
| `inverse_text` | `#ffffff` | `#111111` | Foreground ink on a saturated brand background (e.g. close-button hover/pressed). |

### 1.3 Borders

| Token | Light | Dark | Used for |
|---|---|---|---|
| `border_default` | `#c0c0c0` | `#B0B0BF` | Every chrome separator: title-bar bottom border, splitter handles, `#timelineSeparator`, `#plotTabsSeparator`. |
| `border_hover` | `#62c5ff` | `#148CD2` | Focus / hover border on data-input widgets (currently `IntScrubber` etc.). |
| `border_checked` | `#1177ff` | `#1177ff` | Checked-state border on toggleable widgets. |

### 1.4 Brand accents (theme-agnostic — same in both stylesheets)

| Token | Value | Used for |
|---|---|---|
| `blue` | `#1177FF` | Primary call-to-action; selection sub-page on `#timeSlider`. |
| `light_blue` | `#C2DCFF` | Hover state on tabs / menu items in light theme; track of the selected portion of `#timeSlider`. |
| `purple` | `#CC00CC` | Destructive / window-close hover on dark theme; pressed state in light. |
| `purple_dark` (dark only) | `#990099` | Window-close pressed state in dark theme. |
| `light_purple` | `#FFAEFF` | Window-close hover state in light theme. |

### 1.5 Overlays

`hover_overlay` and `pressed_overlay` are translucent inks that
darken (light theme) or lighten (dark theme) whatever sits beneath
them. Always prefer these to a solid background swap for transient
states — the parent's color stays visible through them, so the
effect reads as "this thing is interactive" rather than "this thing
swapped identity".

| Token | Light | Dark |
|---|---|---|
| `hover_overlay` | `rgba(0, 0, 0, 40)` | `rgba(255, 255, 255, 30)` |
| `pressed_overlay` | `rgba(0, 0, 0, 70)` | `rgba(255, 255, 255, 70)` |
| `ink_select` | `rgba(0, 0, 0, 60)` | `rgba(255, 255, 255, 60)` |

### 1.6 Scrollbars / sliders

| Token | Light | Dark | Used for |
|---|---|---|---|
| `scrollbar_handle` | `#787878` | `#787878` | Default scrollbar thumb fill. |
| `scrollbar_handle_hover` | `#148CD2` | `#148CD2` | Hover/pressed thumb. |
| `slider_handle` | `#999999` | `#999999` | Generic `QSlider` thumb (not the time slider). |
| `slider_handle_hover` | `#99ccff` | `#99ccff` | Generic slider hover. |

The playback `#timeSlider` is special-cased: handle is `light_purple`
nominally, `purple` on hover/pressed.

### 1.7 Selection

| Token | Light | Dark |
|---|---|---|
| `item_selection_background` | `#C2DCFF` (= `light_blue`) | `#148CD2` |

---

## 2. Chrome geometry

Every horizontal chrome row in the app is **exactly 24 px tall**.
That includes:

- TitleBar (top app bar): app icon + embedded QMenuBar (File /
  Toolbox / Help) on the left; notification bell, panel toggles and
  window controls on the right
- Sources / Datasets / Custom Series header bands
- Playback row
- Right-side toolbar column width (same value, vertical)

The tab strip is the one exception: the strip itself is **23 px**,
followed by a **1-px** sibling separator (`tabs_separator`) — total
chrome contribution still 24 px so the plot area's top edge aligns
with the curve-list content on the left column.

Geometry tokens:

| Token | Value | Used for |
|---|---|---|
| `chrome_bar_height` | `24` | Chrome row height in `.ui` files and runtime layout code. |
| `chrome_tab_strip_height` | `23` | TabbedPlotWidget's strip. |
| `chrome_button_size` | `24` | Flush button that fills its chrome row (Sources / Datasets / Custom Series / RHS / Playback). |
| `chrome_button_size_inset` | `23` | Inset button that fits inside a chrome row that draws its own `border-bottom` (TitleBar, dialog title bar). |
| `chrome_icon_size` | `20` | Icon rendered inside every chrome button. |
| `chrome_spacing` | `4` | Horizontal gap between icon buttons in the same row; also label-text left padding. |
| `chrome_border_width` | `1` | Every chrome border. |
| `corner_radius` | `4` | Corner radius for input chrome (`QLineEdit`, `QAbstractSpinBox`, `QComboBox`, `DoubleScrubber`). Keep `ScrubberBase::kCornerRadiusPx` in lockstep. |
| `input_min_height` | `18` | Min **content** height shared by every input-chrome widget so text fields, spin boxes and combo boxes resolve to one identical outer height (18 + 2×2 padding + 2×1 border = 24). |

### 2.1 Why two button sizes

The buttons are sized to **fill the inner content rect of their
row, flush** — no 0.5-px asymmetry, no breathing room above or
below.

- Bars **without** a QSS `border-bottom` (Sources, Datasets, Custom
  Series, RHS toolbar, Playback) have an inner rect equal to their
  outer height: 24 px. Buttons inside them are `24×24`.
- Bars **with** a `border-bottom: 1px solid border_default`
  (TitleBar, dialog title bar) have an inner rect of `24 − 1 = 23`
  px. Buttons inside them are `23×23`.

The tab strip's elements (`PlotTabFrame`, `[+]` button) are also
`23×23`, because the strip is 23 px and they fill its inner rect.
The panel toggles share that size but live in the TitleBar's right
cluster (created by TabbedPlotWidget, reparented by the shell).

Icons inside any chrome button are always `20×20`, leaving a 1–2 px
ink-ring of padding inside the button. **Do not** override icon
size per widget — set it once on the button via `iconSize` and let
the chrome look uniform.

### 2.2 Spacing rules

- Horizontal `spacing` between icon buttons in a chrome row: **0**
  inside Sources/Datasets/Custom Series headers (buttons sit flush
  next to each other and the gap is the icon ring); **4** inside
  TitleBar and Playback (where the buttons need visual breathing
  room because they carry text-adjacent icons).
- Label-text → first widget gap: **`chrome_spacing` (4 px)**, set
  via the label's `padding-left` or `setContentsMargins(4, 0, 0, 0)`.
- Layout `contentsMargins` on chrome row containers: **`0, 0, 0, 0`**.
  Vertical centering inside the row is done by Qt's automatic
  alignment when the button is `Fixed,Fixed`-policy and matches the
  row's inner content height.
- Inter-row spacing (between rows in a stacked widget like
  `pageStream`): **`chrome_spacing` (4 px)** vertical.

### 2.3 Splitter handles

All `QSplitter` instances have `setHandleWidth(1)` in C++ and the
handle paints with `border_default` via QSS. There's no separate
splitter-handle widget to style.

### 2.4 Border radii

**Zero** everywhere **except input chrome.** Chrome bars, panels,
buttons, tabs and views all use square corners (`border-radius: 0px`) —
that's the house style, so push back if you reach for a rounded corner
on those.

The one deliberate exception is the **input-chrome family** —
`QLineEdit`, `QAbstractSpinBox` and `QComboBox` (plus the C++-painted
`DoubleScrubber`) — which share `${corner_radius}px` (4px) rounded
corners, the `${input_background}` fill, and a `${border_hover}` focus
ring, so text fields, spin boxes and combo boxes read as one consistent
inset-input family. The `PJ::MessageBox` card is a second, separate
exception (`6`/`8px`) covered in its own section.

Inline search fields embedded in header bands (`#lineEditFilter`,
`#lineEditCustomFilter`, `#lineEditCurvesFilter`, `#lineEditPrefix`) opt
back out via id selectors (transparent, borderless, `padding: 0`) — the
leading magnifier icon already cues "type here", so they stay flat.

---

## 3. Icons

### 3.1 Source family

All Material-design icons in `resources/svg/` come from **Google
Material Symbols** with these axis settings:

| Axis | Value | Why |
|---|---|---|
| `style` | `sharp` | Square geometry matches the app's chrome. |
| `weight` | `300` | Light stroke — slim but readable at toolbar sizes. |
| `grade` | `0` | Neutral grade. |
| `fill` | `1` | Filled glyphs. |
| `optical-size` | `24` | Standard for 16–24 px UI use. |

### 3.2 Recipe

Icons are generated by `resources/scripts/download_material_icons.py`,
which fetches Google Material Symbols SVGs from the gstatic CDN per the
inline `ICON_MAPPING` table (key = target filename, value = Material
Symbol name + optional transforms) and injects the `--fill-color`
(default `#3D3D3D`, the canonical light-theme ink). To add an icon, add
an entry to `ICON_MAPPING` and re-run the script. The script's axis
defaults differ from PJ4's house axes (§3.1), so pass them explicitly to
match the existing set, e.g.:

```bash
python3 resources/scripts/download_material_icons.py \
    --style sharp --weight 300 --grade 0 --optical-size 24 --fill 1 \
    -o resources/svg/
```

Under the hood the script does, per icon:

1. Pull the icon SVG from Google's CDN at the path
   `https://fonts.gstatic.com/s/i/short-term/release/materialsymbolssharp/<icon_id>/wght300grad0fill1/24px.svg`
   where `<icon_id>` is the snake-case Material Symbol name
   (e.g. `Add Column Right` → `add_column_right`).
2. The CDN returns the SVG with no `fill` attribute; inject
   `fill="#3D3D3D"` on every `<path>` element. This is the canonical
   "light theme ink" — runtime recoloring in
   `pj_widgets/SvgUtil.h::RecolorSvgInk()` swaps it to `#E0E0E0`
   when the dark theme is active.
3. Save as `resources/svg/<target_filename>.svg`.

For state variants (`_dark`, `_disabled_*`, `_hover`, `_pressed`):
copy the base SVG, then replace the `fill` attribute. The runtime
recoloring helper (`PJ::RecolorSvgInk` in
`pj_widgets/include/pj_widgets/SvgUtil.h`) does this swap
automatically based on the active theme.

| Suffix | Fill / treatment | Used for |
|---|---|---|
| _(none)_ / `_light` | `fill="#3D3D3D"` | Light theme. |
| `_dark` | `fill="#E0E0E0"` | Dark theme. |
| `_disabled_light` | `fill="#3D3D3D"` + `opacity="0.4"` | Disabled in light. |
| `_disabled_dark` | `fill="#E0E0E0"` + `opacity="0.4"` | Disabled in dark. |
| `_hover` | `fill="#1177FF"` | QSS `:hover` (scrollbar arrows etc.). |
| `_pressed` | `fill="#CC00CC"` | QSS `:pressed`. |

### 3.3 Hand-crafted assets

A small set of SVGs aren't from Material Symbols and follow their
own palette:

| File | Ink |
|---|---|
| `branch_*_light.svg` | `#B8B8B8` (intentional gray accent for `QTreeView` indicators) |
| `branch_*_dark.svg` | `#454545` |
| `close_windows_light.svg` | `stroke="#3D3D3D"` |
| `close_windows_dark.svg` | `stroke="#E0E0E0"` |
| `maximize.svg` / `minimize.svg` | `stroke="#3D3D3D"` |
| `panel_{bottom,left,right}.svg` | `#3D3D3D` (fill + stroke) |
| `play_arrow{,_left}.svg` | `fill="#3D3D3D"` |
| `plotjuggler.svg` | Multi-color app logo — do not recolor. |

---

## 4. QSS authoring discipline

### 4.1 Tokens, not literals

**Always reference a palette token.** A hard-coded hex / rgba / px
value in the body of `stylesheet_*.qss` is a code smell — promote
it to a token in the `PALETTE` block before re-using it in a second
rule. The intent: a future contributor can change a color or a
spacing in one place and have the whole app update consistently.

Counter-example (avoid):

```css
QFrame#mySection {
    background: #e0e0e0;          /* ← what color was this again? */
    border-bottom: 1px solid #c0c0c0;
    padding: 4px;
}
```

Use:

```css
QFrame#mySection {
    background: ${titlebar_background};
    border-bottom: ${chrome_border_width}px solid ${border_default};
    padding: ${chrome_spacing}px;
}
```

### 4.2 Reuse, don't copy

If two widgets share visual treatment, **combine the selectors in
one rule** rather than authoring two near-identical blocks.

Counter-example (avoid):

```css
#TitleBar { background: ${titlebar_background}; border-bottom: 1px solid ${border_default}; }
QDialog QWidget#dialogTitleBar { background: ${titlebar_background}; border-bottom: 1px solid ${border_default}; }
```

Use:

```css
#TitleBar,
QDialog QWidget#dialogTitleBar {
    background: ${titlebar_background};
    border-bottom: ${chrome_border_width}px solid ${border_default};
}
```

Cluster the related rules together with a comment header. See the
"`Frameless chrome bars`" and "`Chrome buttons`" sections in the
stylesheets for the canonical pattern.

### 4.3 Theme symmetry

The light and dark stylesheets should differ **only in the values
of the palette tokens** — never in selectors or property values.
If you find a rule in one stylesheet that isn't in the other,
that's a bug.

### 4.4 Border radii

Stay at `0px`. The house style is sharp corners across every
widget. If a designer asks for rounded corners on something, raise
it as an explicit design exception rather than slipping it into a
rule.

### 4.5 Padding / margin discipline

Inside chrome rows:

- Outer row layout: `contentsMargins(0, 0, 0, 0)`.
- Inter-button spacing: `0` or `chrome_spacing` (see § 2.2).
- Per-button padding: `0`. The icon's intrinsic padding inside its
  button is the only ring around the glyph.
- Label content margin: `(chrome_spacing, 0, 0, 0)` if the label
  needs separation from the row's left edge.

---

## 4.6 Inline `setStyleSheet` is the exception, not the rule

`QWidget::setStyleSheet(...)` calls in C++ should be **rare**. Audit
checklist before you reach for one:

1. **Could this be an `objectName` + a QSS rule?** If yes, do that.
   The pattern is: in C++, `widget->setObjectName("foo")`; in
   `stylesheet_*.qss`, write `QPushButton#foo { ... }`.
2. **Is the value baked into a dynamic data element** (e.g. a
   per-row colour swatch where each instance carries its own
   colour)? Then inline `setStyleSheet` is the correct expression —
   the colour is *data*, not chrome.
3. **Is it a runtime polish for a Qt internal that doesn't honour
   QSS** (e.g. `QwtPlotCanvas` backing store, `QComboBox` popup
   palettes inside `WidgetTuner`)? Then it lives in C++ as a
   documented exception. Add a `TODO(theme):` comment.

The only `setStyleSheet("")` (empty) calls in the tree are
deliberate "clear inherited Qt styling" calls (e.g. `PlotDocker`
disabling Qt-Advanced-Docking's own QSS layer). Those are fine.

## 5. Runtime helpers

A few pieces of chrome live in C++ rather than QSS because Qt's
default styling has hard-to-style internals:

- `pj_widgets/include/pj_widgets/SvgUtil.h::RecolorSvgInk()` — swaps `#3D3D3D` ↔
  `#E0E0E0` (and folds `#000000` / `#ffffff` onto the palette) so
  every C++-loaded SVG follows the active theme without needing a
  `_light` / `_dark` filename. Called from `LoadSvg()` and
  `RenderSvgPixmap()`.
- `pj_app/src/WidgetTuner` — application-wide event filter that
  stamps `objectName="PJMenu"` on auto-generated context menus
  (so they pick up the `QMenu#PJMenu` QSS rules) and clamps
  `QComboBox` popup palettes.
- `pj_widgets/include/pj_widgets/Style.h` — `PJ::Style`, a
  `QProxyStyle` over Fusion that suppresses the platform's
  auto-injected dialog-button icons (`SH_DialogButtonBox_ButtonsHaveIcons`)
  and mnemonic underlines (`SH_UnderlineShortcut`); installed in
  `pj_app/src/main.cpp` via
  `QApplication::setStyle(new PJ::Style("Fusion"))`.

If you add a new widget category that needs runtime polish, prefer
extending one of these helpers over inventing a new one.

---

## 6. When to update this document

- A new palette token enters either stylesheet.
- A new chrome geometry value (button size, bar height, spacing)
  is introduced.
- A new icon family or recoloring rule is added.
- A new "duplicate the rule, change one thing" pattern emerges
  that should be unified into a shared selector.

Edits to this file should land in the same MR as the change they
describe.

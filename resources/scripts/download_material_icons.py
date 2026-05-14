#!/usr/bin/env python3
"""Download Google Material Symbols SVGs and save them under PJ4 icon names.

The icon mapping is defined inline below as `ICON_MAPPING`. The key is the
target filename (unique); the value is a `(Material Symbol Name, transforms)`
tuple. Transforms is an optional dict supporting:

    rotate=<deg>    Wrap the SVG body in a rotate transform centered on the viewBox.
    hflip=1         Horizontal mirror.
    vflip=1         Vertical mirror.
    fill=0|1        Per-entry override of Material's fill axis.

Variant axes (configurable via flags):

    --style         outlined | rounded | sharp     (sharpness; default outlined)
    --weight        100..700                       (default 300)
    --fill          0 | 1                          (default 0; per-entry fill= wins)
    --grade         -25 | 0 | 200                  (default 0)
    --optical-size  20 | 24 | 40 | 48              (default 24)

URL pattern used:

    https://fonts.gstatic.com/s/i/short-term/release/
        materialsymbols{style}/{name}/{variant}/{size}px.svg

where {variant} is "default" if every axis is at its default, otherwise
the non-default axes concatenated as "wght{N}grad{N}fill{N}".

Example:

    python3 scripts/download_material_icons.py \
        --style rounded --optical-size 24 \
        -o resources/svg/

A duplicate Material name (e.g. "Circle" -> green/red, or "Position Top
Right" -> top-right and h-flipped top-left) is fetched once per
(name, variant) pair and written to each target.
"""

from __future__ import annotations

import argparse
import re
import sys
import urllib.error
import urllib.request
from pathlib import Path

GSTATIC_URL = (
    "https://fonts.gstatic.com/s/i/short-term/release/"
    "materialsymbols{style}/{name}/{variant}/{size}px.svg"
)


# {target_filename: (material_symbol_name, transforms)}. Keyed by target so
# each output file is unambiguous; the same Material name can appear in
# multiple entries (different transforms / different recolor targets).
ICON_MAPPING: dict[str, tuple[str, dict[str, str]]] = {
    "checkbox_checked_light.svg":          ("Check Box",                  {}),
    "checkbox_unchecked_light.svg":        ("Check Box Outline Blank",    {}),
    "radio_checked_light.svg":             ("Radio Button Checked",       {}),
    "radio_unchecked_light.svg":           ("Radio Button Unchecked",     {}),
    "light_mode_light.svg":                ("Light Mode",                 {}),
    "dark_mode_light.svg":                 ("Dark Mode",                  {}),
    "logout.svg":                          ("Logout",                     {}),
    "archive.svg":                         ("Archive",                    {}),
    "save_as.svg":                         ("Save As",                    {}),
    "dashboard_load.svg":                  ("Dashboard 2 Gear",           {}),
    "acute.svg":                           ("Acute",                      {}),
    "add_column.svg":                      ("Add Column Right",           {}),
    "add_row.svg":                         ("Add Row Below",              {}),
    "add_tab.svg":                         ("Add",                        {}),
    "alarm-bell.svg":                      ("Notifications",              {}),
    "alarm-bell-active.svg":               ("Notifications Active",       {"fill": "1"}),
    "diag_info.svg":                       ("Info",                       {}),
    "diag_warning.svg":                    ("Warning",                    {}),
    "diag_error.svg":                      ("Release Alert",              {}),
    "numbers.svg":                         ("123",                        {}),
    "apps_box.svg":                        ("Apps",                       {}),
    "cast.svg":                            ("Cast",                       {}),
    "clear.svg":                           ("Mop",                        {}),
    "cloud.svg":                           ("Cloud",                      {}),
    "close-button.svg":                    ("Close",                      {}),
    "collapse.svg":                        ("Collapse Content",           {}),
    "color_background.svg":                ("Filter B And W",             {}),
    "filter_list.svg":                     ("Filter List",                {}),
    "colored_charts.svg":                  ("Ssid Chart",                 {}),
    "check.svg":                           ("Check",                      {}),
    "copy.svg":                            ("Content Copy",               {}),
    "datetime.svg":                        ("Calendar Clock",             {}),
    "delete_forever.svg":                  ("Delete Forever",             {}),
    "draft.svg":                           ("Draft",                      {}),
    "drag_handle_horizontal.svg":          ("Drag Handle",                {}),
    "drag_handle_vertical.svg":            ("Drag Handle",                {"rotate": "90"}),
    "expand.svg":                          ("Expand Content",             {}),
    "expand_more.svg":                     ("Expand More",                {}),
    "extension.svg":                       ("Extension",                  {}),
    "export.svg":                          ("Upload",                     {}),
    "fullscreen.svg":                      ("Fullscreen",                 {}),
    "Fx.svg":                              ("Function",                   {}),
    "green_circle.svg":                    ("Circle",                     {}),
    "red_circle.svg":                      ("Circle",                     {}),
    "grid.svg":                            ("Background Grid Small",      {"fill": "1"}),
    "import.svg":                          ("Download",                   {}),
    "upload_file.svg":                     ("Upload File",                {}),
    "left-arrow.svg":                      ("Arrow Left",                 {}),
    "legend.svg":                          ("List",                       {"fill": "1"}),
    "line_width_1_0.svg":                  ("Pen Size 1",                 {"fill": "1"}),
    "line_width_1_5.svg":                  ("Pen Size 2",                 {"fill": "1"}),
    "line_width_2_0.svg":                  ("Pen Size 3",                 {"fill": "1"}),
    "line_width_3_0.svg":                  ("Pen Size 4",                 {"fill": "1"}),
    "link.svg":                            ("Link 2",                     {"fill": "1"}),
    "list.svg":                            ("List",                       {}),
    "keyboard_arrow_down_light.svg":       ("Keyboard Arrow Down",        {}),
    "keyboard_arrow_left_light.svg":       ("Keyboard Arrow Left",        {}),
    "keyboard_arrow_right_light.svg":      ("Keyboard Arrow Right",       {}),
    "keyboard_arrow_up_light.svg":         ("Keyboard Arrow Up",          {}),
    "loop.svg":                            ("Laps",                       {}),
    "mobile_layout.svg":                   ("Mobile Layout",              {}),
    "move_selection_right.svg":            ("Move Selection Right",       {}),
    "move_view.svg":                       ("Drag Pan",                   {}),
    "more_vert.svg":                       ("More Vert",                  {}),
    "panel_left.svg":                      ("Dock To Left",               {"fill": "1"}),
    "panel_right.svg":                     ("Dock To Right",              {"fill": "1"}),
    "panel_bottom.svg":                    ("Dock To Bottom",             {"fill": "1"}),
    "paste.svg":                           ("Content Paste",              {}),
    "pause.svg":                           ("Pause",                      {}),
    "plot_image.svg":                      ("Bid Landscape",              {}),
    "point_chart.svg":                     ("Timeline",                   {}),
    "ratio.svg":                           ("View Real Size",             {}),
    "reference_line.svg":                  ("Line Axis",                  {}),
    "reload_light.svg":                    ("Refresh",                    {}),
    "restore_page.svg":                    ("Restore Page",               {}),
    "remove_list.svg":                     ("Playlist Remove",            {}),
    "remove_red.svg":                      ("Cancel",                     {}),
    "right-arrow.svg":                     ("Arrow Right",                {}),
    "save.svg":                            ("Save",                       {}),
    "scatter.svg":                         ("Grain",                      {}),
    "search_light.svg":                    ("Search",                     {}),
    "settings_cog_light.svg":              ("Settings",                   {}),
    "share_eta.svg":                       ("Share Eta",                  {}),
    "show_point.svg":                      ("Step Into",                  {"fill": "1"}),
    "t0.svg":                              ("Start",                      {}),
    "tune.svg":                            ("Tune",                       {}),
    "trash.svg":                           ("Delete",                     {}),
    "tree.svg":                            ("Account Tree",               {}),
    "xy.svg":                              ("Table Chart View",           {}),
    "zoom_horizontal.svg":                 ("Arrows Outward",             {}),
    "zoom_in.svg":                         ("Zoom In",                    {}),
    "zoom_max.svg":                        ("Open With",                  {}),
    "zoom_vertical.svg":                   ("Arrows Outward",             {"rotate": "90"}),
    "position_bottom_right.svg":           ("Position Bottom Right",      {"fill": "1"}),
    "position_bottom_left.svg":            ("Position Bottom Left",       {"fill": "1"}),
    "position_top_right.svg":              ("Position Top Right",         {"fill": "1"}),
    "position_top_left.svg":               ("Position Top Right",         {"fill": "1", "hflip": "1"}),
    "scatter_plot.svg":                    ("Scatter Plot",               {"fill": "1"}),
}


def material_name_to_id(name: str) -> str:
    """`Add Column Right` -> `add_column_right`."""
    tokens = re.findall(r"[A-Za-z0-9]+", name)
    return "_".join(tokens).lower()


def build_variant(weight: int, fill: int, grade: int) -> str:
    if weight == 400 and fill == 0 and grade == 0:
        return "default"
    # Google's CDN expects non-default axes in the order: wght, grad, fill.
    parts: list[str] = []
    if weight != 400:
        parts.append(f"wght{weight}")
    if grade != 0:
        parts.append(f"grad{grade}")
    if fill != 0:
        parts.append(f"fill{fill}")
    return "".join(parts)


def inject_fill(svg_bytes: bytes, color: str) -> bytes:
    """Set `fill="<color>"` on every `<path>` element, replacing any existing
    fill attribute. Used to colorize freshly-downloaded Material Symbols
    SVGs (which ship without a fill) into the project's theme palette."""
    text = svg_bytes.decode("utf-8")

    def repl(match: re.Match[str]) -> str:
        tag = match.group(0)
        tag = re.sub(r'\s+fill="[^"]*"', "", tag)
        return tag.replace("<path", f'<path fill="{color}"', 1)

    text = re.sub(r"<path\b[^>]*?(?:/>|>)", repl, text)
    return text.encode("utf-8")


def apply_rotation(svg_bytes: bytes, angle: float) -> bytes:
    """Wrap the SVG body in `<g transform="rotate(angle cx cy)">` centered
    on the viewBox. Falls back to (0, 0) if no viewBox is present."""
    text = svg_bytes.decode("utf-8")
    cx = cy = 0.0
    box = re.search(r'viewBox\s*=\s*"([^"]+)"', text)
    if box is not None:
        try:
            x, y, w, h = (float(v) for v in box.group(1).split())
            cx, cy = x + w / 2, y + h / 2
        except ValueError:
            pass
    open_end = text.find(">")
    close_start = text.rfind("</svg>")
    if open_end < 0 or close_start < 0 or close_start <= open_end:
        return svg_bytes
    rotated = (
        text[: open_end + 1]
        + f'<g transform="rotate({angle} {cx} {cy})">'
        + text[open_end + 1 : close_start]
        + "</g>"
        + text[close_start:]
    )
    return rotated.encode("utf-8")


def apply_flip(svg_bytes: bytes, axis: str) -> bytes:
    """Wrap the SVG body in `<g transform="...">` to mirror it.

    axis="h" flips horizontally (mirrors left/right), axis="v" flips
    vertically. The transform is centered on the viewBox so the icon
    stays inside its drawing area; falls back to (0, 0) if no viewBox.
    """
    text = svg_bytes.decode("utf-8")
    cx = cy = 0.0
    box = re.search(r'viewBox\s*=\s*"([^"]+)"', text)
    if box is not None:
        try:
            x, y, w, h = (float(v) for v in box.group(1).split())
            cx, cy = x + w / 2, y + h / 2
        except ValueError:
            pass
    open_end = text.find(">")
    close_start = text.rfind("</svg>")
    if open_end < 0 or close_start < 0 or close_start <= open_end:
        return svg_bytes
    if axis == "h":
        transform = f"translate({2 * cx} 0) scale(-1 1)"
    elif axis == "v":
        transform = f"translate(0 {2 * cy}) scale(1 -1)"
    else:
        return svg_bytes
    flipped = (
        text[: open_end + 1]
        + f'<g transform="{transform}">'
        + text[open_end + 1 : close_start]
        + "</g>"
        + text[close_start:]
    )
    return flipped.encode("utf-8")


def download(url: str, user_agent: str, timeout: float) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": user_agent})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read()


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--style",
        choices=["outlined", "rounded", "sharp"],
        default="outlined",
        help="Material Symbols style (sharpness). Default: outlined.",
    )
    parser.add_argument(
        "--weight",
        type=int,
        default=300,
        choices=[100, 200, 300, 400, 500, 600, 700],
        help="Stroke weight axis. Default: 300 (matches the PJ4 icon family).",
    )
    parser.add_argument(
        "--fill",
        type=int,
        default=0,
        choices=[0, 1],
        help="Filled (1) vs unfilled (0). Default: 0.",
    )
    parser.add_argument(
        "--grade",
        type=int,
        default=0,
        choices=[-25, 0, 200],
        help="Grade axis. Default: 0.",
    )
    parser.add_argument(
        "--optical-size",
        "--size",
        dest="optical_size",
        type=int,
        default=24,
        choices=[20, 24, 40, 48],
        help="Optical size in pixels. Default: 24.",
    )
    parser.add_argument(
        "--fill-color",
        default=None,
        help="If set, inject fill=\"<color>\" into each downloaded SVG's <path> "
        "elements (e.g. '#474747'). Default: no fill (renders as currentColor/black).",
    )
    parser.add_argument(
        "--output-dir",
        "-o",
        default="material_icons",
        help="Output directory. Default: material_icons/",
    )
    parser.add_argument(
        "--overwrite",
        action="store_true",
        help="Overwrite existing files (otherwise they are skipped).",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print URLs without downloading.",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=30.0,
        help="HTTP timeout in seconds. Default: 30.",
    )
    parser.add_argument(
        "--user-agent",
        default="PJ4-icon-fetcher/1.0",
        help="User-Agent header sent with each request.",
    )
    args = parser.parse_args(argv)

    output_dir = Path(args.output_dir)
    if not args.dry_run:
        output_dir.mkdir(parents=True, exist_ok=True)

    # Cache so a duplicate Material name (e.g. "Circle" -> green/red) is
    # fetched once and written to each target file. Keyed by (icon_id,
    # variant) so two entries that need the same icon at different fill
    # axes don't trample each other.
    cache: dict[tuple[str, str], bytes] = {}
    ok = failed = skipped = 0

    for entry_no, (target, (material_name, transforms)) in enumerate(ICON_MAPPING.items(), start=1):
        target_path = output_dir / target
        if target_path.exists() and not args.overwrite and not args.dry_run:
            print(f"skip    {target}  (exists; rerun with --overwrite)")
            skipped += 1
            continue
        # Per-entry [fill=1] overrides the global --fill flag so individual
        # entries can opt into the solid variant (e.g. Position icons want
        # the filled corner indicator).
        line_fill = int(transforms.get("fill", args.fill))
        variant = build_variant(args.weight, line_fill, args.grade)
        icon_id = material_name_to_id(material_name)
        url = GSTATIC_URL.format(
            style=args.style, name=icon_id, variant=variant, size=args.optical_size
        )
        suffix_parts = []
        for key in ("rotate", "hflip", "vflip", "fill"):
            if key in transforms:
                suffix_parts.append(f"[{key}={transforms[key]}]")
        suffix = (" " + " ".join(suffix_parts)) if suffix_parts else ""
        if args.dry_run:
            print(f"would   {target}{suffix}  <- {url}")
            ok += 1
            continue

        cache_key = (icon_id, variant)
        try:
            if cache_key in cache:
                blob = cache[cache_key]
            else:
                blob = download(url, args.user_agent, args.timeout)
                cache[cache_key] = blob
        except urllib.error.HTTPError as exc:
            print(
                f"FAIL    {target}  (entry {entry_no}, '{material_name}' -> "
                f"{icon_id}, {variant}): HTTP {exc.code} {exc.reason} for {url}",
                file=sys.stderr,
            )
            failed += 1
            continue
        except urllib.error.URLError as exc:
            print(
                f"FAIL    {target}  (entry {entry_no}, '{material_name}'): {exc.reason}",
                file=sys.stderr,
            )
            failed += 1
            continue

        if "rotate" in transforms:
            try:
                blob = apply_rotation(blob, float(transforms["rotate"]))
            except ValueError:
                print(
                    f"warning: entry {entry_no} has non-numeric rotate value "
                    f"{transforms['rotate']!r}; writing un-rotated",
                    file=sys.stderr,
                )

        if transforms.get("hflip") == "1":
            blob = apply_flip(blob, "h")
        if transforms.get("vflip") == "1":
            blob = apply_flip(blob, "v")

        if args.fill_color:
            blob = inject_fill(blob, args.fill_color)

        target_path.parent.mkdir(parents=True, exist_ok=True)
        target_path.write_bytes(blob)
        print(f"ok      {target}{suffix}  <- {icon_id}/{variant}")
        ok += 1

    summary = f"\nDone: {ok} ok, {failed} failed, {skipped} skipped"
    if not args.dry_run:
        summary += f" -> {output_dir}/"
    print(summary, file=sys.stderr)
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

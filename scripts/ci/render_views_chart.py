#!/usr/bin/env python3
"""Render GitHub repository traffic views history into light/dark SVG charts."""
import argparse
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import sys
from xml.sax.saxutils import escape

PALETTES = {
    "light": {
        "bg": "#ffffff",
        "card_alt": "#f6f8fa",
        "border": "#d0d7de",
        "text": "#1f2328",
        "muted": "#656d76",
        "grid": "#eaeef2",
        "axis": "#d0d7de",
        "views": "#0969da",
        "uniques": "#1a7f37",
    },
    "dark": {
        "bg": "#0d1117",
        "card_alt": "#161b22",
        "border": "#30363d",
        "text": "#e6edf3",
        "muted": "#8b949e",
        "grid": "#21262d",
        "axis": "#30363d",
        "views": "#58a6ff",
        "uniques": "#3fb950",
    },
}


def _non_neg_int(val: object) -> int:
    try:
        return max(0, int(val))
    except (TypeError, ValueError):
        return 0


def _norm_date(raw: object) -> str:
    if not isinstance(raw, str) or len(raw) < 10:
        return ""
    day = raw[:10]
    try:
        datetime.strptime(day, "%Y-%m-%d")
    except ValueError:
        return ""
    return day


def merge_views_history(existing: dict[str, dict], traffic_payload: dict) -> dict[str, dict]:
    """Merge existing day->counts history with GitHub /traffic/views JSON payload."""
    merged: dict[str, dict] = {}
    if isinstance(existing, dict):
        for raw_day, entry in existing.items():
            day = _norm_date(raw_day)
            if not day or not isinstance(entry, dict):
                continue
            merged[day] = {
                "count": _non_neg_int(entry.get("count", 0)),
                "uniques": _non_neg_int(entry.get("uniques", 0)),
            }

    views = traffic_payload.get("views", []) if isinstance(traffic_payload, dict) else []
    if isinstance(views, list):
        for item in views:
            if not isinstance(item, dict):
                continue
            day = _norm_date(item.get("timestamp"))
            if not day:
                continue
            cnt = _non_neg_int(item.get("count", 0))
            unq = _non_neg_int(item.get("uniques", 0))
            prev = merged.get(day)
            if prev is None:
                merged[day] = {"count": cnt, "uniques": unq}
            else:
                merged[day] = {
                    "count": max(prev["count"], cnt),
                    "uniques": max(prev["uniques"], unq),
                }

    return dict(sorted(merged.items()))


def _nice_step(max_val: int) -> int:
    if max_val <= 4:
        return 1
    raw = math.ceil(max_val / 4)
    mag = 10 ** max(0, int(math.floor(math.log10(raw))))
    for mult in (1, 2, 2.5, 5, 10):
        step = int(math.ceil(mult * mag))
        if 4 * step >= max_val:
            return max(1, step)
    return raw


def _smooth_path(pts: list[tuple[float, float]]) -> str:
    if not pts:
        return ""
    if len(pts) == 1:
        x, y = pts[0]
        return f"M {x:.1f} {y:.1f}"
    parts = [f"M {pts[0][0]:.1f} {pts[0][1]:.1f}"]
    for i in range(len(pts) - 1):
        x0, y0 = pts[i]
        x1, y1 = pts[i + 1]
        cx = (x0 + x1) / 2.0
        parts.append(f"C {cx:.1f} {y0:.1f}, {cx:.1f} {y1:.1f}, {x1:.1f} {y1:.1f}")
    return " ".join(parts)


def _even_indices(n: int, max_ticks: int = 7) -> list[int]:
    if n <= 0:
        return []
    if n <= max_ticks:
        return list(range(n))
    idxs = {0, n - 1}
    for i in range(1, max_ticks - 1):
        idxs.add(round(i * (n - 1) / (max_ticks - 1)))
    return sorted(idxs)


def render_svg(history: dict[str, dict], repo: str, theme: str = "light") -> str:
    """Render an 800x320 SVG chart for repository total views and unique visitors."""
    if theme not in PALETTES:
        raise ValueError(f"Unsupported theme {theme!r}; expected one of {sorted(PALETTES)}")
    p = PALETTES[theme]

    days = sorted(history.keys())
    counts = [_non_neg_int(history[d].get("count", 0)) for d in days]
    uniques = [_non_neg_int(history[d].get("uniques", 0)) for d in days]
    total_views = sum(counts)
    total_uniques = sum(uniques)
    updated = days[-1] if days else datetime.now(timezone.utc).strftime("%Y-%m-%d")
    if not days:
        updated = "N/A"

    w, h = 800, 320
    pad_l, pad_r, pad_t, pad_b = 58, 28, 76, 46
    plot_w = w - pad_l - pad_r
    plot_h = h - pad_t - pad_b
    y_bot = pad_t + plot_h

    max_val = max(counts + uniques) if days else 0
    step = _nice_step(max_val)
    top_val = 4 * step

    def map_x(i: int) -> float:
        if len(days) <= 1:
            return pad_l + plot_w / 2.0
        return pad_l + (i / (len(days) - 1)) * plot_w

    def map_y(v: int) -> float:
        return y_bot - (v / top_val) * plot_h

    grid_lines = []
    for k in range(5):
        val = k * step
        y = map_y(val)
        stroke = p["axis"] if k == 0 else p["grid"]
        grid_lines.append(
            f'<line x1="{pad_l}" y1="{y:.1f}" x2="{w - pad_r}" y2="{y:.1f}" '
            f'stroke="{stroke}" stroke-width="1"/>'
        )
        grid_lines.append(
            f'<text x="{pad_l - 10}" y="{y + 4:.1f}" text-anchor="end" '
            f'font-size="11" fill="{p["muted"]}">{val:,}</text>'
        )

    x_labels = []
    for idx in _even_indices(len(days), max_ticks=7):
        x = map_x(idx)
        lbl = days[idx][5:]
        anchor = "middle"
        if len(days) > 1:
            if idx == 0:
                anchor = "start"
            elif idx == len(days) - 1:
                anchor = "end"
        x_labels.append(
            f'<text x="{x:.1f}" y="{y_bot + 20}" text-anchor="{anchor}" '
            f'font-size="11" fill="{p["muted"]}">{escape(lbl)}</text>'
        )

    series_elems = []
    if days:
        view_pts = [(map_x(i), map_y(counts[i])) for i in range(len(days))]
        uniq_pts = [(map_x(i), map_y(uniques[i])) for i in range(len(days))]
        if len(days) >= 2:
            v_line = _smooth_path(view_pts)
            u_line = _smooth_path(uniq_pts)
            v_area = f"{v_line} L {view_pts[-1][0]:.1f} {y_bot:.1f} L {view_pts[0][0]:.1f} {y_bot:.1f} Z"
            u_area = f"{u_line} L {uniq_pts[-1][0]:.1f} {y_bot:.1f} L {uniq_pts[0][0]:.1f} {y_bot:.1f} Z"
            series_elems.append(f'<path d="{v_area}" fill="{p["views"]}" fill-opacity="0.14"/>')
            series_elems.append(f'<path d="{u_area}" fill="{p["uniques"]}" fill-opacity="0.12"/>')
            series_elems.append(
                f'<path d="{v_line}" fill="none" stroke="{p["views"]}" '
                f'stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"/>'
            )
            series_elems.append(
                f'<path d="{u_line}" fill="none" stroke="{p["uniques"]}" '
                f'stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"/>'
            )
        dot_r = "2.2" if len(days) > 21 else "3.3"
        for vx, vy in view_pts:
            series_elems.append(f'<circle cx="{vx:.1f}" cy="{vy:.1f}" r="{dot_r}" fill="{p["views"]}"/>')
        for ux, uy in uniq_pts:
            series_elems.append(f'<circle cx="{ux:.1f}" cy="{uy:.1f}" r="{dot_r}" fill="{p["uniques"]}"/>')
    else:
        series_elems.append(
            f'<text x="{pad_l + plot_w / 2:.1f}" y="{pad_t + plot_h / 2:.1f}" '
            f'text-anchor="middle" font-size="13" fill="{p["muted"]}">No traffic data recorded yet</text>'
        )

    summary = (
        f"Total: {total_views:,} views · {total_uniques:,} unique visitors · "
        f"Days: {len(days)} · Updated {updated}"
    )

    svg_lines = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}" role="img" aria-label="{escape(repo)} repository views chart">',
        f'  <rect x="0.5" y="0.5" width="{w - 1}" height="{h - 1}" rx="12" fill="{p["bg"]}" stroke="{p["border"]}"/>',
        '  <g font-family="-apple-system, BlinkMacSystemFont, \'Segoe UI\', Helvetica, Arial, sans-serif">',
        f'    <text x="{pad_l}" y="30" font-size="16" font-weight="600" fill="{p["text"]}">Repository Views — {escape(repo)}</text>',
        f'    <text x="{pad_l}" y="50" font-size="12" fill="{p["muted"]}">{escape(summary)}</text>',
        f'    <line x1="{w - 220}" y1="25" x2="{w - 202}" y2="25" stroke="{p["views"]}" stroke-width="2.5" stroke-linecap="round"/>',
        f'    <circle cx="{w - 211}" cy="25" r="3" fill="{p["views"]}"/>',
        f'    <text x="{w - 196}" y="29" font-size="12" fill="{p["text"]}">Views</text>',
        f'    <line x1="{w - 135}" y1="25" x2="{w - 117}" y2="25" stroke="{p["uniques"]}" stroke-width="2.5" stroke-linecap="round"/>',
        f'    <circle cx="{w - 126}" cy="25" r="3" fill="{p["uniques"]}"/>',
        f'    <text x="{w - 111}" y="29" font-size="12" fill="{p["text"]}">Unique</text>',
        *(f"    {el}" for el in grid_lines),
        *(f"    {el}" for el in x_labels),
        *(f"    {el}" for el in series_elems),
        "  </g>",
        "</svg>",
        "",
    ]
    return "\n".join(svg_lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--history", required=True, help="Path to persistent views-history.json")
    parser.add_argument("--traffic", required=True, help="Path to GitHub /traffic/views JSON")
    parser.add_argument("--out-light", required=True, help="Output light-theme SVG path")
    parser.add_argument("--out-dark", required=True, help="Output dark-theme SVG path")
    parser.add_argument("--repo", default="MahanYTT/s23-windows-11", help="Repository owner/name label")
    args = parser.parse_args(argv)

    hist_path = Path(args.history)
    existing: dict = {}
    if hist_path.exists():
        raw_hist = json.loads(hist_path.read_text(encoding="utf-8-sig"))
        if isinstance(raw_hist, dict):
            existing = raw_hist.get("days", raw_hist)

    traffic_payload = json.loads(Path(args.traffic).read_text(encoding="utf-8-sig"))
    merged = merge_views_history(existing, traffic_payload)

    hist_path.parent.mkdir(parents=True, exist_ok=True)
    hist_doc = {
        "repo": args.repo,
        "total_views": sum(v["count"] for v in merged.values()),
        "total_uniques": sum(v["uniques"] for v in merged.values()),
        "days": merged,
    }
    hist_path.write_text(json.dumps(hist_doc, indent=2) + "\n", encoding="utf-8")

    for out_path, theme in ((args.out_light, "light"), (args.out_dark, "dark")):
        p = Path(out_path)
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(render_svg(merged, args.repo, theme=theme), encoding="utf-8")

    return 0


if __name__ == "__main__":
    sys.exit(main())

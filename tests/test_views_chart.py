"""Unit tests for scripts/ci/render_views_chart.py."""
import json
import os
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts", "ci"))
import render_views_chart  # noqa: E402


class ViewsChartTest(unittest.TestCase):
    def test_merge_preserves_older_history_and_takes_max_on_overlap(self):
        existing = {
            "2026-11-01": {"count": 12, "uniques": 4},
            "2026-11-21": {"count": 3, "uniques": 1},
            "2026-11-22": {"count": 10, "uniques": 3},
        }
        payload = {
            "count": 14,
            "uniques": 2,
            "views": [
                {"timestamp": "2026-11-21T00:00:00Z", "count": 6, "uniques": 2},
                {"timestamp": "2026-11-22T00:00:00Z", "count": 8, "uniques": 4},
                {"timestamp": "2026-11-23T00:00:00Z", "count": 20, "uniques": 5},
                {"timestamp": "bad-date", "count": 99, "uniques": 99},
            ],
        }
        merged = render_views_chart.merge_views_history(existing, payload)
        self.assertEqual(list(merged.keys()), ["2026-11-01", "2026-11-21", "2026-11-22", "2026-11-23"])
        self.assertEqual(merged["2026-11-01"], {"count": 12, "uniques": 4})
        self.assertEqual(merged["2026-11-21"], {"count": 6, "uniques": 2})
        self.assertEqual(merged["2026-11-22"], {"count": 10, "uniques": 4})
        self.assertEqual(merged["2026-11-23"], {"count": 20, "uniques": 5})

    def test_render_svg_valid_xml_for_empty_zero_single_and_long_series(self):
        datasets = [
            {},
            {"2026-11-20": {"count": 0, "uniques": 0}},
            {"2026-11-20": {"count": 0, "uniques": 0}, "2026-11-21": {"count": 6, "uniques": 1}},
            {f"2026-10-{d:02d}": {"count": d * 3, "uniques": d} for d in range(1, 31)}
            | {f"2026-11-{d:02d}": {"count": d * 2, "uniques": d} for d in range(1, 31)},
        ]
        for data in datasets:
            for theme in ("light", "dark"):
                svg = render_views_chart.render_svg(data, "MahanYTT/s23-windows-11", theme=theme)
                root = ET.fromstring(svg)
                self.assertTrue(root.tag.endswith("svg"))
                total_v = sum(v["count"] for v in data.values())
                total_u = sum(v["uniques"] for v in data.values())
                self.assertIn(f"Total: {total_v:,} views", svg)
                self.assertIn(f"{total_u:,} unique visitors", svg)

    def test_render_svg_rejects_unknown_theme(self):
        with self.assertRaises(ValueError):
            render_views_chart.render_svg({}, "owner/repo", theme="neon")

    def test_cli_round_trip(self):
        with tempfile.TemporaryDirectory() as tmp:
            hist = os.path.join(tmp, "views-history.json")
            traf = os.path.join(tmp, "traffic.json")
            light = os.path.join(tmp, "views-light.svg")
            dark = os.path.join(tmp, "views-dark.svg")
            with open(traf, "w", encoding="utf-8") as f:
                json.dump(
                    {
                        "views": [
                            {"timestamp": "2026-11-21T00:00:00Z", "count": 6, "uniques": 1},
                            {"timestamp": "2026-11-22T00:00:00Z", "count": 8, "uniques": 2},
                        ]
                    },
                    f,
                )
            rc = render_views_chart.main(
                ["--history", hist, "--traffic", traf, "--out-light", light, "--out-dark", dark, "--repo", "MahanYTT/s23-windows-11"]
            )
            self.assertEqual(rc, 0)
            from pathlib import Path
            doc = json.loads(Path(hist).read_text(encoding="utf-8"))
            self.assertEqual(doc["total_views"], 14)
            self.assertEqual(doc["total_uniques"], 3)
            ET.fromstring(Path(light).read_text(encoding="utf-8"))
            ET.fromstring(Path(dark).read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()

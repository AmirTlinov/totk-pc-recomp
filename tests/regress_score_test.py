r"""Plain asserts: .venv\Scripts\python tests\regress_score_test.py"""
import os
import random
import sys

from PIL import Image

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools'))
from regress_score import black_blocks, flat_frames, parse_perf_text, vuid_counts  # noqa: E402


def noise_image(seed):
    rng = random.Random(seed)
    img = Image.new('L', (480, 270))
    img.putdata([rng.randrange(60, 200) for _ in range(480 * 270)])
    return img.convert('RGB')


def test_parse_perf_slow_frames():
    log = ("perf: slow frame 1601: 93.4 ms; blocked pipeline 61.0 xlate 0.0 texture 2.0 astc 0.0 fence 0.0 pacing 0.0\n"
           "perf: frame 1620: interval avg 33.40 ms max 93.40 ms (29.9 fps); render thread blocked: pacing 60% ... ; render busy avg 12.5 ms; replay avg 9.5 ms\n")
    r = parse_perf_text(log, 1560, 2600)
    assert r["slow_frames"] == [(1601, 93.4, "pipeline")]
    assert r["max_frame_ms"] == 93.4 and r["render_busy_ms"] == 12.5 and r["replay_ms"] == 9.5 and not r["crashed"]


def test_parse_perf_other_and_crash():
    log = "perf: slow frame 1700: 60.0 ms; blocked pipeline 1.0 xlate 0.0 texture 2.0 astc 0.0 fence 0.0 pacing 0.0\n"
    r = parse_perf_text(log, 1560, 2600)
    assert r["slow_frames"] == [(1700, 60.0, "other")] and r["crashed"]
    fatal = "perf: frame 1620: interval avg 33.0 ms max 40.0 ms (30.0 fps)\nrt_fatal: x\n"
    assert parse_perf_text(fatal, 1560, 2600)["crashed"]


def test_black_blocks_counts_hole_not_dark_frame():
    img = Image.new("RGB", (480, 270), (120, 120, 120)); img.paste((0, 0, 0), (160, 96, 176, 112))
    assert black_blocks(img) == 1
    assert black_blocks(Image.new("RGB", (480, 270), (0, 0, 0))) == 0


def test_flat_frame_detected():
    sharp = noise_image(seed=1); flat = sharp.copy(); flat.paste((128, 128, 128), (0, 0, 480, 135))
    frames = [(i, sharp) for i in range(10)] + [(10, flat)] + [(i, sharp) for i in range(11, 21)]
    assert flat_frames(frames) == [10]


def test_dark_frames_not_scored():
    sharp = noise_image(seed=2); dark = Image.new("RGB", (480, 270), (10, 10, 10))
    assert flat_frames([(i, sharp) for i in range(5)] + [(5, dark)]) == []


def test_vuid_counts():
    assert vuid_counts("x VUID-vkCmdDraw-None-08600 y\nVUID-vkCmdDraw-None-08600\n") == {"VUID-vkCmdDraw-None-08600": 2}


for name, fn in list(globals().items()):
    if name.startswith('test_'):
        fn()
print('regress_score_test: passed')

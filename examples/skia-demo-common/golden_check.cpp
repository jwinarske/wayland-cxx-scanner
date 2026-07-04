// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// Golden-image regression check for the demo scene's deterministic content.
//
// The scene is rendered without its text card (system fonts are not
// reproducible) into a CPU raster bitmap and compared against a committed
// golden PNG within a small tolerance.  Setting SKIA_REGOLDEN=1 rewrites the
// golden instead of comparing — the only way the golden changes.
//
// Usage:
//   golden_check <golden.png>

#include "scene.hpp"

#include "include/codec/SkCodec.h"
#include "include/codec/SkPngDecoder.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkData.h"
#include "include/core/SkSpan.h"
#include "include/core/SkStream.h"
#include "include/encode/SkPngEncoder.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr int kWidth = 480;
constexpr int kHeight = 320;

// A channel may differ by this much before a pixel counts as changed, and at
// most this fraction of pixels may exceed it.  Same-build, same-code output is
// exact; the margin only absorbs cross-CPU rasterizer noise.
constexpr int kChannelTolerance = 2;
constexpr double kMaxChangedFraction = 0.001;

void RenderShapes(SkBitmap& out) {
  out.allocN32Pixels(kWidth, kHeight);
  SkCanvas canvas(out);
  demo::SceneState state;
  state.width = kWidth;
  state.height = kHeight;
  state.draw_text = false;
  demo::DemoScene::Render(&canvas, state, nullptr);
}

bool WritePng(const SkBitmap& bmp, const std::string& path) {
  SkFILEWStream stream(path.c_str());
  return stream.isValid() &&
         SkPngEncoder::Encode(&stream, bmp.pixmap(), SkPngEncoder::Options{});
}

// Decodes `path` into an N32 bitmap sized kWidth x kHeight; false on any error.
bool DecodeGolden(const std::string& path, SkBitmap& out) {
  sk_sp<SkData> data = SkData::MakeFromFileName(path.c_str());
  if (data == nullptr)
    return false;
  std::unique_ptr<SkCodec> codec = SkPngDecoder::Decode(data, nullptr);
  if (codec == nullptr)
    return false;
  if (codec->dimensions().width() != kWidth ||
      codec->dimensions().height() != kHeight)
    return false;
  out.allocN32Pixels(kWidth, kHeight);
  return codec->getPixels(out.pixmap()) == SkCodec::kSuccess;
}

// Number of pixels whose max per-channel difference exceeds the tolerance.
std::size_t CountChanged(const SkBitmap& a, const SkBitmap& b) {
  std::size_t changed = 0;
  for (int y = 0; y < kHeight; ++y) {
    const SkSpan<const uint32_t> ra(a.getAddr32(0, y), kWidth);
    const SkSpan<const uint32_t> rb(b.getAddr32(0, y), kWidth);
    for (int x = 0; x < kWidth; ++x) {
      const uint32_t pa = ra[static_cast<std::size_t>(x)];
      const uint32_t pb = rb[static_cast<std::size_t>(x)];
      int max_diff = 0;
      for (int shift = 0; shift < 32; shift += 8) {
        const int d = static_cast<int>((pa >> shift) & 0xFFu) -
                      static_cast<int>((pb >> shift) & 0xFFu);
        max_diff = std::max(max_diff, d < 0 ? -d : d);
      }
      if (max_diff > kChannelTolerance)
        ++changed;
    }
  }
  return changed;
}

}  // namespace

int main(int argc, char* argv[]) {
  const std::vector<std::string_view> args(argv, std::next(argv, argc));
  if (args.size() < 2) {
    std::fprintf(stderr, "usage: golden_check <golden.png>\n");
    return EXIT_FAILURE;
  }
  const std::string golden_path(args[1]);

  SkBitmap rendered;
  RenderShapes(rendered);

  if (std::getenv("SKIA_REGOLDEN") != nullptr) {
    if (!WritePng(rendered, golden_path)) {
      std::fprintf(stderr, "golden_check: failed to write %s\n",
                   golden_path.c_str());
      return EXIT_FAILURE;
    }
    std::printf("golden_check: wrote golden %s\n", golden_path.c_str());
    return EXIT_SUCCESS;
  }

  SkBitmap golden;
  if (!DecodeGolden(golden_path, golden)) {
    std::fprintf(stderr,
                 "golden_check: cannot read %dx%d golden %s "
                 "(run with SKIA_REGOLDEN=1 to create it)\n",
                 kWidth, kHeight, golden_path.c_str());
    return EXIT_FAILURE;
  }

  const std::size_t changed = CountChanged(rendered, golden);
  const auto total = static_cast<double>(kWidth) * kHeight;
  const double fraction = static_cast<double>(changed) / total;
  if (fraction > kMaxChangedFraction) {
    std::fprintf(
        stderr,
        "golden_check: FAIL %zu/%.0f pixels changed (%.4f%% > %.4f%%)\n",
        changed, total, fraction * 100.0, kMaxChangedFraction * 100.0);
    return EXIT_FAILURE;
  }
  std::printf("golden_check: OK (%zu pixels within tolerance)\n", changed);
  return EXIT_SUCCESS;
}

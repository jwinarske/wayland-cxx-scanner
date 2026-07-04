// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// Renders the demo scene into an offscreen raster surface and encodes it to a
// PNG.  Runs with no compositor and no GPU, so it doubles as the earliest
// build-health check: if this links and produces a non-empty PNG, the Skia
// prefix is wired correctly and the scene draws.

#include "scene.hpp"

#include "include/core/SkAlphaType.h"
#include "include/core/SkColorType.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkStream.h"
#include "include/core/SkSurface.h"
#include "include/encode/SkPngEncoder.h"

#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

int main(int argc, char* argv[]) {
  const std::vector<std::string_view> args(argv, std::next(argv, argc));
  const std::string out_path =
      args.size() > 1 ? std::string(args[1]) : std::string("demo-scene.png");

  constexpr int kWidth = 480;
  constexpr int kHeight = 320;

  const SkImageInfo info =
      SkImageInfo::Make(kWidth, kHeight, kN32_SkColorType, kPremul_SkAlphaType);
  sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
  if (surface == nullptr) {
    std::fprintf(stderr, "skia-demo-common: failed to create raster surface\n");
    return EXIT_FAILURE;
  }

  demo::SceneState state;
  state.width = kWidth;
  state.height = kHeight;
  demo::DemoScene::Render(surface->getCanvas(), state, nullptr);

  const sk_sp<SkImage> image = surface->makeImageSnapshot();
  SkFILEWStream stream(out_path.c_str());
  if (!stream.isValid()) {
    std::fprintf(stderr, "skia-demo-common: cannot open %s for writing\n",
                 out_path.c_str());
    return EXIT_FAILURE;
  }

  const SkPixmap pixmap = [&] {
    SkPixmap pm;
    image->peekPixels(&pm);
    return pm;
  }();
  if (!SkPngEncoder::Encode(&stream, pixmap, SkPngEncoder::Options{})) {
    std::fprintf(stderr, "skia-demo-common: PNG encode failed\n");
    return EXIT_FAILURE;
  }

  std::printf("skia-demo-common: wrote %s (%dx%d)\n", out_path.c_str(), kWidth,
              kHeight);
  return EXIT_SUCCESS;
}

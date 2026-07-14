// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// Live wl_display dispatch test for wl::DmabufFeedback<App>.
//
// A hand-rolled libwayland server plays the compositor: it advertises
// zwp_linux_dmabuf_v1 and, on get_default_feedback, replays a captured feedback
// sequence (format_table over a memfd, main_device, tranches, done).  The
// client side is the generated proxies driven by wl::DmabufFeedback — it must
// decode the tranches, resolve the format/modifier pairs through the table, and
// answer the modifier queries.  Also covers re-advertisement (table reuse and
// replacement), malformed input, and the v1-v3 legacy ladder.  Run under
// ASan+UBSan.

#include "ldbf_client.hpp"  // client-header, --emit-interface-tables

#include <wl/dmabuf_feedback.hpp>
#include <wl/registry.hpp>

extern "C" {
#include <dirent.h>    // opendir, readdir (fd-leak oracle)
#include <sys/mman.h>  // memfd_create, mmap, munmap
#include <unistd.h>    // close, ftruncate
#include <wayland-client-core.h>
#include <wayland-server-core.h>
}

#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace ldb = linux_dmabuf_unstable_v1::client;

namespace {

// ── DRM fourcc / modifier constants used by the captured table ───────────────
constexpr std::uint32_t kARGB8888 = 0x34325241u;  // 'AR24'
constexpr std::uint32_t kXRGB8888 = 0x34325258u;  // 'XR24'
constexpr std::uint32_t kNV12 = 0x3231564eu;      // 'NV12'
constexpr std::uint64_t kLinear = 0x0ull;
constexpr std::uint64_t kModA = 0x0300000000000001ull;  // arbitrary vendor mod
constexpr std::uint64_t kInvalid = 0x00ffffffffffffffull;

constexpr std::uint64_t kMainDev = 0xE200ull;
constexpr std::uint64_t kTrancheDevA = 0xE201ull;

const wl_interface* DmabufIface() {
  return &ldb::zwp_linux_dmabuf_v1_traits::wl_iface();
}
const wl_interface* FeedbackIface() {
  return &ldb::zwp_linux_dmabuf_feedback_v1_traits::wl_iface();
}

// 16-byte format-table entry, matching the protocol layout.
struct RawEntry {
  std::uint32_t format;
  std::uint32_t pad;
  std::uint64_t modifier;
};
static_assert(sizeof(RawEntry) == 16, "format-table entry must be 16 bytes");

// The canonical 4-entry table referenced by tranche indices below.
const std::vector<RawEntry>& BaseTable() {
  static const std::vector<RawEntry> t = {
      {kARGB8888, 0, kLinear},   // [0]
      {kARGB8888, 0, kModA},     // [1]
      {kNV12, 0, kLinear},       // [2]
      {kXRGB8888, 0, kInvalid},  // [3]
  };
  return t;
}

// ── Server side: raw libwayland acting as the compositor ─────────────────────

enum class Scenario { kBasic, kReadvertise, kMalformed, kLegacyV3 };

struct ServerCtx {
  Scenario scenario = Scenario::kBasic;
  std::vector<int> tables;  // memfds kept open until the client has copied them

  int MakeTable(const std::vector<RawEntry>& entries) {
    const std::size_t sz = entries.size() * sizeof(RawEntry);
    const int fd = ::memfd_create("ftable", MFD_CLOEXEC);
    if (fd < 0)
      return -1;
    if (::ftruncate(fd, static_cast<off_t>(sz)) == 0 && sz != 0) {
      void* m = ::mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
      if (m != MAP_FAILED) {
        std::memcpy(m, entries.data(), sz);
        ::munmap(m, sz);
      }
    }
    tables.push_back(fd);
    return fd;
  }
  void CloseTables() {
    for (int fd : tables)
      if (fd >= 0)
        ::close(fd);
    tables.clear();
  }
};

void PostDevArray(wl_resource* r, std::uint32_t op, std::uint64_t dev) {
  wl_array a{};
  wl_array_init(&a);
  void* p = wl_array_add(&a, sizeof(std::uint64_t));
  std::memcpy(p, &dev, sizeof(dev));
  wl_resource_post_event(r, op, &a);
  wl_array_release(&a);
}

void PostFormats(wl_resource* r,
                 std::uint32_t op,
                 const std::vector<std::uint16_t>& idx) {
  wl_array a{};
  wl_array_init(&a);
  if (!idx.empty()) {
    void* p = wl_array_add(&a, idx.size() * sizeof(std::uint16_t));
    std::memcpy(p, idx.data(), idx.size() * sizeof(std::uint16_t));
  }
  wl_resource_post_event(r, op, &a);
  wl_array_release(&a);
}

void PostFormatTable(wl_resource* r, int fd, std::uint32_t size) {
  wl_resource_post_event(
      r, ldb::zwp_linux_dmabuf_feedback_v1_traits::Evt::FormatTable, fd, size);
}

using Evt = ldb::zwp_linux_dmabuf_feedback_v1_traits::Evt;

// Post the two-tranche advertisement (tranche A scanout, tranche B not).
void PostTwoTranches(wl_resource* fb) {
  PostDevArray(fb, Evt::MainDevice, kMainDev);
  // tranche A: scanout, target device set, formats [0, 3].
  PostDevArray(fb, Evt::TrancheTargetDevice, kTrancheDevA);
  wl_resource_post_event(fb, Evt::TrancheFlags, 1u);  // scanout
  PostFormats(fb, Evt::TrancheFormats, {0, 3});
  wl_resource_post_event(fb, Evt::TrancheDone);
  // tranche B: no scanout, no explicit target, formats [0, 1, 2].
  wl_resource_post_event(fb, Evt::TrancheFlags, 0u);
  PostFormats(fb, Evt::TrancheFormats, {0, 1, 2});
  wl_resource_post_event(fb, Evt::TrancheDone);
  wl_resource_post_event(fb, Evt::Done);
}

void PostScenario(ServerCtx* ctx, wl_resource* fb) {
  switch (ctx->scenario) {
    case Scenario::kBasic: {
      PostFormatTable(fb, ctx->MakeTable(BaseTable()),
                      BaseTable().size() * sizeof(RawEntry));
      PostTwoTranches(fb);
      break;
    }
    case Scenario::kReadvertise: {
      // Cycle 1: fresh table + two tranches.
      PostFormatTable(fb, ctx->MakeTable(BaseTable()),
                      BaseTable().size() * sizeof(RawEntry));
      PostTwoTranches(fb);
      // Cycle 2: NO new table (reuse the last one) + a single tranche.
      PostDevArray(fb, Evt::MainDevice, kMainDev);
      wl_resource_post_event(fb, Evt::TrancheFlags, 1u);
      PostFormats(fb, Evt::TrancheFormats, {3});  // XRGB/INVALID
      wl_resource_post_event(fb, Evt::TrancheDone);
      wl_resource_post_event(fb, Evt::Done);
      // Cycle 3: replacement table of a different length + one tranche.
      const std::vector<RawEntry> small = {{kNV12, 0, kModA}};
      PostFormatTable(fb, ctx->MakeTable(small),
                      small.size() * sizeof(RawEntry));
      PostDevArray(fb, Evt::MainDevice, kMainDev);
      wl_resource_post_event(fb, Evt::TrancheFlags, 0u);
      PostFormats(fb, Evt::TrancheFormats, {0});  // NV12/kModA
      wl_resource_post_event(fb, Evt::TrancheDone);
      wl_resource_post_event(fb, Evt::Done);
      break;
    }
    case Scenario::kMalformed: {
      // Odd-size table (not a multiple of 16) → helper truncates to 1 entry.
      const int fd = ctx->MakeTable(BaseTable());
      PostFormatTable(fb, fd, 40u);  // 40 = 2*16 + 8, truncates to 2 entries
      PostDevArray(fb, Evt::MainDevice, kMainDev);
      // tranche_formats before any valid index range: out-of-range index 7 is
      // dropped, index 0 survives (table truncated to 2 entries: 0 and 1).
      wl_resource_post_event(fb, Evt::TrancheFlags, 0u);
      PostFormats(fb, Evt::TrancheFormats, {0, 7, 1});
      wl_resource_post_event(fb, Evt::TrancheDone);
      wl_resource_post_event(fb, Evt::Done);
      break;
    }
    case Scenario::kLegacyV3:
      break;  // legacy events are posted from the bind handler instead
  }
}

void SrvDestroy(wl_client* /*c*/, wl_resource* r) {
  wl_resource_destroy(r);
}

struct FeedbackImpl {
  void (*destroy)(wl_client*, wl_resource*);
};
constexpr FeedbackImpl kFeedbackImpl{&SrvDestroy};

void SrvGetDefaultFeedback(wl_client* c,
                           wl_resource* dmabuf,
                           std::uint32_t id) {
  auto* ctx = static_cast<ServerCtx*>(wl_resource_get_user_data(dmabuf));
  const int ver = wl_resource_get_version(dmabuf);
  wl_resource* fb =
      wl_resource_create(c, FeedbackIface(), ver, static_cast<int>(id));
  wl_resource_set_implementation(fb, &kFeedbackImpl, nullptr, nullptr);
  PostScenario(ctx, fb);
}

struct DmabufImpl {
  void (*destroy)(wl_client*, wl_resource*);
  void (*create_params)(wl_client*, wl_resource*, std::uint32_t);
  void (*get_default_feedback)(wl_client*, wl_resource*, std::uint32_t);
  void (*get_surface_feedback)(wl_client*,
                               wl_resource*,
                               std::uint32_t,
                               wl_resource*);
};
constexpr DmabufImpl kDmabufImpl{&SrvDestroy, nullptr, &SrvGetDefaultFeedback,
                                 nullptr};

void DmabufBind(wl_client* c,
                void* data,
                std::uint32_t version,
                std::uint32_t id) {
  auto* ctx = static_cast<ServerCtx*>(data);
  wl_resource* r = wl_resource_create(
      c, DmabufIface(), static_cast<int>(version), static_cast<int>(id));
  wl_resource_set_implementation(r, &kDmabufImpl, ctx, nullptr);
  // A v1-v3 compositor streams format/modifier events right after bind (there
  // is no feedback object).
  if (ctx->scenario == Scenario::kLegacyV3) {
    using LEvt = ldb::zwp_linux_dmabuf_v1_traits::Evt;
    wl_resource_post_event(r, LEvt::Format, kARGB8888);
    wl_resource_post_event(r, LEvt::Modifier, kARGB8888,
                           static_cast<std::uint32_t>(kModA >> 32),
                           static_cast<std::uint32_t>(kModA & 0xffffffffu));
  }
}

// ── Client side ──────────────────────────────────────────────────────────────

struct App {
  wl::DmabufFeedback<App> feedback;
  std::vector<wl::FeedbackSnapshot> snapshots;
  int legacy_calls = 0;
  void OnDmabufFeedback(const wl::FeedbackSnapshot& s) {
    snapshots.push_back(s);
  }
  void OnDmabufLegacyFormat(std::uint32_t /*fourcc*/, std::uint64_t /*mod*/) {
    ++legacy_calls;
  }
};

int CountOpenFds() {
  int n = 0;
  DIR* d = ::opendir("/proc/self/fd");
  if (d == nullptr)
    return -1;
  while (::readdir(d) != nullptr)
    ++n;
  ::closedir(d);
  return n;
}

// Drives one server scenario against a fresh client, invoking @p body with the
// connected App after the initial feedback roundtrip.
template <typename Body>
void RunScenario(Scenario scenario, std::uint32_t global_version, Body body) {
  std::array<int, 2> sv{-1, -1};
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv.data()), 0);

  ServerCtx ctx;
  ctx.scenario = scenario;
  wl_display* server = wl_display_create();
  ASSERT_NE(server, nullptr);
  ASSERT_NE(wl_client_create(server, sv[0]), nullptr);
  wl_global_create(server, DmabufIface(), static_cast<int>(global_version),
                   &ctx, &DmabufBind);
  std::thread server_thread([server] { wl_display_run(server); });

  wl_display* client = wl_display_connect_to_fd(sv[1]);
  ASSERT_NE(client, nullptr);
  {
    App app;
    wl::CRegistry registry;
    ASSERT_TRUE(registry.Create(client));
    registry.OnGlobal([&](wl::CRegistry&, std::uint32_t n,
                          std::string_view iface, std::uint32_t v) {
      if (iface == ldb::zwp_linux_dmabuf_v1_traits::interface_name)
        app.feedback.Record(n, v);
    });
    ASSERT_NE(wl_display_roundtrip(client), -1);
    ASSERT_TRUE(app.feedback.Bind(registry, &app));

    body(app, client, ctx);

    app.feedback.Release();
  }
  wl_display_disconnect(client);
  wl_display_terminate(server);
  server_thread.join();
  ctx.CloseTables();
  // Reap the client explicitly: terminate can stop the loop before it processes
  // the disconnect above, and wl_display_destroy does not destroy surviving
  // clients, so their resources would leak on whichever run loses that race.
  wl_display_destroy_clients(server);
  wl_display_destroy(server);
}

TEST(DmabufFeedbackRoundtrip, DefaultFeedbackDecodesTranches) {
  RunScenario(
      Scenario::kBasic, 5, [](App& app, wl_display* client, ServerCtx&) {
        EXPECT_EQ(app.feedback.BoundVersion(), 5u);
        ASSERT_TRUE(app.feedback.StartDefault(client));
        ASSERT_NE(wl_display_roundtrip(client), -1);

        ASSERT_EQ(app.snapshots.size(), 1u);
        EXPECT_EQ(app.feedback.commit_count(), 1u);
        const wl::FeedbackSnapshot& s = app.snapshots.front();
        EXPECT_EQ(s.main_device, static_cast<dev_t>(kMainDev));
        ASSERT_EQ(s.tranches.size(), 2u);

        // Tranche A: scanout, target device, {ARGB/LINEAR, XRGB/INVALID}.
        EXPECT_TRUE(s.tranches[0].Scanout());
        EXPECT_EQ(s.tranches[0].target_device,
                  static_cast<dev_t>(kTrancheDevA));
        ASSERT_EQ(s.tranches[0].formats.size(), 2u);
        EXPECT_EQ(s.tranches[0].formats[0].format, kARGB8888);
        EXPECT_EQ(s.tranches[0].formats[0].modifier, kLinear);
        EXPECT_EQ(s.tranches[0].formats[1].format, kXRGB8888);
        EXPECT_EQ(s.tranches[0].formats[1].modifier, kInvalid);

        // Tranche B: no scanout, {ARGB/LINEAR, ARGB/kModA, NV12/LINEAR}.
        EXPECT_FALSE(s.tranches[1].Scanout());
        ASSERT_EQ(s.tranches[1].formats.size(), 3u);
        EXPECT_EQ(s.tranches[1].formats[1].format, kARGB8888);
        EXPECT_EQ(s.tranches[1].formats[1].modifier, kModA);

        // Queries: scanout-only vs all, tranche order preserved.
        EXPECT_EQ(app.feedback.Current().ScanoutModifiersFor(kARGB8888),
                  (std::vector<std::uint64_t>{kLinear}));
        EXPECT_EQ(app.feedback.Current().ModifiersFor(kARGB8888),
                  (std::vector<std::uint64_t>{kLinear, kLinear, kModA}));
        EXPECT_TRUE(app.feedback.Current().Supports(kXRGB8888, kInvalid));
        EXPECT_FALSE(app.feedback.Current().Supports(kNV12, kModA));
      });
}

TEST(DmabufFeedbackRoundtrip, ReadvertiseReusesAndReplacesTable) {
  RunScenario(
      Scenario::kReadvertise, 5, [](App& app, wl_display* client, ServerCtx&) {
        ASSERT_TRUE(app.feedback.StartDefault(client));
        ASSERT_NE(wl_display_roundtrip(client), -1);

        // Exactly one OnDmabufFeedback per `done`.
        ASSERT_EQ(app.snapshots.size(), 3u);
        EXPECT_EQ(app.feedback.commit_count(), 3u);

        // Cycle 2 reused the cycle-1 table: index 3 → XRGB/INVALID.
        ASSERT_EQ(app.snapshots[1].tranches.size(), 1u);
        ASSERT_EQ(app.snapshots[1].tranches[0].formats.size(), 1u);
        EXPECT_EQ(app.snapshots[1].tranches[0].formats[0].format, kXRGB8888);
        EXPECT_EQ(app.snapshots[1].tranches[0].formats[0].modifier, kInvalid);

        // Cycle 3 replaced the table: index 0 → NV12/kModA.
        ASSERT_EQ(app.snapshots[2].tranches.size(), 1u);
        ASSERT_EQ(app.snapshots[2].tranches[0].formats.size(), 1u);
        EXPECT_EQ(app.snapshots[2].tranches[0].formats[0].format, kNV12);
        EXPECT_EQ(app.snapshots[2].tranches[0].formats[0].modifier, kModA);

        // Value semantics: the cycle-1 snapshot copy is untouched by later
        // cycles.
        ASSERT_EQ(app.snapshots[0].tranches.size(), 2u);
        EXPECT_TRUE(app.snapshots[0].tranches[0].Scanout());
        EXPECT_EQ(app.snapshots[0].tranches[0].formats[1].format, kXRGB8888);
      });
}

TEST(DmabufFeedbackRoundtrip, MalformedInputDegradesGracefully) {
  const int before = CountOpenFds();
  RunScenario(
      Scenario::kMalformed, 5, [](App& app, wl_display* client, ServerCtx&) {
        ASSERT_TRUE(app.feedback.StartDefault(client));
        ASSERT_NE(wl_display_roundtrip(client), -1);

        ASSERT_EQ(app.snapshots.size(), 1u);
        const wl::FeedbackSnapshot& s = app.snapshots.front();
        ASSERT_EQ(s.tranches.size(), 1u);
        // 40-byte table truncated to 2 entries; indices {0, 7, 1} keep 0 and 1,
        // drop the out-of-range 7.
        ASSERT_EQ(s.tranches[0].formats.size(), 2u);
        EXPECT_EQ(s.tranches[0].formats[0].format, kARGB8888);  // entry 0
        EXPECT_EQ(s.tranches[0].formats[1].format, kARGB8888);  // entry 1
        EXPECT_EQ(s.tranches[0].formats[1].modifier, kModA);
      });
  const int after = CountOpenFds();
  // No format-table fd leaked (client copies then closes; server closes its
  // memfds in teardown).
  EXPECT_EQ(before, after);
}

TEST(DmabufFeedbackRoundtrip, LegacyV3SynthesizesSnapshot) {
  RunScenario(
      Scenario::kLegacyV3, 3, [](App& app, wl_display* client, ServerCtx&) {
        EXPECT_EQ(app.feedback.BoundVersion(), 3u);
        // No feedback object at v3.
        EXPECT_FALSE(app.feedback.StartDefault(client));
        // Drain the format/modifier events streamed after bind, then publish.
        ASSERT_NE(wl_display_roundtrip(client), -1);
        app.feedback.CommitLegacy();

        ASSERT_EQ(app.snapshots.size(), 1u);
        const wl::FeedbackSnapshot& s = app.snapshots.front();
        ASSERT_EQ(s.tranches.size(), 1u);
        EXPECT_FALSE(
            s.tranches[0].Scanout());  // legacy is never scanout-flagged
        // format(ARGB) → INVALID modifier; modifier(ARGB, kModA) → kModA.
        EXPECT_TRUE(
            s.Supports(kARGB8888, wl::DmabufFeedback<App>::kModifierInvalid));
        EXPECT_TRUE(s.Supports(kARGB8888, kModA));
        EXPECT_EQ(app.legacy_calls, 2);  // OnDmabufLegacyFormat fired per event
      });
}

}  // namespace

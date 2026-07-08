// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// ext_data_control — a protocol-traits bundle that drives wl::DataDevice<App,
// P> over the ext-data-control-v1 clipboard family instead of the core
// wl_data_device family.  ext-data-control is focus-free: a client reads and
// sets the selection with no surface and no keyboard focus, which is exactly
// what makes a headless copy/paste CLI possible.
//
// The two families are structurally isomorphic, so the one wl::DataDevice
// helper drives both; only the trait types, the CRTP bases, and two shape flags
// differ (see CoreDataProtocol in <wl/data_device.hpp> for the core bundle).
//
// Include AFTER the generated ext-data-control client header (which defines
// ext_data_control_v1::client) and <wl/data_device.hpp>:
//   #include "ext_data_control_client.hpp"  // ext_data_control_v1::client
//   #include <wl/data_device.hpp>           // wl::DataDevice, CoreDataProtocol
//   #include <wl/ext_data_control.hpp>       // wl::ExtDataControlProtocol
//
// Then instantiate: wl::DataDevice<App, wl::ExtDataControlProtocol>.

#pragma once

namespace wl {

// The ext-data-control-v1 counterpart of CoreDataProtocol.  Two shape flags
// differ from the core family:
//   * set_selection carries no input serial — the protocol is focus-free, so
//     the compositor does not gate it on a real input event.
//   * the device is torn down by its own `destroy` destructor request (sent by
//     WlPtr::Reset), not an explicit, version-gated `release` request.
struct ExtDataControlProtocol {
  using ManagerTraits =
      ext_data_control_v1::client::ext_data_control_manager_v1_traits;
  using DeviceTraits =
      ext_data_control_v1::client::ext_data_control_device_v1_traits;
  using OfferTraits =
      ext_data_control_v1::client::ext_data_control_offer_v1_traits;
  using SourceTraits =
      ext_data_control_v1::client::ext_data_control_source_v1_traits;
  template <class D>
  using ManagerBase = ext_data_control_v1::client::CExtDataControlManagerV1<D>;
  template <class D>
  using DeviceBase = ext_data_control_v1::client::CExtDataControlDeviceV1<D>;
  template <class D>
  using OfferBase = ext_data_control_v1::client::CExtDataControlOfferV1<D>;
  template <class D>
  using SourceBase = ext_data_control_v1::client::CExtDataControlSourceV1<D>;
  static constexpr bool set_selection_takes_serial = false;
  static constexpr bool device_release_is_explicit = false;
};

}  // namespace wl

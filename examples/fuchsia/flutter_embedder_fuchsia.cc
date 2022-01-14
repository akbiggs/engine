// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <embedder.h>

#include <fuchsia/sysmem/cpp/fidl.h>
#include <fuchsia/ui/app/cpp/fidl.h>
#include <fuchsia/ui/composition/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async/cpp/wait.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/io.h>
#include <lib/fdio/namespace.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/fidl/cpp/interface_request.h>
#include <lib/sys/cpp/component_context.h>
#include <lib/sys/cpp/service_directory.h>
#include <lib/sys/inspect/cpp/component.h>
#include <lib/syslog/global.h>
#include <lib/trace-provider/provider.h>
#include <lib/trace/event.h>
#include <lib/ui/scenic/cpp/commands.h>
#include <lib/ui/scenic/cpp/id.h>
#include <lib/ui/scenic/cpp/resources.h>
#include <lib/ui/scenic/cpp/view_token_pair.h>
#include <lib/zx/event.h>
#include <lib/zx/vmo.h>
#include <zircon/status.h>

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>

#include "flutter/examples/fuchsia/utils/build_info.h"
#include "flutter/examples/fuchsia/utils/root_inspect_node.h"
#include "flutter/examples/fuchsia/utils/temp_fs.h"
#include "flutter/fml/message_loop.h"
#include "lib/async/default.h"
#include "platform/utils.h"
//#include "third_party/dart/runtime/platform/utils.h"

// A value that will be stored and checked when the engine hasn't given us a
// baton for Vsync. See FlutterProjectArgs.vsync_callback for more details.
constexpr intptr_t kNoVsyncBaton = -1;
// This tag will appear in fx logs.
static constexpr char kLogTag[] = "flutter_embedder_fuchsia";

// Store all state globally because I'm too lazy to do proper class design
// right now.

// Flutter Engine state.
static FlutterEngine gFlutterEngine = nullptr;
static intptr_t gVsyncBaton = kNoVsyncBaton;

// Fuchsia state.
static fuchsia::sysmem::AllocatorSyncPtr gSysmemAllocator;
static fuchsia::ui::composition::AllocatorSyncPtr gUiAllocator;
static std::unique_ptr<sys::ComponentContext> gComponentContext;
static fidl::BindingSet<fuchsia::ui::app::ViewProvider> gBindings;

// Scenic state.
static fuchsia::ui::scenic::ScenicPtr gScenic;
static scenic::Session* gScenicSession = nullptr;
static scenic::View* gScenicView = nullptr;
static scenic::EntityNode* gRootNode = nullptr;
static bool gUseFlatland = false;
// Scenic buffer state.
static fuchsia::sysmem::BufferCollectionTokenHandle gScenicBufferToken;
static bool gScenicHasBufferToken = false;
static size_t gScenicBufferWidth;
static size_t gScenicBufferHeight;
// Scenic presentation state.
static zx::event gScenicFenceAcquireEvent;
static zx::event gScenicFenceReleaseEvent;
static bool gScenicReadyToPresent = false;

// Software rendering state.
static zx::vmo gSurfaceVmo;         // VMO that is backing the surface memory.
static uint32_t gSurfaceSizeBytes;  // Size of the surface memory, in bytes.
// static size_t gSurfaceAge =
//     0;  // Number of frames since surface was last written to.
static bool gNeedsCacheClean = false;

class MyViewProvider : public fuchsia::ui::app::ViewProvider {
 public:
  MyViewProvider() {}
  ~MyViewProvider() override {}
  MyViewProvider(const MyViewProvider&) = delete;
  MyViewProvider& operator=(const MyViewProvider&) = delete;

  void CreateView(
      zx::eventpair token,
      fidl::InterfaceRequest<fuchsia::sys::ServiceProvider> incoming_services,
      fidl::InterfaceHandle<fuchsia::sys::ServiceProvider> outgoing_services)
      override {
    FX_LOG(INFO, kLogTag, "ViewProvider::CreateView");

    gScenicView =
        new scenic::View(gScenicSession, scenic::ToViewToken(std::move(token)),
                         "Flutter App Scenic View");

    gRootNode = new scenic::EntityNode(gScenicSession);
    gScenicView->AddChild(*gRootNode);
  }

  void CreateViewWithViewRef(zx::eventpair view_token,
                             fuchsia::ui::views::ViewRefControl control_ref,
                             fuchsia::ui::views::ViewRef view_ref) override {
    FX_LOG(INFO, kLogTag, "ViewProvider::CreateViewWithViewRef");

    gScenicView = new scenic::View(
        gScenicSession, scenic::ToViewToken(std::move(view_token)),
        std::move(control_ref), std::move(view_ref), "Flutter App Scenic View");

    gRootNode = new scenic::EntityNode(gScenicSession);
    gScenicView->AddChild(*gRootNode);

    // gScenicSession->Present(0, [](auto) {
    //   FX_LOG(INFO, kLogTag, "gScenicSession->Present initial callback");
    // });
  };

  void CreateView2(fuchsia::ui::app::CreateView2Args view_args) override {
    FX_LOG(INFO, kLogTag, "ViewProvider::CreateView2");
  }
};

static MyViewProvider* gViewProvider = new MyViewProvider();

static_assert(FLUTTER_ENGINE_VERSION == 1,
              "This Flutter Embedder was authored against the stable Flutter "
              "API at version 1. There has been a serious breakage in the "
              "API. Please read the ChangeLog and take appropriate action "
              "before updating this assertion");

namespace {

std::string GetCurrentProcessName() {
  char name[ZX_MAX_NAME_LEN];
  zx_status_t status =
      zx::process::self()->get_property(ZX_PROP_NAME, name, sizeof(name));
  if (status != ZX_OK) {
    FML_LOG(ERROR) << "Failed to get process name for sysmem; using \"\".";
    return std::string();
  }

  return std::string(name);
}

zx_koid_t GetCurrentProcessId() {
  zx_info_handle_basic_t info;
  zx_status_t status = zx::process::self()->get_info(
      ZX_INFO_HANDLE_BASIC, &info, sizeof(info), /*actual_count*/ nullptr,
      /*avail_count*/ nullptr);
  if (status != ZX_OK) {
    FML_LOG(ERROR) << "Failed to get process ID for sysmem; using 0.";
    return ZX_KOID_INVALID;
  }

  return info.koid;
}

uint32_t BytesPerRow(const fuchsia::sysmem::SingleBufferSettings& settings,
                     uint32_t bytes_per_pixel,
                     uint32_t image_width) {
  const uint32_t bytes_per_row_divisor =
      settings.image_format_constraints.bytes_per_row_divisor;
  const uint32_t min_bytes_per_row =
      settings.image_format_constraints.min_bytes_per_row;
  const uint32_t unrounded_bytes_per_row =
      std::max(image_width * bytes_per_pixel, min_bytes_per_row);
  const uint32_t roundup_bytes =
      unrounded_bytes_per_row % bytes_per_row_divisor;

  return unrounded_bytes_per_row + roundup_bytes;
}

}  // namespace

bool RunFlutter(const std::string& assets_path) {
  FlutterRendererConfig config = {};
  config.type = kSoftware;
  config.software.struct_size = sizeof(config.software);
  config.software.surface_acquire_callback = [](void* user_data, size_t width,
                                                size_t height,
                                                uint8_t** allocation,
                                                size_t* stride) {
    // Connect to sysmem Allocator.
    zx_status_t status = fdio_service_connect(
        "/svc/fuchsia.sysmem.Allocator",
        gSysmemAllocator.NewRequest().TakeChannel().release());
    if (status != ZX_OK) {
      FX_LOGF(ERROR, kLogTag,
              "Failed to connect to fuchsia.sysmem.Allocator: %s",
              zx_status_get_string(status));
    }
    gSysmemAllocator->SetDebugClientInfo(GetCurrentProcessName(),
                                         GetCurrentProcessId());

    FX_LOGF(INFO, kLogTag, "surface_acquire_callback width=%lu height=%lu",
            width, height);
    if (width <= 0 || height <= 0) {
      FX_LOG(ERROR, kLogTag, "Failed to allocate surface, size is empty.");
      return false;
    }

    // Allocate a "local" sysmem token to represent flutter's handle to the
    // sysmem buffer.
    fuchsia::sysmem::BufferCollectionTokenSyncPtr local_token;
    status =
        gSysmemAllocator->AllocateSharedCollection(local_token.NewRequest());
    if (status != ZX_OK) {
      FX_LOGF(ERROR, kLogTag, "Failed to allocate collection: %s",
              zx_status_get_string(status));
      return false;
    }

    // Create a single Duplicate of the token and Sync it; the single
    // duplicate token represents scenic's handle to the sysmem buffer.
    std::vector<fuchsia::sysmem::BufferCollectionTokenHandle> duplicate_tokens;
    status = local_token->DuplicateSync(
        std::vector<zx_rights_t>{ZX_RIGHT_SAME_RIGHTS}, &duplicate_tokens);
    if (status != ZX_OK) {
      FX_LOGF(ERROR, kLogTag, "Failed to duplicate collection token: %s",
              zx_status_get_string(status));
      return false;
    }
    if (duplicate_tokens.size() != 1u) {
      FX_LOG(ERROR, kLogTag,
             "Failed to duplicate collection token: Incorrect number of "
             "tokens returned");
      return false;
    }

    gScenicBufferToken = std::move(duplicate_tokens[0]);
    gScenicBufferWidth = width;
    gScenicBufferHeight = height;
    gScenicHasBufferToken = true;

    // Instead of RegisterBufferCollection:
    // - Connect to allocator interface on render thread.
    //     fuchsia.ui.composition/allocator.fidl
    // - Connect to Scenic session on the platform thread.
    // - Create two sysmem tokens. Give one to allocator and it will
    //   give you another token back (buffer import token or something like
    //   that).
    // - Pass that token to Image3 or to CreateImage in Flatland.
    // - This lets you allocate images in memory without having a
    // scenic::Session
    //   first.
    // - Benefit is that we can keep the work on separate threads.

    // SO:
    // In software_allocate_callback:
    // - Create sysmem tokens.
    // - Give sysmem token to scenic.
    // - Get buffer collection token.
    // - Hold on to that.
    // - Token corresponds to image for the frame.
    // - Inside the software_surface_present callback, take token and post a
    // message to the platform thread
    //   saying "hey draw a frame with this token".
    // - Platform thread will call CreateImage3 and then call Present.
    // Note: Because you'll be listening to events on the platform thread,
    // you won't have to coordinate work across threads (unlike today).
    // Note: Do NOT touch the session on the render thread. You can touch the
    // allocator but not the session.
    //
    // InterfaceHandle (channel) vs. InterfacePtr
    // (async dispatcher): Can connect to Scenic on raster thread, but you could
    // also reach out to ComponentContext, get the InterfaceHandle for the
    // session, and then bind it on the raster thread.

    // Acquire flutter's local handle to the sysmem buffer.
    fuchsia::sysmem::BufferCollectionSyncPtr buffer_collection;
    status = gSysmemAllocator->BindSharedCollection(
        std::move(local_token), buffer_collection.NewRequest());
    if (status != ZX_OK) {
      FX_LOGF(ERROR, kLogTag, "Failed to bind collection token: %s",
              zx_status_get_string(status));
      return false;
    }

    // Set flutter's constraints on the sysmem buffer.  Software rendering
    // only requires CPU access to the surface and a basic R8G8B8A8 pixel
    // format.
    fuchsia::sysmem::BufferCollectionConstraints constraints;
    constraints.min_buffer_count = 1;
    constraints.usage.cpu =
        fuchsia::sysmem::cpuUsageWrite | fuchsia::sysmem::cpuUsageWriteOften;
    constraints.has_buffer_memory_constraints = true;
    constraints.buffer_memory_constraints.physically_contiguous_required =
        false;
    constraints.buffer_memory_constraints.secure_required = false;
    constraints.buffer_memory_constraints.ram_domain_supported = true;
    constraints.buffer_memory_constraints.cpu_domain_supported = true;
    constraints.buffer_memory_constraints.inaccessible_domain_supported = false;
    constraints.image_format_constraints_count = 1;
    fuchsia::sysmem::ImageFormatConstraints& image_constraints =
        constraints.image_format_constraints[0];
    image_constraints = fuchsia::sysmem::ImageFormatConstraints();
    image_constraints.min_coded_width = static_cast<uint32_t>(width);
    image_constraints.min_coded_height = static_cast<uint32_t>(height);
    image_constraints.min_bytes_per_row = static_cast<uint32_t>(width) * 4;
    image_constraints.pixel_format.type =
        fuchsia::sysmem::PixelFormatType::R8G8B8A8;
    image_constraints.color_spaces_count = 1;
    image_constraints.color_space[0].type =
        fuchsia::sysmem::ColorSpaceType::SRGB;
    image_constraints.pixel_format.has_format_modifier = true;
    image_constraints.pixel_format.format_modifier.value =
        fuchsia::sysmem::FORMAT_MODIFIER_LINEAR;
    status = buffer_collection->SetConstraints(true, constraints);
    if (status != ZX_OK) {
      FX_LOGF(ERROR, kLogTag, "Failed to set constraints: %s",
              zx_status_get_string(status));
      return false;
    }

    // Wait for sysmem to allocate, now that constraints are set.
    fuchsia::sysmem::BufferCollectionInfo_2 buffer_collection_info;
    zx_status_t allocation_status = ZX_OK;
    zx_status_t wait_for_allocated_status =
        buffer_collection->WaitForBuffersAllocated(&allocation_status,
                                                   &buffer_collection_info);
    if (allocation_status != ZX_OK) {
      FX_LOGF(ERROR, kLogTag, "Failed to allocate: %s",
              zx_status_get_string(allocation_status));
      return false;
    }
    if (wait_for_allocated_status != ZX_OK) {
      FX_LOGF(ERROR, kLogTag, "Failed to wait for allocate: %s",
              zx_status_get_string(wait_for_allocated_status));
      return false;
    }

    // Cache the allocated surface VMO and metadata.
    FML_CHECK(buffer_collection_info.settings.buffer_settings.size_bytes != 0);
    FML_CHECK(buffer_collection_info.buffers[0].vmo != ZX_HANDLE_INVALID);
    gSurfaceVmo = std::move(buffer_collection_info.buffers[0].vmo);
    gSurfaceSizeBytes =
        buffer_collection_info.settings.buffer_settings.size_bytes;
    if (buffer_collection_info.settings.buffer_settings.coherency_domain ==
        fuchsia::sysmem::CoherencyDomain::RAM) {
      // RAM coherency domain requires a cache clean when writes are
      // finished.
      gNeedsCacheClean = true;
    }

    // Map the allocated buffer to the CPU.
    uint8_t* vmo_base = nullptr;
    status = zx::vmar::root_self()->map(
        ZX_VM_PERM_WRITE | ZX_VM_PERM_READ, 0, gSurfaceVmo, 0,
        gSurfaceSizeBytes, reinterpret_cast<uintptr_t*>(&vmo_base));
    if (status != ZX_OK) {
      FX_LOGF(ERROR, kLogTag, "Failed to map buffer memory: %s",
              zx_status_get_string(status));
      return false;
    }

    // Now that the buffer is CPU-readable, it's safe to discard flutter's
    // connection to sysmem.
    status = buffer_collection->Close();
    if (status != ZX_OK) {
      FX_LOGF(ERROR, kLogTag, "Failed to close buffer: %s",
              zx_status_get_string(status));
      return false;
    }

    // Wrap the buffer in a software-rendered Skia surface.
    const uint64_t vmo_offset =
        buffer_collection_info.buffers[0].vmo_usable_start;

    *stride = BytesPerRow(buffer_collection_info.settings, 4u, width);
    *allocation = vmo_base + vmo_offset;

    FX_LOG(INFO, kLogTag, "Successfully created surface.");
    return true;
  };

  config.software.surface_present_callback =
      [](void* user_data, const void* allocation, size_t row_bytes,
         size_t height) -> bool {
    FX_LOGF(INFO, kLogTag, "surface_present row_bytes=%lu height=%lu",
            row_bytes, height);

    gScenicReadyToPresent = true;

    return true;
  };

  FlutterProjectArgs args = {
      .struct_size = sizeof(FlutterProjectArgs),
      .assets_path = assets_path.c_str(),
      .icu_data_path = "/pkg/data/icudtl.dat",
      .vsync_callback =
          [](void* /* user data */, intptr_t baton) {
            // When you start the engine, it starts several threads internally
            // (including animator). This callback comes from the animator
            // with the baton. Flutter gives you this baton. When Vsync has
            // actually occurred and you're ready to make a new frame, call
            // FlutterEngineVsync and pass the baton in. Then SurfaceCallback
            // will get invoked as it needs new frames.
            //
            // This lets you throttle frame production by holding on to the
            // baton without passing it.

            // To simulate Scenic, wait for baton. Async in ScheduleFrame
            // call, pass the baton in. Each time that function ticks, if
            // there's no baton, schedule another frame.
            FX_LOG(INFO, kLogTag, "Vsync!");
            gVsyncBaton = baton;
          },
      .log_message_callback =
          [](const char* tag, const char* message, void* /* user_data */) {
            FX_LOG(INFO, tag, message);
          },
      .log_tag = "my_flutter_app",
  };

  FlutterEngineResult result =
      FlutterEngineRun(FLUTTER_ENGINE_VERSION, &config, &args,
                       nullptr /* user_data */, &gFlutterEngine);
  if (result != kSuccess || gFlutterEngine == nullptr) {
    FX_LOGF(ERROR, kLogTag, "Could not run the Flutter Engine. Error code: %d",
            static_cast<int>(result));
    return false;
  }

  return true;
}

void PrintUsage() {
  FX_LOG(ERROR, kLogTag, "usage: executable <path to flutter bundle>");
}

void ScheduleFrame(fml::RefPtr<fml::TaskRunner> task_runner,
                   std::string assets_path) {
  task_runner->PostTask([task_runner, assets_path]() {
    if (gVsyncBaton != kNoVsyncBaton) {
      // Encourage the Engine to vsync for a 60Hz display.
      const auto now_nanos = FlutterEngineGetCurrentTime();
      const auto target_vsync_time = now_nanos + 16.6 * 1e6;

      FX_LOGF(INFO, kLogTag,
              "OnVsync gVsyncBaton=%lu now_nanos=%lu "
              "target_vsync_time=%f",
              gVsyncBaton, now_nanos, target_vsync_time);

      FlutterEngineOnVsync(gFlutterEngine, gVsyncBaton, now_nanos,
                           target_vsync_time);
      gVsyncBaton = kNoVsyncBaton;
    }

    if (gScenicSession && gScenicHasBufferToken) {
      gScenicHasBufferToken = false;

      // Register the sysmem token with flatland (or scenic's legacy gfx
      // interface).
      //
      // This binds the sysmem token to a composition token, which is used
      // later to associate the rendering surface with a specific flatland
      // Image.
      //
      // Under gfx, scenic uses an integral `buffer_id` instead of the
      // composition token.
      // TODO(akbiggs): Don't hardcode this ID.
      constexpr auto kBufferId = 1;
      gScenicSession->RegisterBufferCollection(kBufferId,
                                               std::move(gScenicBufferToken));

      gScenicSession->Enqueue(scenic::NewCreateImage2Cmd(
          gScenicSession->AllocResourceId(), gScenicBufferWidth,
          gScenicBufferHeight, kBufferId, 0 /* buffer_collection_index */));
    }

    if (gScenicSession && gScenicReadyToPresent) {
      gScenicReadyToPresent = false;

      zx::event acquire;
      zx_status_t status =
          gScenicFenceAcquireEvent.duplicate(ZX_RIGHT_SAME_RIGHTS, &acquire);
      if (status != ZX_OK) {
        FX_LOGF(ERROR, kLogTag, "Failed to duplicate acquire event: %s",
                zx_status_get_string(status));
        return;
      }

      zx::event release;
      status =
          gScenicFenceReleaseEvent.duplicate(ZX_RIGHT_SAME_RIGHTS, &release);
      if (status != ZX_OK) {
        FX_LOGF(ERROR, kLogTag, "Failed to duplicate release event: %s",
                zx_status_get_string(status));
        return;
      }

      // TODO(akbiggs): This makes Present never finish.
      // FX_LOG(INFO, kLogTag, "Enqueue acquire fence.");
      // gScenicSession->EnqueueAcquireFence(std::move(acquire));
      // FX_LOG(INFO, kLogTag, "Enqueue release fence.");
      // gScenicSession->EnqueueReleaseFence(std::move(release));

      status = gScenicFenceAcquireEvent.signal(ZX_EVENT_SIGNALED, 0);
      if (status != ZX_OK) {
        FX_LOGF(ERROR, kLogTag, "Failed to signal acquire event: %s",
                zx_status_get_string(status));
        return;
      }

      FX_LOG(INFO, kLogTag, "gScenicSession->Present");
      gScenicSession->Present(0, [](auto) {
        FX_LOG(INFO, kLogTag, "gScenicSession->Present callback");
      });
    }

    ScheduleFrame(task_runner, assets_path);
  });
}

int main(int argc, const char* argv[]) {
  if (argc != 2) {
    PrintUsage();
    return EXIT_FAILURE;
  }

  // This directory is generated by `flutter build bundle`.
  const std::string assets_path = argv[1];

  // TODO(akbiggs): Replace this FML dependency.
  fml::MessageLoop::EnsureInitializedForCurrentThread();

  FX_LOG(INFO, kLogTag, "Setting up trace provider.");
  std::unique_ptr<trace::TraceProviderWithFdio> provider;
  {
    bool already_started_unused;
    // Use CreateSynchronously to prevent loss of early events.
    trace::TraceProviderWithFdio::CreateSynchronously(
        async_get_default_dispatcher(), "flutter_embedder", &provider,
        &already_started_unused);
  }

  // We inject the 'vm' node into the dart vm so that it can add any inspect
  // data that it needs to the inspect tree.
  //
  // NOTE: IF YOU DON'T DO THIS, THE DART VM WILL SERVE OUTGOING PERMISSIONS
  // ON BEHALF OF YOUR APP AND YOU WILL NEVER BE ABLE TO SERVE OUTGOING
  // PERMISSIONS. SEE fxb/75282.
  gComponentContext = sys::ComponentContext::Create();
  dart_utils::RootInspectNode::Initialize(gComponentContext.get());
  auto build_info = dart_utils::RootInspectNode::CreateRootChild("build_info");
  dart_utils::BuildInfo::Dump(build_info);
  dart::SetDartVmNode(std::make_unique<inspect::Node>(
      dart_utils::RootInspectNode::CreateRootChild("vm")));

  // Set up the process-wide /tmp memfs.
  FX_LOG(INFO, kLogTag, "Initializing /tmp memfs.");
  dart_utils::TempFs temp_fs;

  FX_LOG(INFO, kLogTag, "Running Flutter.");

  bool success = RunFlutter(assets_path);
  if (!success) {
    FX_LOG(ERROR, kLogTag, "RunFlutter failed.");
    return EXIT_FAILURE;
  }

  // Necessary to get Flutter to render frames.
  FlutterWindowMetricsEvent window_metrics_event = {
      .struct_size = sizeof(FlutterWindowMetricsEvent),
      .width = 800,
      .height = 800,
      .pixel_ratio = 1,
      .left = 0,
      .top = 0,
      .physical_view_inset_top = 0,
      .physical_view_inset_right = 0,
      .physical_view_inset_bottom = 0,
      .physical_view_inset_left = 0,
  };
  FlutterEngineSendWindowMetricsEvent(gFlutterEngine, &window_metrics_event);

  // Connect to Scenic.
  zx_status_t status =
      fdio_service_connect("/svc/fuchsia.ui.scenic.Scenic",
                           gScenic.NewRequest().TakeChannel().release());
  if (status != ZX_OK) {
    FX_LOGF(ERROR, kLogTag, "Failed to connect to fuchsia.ui.scenic.Scenic: %s",
            zx_status_get_string(status));
  }

  fuchsia::ui::scenic::SessionEndpoints gfx_protocols;
  fuchsia::ui::scenic::SessionHandle session;
  gfx_protocols.set_session(session.NewRequest());
  fuchsia::ui::scenic::SessionListenerHandle session_listener;
  auto session_listener_request = session_listener.NewRequest();
  gfx_protocols.set_session_listener(session_listener.Bind());
  fuchsia::ui::views::FocuserHandle focuser;
  fuchsia::ui::views::ViewRefFocusedHandle view_ref_focused;
  // fuchsia::ui::pointer::TouchSourceHandle touch_source;
  // fuchsia::ui::pointer::MouseSourceHandle mouse_source;

  fuchsia::ui::composition::ViewBoundProtocols flatland_view_protocols;
  if (gUseFlatland) {
    flatland_view_protocols.set_view_focuser(focuser.NewRequest());
    flatland_view_protocols.set_view_ref_focused(view_ref_focused.NewRequest());
    // flatland_view_protocols.set_touch_source(touch_source.NewRequest());
    // flatland_view_protocols.set_mouse_source(mouse_source.NewRequest());
  } else {
    gfx_protocols.set_view_focuser(focuser.NewRequest());
    gfx_protocols.set_view_ref_focused(view_ref_focused.NewRequest());
    // TODO(fxbug.dev/85125): Enable TouchSource for GFX.
    // gfx_protocols.set_touch_source(touch_source.NewRequest());
  }
  FX_LOG(INFO, kLogTag, "Creating Scenic session.");
  gScenic->CreateSessionT(std::move(gfx_protocols), [] {});

  gScenicSession = new scenic::Session(session.Bind(), nullptr);
  status = zx::event::create(0, &gScenicFenceAcquireEvent);
  if (status != ZX_OK) {
    FX_LOGF(ERROR, kLogTag, "Failed to create acquire event: %s",
            zx_status_get_string(status));
  }
  gScenicSession->set_error_handler([](zx_status_t status) {
    FX_LOGF(ERROR, kLogTag, "Scenic Session error: %s",
            zx_status_get_string(status));
  });

  status = zx::event::create(0, &gScenicFenceReleaseEvent);
  if (status != ZX_OK) {
    FX_LOGF(ERROR, kLogTag, "Failed to create release event: %s",
            zx_status_get_string(status));
  }

  FX_LOG(INFO, kLogTag, "hi.");
  FX_LOG(INFO, kLogTag, "Creating component context.");
  status = gComponentContext->outgoing()
               ->AddPublicService<fuchsia::ui::app::ViewProvider>(
                   [](fidl::InterfaceRequest<fuchsia::ui::app::ViewProvider>
                          request) {
                     FX_LOG(INFO, kLogTag, "Adding ViewProvider binding.");
                     gBindings.AddBinding(gViewProvider, std::move(request));
                   });
  if (status != ZX_OK) {
    FX_LOGF(ERROR, kLogTag, "Failed to add ViewProvider service: %s",
            zx_status_get_string(status));
  }

  // Wait to serve until we have finished all of our setup.
  FX_LOG(INFO, kLogTag, "Serving component context.");
  status = gComponentContext->outgoing()->ServeFromStartupInfo();
  if (status != ZX_OK) {
    FX_LOGF(ERROR, kLogTag, "Failed to serve from startup: %s",
            zx_status_get_string(status));
  }

  FX_LOG(INFO, kLogTag, "Starting Flutter loop.");
  fml::MessageLoop& loop = fml::MessageLoop::GetCurrent();

  ScheduleFrame(loop.GetTaskRunner(), assets_path);
  loop.Run();

  if (gScenicSession) {
    gScenicSession->DeregisterBufferCollection(1 /* buffer_id */);
  }

  FX_LOG(INFO, kLogTag, "Flutter application services terminated.");

  return EXIT_SUCCESS;
}

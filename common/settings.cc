// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/common/settings.h"

#include <sstream>

namespace flutter {

constexpr FrameTiming::Phase FrameTiming::kPhases[FrameTiming::kCount];

Settings::Settings() = default;

Settings::Settings(const Settings& other) = default;

Settings::~Settings() = default;

std::string Settings::ToString() const {
  std::stringstream stream;
  stream << "Settings: "
         << ", ";
  stream << "vm_snapshot_data_path: " << vm_snapshot_data_path << ", ";
  stream << "vm_snapshot_instr_path: " << vm_snapshot_instr_path << ", ";
  stream << "isolate_snapshot_data_path: " << isolate_snapshot_data_path
         << ", ";
  stream << "isolate_snapshot_instr_path: " << isolate_snapshot_instr_path
         << ", ";
  stream << "application_library_path:"
         << ", ";
  for (const auto& path : application_library_path) {
    stream << "    " << path << ", ";
  }
  stream << "temp_directory_path: " << temp_directory_path << ", ";
  stream << "dart_flags:"
         << ", ";
  for (const auto& dart_flag : dart_flags) {
    stream << "    " << dart_flag << ", ";
  }
  stream << "start_paused: " << start_paused << ", ";
  stream << "trace_skia: " << trace_skia << ", ";
  stream << "trace_startup: " << trace_startup << ", ";
  stream << "trace_systrace: " << trace_systrace << ", ";
  stream << "dump_skp_on_shader_compilation: " << dump_skp_on_shader_compilation
         << ", ";
  stream << "cache_sksl: " << cache_sksl << ", ";
  stream << "purge_persistent_cache: " << purge_persistent_cache << ", ";
  stream << "endless_trace_buffer: " << endless_trace_buffer << ", ";
  stream << "enable_dart_profiling: " << enable_dart_profiling << ", ";
  stream << "disable_dart_asserts: " << disable_dart_asserts << ", ";
  stream << "enable_observatory: " << enable_observatory << ", ";
  stream << "enable_observatory_publication: " << enable_observatory_publication
         << ", ";
  stream << "observatory_host: " << observatory_host << ", ";
  stream << "observatory_port: " << observatory_port << ", ";
  stream << "use_test_fonts: " << use_test_fonts << ", ";
  stream << "enable_software_rendering: " << enable_software_rendering << ", ";
  stream << "log_tag: " << log_tag << ", ";
  stream << "icu_initialization_required: " << icu_initialization_required
         << ", ";
  stream << "icu_data_path: " << icu_data_path << ", ";
  stream << "assets_dir: " << assets_dir << ", ";
  stream << "assets_path: " << assets_path << ", ";
  stream << "frame_rasterized_callback set: " << !!frame_rasterized_callback
         << ", ";
  stream << "old_gen_heap_size: " << old_gen_heap_size;
  return stream.str();
}

}  // namespace flutter

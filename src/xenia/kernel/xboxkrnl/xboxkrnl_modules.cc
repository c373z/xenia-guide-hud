/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/power_reset.h"

#include <string>
#include <vector>
#include "xenia/kernel/xboxkrnl/xboxkrnl_modules.h"
#include "xenia/base/logging.h"
#include "xenia/base/utf8.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/processor.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_flags.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xthread.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

dword_result_t XexCheckExecutablePrivilege_entry(dword_t privilege) {
  // BOOL
  // DWORD Privilege

  // Privilege is bit position in xe_xex2_system_flags enum - so:
  // Privilege=6 -> 0x00000040 -> XEX_SYSTEM_INSECURE_SOCKETS
  uint32_t mask = 1 << privilege;

  auto module = kernel_state()->GetExecutableModule();
  if (!module) {
    return 0;
  }

  uint32_t flags = 0;
  module->GetOptHeader<uint32_t>(XEX_HEADER_SYSTEM_FLAGS, &flags);

  return (flags & mask) > 0;
}
DECLARE_XBOXKRNL_EXPORT1(XexCheckExecutablePrivilege, kModules, kImplemented);

dword_result_t XexGetModuleHandle(std::string module_name,
                                  xe::be<uint32_t>* hmodule_ptr) {
  object_ref<XModule> module;

  if (module_name.empty()) {
    module = kernel_state()->GetExecutableModule();
  } else {
    module = kernel_state()->GetModule(module_name);
  }

  if (!module) {
    *hmodule_ptr = 0;
    return X_ERROR_NOT_FOUND;
  }

  // NOTE: we don't retain the handle for return.
  *hmodule_ptr = module->hmodule_ptr();

  return X_ERROR_SUCCESS;
}

dword_result_t XexGetModuleHandle_entry(lpstring_t module_name,
                                        lpdword_t hmodule_ptr,
                                        const ppc_context_t& ctx) {
  // Old-dashboard support: a name of (PSZ)-1 means "the module this call
  // came from". The 17489 kernel (800A2B40) takes the caller's return
  // address (800A2218: [[r1]-8]) and returns the loader entry whose image
  // contains it (800A1D88: +0x18 base, +0x20 size), or 0 and
  // STATUS_NOT_FOUND (C0000225). dash.firstuse.xex (2.0.12625-15574) builds
  // its own resource locator this way; reading -1 as a string failed it and
  // the dashboard showed "This feature is temporarily unavailable
  // (80004005)" at every boot.
  if (cvars::kernel_xex_module_handle_self &&
      module_name.guest_address() == 0xFFFFFFFF) {
    auto module =
        kernel_state()->GetUserModuleByAddress(uint32_t(ctx->lr));
    if (!module) {
      *hmodule_ptr = 0;
      return X_STATUS_NOT_FOUND;
    }
    *hmodule_ptr = module->hmodule_ptr();
    return X_STATUS_SUCCESS;
  }
  return XexGetModuleHandle(module_name ? module_name.value() : "",
                            hmodule_ptr);
}
DECLARE_XBOXKRNL_EXPORT1(XexGetModuleHandle, kModules, kImplemented);

dword_result_t XexGetModuleSection_entry(lpvoid_t hmodule, lpstring_t name,
                                         lpdword_t data_ptr,
                                         lpdword_t size_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;

  auto module = XModule::GetFromHModule(kernel_state(), hmodule);
  if (!module) {
    // Some callers hold a kernel object handle rather than an hmodule (which
    // in Xenia is a pointer to a guest LDR entry whose checksum field stashes
    // the handle). hud does this when looking up its own "hud" resource
    // section, so accept both forms rather than failing the lookup.
    module = kernel_state()->object_table()->LookupObject<XModule>(
        hmodule.guest_address());
    if (module) {
      XELOGD("XexGetModuleSection: hmodule {:08X} was a kernel handle",
             hmodule.guest_address());
    }
  }
  if (module) {
    uint32_t section_data = 0;
    uint32_t section_size = 0;
    result = module->GetSection(name.value(), &section_data, &section_size);
    if (XSUCCEEDED(result)) {
      *data_ptr = section_data;
      *size_ptr = section_size;
    }
    {
      // Attribute the caller: hud's scene path and its string-table path both
      // need this section, so knowing which one asked says whether the
      // string-table load ever gets this far.
      auto* th = XThread::GetCurrentThread();
      auto* c = th && th->thread_state() ? th->thread_state()->context()
                                         : nullptr;
      // NOTE: this line used to print `result` under a bare "-> {:08X}",
      // which reads exactly like a returned pointer. It is the STATUS
      // (0 = SUCCESS) and was misread that way once. Print the section base
      // explicitly and label both.
      XELOGI("XexGetModuleSection: module='{}' section='{}' status={:08X} "
             "data={:08X} size={} lr={:08X}",
             module->name(), name.value(), result, section_data, section_size,
             c ? static_cast<uint32_t>(c->lr) : 0);
    }
  } else {
    XELOGE("XexGetModuleSection: no module for hmodule {:08X} (section '{}')",
           hmodule.guest_address(), name.value());
    result = X_STATUS_INVALID_HANDLE;
  }

  return result;
}
DECLARE_XBOXKRNL_EXPORT1(XexGetModuleSection, kModules, kImplemented);

// 2026-09-16 (disc-agent): the import-library version check the console makes
// while binding an image's imports. The 17489 kernel hands import binding to
// the hypervisor (8009F920 = syscall 0x17); the HV (se_17489_hv_kernel.bin
// 2AC6C-2ACF4) reads the exporting module's xex2 export table and fails with
// STATUS_REVISION_MISMATCH when export version < import version_min, or when
// import version < the export's minimum compatible version. Checked only
// against real guest modules (LLE xam and friends): Xenia's HLE modules have
// no export table, and its reported kernel version is a cvar (default 1888).
// Measured without it: Sonic & All-Stars Racing Transformed (xam min
// 2.0.15574) on 2.0.6770 bound its missing xam ordinals to xam's image base
// and crashed at 824B5A14 (ctr=81870000) 0.6 s after start.
static X_STATUS CheckImportLibraryVersions(UserModule* module) {
  if (!module->xex_module()) {
    return X_STATUS_SUCCESS;
  }
  xex2_opt_import_libraries* libs = nullptr;
  if (!module->xex_module()->GetOptHeader(XEX_HEADER_IMPORT_LIBRARIES, &libs) ||
      !libs) {
    return X_STATUS_SUCCESS;
  }
  std::vector<std::string_view> names;
  for (size_t i = 0; i < libs->string_table.size &&
                     names.size() < libs->string_table.count;) {
    std::string_view s(&libs->string_table.data[i]);
    names.push_back(s);
    i += s.size() + 1;
    i = (i + 3) & ~size_t(3);
  }
  auto* data = reinterpret_cast<const uint8_t*>(libs);
  for (uint32_t off = libs->string_table.size + 12; off < libs->size;) {
    auto* lib = reinterpret_cast<const xex2_import_library*>(data + off);
    if (!lib->size) {
      break;
    }
    off += lib->size;
    const size_t index = lib->name_index & 0xFF;
    if (index >= names.size()) {
      continue;
    }
    const std::string name(names[index]);
    // Only libraries that bind to the guest image (see SetupLibraryImports).
    const bool is_xam = utf8::equal_case(name, "xam.xex");
    if (is_xam && (cvars::lle_xam.empty() || !cvars::lle_xam_scope.empty())) {
      continue;
    }
    auto exporter = kernel_state()->GetModule(name, true);
    auto* exporter_module =
        exporter ? static_cast<UserModule*>(exporter.get()) : nullptr;
    if (!exporter_module || !exporter_module->xex_module() ||
        !exporter_module->xex_module()->xex_security_info()->export_table) {
      continue;
    }
    auto* table = kernel_memory()->TranslateVirtual<const xex2_export_table*>(
        exporter_module->xex_module()->xex_security_info()->export_table);
    const uint32_t tail = table->version[2];
    const uint32_t exp_version =
        ((tail >> 8) & 0xFF) << 24 | (table->version[0] & 0xFFFFFF);
    const uint32_t exp_min =
        (tail & 0xFF) << 24 | (table->version[1] & 0xFFFFFF);
    const uint32_t imp_version = lib->version_value;
    const uint32_t imp_min = lib->version_min_value;
    if (exp_version < imp_min || imp_version < exp_min) {
      XELOGW("XexLoadImage: {} needs {} {:08X} (built against {:08X}); the "
             "loaded one is {:08X} (compatible back to {:08X}) -> C0000059",
             module->path(), name, imp_min, imp_version, exp_version, exp_min);
      return X_STATUS(0xC0000059);  // STATUS_REVISION_MISMATCH
    }
  }
  return X_STATUS_SUCCESS;
}

dword_result_t xeXexLoadImage(
    lpstring_t module_name, dword_t module_flags, dword_t min_version,
    lpdword_t hmodule_ptr,
    const std::function<object_ref<UserModule>()>& load_callback,
    bool isFromMemory) {
  X_STATUS result = X_STATUS_NO_SUCH_FILE;

  if (!hmodule_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  uint32_t hmodule = 0;
  auto module = kernel_state()->GetModule(module_name.value());
  if (module) {
    if (isFromMemory) {
      // Existing module found; return error status.
      *hmodule_ptr = hmodule;
      return X_STATUS_OBJECT_NAME_COLLISION;
    } else {
      // Existing module found.
      hmodule = module->hmodule_ptr();
      result = X_STATUS_SUCCESS;
    }
  } else {
    // Not found; attempt to load as a user module.
    auto user_module = load_callback();
    if (user_module) {
      kernel_state()->ApplyTitleUpdate(user_module);
      const X_STATUS version_status =
          cvars::kernel_check_title_system_version
              ? CheckImportLibraryVersions(user_module.get())
              : X_STATUS_SUCCESS;
      if (XFAILED(version_status)) {
        kernel_state()->UnloadUserModule(user_module, false);
        user_module->Unload();
        *hmodule_ptr = 0;
        return version_status;
      }
      kernel_state()->FinishLoadingUserModule(user_module);
      // Give up object ownership, this reference will be released by the last
      // XexUnloadImage call
      auto user_module_raw = user_module.release();
      hmodule = user_module_raw->hmodule_ptr();
      result = X_STATUS_SUCCESS;
    }
  }

  // Increment the module's load count.
  if (hmodule) {
    auto ldr_data =
        kernel_memory()->TranslateVirtual<X_LDR_DATA_TABLE_ENTRY*>(hmodule);
    ldr_data->load_count++;
  }

  *hmodule_ptr = hmodule;

  return result;
}

dword_result_t XexLoadImage_entry(lpstring_t module_name, dword_t module_flags,
                                  dword_t min_version, lpdword_t hmodule_ptr) {
  auto load_callback = [module_name] {
    return kernel_state()->LoadUserModule(module_name.value());
  };
  return xeXexLoadImage(module_name, module_flags, min_version, hmodule_ptr,
                        load_callback, false);
}
DECLARE_XBOXKRNL_EXPORT1(XexLoadImage, kModules, kImplemented);

dword_result_t XexLoadImageFromMemory_entry(lpdword_t buffer, dword_t size,
                                            lpstring_t module_name,
                                            dword_t module_flags,
                                            dword_t min_version,
                                            lpdword_t hmodule_ptr) {
  auto load_callback = [module_name, buffer, size] {
    return kernel_state()->LoadUserModuleFromMemory(module_name.value(), buffer,
                                                    size);
  };
  return xeXexLoadImage(module_name, module_flags, min_version, hmodule_ptr,
                        load_callback, true);
}
DECLARE_XBOXKRNL_EXPORT1(XexLoadImageFromMemory, kModules, kImplemented);

// Phase 1099t: the signature was XexLoadImage's, and it is not this function's.
// The real kernel's XexLoadExecutable (80069930) takes
//   (r3 name, r4 COMMAND LINE, r5 module flags, r6 minimum version)
// loads into a LOCAL handle (80069838 with &[r1+0x50]) and passes the command
// line to 80067178, which builds the title's command-line string - there is no
// output handle argument. Treating r6 as an out pointer wrote the module handle
// through the version number: HOST FAULT at xeXexLoadImage `*hmodule_ptr =`
// with fault_addr 120076000 (guest 20076000), reached from xam's title launcher
// (8175BDB4/8175BEF8/8175BF68) when the user opened the dashboard's Games tab.
// Phase 1099z4 research probe: dump xam's launcher object (*81D41318) so the
// state for a Xenia-booted title can be diffed against a xam-launched one.
static void GuideDumpLauncher(const char* tag) {
  auto* mem = kernel_state()->memory();
  const uint32_t obj = xe::load_and_swap<uint32_t>(mem->TranslateVirtual(0x81D41318));
  if (!obj) {
    XELOGI("GuideLauncherDump[{}]: launcher object not set", tag);
    return;
  }
  for (uint32_t off = 0; off < 0x2000; off += 0x40) {
    std::string row;
    for (uint32_t i = 0; i < 0x40; i += 4) {
      row += fmt::format(" {:08X}", xe::load_and_swap<uint32_t>(
                                        mem->TranslateVirtual(obj + off + i)));
    }
    XELOGI("GuideLauncherDump[{}] {:08X}+{:04X}:{}", tag, obj, off, row);
  }
}

// One-time terminate of the title Xenia booted (see XexLoadExecutable).
static bool g_host_terminated_boot_title = false;
void ResetModulesStateForPowerOff() { g_host_terminated_boot_title = false; }

dword_result_t XexLoadExecutable_entry(lpstring_t module_name,
                                       lpstring_t command_line,
                                       dword_t module_flags,
                                       dword_t min_version) {
  XELOGI("XexLoadExecutable('{}', command line '{}', flags {:08X}, min "
         "version {:08X})",
         module_name ? module_name.value() : "",
         command_line ? command_line.value() : "",
         uint32_t(module_flags), uint32_t(min_version));
  XELOGI("XexLoadExecutable: chain {}", kernel_state()->GuestBackChain());
  if (cvars::guide_dump_launcher) {
    GuideDumpLauncher("load");
  }
  // The periodic GuideLiveCoverage report rides a hook that stops firing
  // before a launch; report the coverage function here as well.
  if (cvars::guide_coverage_fn) {
    auto* fn = kernel_state()->processor()->LookupFunction(
        cvars::guide_coverage_fn);
    auto* gfn = fn ? dynamic_cast<xe::cpu::GuestFunction*>(fn) : nullptr;
    if (gfn && gfn->trace_data().is_valid()) {
      auto& td = gfn->trace_data();
      auto* c = reinterpret_cast<uint64_t*>(td.instruction_execute_counts());
      uint32_t ex = 0;
      for (uint32_t i = 0; i < td.instruction_count(); ++i) {
        if (c[i]) ++ex;
      }
      XELOGI("GuideLaunchCoverage: {:08X} {}/{} executed, calls={}",
             uint32_t(cvars::guide_coverage_fn), ex, td.instruction_count(),
             td.header()->function_call_count);
    } else {
      XELOGI("GuideLaunchCoverage: {:08X} never compiled (not run)",
             uint32_t(cvars::guide_coverage_fn));
    }
  }
  // Phase 1099z6: guide_xam_boot_launch - the first launch xam makes names
  // the boot title Xenia already loaded but did not start. Adopt that image
  // instead of loading a second copy (the load itself went through the same
  // XexLoadImage path); everything else is xam's own launch.
  if (auto boot = kernel_state()->boot_launch_module) {
    kernel_state()->boot_launch_module = nullptr;
    std::string want = utf8::lower_ascii(
        utf8::find_name_from_guest_path(module_name.value()));
    std::string have = utf8::lower_ascii(boot->name());
    if (want == have || want == have + ".xex") {
      XELOGI("XexLoadExecutable: adopting boot image {} for '{}'",
             boot->path(), module_name.value());
      kernel_state()->SetExecutableModule(boot);
      return X_STATUS_SUCCESS;
    }
    XELOGW("XexLoadExecutable: boot image {} does not match '{}'; loading "
           "normally and leaving the boot image unstarted",
           boot->path(), module_name.value());
    // 2026-09-16: and not loaded - a console had nothing loaded before xam's
    // first launch (6770 + disc at power-on; see DiscardUnstartedBootImage).
    kernel_state()->DiscardUnstartedBootImage(boot);
  }
  // Phase 1099v: the real XexLoadExecutable (80069888) refuses while an
  // executable is still loaded - XexExecutableModuleHandle must be 0, i.e. the
  // previous title must have been terminated first. xam's failure path then
  // calls ExTerminateTitleProcess and retries (8175EC38).
  if (kernel_state()->GetExecutableModule()) {
    // DECLARED BYPASS. xam's launcher only runs ExTerminateTitleProcess for a
    // title IT launched: its boot path (817274B8 -> 8175A240) sets launcher
    // +0x2E8 = 1, "no title launched yet", and 8175E770 skips the terminate
    // while that holds. Xenia, not xam, launched the boot title, so xam never
    // tears it down and every relaunch is refused (measured: dash, xshell,
    // dash -> C0000022 x3, no ExTerminateTitleProcess). For a TITLE executable
    // (0x40000000) replacing the title Xenia booted, do the terminate xam
    // would have done, once, then load normally.
    if ((uint32_t(module_flags) & 0x40000000u) &&
        !g_host_terminated_boot_title) {
      g_host_terminated_boot_title = true;
      XELOGI("XexLoadExecutable: boot title {} was launched by Xenia, not xam; "
             "running the title terminate xam skips",
             kernel_state()->GetExecutableModule()->path());
      kernel_state()->TerminateTitleProcessSelective();
    }
  }
  if (kernel_state()->GetExecutableModule()) {
    XELOGI("XexLoadExecutable: an executable is still loaded ({}) -> C0000022",
           kernel_state()->GetExecutableModule()->path());
    return 0xC0000022;  // STATUS_ACCESS_DENIED
  }
  auto* mem = kernel_memory();
  const uint32_t handle_slot = mem->SystemHeapAlloc(4);
  if (!handle_slot) {
    return X_STATUS_NO_MEMORY;
  }
  xe::store_and_swap<uint32_t>(mem->TranslateVirtual(handle_slot), 0);
  shim::PrimitivePointerParam<uint32_t> handle_param(
      mem->TranslateVirtual<uint32_t*>(handle_slot));
  const uint32_t result = XexLoadImage_entry(module_name, module_flags,
                                             min_version, handle_param);
  const uint32_t hmodule =
      xe::load_and_swap<uint32_t>(mem->TranslateVirtual(handle_slot));
  XELOGI("XexLoadExecutable -> {:08X} (module handle {:08X})", result, hmodule);
  mem->SystemHeapFree(handle_slot);
  // Phase 1099v: make it THE title executable (sets XexExecutableModuleHandle,
  // title KPROCESS TLS/stack from the header) - what flag 0x40000000 means.
  if (result == X_STATUS_SUCCESS && hmodule) {
    auto module = kernel_state()->GetModule(module_name.value(), true);
    if (module) {
      kernel_state()->SetExecutableModule(object_ref<UserModule>(
          static_cast<UserModule*>(module.release())));
    }
  }
  return result;
}
DECLARE_XBOXKRNL_EXPORT1(XexLoadExecutable, kModules, kSketchy);

// Phase 1099v: XexStartExecutable(StartRoutine) (real 800672D8): fail with
// C000000D if no executable is loaded; otherwise create the title's main
// thread - a TITLE-process thread whose startup runs StartRoutine(0) and then
// the executable's entry point. xam passes 8175DF50, which traps unless it is on
// a title thread and performs the launcher handshake (+0x330/+0x340) and lock
// release. It was an undefined extern: xam logged "XexStartExecutable failed".
dword_result_t XexStartExecutable_entry(dword_t start_routine) {
  auto exe = kernel_state()->GetExecutableModule();
  if (!exe) {
    XELOGW("XexStartExecutable({:08X}): no executable loaded -> C000000D",
           uint32_t(start_routine));
    return X_STATUS_INVALID_PARAMETER;
  }
  auto thread = object_ref<XThread>(
      new XThread(kernel_state(), exe->stack_size(), 0, exe->entry_point(), 0,
                  0, true, true));
  thread->set_name("Main XThread");
  thread->set_pre_start_routine(start_routine);
  X_STATUS result = thread->Create();
  XELOGI("XexStartExecutable({:08X}): {} entry {:08X} -> {:08X}",
         uint32_t(start_routine), exe->path(), exe->entry_point(), result);
  // Research probe: dump a xam-launched title's image (flat, VA-indexed), as
  // guide_dump_title_path does for the boot title.
  if (!cvars::guide_dump_launched_title_path.empty() && exe->xex_module()) {
    auto* mem = kernel_memory();
    const uint32_t base = exe->xex_module()->base_address();
    const uint32_t size = exe->xex_module()->image_size();
    if (FILE* f = std::fopen(cvars::guide_dump_launched_title_path.c_str(),
                             "wb")) {
      static const uint8_t zero[0x1000] = {0};
      for (uint32_t off = 0; off < size; off += 0x1000) {
        const uint32_t n = std::min<uint32_t>(0x1000, size - off);
        auto* heap = mem->LookupHeap(base + off);
        std::fwrite(heap ? mem->TranslateVirtual(base + off) : zero, 1, n, f);
      }
      std::fclose(f);
      XELOGI("DumpLaunchedTitle: {:08X}+{:08X} -> {}", base, size,
             cvars::guide_dump_launched_title_path);
    }
  }
  if (cvars::guide_dump_launcher) {
    std::thread([] {
      std::this_thread::sleep_for(std::chrono::seconds(15));
      GuideDumpLauncher("relaunched");
    }).detach();
  }
  return result;
}
DECLARE_XBOXKRNL_EXPORT1(XexStartExecutable, kModules, kImplemented);

dword_result_t XexSendDeferredNotifications_entry() {
  return kernel_state()->SendDeferredNotifications();
}
DECLARE_XBOXKRNL_EXPORT1(XexSendDeferredNotifications, kModules, kImplemented);

dword_result_t XexUnloadImage_entry(lpvoid_t hmodule) {
  auto module = XModule::GetFromHModule(kernel_state(), hmodule);
  if (!module) {
    return X_STATUS_INVALID_HANDLE;
  }

  // Can't unload kernel modules from user code.
  if (module->module_type() != XModule::ModuleType::kKernelModule) {
    auto ldr_data = hmodule.as<X_LDR_DATA_TABLE_ENTRY*>();
    if (--ldr_data->load_count == 0) {
      // No more references, free it.
      module->Release();
      kernel_state()->UnloadUserModule(object_ref<UserModule>(
          reinterpret_cast<UserModule*>(module.release())));
    }
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(XexUnloadImage, kModules, kImplemented);

dword_result_t XexGetProcedureAddress_entry(lpvoid_t hmodule, dword_t ordinal,
                                            lpdword_t out_function_ptr) {
  // May be entry point?
  assert_not_zero(ordinal);

  bool is_string_name = (ordinal & 0xFFFF0000) != 0;
  auto string_name =
      reinterpret_cast<const char*>(kernel_memory()->TranslateVirtual(ordinal));

  X_STATUS result = X_STATUS_INVALID_HANDLE;

  object_ref<XModule> module;
  if (!hmodule) {
    module = kernel_state()->GetExecutableModule();
  } else {
    module = XModule::GetFromHModule(kernel_state(), hmodule);
  }
  if (module) {
    uint32_t ptr = 0;
    // With LLE xam, runtime lookups against xam keep answering from Xenia's
    // HLE XamModule (what worked before), with two exceptions that resolve
    // against the real xam.xex's export table instead:
    //  1. Exports HLE does not implement (HLE returns 0). Fable III's
    //     XexGetProcedureAddress(xam, 0x48C XamVoiceSetMicArrayIdleUsers)
    //     got 0 although retail xam 17559 exports it (816FB9C0).
    //  2. Enumerator-creating exports (*CreateEnumerator*,
    //     XamCreateEnumeratorHandle): the object they return is consumed by
    //     real xam's enumerate path (static imports, XMsgInProcessCall). Fable
    //     III's HLE XamContentAggregateCreateEnumerator (0x279) built an
    //     8-byte stub and real xam's handler 81687B48 then entered an
    //     uninitialized critical section in it forever (the loading screen
    //     after choosing a character).
    // Resolving EVERYTHING against real xam first was tried and broke two
    // titles: NetDll_XNetStartupEx (Fable III first loading screen, real
    // xam's XNet never gets an address) and XamMediaVerificationCreate
    // (Sonic Generations set XeKeysSecuritySetDetected(12) 84 ms later and
    // in-game sign-in failed).
    const bool module_is_xam = utf8::equal_case(
        utf8::find_base_name_from_guest_path(module->name()), "xam");
    bool caller_wants_real_xam = cvars::lle_xam_scope.empty();
    if (!caller_wants_real_xam) {
      if (auto exe = kernel_state()->GetExecutableModule()) {
        const auto self = utf8::find_base_name_from_guest_path(exe->name());
        const std::string& spec = cvars::lle_xam_scope;
        size_t pos = 0;
        while (pos <= spec.size() && !caller_wants_real_xam) {
          size_t comma = spec.find(',', pos);
          std::string tok = spec.substr(
              pos, comma == std::string::npos ? std::string::npos
                                              : comma - pos);
          pos = (comma == std::string::npos) ? spec.size() + 1 : comma + 1;
          while (!tok.empty() && tok.front() == ' ') tok.erase(tok.begin());
          while (!tok.empty() && tok.back() == ' ') tok.pop_back();
          caller_wants_real_xam =
              !tok.empty() &&
              utf8::equal_case(self,
                               utf8::find_base_name_from_guest_path(tok));
        }
      }
    }
    bool creates_enumerator = false;
    if (module_is_xam) {
      std::string_view export_name;
      if (is_string_name) {
        export_name = string_name;
      } else if (auto* exp = kernel_state()->emulator()->export_resolver()
                                 ->GetExportByOrdinal("xam.xex",
                                                      uint16_t(ordinal))) {
        export_name = exp->name;
      }
      creates_enumerator =
          export_name.find("CreateEnumerator") != std::string_view::npos ||
          export_name == "XamCreateEnumeratorHandle";
    }
    const bool lle_xam_active =
        module_is_xam && !cvars::lle_xam.empty() && caller_wants_real_xam;
    if (!(lle_xam_active && creates_enumerator)) {
      ptr = is_string_name ? module->GetProcAddressByName(string_name)
                           : module->GetProcAddressByOrdinal(ordinal);
    }
    if (!ptr && lle_xam_active) {
      if (auto real_xam = kernel_state()->GetModule("xam.xex", true)) {
        ptr = is_string_name ? real_xam->GetProcAddressByName(string_name)
                             : real_xam->GetProcAddressByOrdinal(ordinal);
        if (ptr) {
          static std::atomic<uint32_t> logged{0};
          if (logged++ < 32) {
            XELOGI("XexGetProcedureAddress: {} {} resolved against real xam "
                   "-> {:08X}",
                   is_string_name ? "export" : "ordinal",
                   is_string_name ? std::string(string_name)
                                  : fmt::format("0x{:X}", uint32_t(ordinal)),
                   ptr);
          }
        }
      }
    }
    if (!ptr && lle_xam_active && creates_enumerator) {
      // Real xam lacks it: fall back to HLE.
      ptr = is_string_name ? module->GetProcAddressByName(string_name)
                           : module->GetProcAddressByOrdinal(ordinal);
    }
    if (ptr) {
      *out_function_ptr = ptr;
      result = X_STATUS_SUCCESS;
    } else {
      if (is_string_name) {
        XELOGW("ERROR: XexGetProcedureAddress export '{}' in '{}' not found!",
               string_name, module->name());
      } else {
        XELOGW(
            "ERROR: XexGetProcedureAddress ordinal {} (0x{:X}) in '{}' not "
            "found!",
            static_cast<uint32_t>(ordinal), static_cast<uint32_t>(ordinal),
            module->name());
      }
      *out_function_ptr = 0;
      result = X_STATUS_DRIVER_ENTRYPOINT_NOT_FOUND;
    }
  }

  return result;
}
DECLARE_XBOXKRNL_EXPORT1(XexGetProcedureAddress, kModules, kImplemented);

void ExRegisterTitleTerminateNotification_entry(
    pointer_t<X_EX_TITLE_TERMINATE_REGISTRATION> reg, dword_t create) {
  if (create) {
    // Adding.
    kernel_state()->RegisterTitleTerminateNotification(
        reg->notification_routine, reg->priority, reg.guest_address());
  } else {
    // Removing.
    kernel_state()->RemoveTitleTerminateNotification(reg->notification_routine);
  }
}
DECLARE_XBOXKRNL_EXPORT1(ExRegisterTitleTerminateNotification, kModules,
                         kImplemented);
// todo: replace magic numbers
dword_result_t XexLoadImageHeaders_entry(pointer_t<X_ANSI_STRING> path,
                                         pointer_t<xex2_header> header,
                                         dword_t buffer_size,
                                         const ppc_context_t& ctx) {
  if (buffer_size < 0x800) {
    return X_STATUS_BUFFER_TOO_SMALL;
  }
  auto current_kernel = ctx->kernel_state;
  auto target_path = util::TranslateAnsiPath(current_kernel->memory(), path);

  vfs::File* vfs_file = nullptr;
  vfs::FileAction file_action;
  X_STATUS result = current_kernel->file_system()->OpenFile(
      nullptr, target_path, vfs::FileDisposition::kOpen,
      vfs::FileAccess::kGenericRead, false, true, &vfs_file, &file_action);

  if (!vfs_file) {
    return result;
  }
  size_t bytes_read = 0;

  X_STATUS result_status = vfs_file->ReadSync(
      std::span<uint8_t>(reinterpret_cast<uint8_t*>(header.host_address()),
                         2048),
      0, &bytes_read);

  if (result_status < 0) {
    vfs_file->Destroy();
    return result_status;
  }

  if (header->magic != 'XEX2') {
    vfs_file->Destroy();
    return X_STATUS_INVALID_IMAGE_FORMAT;
  }
  unsigned int header_size = header->header_size;

  if (header_size < 0x800 || header_size > 0x10000 ||
      (header_size & 0x7FF) != 0) {
    result_status = X_STATUS_INVALID_IMAGE_FORMAT;
  } else if (header_size <= buffer_size) {
    if (header_size <= 0x800) {
      result_status = X_STATUS_SUCCESS;
    } else {
      result_status = vfs_file->ReadSync(
          std::span<uint8_t>(
              reinterpret_cast<uint8_t*>(header.host_address() + 2048),
              header_size - 2048),
          2048, &bytes_read);
      if (result_status >= X_STATUS_SUCCESS) {
        result_status = X_STATUS_SUCCESS;
      }
    }

  } else {
    result_status = X_STATUS_BUFFER_TOO_SMALL;
  }

  // Phase 1099z159: the real XexLoadImageHeaders (17489 800A0B18) ends with
  // 800A08C8(header, 1, 0), which bounds-checks the security info
  // (offset + 4 <= header_size, its size in 0x184..header_size - offset) and,
  // because the second argument is 1, stores the ABSOLUTE address of the
  // security info into header+0x10 (800A0A8C). xam 17559 then hashes 0x100
  // bytes at [header+0x10]; with the raw offset left in place it hashed guest
  // address 0x98 and faulted (system manifest check of AvatarEditor.xex).
  // HOST-SIDE: that routine's RSA check of the header signature (8014A168) is
  // not performed.
  if (result_status == X_STATUS_SUCCESS &&
      cvars::kernel_xex_load_headers_fixup) {
    const uint32_t sec_offset = header->security_offset;
    const uint32_t hdr_size = header->header_size;
    if (sec_offset > hdr_size || hdr_size - sec_offset < 4) {
      result_status = X_STATUS_INVALID_IMAGE_FORMAT;
    } else {
      const uint32_t sec_size = xe::load_and_swap<uint32_t>(
          reinterpret_cast<const uint8_t*>(header.host_address()) + sec_offset);
      if (sec_size < 0x184 || sec_size > hdr_size - sec_offset) {
        result_status = X_STATUS_INVALID_IMAGE_FORMAT;
      } else {
        header->security_offset = header.guest_address() + sec_offset;
      }
    }
  }

  vfs_file->Destroy();
  return result_status;
}
DECLARE_XBOXKRNL_EXPORT1(XexLoadImageHeaders, kModules, kImplemented);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Module);

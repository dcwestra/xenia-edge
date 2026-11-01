/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cstring>

#if XE_ARCH_AMD64
#include <xmmintrin.h>
#endif  // XE_ARCH_AMD64

#include "xenia/base/cvar.h"
#include "xenia/base/debugging.h"
#include "xenia/base/exception_handler.h"
#include "xenia/base/logging.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xthread.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/window.h"
#include "xenia/ui/windowed_app_context.h"

DEFINE_bool(guest_exception_dispatch, true,
            "Hand an access violation in guest code to the guest's own "
            "exception handling, a __try around it, as the console does. "
            "Faults no guest handler takes still crash. Dance Central 2 "
            "runs its game loop from such a handler.",
            "Kernel");

namespace xe {
namespace kernel {
namespace xboxkrnl {

void DbgBreakPoint_entry() { xe::debugging::Break(); }
DECLARE_XBOXKRNL_EXPORT2(DbgBreakPoint, kDebug, kStub, kImportant);

// https://msdn.microsoft.com/en-us/library/xcb2z8hs.aspx
typedef struct {
  xe::be<uint32_t> type;
  xe::be<uint32_t> name_ptr;
  xe::be<uint32_t> thread_id;
  xe::be<uint32_t> flags;
} X_THREADNAME_INFO;
static_assert_size(X_THREADNAME_INFO, 0x10);

void HandleSetThreadName(pointer_t<X_EXCEPTION_RECORD> record) {
  // SetThreadName. FFS.
  // https://msdn.microsoft.com/en-us/library/xcb2z8hs.aspx

  // TODO(benvanik): check record->number_parameters to make sure it's a
  // correct size.
  auto thread_info =
      reinterpret_cast<X_THREADNAME_INFO*>(&record->exception_information[0]);

  assert_true(thread_info->type == 0x1000);

  if (!thread_info->name_ptr) {
    XELOGD("SetThreadName called with null name_ptr");
    return;
  }

  // 4D5307D6 (and its demo) has a bug where it ends up passing freed memory for
  // the name, so at the point of SetThreadName it's filled with junk.

  // TODO(gibbed): cvar for thread name encoding for conversion, some games use
  // SJIS and there's no way to automatically know this.
  auto name = std::string(
      kernel_memory()->TranslateVirtual<const char*>(thread_info->name_ptr));
  std::ranges::replace_if(name, [](auto c) { return c < 32 || c > 127; }, '?');

  object_ref<XThread> thread;
  if (thread_info->thread_id == -1) {
    // Current thread.
    thread = retain_object(XThread::GetCurrentThread());
  } else {
    // Lookup thread by ID.
    thread = kernel_state()->GetThreadByID(thread_info->thread_id);
  }

  if (thread) {
    XELOGD("SetThreadName({}, {})", thread->thread_id(), name);
    thread->set_name(name);
  }

  // TODO(benvanik): unwinding required here?
}

typedef struct {
  xe::be<int32_t> mdisp;
  xe::be<int32_t> pdisp;
  xe::be<int32_t> vdisp;
} x_PMD;

typedef struct {
  xe::be<uint32_t> properties;
  xe::be<uint32_t> type_ptr;
  x_PMD this_displacement;
  xe::be<int32_t> size_or_offset;
  xe::be<uint32_t> copy_function_ptr;
} x_s__CatchableType;

typedef struct {
  xe::be<int32_t> number_catchable_types;
  xe::be<uint32_t> catchable_type_ptrs[1];
} x_s__CatchableTypeArray;

typedef struct {
  xe::be<uint32_t> attributes;
  xe::be<uint32_t> unwind_ptr;
  xe::be<uint32_t> forward_compat_ptr;
  xe::be<uint32_t> catchable_type_array_ptr;
} x_s__ThrowInfo;

// MSVC RTTI type descriptor, the mangled name follows the two pointers.
typedef struct {
  xe::be<uint32_t> vftable_ptr;
  xe::be<uint32_t> spare_ptr;
  char name[1];
} x_TypeDescriptor;

// The C++ EH tables a __CxxFrameHandler frame points at.
typedef struct {
  xe::be<uint32_t> magic_number;
  xe::be<int32_t> max_state;
  xe::be<uint32_t> unwind_map_ptr;
  xe::be<int32_t> try_block_count;
  xe::be<uint32_t> try_block_map_ptr;
  xe::be<int32_t> ip_map_entry_count;
  xe::be<uint32_t> ip_to_state_map_ptr;
} x_FuncInfo;

typedef struct {
  xe::be<int32_t> try_low;
  xe::be<int32_t> try_high;
  xe::be<int32_t> catch_high;
  xe::be<int32_t> catch_count;
  xe::be<uint32_t> handler_array_ptr;
} x_TryBlockMapEntry;

typedef struct {
  xe::be<uint32_t> adjectives;
  xe::be<uint32_t> type_ptr;
  xe::be<int32_t> catch_object_displacement;
  xe::be<uint32_t> handler_address;
} x_HandlerType;

typedef struct {
  xe::be<uint32_t> ip;
  xe::be<int32_t> state;
} x_IpToStateMapEntry;

// True when every page of [address, address + size) is mapped and readable.
static bool IsGuestRangeReadable(uint32_t address, uint32_t size) {
  if (!address || !size || address > 0xFFFFFFFFu - (size - 1)) {
    return false;
  }
  auto* memory = kernel_memory();
  const uint32_t last_page = (address + size - 1) & ~uint32_t(0xFFF);
  for (uint32_t page = address & ~uint32_t(0xFFF);; page += 0x1000) {
    auto* heap = memory->LookupHeap(page);
    uint32_t protect = 0;
    if (!heap || !heap->QueryProtect(page, &protect) ||
        !(protect & kMemoryProtectRead)) {
      return false;
    }
    if (page == last_page) {
      return true;
    }
  }
}

static uint32_t ReadGuestWord(uint32_t address) {
  return *kernel_memory()->TranslateVirtual<xe::be<uint32_t>*>(address);
}

// Reads a NUL terminated guest string, stopping at the first unreadable page.
static std::string ReadGuestString(uint32_t address, uint32_t max_length) {
  std::string value;
  while (value.size() < max_length) {
    uint32_t chunk = 0x1000 - (address & 0xFFF);
    const uint32_t remaining = max_length - static_cast<uint32_t>(value.size());
    if (chunk > remaining) {
      chunk = remaining;
    }
    if (!IsGuestRangeReadable(address, chunk)) {
      break;
    }
    auto* host = kernel_memory()->TranslateVirtual<const char*>(address);
    uint32_t length = 0;
    while (length < chunk && host[length]) {
      ++length;
    }
    value.append(host, length);
    if (length < chunk) {
      break;
    }
    address += chunk;
  }
  std::ranges::replace_if(value, [](auto c) { return c < 32 || c > 127; }, '?');
  return value;
}

// Reads the mangled name out of an RTTI type descriptor.
static std::string ReadGuestTypeName(uint32_t descriptor_ptr) {
  if (!IsGuestRangeReadable(descriptor_ptr, sizeof(x_TypeDescriptor))) {
    return {};
  }
  return ReadGuestString(descriptor_ptr + offsetof(x_TypeDescriptor, name),
                         256);
}

// Describes the try blocks and catch types a frame declares.
// Returns nothing unless the handler data is a FuncInfo, which rejects a plain
// __C_specific_handler scope table.
static std::string DescribeGuestFuncInfo(uint32_t func_info_ptr) {
  if (!IsGuestRangeReadable(func_info_ptr, sizeof(x_FuncInfo))) {
    return {};
  }
  auto* info = kernel_memory()->TranslateVirtual<x_FuncInfo*>(func_info_ptr);
  const uint32_t magic = info->magic_number;
  if (magic < 0x19930520 || magic > 0x19930522) {
    return {};
  }
  int32_t try_block_count = info->try_block_count;
  if (try_block_count > 16) {
    try_block_count = 16;
  }
  std::string text = fmt::format(
      "\n      func_info magic=0x{:08X} max_state={} try_blocks={}", magic,
      int32_t(info->max_state), int32_t(info->try_block_count));
  const uint32_t try_block_map_ptr = info->try_block_map_ptr;
  for (int32_t i = 0; i < try_block_count; ++i) {
    const uint32_t try_ptr =
        try_block_map_ptr + uint32_t(i) * uint32_t(sizeof(x_TryBlockMapEntry));
    if (!IsGuestRangeReadable(try_ptr, sizeof(x_TryBlockMapEntry))) {
      break;
    }
    auto* try_block =
        kernel_memory()->TranslateVirtual<x_TryBlockMapEntry*>(try_ptr);
    int32_t catch_count = try_block->catch_count;
    text.append(fmt::format(
        "\n      try[{}] low={} high={} catch_high={} catches={}", i,
        int32_t(try_block->try_low), int32_t(try_block->try_high),
        int32_t(try_block->catch_high), catch_count));
    if (catch_count > 16) {
      catch_count = 16;
    }
    const uint32_t handler_array_ptr = try_block->handler_array_ptr;
    for (int32_t j = 0; j < catch_count; ++j) {
      const uint32_t handler_ptr =
          handler_array_ptr + uint32_t(j) * uint32_t(sizeof(x_HandlerType));
      if (!IsGuestRangeReadable(handler_ptr, sizeof(x_HandlerType))) {
        break;
      }
      auto* handler =
          kernel_memory()->TranslateVirtual<x_HandlerType*>(handler_ptr);
      const uint32_t type_ptr = handler->type_ptr;
      text.append(fmt::format(
          "\n        catch {} adjectives=0x{:X} handler=0x{:08X}",
          type_ptr ? ReadGuestTypeName(type_ptr) : "...",
          uint32_t(handler->adjectives), uint32_t(handler->handler_address)));
    }
  }
  return text;
}

// Locates the .pdata of the module holding address, trimming its zero padding.
static const xe::be<uint32_t>* GetGuestPdata(uint32_t address,
                                             uint32_t* out_count) {
  auto* xex = dynamic_cast<xe::cpu::XexModule*>(
      kernel_state()->processor()->LookupModule(address));
  auto* section = xex ? xex->GetPESection(".pdata") : nullptr;
  if (!section || !IsGuestRangeReadable(section->address, section->raw_size)) {
    return nullptr;
  }
  auto* entries = kernel_memory()->TranslateVirtual<const xe::be<uint32_t>*>(
      section->address);
  uint32_t count = 0;
  while (count < section->raw_size / 8 && uint32_t(entries[count * 2]) != 0) {
    ++count;
  }
  *out_count = count;
  return entries;
}

// Finds the .pdata entry covering address. Entries are sorted by start.
static bool LookupGuestRuntimeFunction(uint32_t address, uint32_t* out_start,
                                       uint32_t* out_packed) {
  uint32_t count = 0;
  const auto* entries = GetGuestPdata(address, &count);
  if (!entries || !count) {
    return false;
  }
  uint32_t low = 0;
  uint32_t high = count;
  while (low < high) {
    const uint32_t middle = low + (high - low) / 2;
    if (entries[middle * 2] <= address) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  if (!low) {
    return false;
  }
  const uint32_t start = entries[(low - 1) * 2];
  const uint32_t packed = entries[(low - 1) * 2 + 1];
  // Gaps between functions carry no entry, so the nearest start below an
  // address does not have to be the one covering it.
  if (address - start >= ((packed >> 8) & 0x3FFFFF) * 4) {
    return false;
  }
  *out_start = start;
  *out_packed = packed;
  return true;
}

// Walks the back chain, annotating each frame with its .pdata entry.
// A frame holds its entry sp in the back chain word and its saved lr 8 bytes
// below that. The packed word is logged raw so the decode stays checkable.
static std::string GetGuestBacktrace(PPCContext* context) {
  std::string trace;
  uint32_t sp = static_cast<uint32_t>(context->r[1]);
  uint32_t lr = static_cast<uint32_t>(context->lr);
  for (int frame = 0; frame < 24; ++frame) {
    trace.append(
        fmt::format("\n  #{:<2} lr=0x{:08X} sp=0x{:08X}", frame, lr, sp));
    uint32_t start = 0;
    uint32_t packed = 0;
    // See GuestFrame for why this steps back a byte.
    if (LookupGuestRuntimeFunction(lr ? lr - 1 : lr, &start, &packed)) {
      trace.append(fmt::format(
          " fn=0x{:08X} pdata=0x{:08X} prolog={} length={} eh={}", start,
          packed, packed & 0xFF, (packed >> 8) & 0x3FFFFF, packed >> 31));
      // The handler and its data sit in the two words ahead of the prolog.
      if ((packed >> 31) && IsGuestRangeReadable(start - 8, 8)) {
        auto* words =
            kernel_memory()->TranslateVirtual<xe::be<uint32_t>*>(start - 8);
        trace.append(fmt::format(" handler=0x{:08X} handler_data=0x{:08X}",
                                 uint32_t(words[0]), uint32_t(words[1])));
        trace.append(DescribeGuestFuncInfo(words[1]));
      }
    }
    if (!IsGuestRangeReadable(sp, 4)) {
      break;
    }
    uint32_t caller_sp =
        *kernel_memory()->TranslateVirtual<xe::be<uint32_t>*>(sp);
    if (caller_sp <= sp || !IsGuestRangeReadable(caller_sp - 8, 4)) {
      break;
    }
    lr = *kernel_memory()->TranslateVirtual<xe::be<uint32_t>*>(caller_sp - 8);
    sp = caller_sp;
  }
  return trace;
}

// Dumps guest stack words. The nonvolatile registers each frame saved are gone
// from the context by the time the throw lands, but they are still down here.
static std::string GetGuestStackDump(uint32_t sp, uint32_t word_count) {
  std::string dump;
  for (uint32_t i = 0; i < word_count; ++i) {
    const uint32_t address = sp + i * 4;
    if (!IsGuestRangeReadable(address, 4)) {
      break;
    }
    if (!(i % 8)) {
      dump.append(fmt::format("\n  {:08X}:", address));
    }
    dump.append(fmt::format(
        " {:08X}",
        uint32_t(
            *kernel_memory()->TranslateVirtual<xe::be<uint32_t>*>(address))));
  }
  return dump;
}

// Dumps what each distinct pointer on the dumped stack refers to.
// The objects a frame was working on live off the stack, not on it.
static std::string GetGuestPointerDump(uint32_t sp, uint32_t word_count) {
  const uint32_t stack_end = sp + word_count * 4;
  std::vector<uint32_t> seen;
  std::string dump;
  for (uint32_t i = 0; i < word_count && seen.size() < 32; ++i) {
    const uint32_t address = sp + i * 4;
    if (!IsGuestRangeReadable(address, 4)) {
      break;
    }
    const uint32_t target =
        *kernel_memory()->TranslateVirtual<xe::be<uint32_t>*>(address);
    // The stack itself is already dumped above.
    if ((target >= sp && target < stack_end) ||
        !IsGuestRangeReadable(target, 32) ||
        std::find(seen.begin(), seen.end(), target) != seen.end()) {
      continue;
    }
    seen.push_back(target);
    dump.append(fmt::format("\n  {:08X}:", target));
    for (uint32_t word = 0; word < 8; ++word) {
      dump.append(fmt::format(
          " {:08X}",
          uint32_t(*kernel_memory()->TranslateVirtual<xe::be<uint32_t>*>(
              target + word * 4))));
    }
  }
  return dump;
}

// One frame of the guest call chain.
// pc is where the frame resumes, always a return address, so it is looked up
// at pc - 1. A call ending a noreturn function returns onto the next function.
struct GuestFrame {
  uint32_t pc;
  uint32_t sp;
  uint32_t entry_sp;
  uint32_t function;
  uint32_t packed;
};

// Walks the back chain from a frame suspended at the call before pc. False
// means it ran out of readable stack, leaving the last frame with no entry_sp
// and no way to unwind through it.
static bool WalkGuestFramesFrom(uint32_t pc, uint32_t sp,
                                std::vector<GuestFrame>* out_frames) {
  for (int i = 0; i < 24; ++i) {
    GuestFrame frame = {};
    frame.pc = pc;
    frame.sp = sp;
    LookupGuestRuntimeFunction(pc ? pc - 1 : pc, &frame.function,
                               &frame.packed);
    if (!IsGuestRangeReadable(sp, 4)) {
      out_frames->push_back(frame);
      return false;
    }
    const uint32_t caller_sp = ReadGuestWord(sp);
    if (caller_sp <= sp || !IsGuestRangeReadable(caller_sp - 8, 4)) {
      out_frames->push_back(frame);
      return false;
    }
    frame.entry_sp = caller_sp;
    out_frames->push_back(frame);
    pc = ReadGuestWord(caller_sp - 8);
    sp = caller_sp;
  }
  return true;
}

static bool WalkGuestFrames(PPCContext* context,
                            std::vector<GuestFrame>* out_frames) {
  return WalkGuestFramesFrom(static_cast<uint32_t>(context->lr),
                             static_cast<uint32_t>(context->r[1]), out_frames);
}

// Undoes one frame's prolog, reloading the registers it saved on entry.
// Undoing frames outward leaves the context holding the state of the frame past
// the last one undone. A null context validates without writing anything.
// Bails on any prolog form but the plain stores, a save helper call included.
static bool UnwindGuestFrame(const GuestFrame& frame, PPCContext* context) {
  if (!frame.entry_sp || !frame.function) {
    return false;
  }
  uint32_t sp = frame.entry_sp;
  const uint32_t prolog_length = frame.packed & 0xFF;
  for (uint32_t i = 0; i < prolog_length; ++i) {
    const uint32_t address = frame.function + i * 4;
    if (!IsGuestRangeReadable(address, 4)) {
      return false;
    }
    const uint32_t instruction = ReadGuestWord(address);
    if (instruction == 0x7D8802A6) {  // mflr r12
      continue;
    }
    const uint32_t opcode = instruction >> 26;
    const uint32_t rs = (instruction >> 21) & 31;
    const uint32_t ra = (instruction >> 16) & 31;
    const int32_t displacement = static_cast<int16_t>(instruction & 0xFFFF);
    if (opcode == 37) {  // stwu r1, d(r1), the frame allocation
      if (ra != 1 || rs != 1) {
        return false;
      }
      sp += displacement;
      continue;
    }
    if (ra != 1) {
      return false;
    }
    if (opcode == 14) {  // addi rD, r1, d, a frame pointer, nothing saved
      continue;
    }
    if (opcode == 36) {  // stw rS, d(r1)
      const uint32_t at = sp + displacement;
      if (rs >= 13) {
        if (!IsGuestRangeReadable(at, 4)) {
          return false;
        }
        if (context) {
          context->r[rs] = ReadGuestWord(at);
        }
      }
      continue;
    }
    if (opcode == 62 && !(instruction & 3)) {  // std rS, ds(r1)
      const uint32_t at = sp + (displacement & ~int32_t(3));
      if (rs >= 13) {
        if (!IsGuestRangeReadable(at, 8)) {
          return false;
        }
        if (context) {
          context->r[rs] =
              (uint64_t(ReadGuestWord(at)) << 32) | ReadGuestWord(at + 4);
        }
      }
      continue;
    }
    if (opcode == 54) {  // stfd frS, d(r1)
      const uint32_t at = sp + displacement;
      if (rs >= 14) {
        if (!IsGuestRangeReadable(at, 8)) {
          return false;
        }
        if (context) {
          const uint64_t bits =
              (uint64_t(ReadGuestWord(at)) << 32) | ReadGuestWord(at + 4);
          std::memcpy(&context->f[rs], &bits, sizeof(bits));
        }
      }
      continue;
    }
    return false;
  }
  return true;
}

// Finds the catch(...) covering the call a frame is suspended at.
// Only an ellipsis handler is accepted, matching a thrown type against the
// catchable types would be a second step.
static bool FindGuestCatchAll(uint32_t func_info_ptr, uint32_t return_address,
                              uint32_t* out_handler, std::string* out_reason) {
  if (!IsGuestRangeReadable(func_info_ptr, sizeof(x_FuncInfo))) {
    *out_reason = "handler data is unreadable";
    return false;
  }
  auto* info = kernel_memory()->TranslateVirtual<x_FuncInfo*>(func_info_ptr);
  const uint32_t magic = info->magic_number;
  if (magic < 0x19930520 || magic > 0x19930522) {
    *out_reason = "handler data is not a FuncInfo";
    return false;
  }
  // The map is sorted by ip and each entry opens a state, so the last entry at
  // or below the call gives the state it was made in. Step back off the return
  // address, a state can begin on the instruction after the call.
  const uint32_t pc = return_address ? return_address - 1 : return_address;
  int32_t state = -1;
  int32_t ip_map_entry_count = info->ip_map_entry_count;
  if (ip_map_entry_count > 4096) {
    ip_map_entry_count = 4096;
  }
  const uint32_t ip_to_state_map_ptr = info->ip_to_state_map_ptr;
  for (int32_t i = 0; i < ip_map_entry_count; ++i) {
    const uint32_t at = ip_to_state_map_ptr +
                        uint32_t(i) * uint32_t(sizeof(x_IpToStateMapEntry));
    if (!IsGuestRangeReadable(at, sizeof(x_IpToStateMapEntry))) {
      break;
    }
    auto* entry = kernel_memory()->TranslateVirtual<x_IpToStateMapEntry*>(at);
    if (uint32_t(entry->ip) > pc) {
      break;
    }
    state = entry->state;
  }
  if (state < 0) {
    *out_reason = "no state covers the resume pc";
    return false;
  }
  int32_t try_block_count = info->try_block_count;
  if (try_block_count > 16) {
    try_block_count = 16;
  }
  const uint32_t try_block_map_ptr = info->try_block_map_ptr;
  for (int32_t i = 0; i < try_block_count; ++i) {
    const uint32_t try_ptr =
        try_block_map_ptr + uint32_t(i) * uint32_t(sizeof(x_TryBlockMapEntry));
    if (!IsGuestRangeReadable(try_ptr, sizeof(x_TryBlockMapEntry))) {
      break;
    }
    auto* try_block =
        kernel_memory()->TranslateVirtual<x_TryBlockMapEntry*>(try_ptr);
    if (state < int32_t(try_block->try_low) ||
        state > int32_t(try_block->try_high)) {
      continue;
    }
    int32_t catch_count = try_block->catch_count;
    if (catch_count > 16) {
      catch_count = 16;
    }
    const uint32_t handler_array_ptr = try_block->handler_array_ptr;
    for (int32_t j = 0; j < catch_count; ++j) {
      const uint32_t handler_ptr =
          handler_array_ptr + uint32_t(j) * uint32_t(sizeof(x_HandlerType));
      if (!IsGuestRangeReadable(handler_ptr, sizeof(x_HandlerType))) {
        break;
      }
      auto* handler =
          kernel_memory()->TranslateVirtual<x_HandlerType*>(handler_ptr);
      if (!uint32_t(handler->type_ptr)) {
        *out_handler = handler->handler_address;
        return true;
      }
    }
    *out_reason =
        fmt::format("try block {} at state {} has no catch(...)", i, state);
    return false;
  }
  *out_reason = fmt::format("no try block covers state {}", state);
  return false;
}

struct CppDispatchPlan {
  std::vector<GuestFrame> frames;
  size_t catcher = 0;
  uint32_t funclet = 0;
};

// Decides whether a guest C++ throw can reach a catch(...) in an ancestor.
// Only when the unwind needs no guest code of its own, so every frame in
// between must be handler free with a prolog we can undo. Pure analysis, which
// lets the plan be logged before anything acts on it.
static bool PlanCppDispatch(PPCContext* context, CppDispatchPlan* plan,
                            std::string* log, std::string* out_reason) {
  const bool walked = WalkGuestFrames(context, &plan->frames);
  auto& frames = plan->frames;
  size_t catcher = 0;
  while (catcher < frames.size() && !(frames[catcher].packed >> 31)) {
    if (!frames[catcher].function) {
      *out_reason = fmt::format("frame #{} pc=0x{:08X} has no pdata entry",
                                catcher, frames[catcher].pc);
      return false;
    }
    ++catcher;
  }
  if (catcher >= frames.size()) {
    *out_reason =
        fmt::format("no frame with a handler in {} frames{}", frames.size(),
                    walked ? "" : ", stack walk incomplete");
    return false;
  }
  plan->catcher = catcher;
  const GuestFrame& frame = frames[catcher];
  if (!IsGuestRangeReadable(frame.function - 8, 8)) {
    *out_reason = "the handler words are unreadable";
    return false;
  }
  if (!FindGuestCatchAll(ReadGuestWord(frame.function - 4), frame.pc,
                         &plan->funclet, out_reason)) {
    return false;
  }
  if (!IsGuestRangeReadable(plan->funclet, 4)) {
    *out_reason = fmt::format("funclet 0x{:08X} is not code", plan->funclet);
    return false;
  }
  for (size_t i = 0; i < catcher; ++i) {
    if (!UnwindGuestFrame(frames[i], nullptr)) {
      *out_reason =
          fmt::format("frame #{} fn=0x{:08X} has a prolog we cannot undo", i,
                      frames[i].function);
      return false;
    }
  }

  // What it takes to check the handoff, logged before the funclet runs. A
  // fault in there would otherwise take the whole message with it.
  log->append(fmt::format(
      "\n  dispatching to catch(...) 0x{:08X} for frame #{} fn=0x{:08X} "
      "sp=0x{:08X}",
      plan->funclet, catcher, frame.function, frame.sp));
  uint32_t funclet_start = 0;
  uint32_t funclet_packed = 0;
  if (LookupGuestRuntimeFunction(plan->funclet, &funclet_start,
                                 &funclet_packed)) {
    log->append(fmt::format(
        "\n  funclet fn=0x{:08X} pdata=0x{:08X} prolog={} length={} eh={}",
        funclet_start, funclet_packed, funclet_packed & 0xFF,
        (funclet_packed >> 8) & 0x3FFFFF, funclet_packed >> 31));
  }
  if (IsGuestRangeReadable(plan->funclet, 128)) {
    log->append("\n  funclet code:");
    for (uint32_t i = 0; i < 32; ++i) {
      if (!(i % 8)) {
        log->append(fmt::format("\n    {:08X}:", plan->funclet + i * 4));
      }
      log->append(fmt::format(" {:08X}", ReadGuestWord(plan->funclet + i * 4)));
    }
  }
  return true;
}

// Rebuilds the catching frame's registers, runs the funclet the way
// __CxxFrameHandler would, then points lr at the address it hands back so the
// import thunk lands there on the way out.
// That jump strands the host JIT frames between the throw and the catcher.
// Guest state lives in the context and the guest stack, so the only cost is
// host stack, a few frames per throw, never reclaimed for the thread's life.
static void ApplyCppDispatch(PPCContext* context, const CppDispatchPlan& plan) {
  auto* thread = XThread::GetCurrentThread();
  if (!thread) {
    XELOGE("Guest C++ exception dispatch: no current thread");
    return;
  }
  const GuestFrame& frame = plan.frames[plan.catcher];
  for (size_t i = 0; i < plan.catcher; ++i) {
    UnwindGuestFrame(plan.frames[i], context);
  }
  context->r[1] = frame.sp;
  // A catch funclet rebuilds the catching function's frame pointer from r12,
  // reusing the offset that function's own prolog used. So r12 has to hold the
  // establisher frame, the sp the catching function was entered on.
  context->r[12] = frame.entry_sp;
  // ExecuteRaw passes the sentinel as the return address argument but does not
  // put it in lr, and the funclet blrs to whatever lr holds.
  context->lr = 0xBCBCBCBC;

  std::string handover =
      fmt::format("r1=0x{:08X} r12=0x{:08X}", frame.sp, frame.entry_sp);
  for (uint32_t i = 13; i < 32; ++i) {
    handover.append(fmt::format(" r{}=0x{:08X}", i, uint32_t(context->r[i])));
  }
  // On record before the funclet runs, a fault in there loses anything later.
  XELOGD("Guest C++ exception dispatch: entering 0x{:08X} with {}",
         plan.funclet, handover);

  if (!kernel_state()->processor()->ExecuteRaw(thread->thread_state(),
                                               plan.funclet)) {
    XELOGE("Guest C++ exception dispatch: the funclet did not resolve");
    return;
  }
  const uint32_t continuation = static_cast<uint32_t>(context->r[3]);
  if (!IsGuestRangeReadable(continuation, 4)) {
    XELOGE(
        "Guest C++ exception dispatch: funclet returned 0x{:08X}, which is not "
        "readable code, so the resume point is unknown",
        continuation);
    return;
  }
  context->r[1] = frame.sp;
  context->lr = continuation;
  XELOGD("Guest C++ exception dispatch: resuming at 0x{:08X} with sp=0x{:08X}",
         continuation, frame.sp);
}

void HandleCppException(pointer_t<X_EXCEPTION_RECORD> record,
                        PPCContext* context) {
  // C++ exception.
  // https://blogs.msdn.com/b/oldnewthing/archive/2010/07/30/10044061.aspx
  // http://www.drdobbs.com/visual-c-exception-handling-instrumentat/184416600
  // http://www.openrce.org/articles/full_view/21

  assert_true(record->number_parameters == 3);
  assert_true(record->exception_information[0] == 0x19930520);

  const uint32_t thrown_ptr = record->exception_information[1];
  const uint32_t throw_info_ptr = record->exception_information[2];

  // One error line says a throw happened and what became of it. Everything
  // that explains it is detail, and a delivered throw is routine.
  std::string thrown_type_name;
  std::string message = fmt::format("\n  object=0x{:08X} throw_info=0x{:08X}",
                                    thrown_ptr, throw_info_ptr);

  // Vftable for a throw by value, the thrown pointer itself for a throw by
  // pointer, which the leading .PA of the mangled type name marks.
  if (IsGuestRangeReadable(thrown_ptr, 4)) {
    message.append(fmt::format(
        "\n  object[0]=0x{:08X}",
        uint32_t(*kernel_memory()->TranslateVirtual<xe::be<uint32_t>*>(
            thrown_ptr))));
  }

  if (IsGuestRangeReadable(throw_info_ptr, sizeof(x_s__ThrowInfo))) {
    auto* throw_info =
        kernel_memory()->TranslateVirtual<x_s__ThrowInfo*>(throw_info_ptr);
    message.append(fmt::format("\n  attributes=0x{:08X}",
                               uint32_t(throw_info->attributes)));

    // First entry is the most derived type, the rest are its bases.
    const uint32_t array_ptr = throw_info->catchable_type_array_ptr;
    if (IsGuestRangeReadable(array_ptr, sizeof(uint32_t))) {
      auto* catchable_types =
          kernel_memory()->TranslateVirtual<x_s__CatchableTypeArray*>(
              array_ptr);
      int32_t count = catchable_types->number_catchable_types;
      if (count > 32) {
        count = 32;
      }
      for (int32_t i = 0; i < count; ++i) {
        const uint32_t entry_ptr = array_ptr + 4 + i * 4;
        if (!IsGuestRangeReadable(entry_ptr, sizeof(uint32_t))) {
          break;
        }
        const uint32_t catchable_ptr =
            *kernel_memory()->TranslateVirtual<xe::be<uint32_t>*>(entry_ptr);
        if (!IsGuestRangeReadable(catchable_ptr, sizeof(x_s__CatchableType))) {
          continue;
        }
        auto* catchable =
            kernel_memory()->TranslateVirtual<x_s__CatchableType*>(
                catchable_ptr);
        const uint32_t descriptor_ptr = catchable->type_ptr;
        auto name = ReadGuestTypeName(descriptor_ptr);
        if (!i) {
          thrown_type_name = name;
        }
        message.append(fmt::format(
            "\n  {}: {}", i == 0 ? "thrown type" : "  base type", name));
      }
    }
  }

  // Anything deriving from std::exception keeps the what() string right after
  // the vftable, so it is worth a look even though the layout is a guess. Only
  // for a throw by value, a leading .PA means the object is a pointer and the
  // word after it belongs to the caller.
  if (!thrown_type_name.starts_with(".PA") &&
      IsGuestRangeReadable(thrown_ptr + 4, 4)) {
    const uint32_t what_ptr =
        *kernel_memory()->TranslateVirtual<xe::be<uint32_t>*>(thrown_ptr + 4);
    auto what = ReadGuestString(what_ptr, 256);
    if (!what.empty()) {
      message.append(fmt::format("\n  possible what(): \"{}\"", what));
    }
  }

  const uint32_t sp = static_cast<uint32_t>(context->r[1]);
  message.append("\nGuest backtrace at the throw:");
  message.append(GetGuestBacktrace(context));

  CppDispatchPlan plan;
  std::string reason;
  const bool dispatch = PlanCppDispatch(context, &plan, &message, &reason);
  if (!dispatch) {
    message.append("\nGuest stack at the throw:");
    message.append(GetGuestStackDump(sp, 192));
    message.append("\nPointed-to memory:");
    message.append(GetGuestPointerDump(sp, 192));
  }

  const auto type = thrown_type_name.empty() ? "of an unknown type"
                                             : thrown_type_name.c_str();
  if (dispatch) {
    XELOGE(
        "Guest threw a C++ exception {}, delivering it to the catch(...) at "
        "0x{:08X}",
        type, plan.funclet);
  } else {
    XELOGE(
        "Guest threw a C++ exception {} that cannot be delivered, {}. The "
        "guest will run on into the throw.",
        type, reason);
  }

  // xe::debugging::Break();
  // Flushed before any guest code runs, the funclet can fault.
  XELOGD("Guest C++ exception detail:{}", message);

  if (dispatch) {
    ApplyCppDispatch(context, plan);
  }
}

// Faults reach the guest's __try blocks the way the console's dispatcher hands
// them out: to the first frame whose __C_specific_handler scope covers the
// fault and whose filter accepts it. Only the kernel's __C_specific_handler is
// understood. Frames with other handlers are passed over, which is what the
// C++ ones do with a fault.

// __C_specific_handler's scope table: a count, then these.
typedef struct {
  xe::be<uint32_t> begin_address;
  xe::be<uint32_t> end_address;
  // The filter, 1 for a constant EXCEPTION_EXECUTE_HANDLER.
  xe::be<uint32_t> handler_address;
  // The __except block, 0 for a __finally.
  xe::be<uint32_t> jump_target;
} x_ScopeTableEntry;

// CONTEXT fields.
constexpr uint32_t kContextFlags = 0x0;
constexpr uint32_t kContextIar = 0x8;
constexpr uint32_t kContextLr = 0xC;
constexpr uint32_t kContextCtr = 0x10;
constexpr uint32_t kContextGpr = 0x18;
constexpr uint32_t kContextCr = 0x118;
constexpr uint32_t kContextXer = 0x11C;
constexpr uint32_t kContextFpr = 0x128;
// Through the 128 vector registers.
constexpr uint32_t kContextSize = 0xA40;
// CONTEXT_CONTROL | CONTEXT_FLOATING_POINT | CONTEXT_INTEGER.
constexpr uint32_t kContextFullFlags = 0x7;

constexpr uint32_t kExceptionContinueExecution = 0xFFFFFFFF;
constexpr uint32_t kExceptionContinueSearch = 0;

// The records go this far below the faulting sp, past anything the function
// may keep under it, and the filter's frames below them.
constexpr uint32_t kFaultStackGap = 0x400;
constexpr uint32_t kFaultPointers = 0x0;
constexpr uint32_t kFaultRecord = 0x10;
constexpr uint32_t kFaultContext = 0x60;
constexpr uint32_t kFaultRecordsSize = kFaultContext + kContextSize;
constexpr uint32_t kFilterStackGap = 0x100;

// Whether handler is an import of the kernel's __C_specific_handler.
static bool IsCSpecificHandler(uint32_t handler) {
  auto* function = kernel_state()->processor()->LookupFunction(handler);
  if (!function || function->behavior() != cpu::Function::Behavior::kExtern) {
    return false;
  }
  auto* export_data = static_cast<cpu::GuestFunction*>(function)->export_data();
  return export_data &&
         std::strcmp(export_data->name, "__C_specific_handler") == 0;
}

// Walks the frames from a fault at pc. The faulting function may not have
// built its frame yet, or be a leaf that never does, and then its sp is its
// caller's and its return address is still in lr.
static void WalkGuestFaultFrames(PPCContext* context, uint32_t pc,
                                 std::vector<GuestFrame>* out_frames,
                                 bool* out_fault_frame_built) {
  GuestFrame frame = {};
  frame.pc = pc;
  frame.sp = static_cast<uint32_t>(context->r[1]);
  LookupGuestRuntimeFunction(pc, &frame.function, &frame.packed);
  bool built = false;
  bool saves_lr = false;
  for (uint32_t i = 0; frame.function && i < (frame.packed & 0xFF); ++i) {
    const uint32_t address = frame.function + i * 4;
    if (address >= pc || !IsGuestRangeReadable(address, 4)) {
      break;
    }
    const uint32_t instruction = ReadGuestWord(address);
    if (instruction == 0x7D8802A6) {  // mflr r12
      saves_lr = true;
    } else if ((instruction & 0xFFFF0000) == 0x94210000) {  // stwu r1, d(r1)
      built = true;
    }
  }
  *out_fault_frame_built = built;
  uint32_t return_address = static_cast<uint32_t>(context->lr);
  frame.entry_sp = frame.sp;
  if (built) {
    if (!IsGuestRangeReadable(frame.sp, 4)) {
      out_frames->push_back(frame);
      return;
    }
    frame.entry_sp = ReadGuestWord(frame.sp);
    if (saves_lr) {
      if (!IsGuestRangeReadable(frame.entry_sp - 8, 4)) {
        out_frames->push_back(frame);
        return;
      }
      return_address = ReadGuestWord(frame.entry_sp - 8);
    }
  }
  out_frames->push_back(frame);
  WalkGuestFramesFrom(return_address, frame.entry_sp, out_frames);
}

// The __except scope of a frame covering its pc, for which a filter has to be
// asked, or none.
struct GuestScope {
  size_t frame;
  uint32_t filter;
  uint32_t target;
};

// Every __except scope covering the fault, innermost first. The faulting frame
// is at the fault, the others at the call before their return address.
static std::vector<GuestScope> FindGuestScopes(
    const std::vector<GuestFrame>& frames) {
  std::vector<GuestScope> scopes;
  for (size_t i = 0; i < frames.size(); ++i) {
    const GuestFrame& frame = frames[i];
    if (!frame.function || !(frame.packed >> 31) ||
        !IsGuestRangeReadable(frame.function - 8, 8) ||
        !IsCSpecificHandler(ReadGuestWord(frame.function - 8))) {
      continue;
    }
    const uint32_t table = ReadGuestWord(frame.function - 4);
    if (!IsGuestRangeReadable(table, 4)) {
      continue;
    }
    const uint32_t pc = i ? frame.pc - 4 : frame.pc;
    const uint32_t count = std::min<uint32_t>(ReadGuestWord(table), 64);
    for (uint32_t j = 0; j < count; ++j) {
      const uint32_t entry_ptr = table + 4 + j * sizeof(x_ScopeTableEntry);
      if (!IsGuestRangeReadable(entry_ptr, sizeof(x_ScopeTableEntry))) {
        break;
      }
      auto* entry =
          kernel_memory()->TranslateVirtual<x_ScopeTableEntry*>(entry_ptr);
      if (pc >= entry->begin_address && pc < entry->end_address &&
          entry->jump_target) {
        scopes.push_back({i, entry->handler_address, entry->jump_target});
      }
    }
  }
  return scopes;
}

// Fills the exception record, the CONTEXT and the EXCEPTION_POINTERS at base.
static void WriteGuestFaultRecords(PPCContext* context, uint32_t base,
                                   uint32_t pc, uint32_t address,
                                   bool is_write) {
  auto* memory = kernel_memory();
  std::memset(memory->TranslateVirtual(base), 0, kFaultRecordsSize);
  auto* record =
      memory->TranslateVirtual<X_EXCEPTION_RECORD*>(base + kFaultRecord);
  record->code = X_STATUS_ACCESS_VIOLATION;
  record->exception_address = pc;
  record->number_parameters = 2;
  record->exception_information[0] = is_write ? 1 : 0;
  record->exception_information[1] = address;
  auto* p = memory->TranslateVirtual<uint8_t*>(base + kFaultContext);
  xe::store_and_swap<uint32_t>(p + kContextFlags, kContextFullFlags);
  xe::store_and_swap<uint32_t>(p + kContextIar, pc);
  xe::store_and_swap<uint32_t>(p + kContextLr, uint32_t(context->lr));
  xe::store_and_swap<uint64_t>(p + kContextCtr, context->ctr);
  for (uint32_t i = 0; i < 32; ++i) {
    xe::store_and_swap<uint64_t>(p + kContextGpr + i * 8, context->r[i]);
    xe::store_and_swap<double>(p + kContextFpr + i * 8, context->f[i]);
  }
  xe::store_and_swap<uint32_t>(p + kContextCr, uint32_t(context->cr()));
  xe::store_and_swap<uint32_t>(p + kContextXer, context->xer());
  auto* pointers =
      memory->TranslateVirtual<xe::be<uint32_t>*>(base + kFaultPointers);
  pointers[0] = base + kFaultRecord;
  pointers[1] = base + kFaultContext;
}

// Runs the guest from address until the thread's entry function returns, then
// ends the thread with its result, as XThread::Execute does.
[[noreturn]] static void RunGuestToThreadExit(XThread* thread,
                                              uint32_t address) {
  PPCContext* context = thread->thread_state()->context();
  context->lr = 0xBCBCBCBC;
  if (!kernel_state()->processor()->ExecuteRaw(thread->thread_state(),
                                               address)) {
    XELOGE("Guest exception dispatch: no code at 0x{:08X}", address);
  }
  thread->Exit(static_cast<int>(context->r[3]));
  // Exit doesn't return on the thread itself.
  while (true) {
    xe::threading::NanoSleep(int64_t(1'000'000'000));
  }
}

// Asks the filters, innermost first, and acts on the first that takes the
// fault. Returns only if none does.
static void DispatchGuestFault(XThread* thread, uint32_t pc, uint32_t address,
                               bool is_write) {
  PPCContext* context = thread->thread_state()->context();
  std::vector<GuestFrame> frames;
  bool fault_frame_built = false;
  WalkGuestFaultFrames(context, pc, &frames, &fault_frame_built);
  const std::vector<GuestScope> scopes = FindGuestScopes(frames);
  const uint32_t sp = static_cast<uint32_t>(context->r[1]);
  const uint32_t base =
      (sp - kFaultStackGap - kFaultRecordsSize) & ~uint32_t(15);
  const uint32_t filter_sp = base - kFilterStackGap;
  if (scopes.empty() || !IsGuestRangeReadable(filter_sp, sp - filter_sp)) {
    return;
  }
  WriteGuestFaultRecords(context, base, pc, address, is_write);
  for (const GuestScope& scope : scopes) {
    const GuestFrame& frame = frames[scope.frame];
    uint32_t verdict = 1;
    if (scope.filter != 1) {
      XELOGI(
          "Guest exception dispatch: access violation at 0x{:08X} (0x{:08X}), "
          "asking the filter 0x{:08X} of frame #{} fn=0x{:08X}",
          pc, address, scope.filter, scope.frame, frame.function);
      // Like a catch funclet, a filter finds its function's frame from r12.
      context->r[1] = filter_sp;
      *kernel_memory()->TranslateVirtual<xe::be<uint32_t>*>(filter_sp) = sp;
      context->r[3] = base + kFaultPointers;
      context->r[4] = frame.entry_sp;
      context->r[12] = frame.entry_sp;
      context->lr = 0xBCBCBCBC;
      if (!kernel_state()->processor()->ExecuteRaw(thread->thread_state(),
                                                   scope.filter)) {
        XELOGE("Guest exception dispatch: no code at the filter 0x{:08X}",
               scope.filter);
        continue;
      }
      verdict = static_cast<uint32_t>(context->r[3]);
    }
    if (verdict == kExceptionContinueSearch) {
      continue;
    }
    if (verdict == kExceptionContinueExecution) {
      // The filter may have changed the CONTEXT, the fault resumes from it.
      auto* p =
          kernel_memory()->TranslateVirtual<uint8_t*>(base + kFaultContext);
      for (uint32_t i = 0; i < 32; ++i) {
        context->r[i] = xe::load_and_swap<uint64_t>(p + kContextGpr + i * 8);
        context->f[i] = xe::load_and_swap<double>(p + kContextFpr + i * 8);
      }
      context->ctr = xe::load_and_swap<uint64_t>(p + kContextCtr);
      context->set_cr(xe::load_and_swap<uint32_t>(p + kContextCr));
      context->set_xer(xe::load_and_swap<uint32_t>(p + kContextXer));
      const uint32_t resume = xe::load_and_swap<uint32_t>(p + kContextIar);
      const uint32_t lr = xe::load_and_swap<uint32_t>(p + kContextLr);
      XELOGI("Guest exception dispatch: the filter resumes at 0x{:08X}",
             resume);
      context->lr = lr;
      RunGuestToThreadExit(thread, resume);
    }
    // The __except block runs in its function's frame, so undo the frames
    // inside it. Their own handlers (__finally blocks, destructors) don't run.
    for (size_t i = (fault_frame_built ? 0 : 1); i < scope.frame; ++i) {
      if (!UnwindGuestFrame(frames[i], nullptr)) {
        XELOGE(
            "Guest exception dispatch: frame #{} fn=0x{:08X} has a prolog we "
            "cannot undo, so the __except block at 0x{:08X} can't run",
            i, frames[i].function, scope.target);
        return;
      }
    }
    for (size_t i = (fault_frame_built ? 0 : 1); i < scope.frame; ++i) {
      UnwindGuestFrame(frames[i], context);
    }
    XELOGI(
        "Guest exception dispatch: access violation at 0x{:08X} (0x{:08X}) "
        "handled by the __except block at 0x{:08X} of frame #{} fn=0x{:08X}",
        pc, address, scope.target, scope.frame, frame.function);
    context->r[1] = frame.sp;
    // The handler finds the exception code in r3.
    context->r[3] = X_STATUS_ACCESS_VIOLATION;
    RunGuestToThreadExit(thread, scope.target);
  }
}

#if !XE_PLATFORM_WIN32
// Guest code can't run on the signal stack, so the fault diverts to the thunk
// below, on the faulting fiber's own stack.
struct PendingGuestFault {
  HostThreadContext host_context;
  uint32_t pc;
  uint32_t address;
  bool is_write;
};
thread_local PendingGuestFault pending_guest_fault;
// A fault no guest handler took runs again to reach the crash path.
thread_local bool guest_fault_declined = false;
thread_local uint32_t guest_fault_declined_pc = 0;

[[noreturn]] static void GuestFaultDispatchThunk() {
  const PendingGuestFault fault = pending_guest_fault;
#if XE_ARCH_AMD64
  // The signal return put back the MXCSR the fault ran with, which the context
  // doesn't carry and the filters' guest code changes.
  const uint32_t mxcsr = _mm_getcsr();
#endif  // XE_ARCH_AMD64
  XThread* thread = XThread::GetCurrentThread();
  PPCContext* context = thread->thread_state()->context();
  // The filters ran guest code, the crash dump wants the fault's registers.
  struct {
    uint64_t r[32];
    double f[32];
    vec128_t v[128];
    uint64_t lr;
    uint64_t ctr;
    uint64_t cr;
    uint32_t xer;
  } saved;
  std::memcpy(saved.r, context->r, sizeof(saved.r));
  std::memcpy(saved.f, context->f, sizeof(saved.f));
  std::memcpy(saved.v, context->v, sizeof(saved.v));
  saved.lr = context->lr;
  saved.ctr = context->ctr;
  saved.cr = context->cr();
  saved.xer = context->xer();
  DispatchGuestFault(thread, fault.pc, fault.address, fault.is_write);
  std::memcpy(context->r, saved.r, sizeof(saved.r));
  std::memcpy(context->f, saved.f, sizeof(saved.f));
  std::memcpy(context->v, saved.v, sizeof(saved.v));
  context->lr = saved.lr;
  context->ctr = saved.ctr;
  context->set_cr(saved.cr);
  context->set_xer(saved.xer);
  guest_fault_declined = true;
  guest_fault_declined_pc = fault.pc;
#if XE_ARCH_AMD64
  _mm_setcsr(mxcsr);
#endif  // XE_ARCH_AMD64
  ResumeHostContext(&fault.host_context);
}
#endif  // !XE_PLATFORM_WIN32

bool DivertGuestAccessViolation(Exception* ex, uint32_t pc) {
#if XE_PLATFORM_WIN32
  return false;
#else
  if (guest_fault_declined) {
    guest_fault_declined = false;
    if (guest_fault_declined_pc == pc) {
      return false;
    }
  }
  XThread* thread = XThread::GetCurrentThread();
  if (!cvars::guest_exception_dispatch || !thread ||
      ex->code() != Exception::Code::kAccessViolation) {
    return false;
  }
  std::vector<GuestFrame> frames;
  bool fault_frame_built = false;
  WalkGuestFaultFrames(thread->thread_state()->context(), pc, &frames,
                       &fault_frame_built);
  if (FindGuestScopes(frames).empty()) {
    return false;
  }
  auto* memory = kernel_memory();
  const auto* host_address =
      reinterpret_cast<const uint8_t*>(ex->fault_address());
  uint32_t address = 0xFFFFFFFF;
  if (host_address >= memory->virtual_membase() &&
      host_address < memory->virtual_membase() + 0x100000000ull) {
    address = uint32_t(host_address - memory->virtual_membase());
  }
  pending_guest_fault.host_context = *ex->thread_context();
  pending_guest_fault.pc = pc;
  pending_guest_fault.address = address;
  pending_guest_fault.is_write = ex->access_violation_operation() ==
                                 Exception::AccessViolationOperation::kWrite;
  DivertToThunk(ex, &GuestFaultDispatchThunk);
  return true;
#endif  // XE_PLATFORM_WIN32
}

void RtlRaiseException_entry(pointer_t<X_EXCEPTION_RECORD> record,
                             const ppc_context_t& context) {
  switch (record->code) {
    case 0x406D1388: {
      HandleSetThreadName(record);
      return;
    }
    case 0xE06D7363: {
      HandleCppException(record, context);
      return;
    }
  }

  // TODO(benvanik): unwinding.
  // This is going to suck.
  // xe::debugging::Break();

  // RtlRaiseException definitely wasn't a noreturn function, we can return
  // safe-ish
  XELOGE("Guest attempted to trigger a breakpoint!");
}
DECLARE_XBOXKRNL_EXPORT1(RtlRaiseException, kDebug, kStub);

void KeBugCheckEx_entry(dword_t code, dword_t param1, dword_t param2,
                        dword_t param3, dword_t param4) {
  auto msg =
      fmt::format("*** STOP: 0x{:08X} (0x{:08X}, 0x{:08X}, 0x{:08X}, 0x{:08X})",
                  static_cast<uint32_t>(code), static_cast<uint32_t>(param1),
                  static_cast<uint32_t>(param2), static_cast<uint32_t>(param3),
                  static_cast<uint32_t>(param4));
  XELOGE("{}", msg);
  fflush(stdout);

  if (xe::debugging::IsDebuggerAttached()) {
    xe::debugging::Break();
  }

  // Show crash dialog and suspend the guest thread instead of killing the
  // host process.
  auto current_thread = kernel::XThread::GetCurrentThread();
  const auto* emulator = kernel_state()->emulator();
  auto* display_window = emulator->display_window();
  auto* imgui_drawer = emulator->imgui_drawer();
  if (display_window && imgui_drawer) {
    auto dlg_msg = fmt::format(
        "The guest kernel has crashed (KeBugCheck).\n\n{}\n\n"
        "The faulting thread has been suspended.",
        msg);
    display_window->app_context().CallInUIThreadSynchronous(
        [imgui_drawer, &dlg_msg]() {
          xe::ui::ImGuiDialog::ShowMessageBox(imgui_drawer,
                                              "Guest Kernel Crash", dlg_msg);
        });
  }

  if (current_thread) {
    current_thread->Suspend(nullptr);
  }
}
DECLARE_XBOXKRNL_EXPORT2(KeBugCheckEx, kDebug, kStub, kImportant);

void KeBugCheck_entry(dword_t code) { KeBugCheckEx_entry(code, 0, 0, 0, 0); }
DECLARE_XBOXKRNL_EXPORT2(KeBugCheck, kDebug, kImplemented, kImportant);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Debug);

// Guest threads: a Ctx per host thread, with the Xbox per-thread state
// (KPCR at fs, KTHREAD, TLS) and a guest stack.
//
// Xbox TLS (from XAPI's code): TlsDataSize = ((raw_end - raw_start + 15 +
// zero_fill) & ~15) + 4, the TLS block sits right below NtTib.StackBase
// (KTHREAD.TlsData = StackBase - TlsDataSize) and XAPI's TLS index is
// negative relative to fs:[4]. XAPI's thread startup fills the block from
// the template itself.

#include <pthread.h>

#include <cstdio>
#include <mutex>

#include "runtime/platform.h"
#include "runtime/rotk_internal.h"

// Kernel HLE: lays out stack/KPCR/KTHREAD/TLS exactly as its
// PsCreateSystemThreadEx does (kernel_exports.h). Optional.
void rotk_kernel_thread_setup(recomp::Ctx& c, uint32_t stack_size, uint32_t tls_size)
    __attribute__((weak));

namespace rotkrt {

namespace {
thread_local Thread* t_cur = nullptr;
std::mutex g_mu;
uint32_t g_stack_next = rotk_layout::kStackBase;
uint32_t g_next_id = 1;

constexpr uint32_t kKpcrSize = 0x300;
}  // namespace

uint32_t TlsDataSize() {
  if (!g_xbe.tls_va) return 0;
  uint32_t raw = g_xbe.tls_raw_end - g_xbe.tls_raw_start;
  return ((raw + 15 + g_xbe.tls_zero_fill) & ~15u) + 4;
}

namespace {
void InitCpu(RotkCtx& c) {
  recomp::x87::Init(c.fpu);
  recomp::x87::SetControlWord(c.fpu, 0x027F);  // 53-bit precision, as the kernel sets
  c.mxcsr = 0x1F80;
  c.eflags = 0x202;
}

void FillInfo(Thread* t) {
  RotkThreadInfo& i = t->info;
  i.kpcr = t->ctx.fs_base;
  i.kthread = rotk_rd32(i.kpcr + 0x28);
  i.tls_data = i.kthread ? rotk_rd32(i.kthread + 0x28) : 0;
  i.stack_hi = rotk_rd32(i.kpcr + 4);
  i.stack_lo = rotk_rd32(i.kpcr + 8);
}
}  // namespace

Thread* NewThread(uint32_t stack_size) {
  auto* t = new Thread;
  InitCpu(t->ctx);
  if (!stack_size) stack_size = g_xbe.stack_commit;
  if (stack_size < 0x10000) stack_size = 0x10000;
  stack_size = (stack_size + 0xFFFF) & ~0xFFFFu;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    t->info.id = g_next_id++;
    if (g_stack_next + uint64_t(stack_size) >
        uint64_t(rotk_layout::kStackBase) + rotk_layout::kStackSize)
      rotk_fatal("guest stack region full");
    t->info.stack_lo = g_stack_next;
    g_stack_next += stack_size + 0x10000;  // and an unmapped guard
  }
  t->info.stack_hi = t->info.stack_lo + stack_size;
  if (!rotk_commit(t->info.stack_lo, stack_size)) rotk_fatal("stack commit failed");

  const uint32_t kpcr = rotk_hle_alloc(kKpcrSize + rotk_kthread_size, 16);
  const uint32_t kthread = kpcr + kKpcrSize;
  const uint32_t tls = t->info.stack_hi - TlsDataSize();
  rotk_wr32(kpcr + 0x00, 0xFFFFFFFF);          // NtTib.ExceptionList: end
  rotk_wr32(kpcr + 0x04, t->info.stack_hi);    // NtTib.StackBase
  rotk_wr32(kpcr + 0x08, t->info.stack_lo);    // NtTib.StackLimit
  rotk_wr32(kpcr + 0x18, kpcr);                // NtTib.Self
  rotk_wr32(kpcr + 0x1C, kpcr);                // SelfPcr
  rotk_wr32(kpcr + 0x20, kpcr + 0x28);         // Prcb
  rotk_wr32(kpcr + 0x28, kthread);             // Prcb.CurrentThread
  rotk_wr32(kthread + 0x28, tls);              // KTHREAD.TlsData
  t->ctx.fs_base = kpcr;
  t->ctx.esp = tls & ~0xFu;
  t->ctx.ebp = 0;
  FillInfo(t);
  return t;
}

// The main thread, laid out by the kernel HLE when it is linked.
Thread* NewMainThread() {
  if (!rotk_kernel_thread_setup) return NewThread(0);
  auto* t = new Thread;
  InitCpu(t->ctx);
  t->info.id = g_next_id++;
  uint32_t stack = g_xbe.stack_commit < 0x10000 ? 0x10000 : g_xbe.stack_commit;
  rotk_kernel_thread_setup(t->ctx, stack, TlsDataSize());
  FillInfo(t);
  return t;
}

void BindThread(Thread* t) { t_cur = t; }

void RunOnBigStack(void (*body)(void*), void* arg, bool wait) {
  pthread_attr_t a;
  pthread_attr_init(&a);
  // Lifted calls nest like the guest's, each with a host frame: be generous
  // (address space only; pages are touched as used).
  pthread_attr_setstacksize(&a, size_t(256) << 20);
  pthread_t th;
  struct Box {
    void (*body)(void*);
    void* arg;
  };
  auto* box = new Box{body, arg};
  int rc = pthread_create(
      &th, &a,
      [](void* p) -> void* {
        auto* b = static_cast<Box*>(p);
        // The x87 and SSE host fast paths need the default FP environment
        // (round to nearest, no flush to zero, no traps) on every guest thread.
        if (const char* why = recomp::platform::HostFpProblem())
          rotk_fatal("host floating-point environment: %s", why);
        b->body(b->arg);
        delete b;
        return nullptr;
      },
      box);
  pthread_attr_destroy(&a);
  if (rc) rotk_fatal("pthread_create failed (%d)", rc);
  if (wait)
    pthread_join(th, nullptr);
  else
    pthread_detach(th);
}

}  // namespace rotkrt

using namespace rotkrt;

RotkCtx* rotk_current_ctx() { return t_cur ? &t_cur->ctx : nullptr; }
const RotkThreadInfo* rotk_current_thread() { return t_cur ? &t_cur->info : nullptr; }

uint32_t rotk_spawn_thread(uint32_t stack_size, RotkThreadBody body, void* user) {
  Thread* t = NewThread(stack_size);
  struct Start {
    Thread* t;
    RotkThreadBody body;
    void* user;
  };
  auto* s = new Start{t, body, user};
  const uint32_t id = t->info.id;
  RunOnBigStack(
      [](void* p) {
        auto* s = static_cast<Start*>(p);
        BindThread(s->t);
        char name[32];
        std::snprintf(name, sizeof name, "guest-%u", s->t->info.id);
        pthread_setname_np(name);
        s->body(s->t->ctx, s->user);
        delete s;
      },
      s, false);
  return id;
}

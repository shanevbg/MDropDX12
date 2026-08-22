#include "pv_workers.h"

#include <cctype>
#include <cstring>

namespace mdrop {

// ─────────────────────────────────────────────────────────────────────────────
// Parallel-safety analysis
// ─────────────────────────────────────────────────────────────────────────────

namespace {

inline bool IsIdentChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_';
}

// At `p`, skip spaces/tabs and report whether what follows is an assignment:
// '=' not part of '==', '<=', '>=' or '!=', or a compound form (+=, -=, *=,
// /=, %=, |=, &=, ^=). `prevCh` is the character immediately before the run of
// whitespace, needed to rule out '<=' and friends when the operator is split.
bool LooksLikeAssignment(const char* p, const char* end) {
  while (p < end && (*p == ' ' || *p == '\t')) p++;
  if (p >= end) return false;

  // compound assignment: one operator char then '='
  if (p + 1 < end && (*p == '+' || *p == '-' || *p == '*' || *p == '/' ||
                      *p == '%' || *p == '|' || *p == '&' || *p == '^')) {
    return p[1] == '=' && (p + 2 >= end || p[2] != '=');
  }
  if (*p != '=') return false;
  // '==' is a comparison, not a store
  if (p + 1 < end && p[1] == '=') return false;
  return true;
}

// Find the matching ']' for the '[' at `p`. Returns end on failure.
const char* SkipBracket(const char* p, const char* end) {
  int depth = 0;
  for (; p < end; p++) {
    if (*p == '[') depth++;
    else if (*p == ']') { depth--; if (depth == 0) return p + 1; }
  }
  return end;
}

// Find the matching ')' for the '(' at `p`. Returns end on failure.
const char* SkipParen(const char* p, const char* end) {
  int depth = 0;
  for (; p < end; p++) {
    if (*p == '(') depth++;
    else if (*p == ')') { depth--; if (depth == 0) return p + 1; }
  }
  return end;
}

} // namespace

bool IsPerPixelParallelSafe(const char* src) {
  if (!src) return true;
  const char* const begin = src;
  const char* const end   = src + strlen(src);

  for (const char* p = begin; p < end; ) {
    if (!IsIdentChar(*p) || (p > begin && IsIdentChar(p[-1]))) { p++; continue; }

    const char* idStart = p;
    while (p < end && IsIdentChar(*p)) p++;
    const size_t len = (size_t)(p - idStart);

    // reg00 .. reg99 — process-global registers, shared by every VM.
    if (len == 5 && _strnicmp(idStart, "reg", 3) == 0 &&
        isdigit((unsigned char)idStart[3]) && isdigit((unsigned char)idStart[4])) {
      if (LooksLikeAssignment(p, end)) return false;
      continue;
    }

    // megabuf / gmegabuf — per-VM RAM that persists across vertices, so a store
    // makes the result depend on evaluation order. Both the indexed form
    // (megabuf[i] = ...) and the call form (megabuf(i) = ...) are stores.
    const bool isMega  = (len == 7 && _strnicmp(idStart, "megabuf",  7) == 0);
    const bool isGMega = (len == 8 && _strnicmp(idStart, "gmegabuf", 8) == 0);
    if (isMega || isGMega) {
      const char* q = p;
      while (q < end && (*q == ' ' || *q == '\t')) q++;
      if (q < end && *q == '[')      q = SkipBracket(q, end);
      else if (q < end && *q == '(') q = SkipParen(q, end);
      if (LooksLikeAssignment(q, end)) return false;
      continue;
    }
  }
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Thread pool
// ─────────────────────────────────────────────────────────────────────────────

void PvThreadPool::Start(int workers) {
  Stop();
  if (workers < 2) { m_workers = 1; return; }

  m_stop    = false;
  m_gen     = 0;
  m_fn      = nullptr;
  m_pending.store(0, std::memory_order_relaxed);
  m_workers = workers;

  m_threads.reserve((size_t)workers - 1);
  for (int i = 1; i < workers; i++)
    m_threads.emplace_back([this, i] { WorkerLoop(i); });
}

void PvThreadPool::Stop() {
  if (m_threads.empty()) { m_workers = 1; return; }
  {
    std::lock_guard<std::mutex> lk(m_mu);
    m_stop = true;
    m_gen++;
  }
  m_cv.notify_all();
  for (auto& t : m_threads)
    if (t.joinable()) t.join();
  m_threads.clear();
  m_workers = 1;
}

void PvThreadPool::WorkerLoop(int index) {
  unsigned long long seen = 0;
  for (;;) {
    const std::function<void(int)>* fn = nullptr;
    {
      std::unique_lock<std::mutex> lk(m_mu);
      m_cv.wait(lk, [this, &seen] { return m_stop || m_gen != seen; });
      seen = m_gen;
      if (m_stop) return;
      fn = m_fn;
    }
    if (fn) {
      // A throw here would leave m_pending non-zero and hang Run() forever.
      try {
        (*fn)(index);
      } catch (...) {
        m_failed.store(true, std::memory_order_relaxed);
      }
    }
    m_pending.fetch_sub(1, std::memory_order_release);
  }
}

void PvThreadPool::Run(const std::function<void(int)>& fn) {
  if (m_workers < 2 || m_threads.empty()) {
    try { fn(0); } catch (...) { m_failed.store(true, std::memory_order_relaxed); }
    return;
  }

  {
    std::lock_guard<std::mutex> lk(m_mu);
    m_fn = &fn;
    m_pending.store(m_workers - 1, std::memory_order_relaxed);
    m_gen++;
  }
  m_cv.notify_all();

  // The caller is worker 0 — one fewer thread to wake, and one fewer to wait on.
  try { fn(0); } catch (...) { m_failed.store(true, std::memory_order_relaxed); }

  // Spin-then-yield rather than a second condition variable: the workers are
  // doing a few hundred microseconds of work that started at the same moment,
  // so the wait is short, and yielding cannot deadlock the way a missed
  // notification can.
  while (m_pending.load(std::memory_order_acquire) != 0)
    std::this_thread::yield();

  {
    std::lock_guard<std::mutex> lk(m_mu);
    m_fn = nullptr;
  }
}

// ─────────────────────────────────────────────────────────────────────────────

int PvChooseWorkerCount() {
  unsigned hw = std::thread::hardware_concurrency();
  if (hw == 0) hw = 2;

  // Leave a core for the audio capture thread and the message pump; past 8 the
  // per-worker slice is small enough that the barrier starts to dominate.
  int n = (int)hw - 1;
  if (n > 8) n = 8;
  if (n < 1) n = 1;
  return n;
}

bool PvParallelWorthIt(int vertexCount) {
  // 64x48 = 3,185 vertices, the default mesh, clears this comfortably.
  return vertexCount >= 2048;
}

} // namespace mdrop

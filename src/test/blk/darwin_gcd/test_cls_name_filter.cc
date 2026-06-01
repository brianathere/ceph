// Regression test for the cls-plugin filename -> class-name derivation in
// ClassHandler::open_all_classes() (src/osd/ClassHandler.cc), including the
// macOS versioned-dylib skip (the fix for the OSD-startup SIGSEGV where
// libcls_journal.1.dylib was loaded as class "journal.1", register_class
// returned NULL, and the plugin dereferenced the null handle).
//
// open_all_classes() is not unit-testable in isolation (it needs a live
// ClassHandler + CephContext + a populated osd_class_dir), so this test
// REPLICATES the exact derivation/skip algorithm verbatim and asserts the
// classification for representative directory entries on both the macOS
// (".dylib") and Linux (".so") suffix conventions. If the production
// algorithm changes, this test must be updated in lockstep — it is a guard
// against regressing the specific name-stripping bug, and documents the
// intended behavior on both platforms.
//
// Build/run via run.sh (no Ceph deps).
#include <cstdio>
#include <cstring>
#include <string>

// Mirrors ClassHandler.cc: CLS_PREFIX + CLS_SUFFIX (= SHARED_LIB_SUFFIX, which
// is CMAKE_SHARED_LIBRARY_SUFFIX: ".dylib" on macOS, ".so" on Linux).
#define CLS_PREFIX "libcls_"

// Returns:
//   loaded class name   if `fname` is accepted and yields a class to load
//   "<skip>"            if rejected by the prefix/suffix/length test
//   "<skip-versioned>"  if accepted by suffix test but skipped as a versioned
//                       dylib (the APPLE-only guard; only meaningful when
//                       suffix == ".dylib")
// `apple` selects whether the versioned-dot skip is compiled in.
static std::string classify(const char* fname, const char* CLS_SUFFIX, bool apple) {
  size_t dn = std::strlen(fname);
  if (fname[0] == '.') return "<skip>";
  if (!(dn > sizeof(CLS_PREFIX) - 1 + std::strlen(CLS_SUFFIX) &&
        std::strncmp(fname, CLS_PREFIX, sizeof(CLS_PREFIX) - 1) == 0 &&
        std::strcmp(fname + dn - std::strlen(CLS_SUFFIX), CLS_SUFFIX) == 0))
    return "<skip>";
  char cname[1024];
  std::strncpy(cname, fname + sizeof(CLS_PREFIX) - 1, sizeof(cname) - 1);
  cname[sizeof(cname) - 1] = '\0';
  cname[std::strlen(cname) - std::strlen(CLS_SUFFIX)] = '\0';
  if (apple && std::strchr(cname, '.')) return "<skip-versioned>";
  return std::string(cname);
}

static int g_fail = 0;
static void expect(const char* fname, const char* suffix, bool apple,
                   const char* want, const char* note) {
  std::string got = classify(fname, suffix, apple);
  bool ok = (got == want);
  if (!ok) {
    std::fprintf(stderr, "FAIL: %-34s (%s,%s) got '%s' want '%s'  [%s]\n",
                 fname, suffix, apple ? "apple" : "linux", got.c_str(), want, note);
    g_fail++;
  } else {
    std::printf("  ok: %-34s -> %-16s [%s]\n", fname, got.c_str(), note);
  }
}

int main() {
  std::printf("== macOS (.dylib) ==\n");
  // Canonical unversioned plugin loads under its real name.
  expect("libcls_journal.dylib",        ".dylib", true,  "journal",        "canonical");
  // Versioned copies (the bug): accepted by suffix test, must be skipped.
  expect("libcls_journal.1.dylib",      ".dylib", true,  "<skip-versioned>", "major-versioned");
  expect("libcls_journal.1.0.0.dylib",  ".dylib", true,  "<skip-versioned>", "full-versioned");
  // Real class names with underscores / digits must NOT be mistaken for versions.
  expect("libcls_2pc_queue.dylib",      ".dylib", true,  "2pc_queue",      "digit+underscore");
  expect("libcls_rgw_gc.dylib",         ".dylib", true,  "rgw_gc",         "underscore");
  expect("libcls_user.dylib",           ".dylib", true,  "user",           "short");
  // Non-cls / wrong suffix are rejected outright.
  expect("libceph-common.dylib",        ".dylib", true,  "<skip>",         "not-a-cls");
  expect("libcls_journal.a",            ".dylib", true,  "<skip>",         "static-archive");
  expect(".",                           ".dylib", true,  "<skip>",         "dot-entry");
  expect("libcls_.dylib",               ".dylib", true,  "<skip>",         "empty-name-too-short");

  std::printf("== Linux (.so) ==\n");
  // On Linux the versioned libs are libcls_x.so.N (suffix .so.N != .so) so the
  // suffix test already rejects them; the dot-skip is not even compiled in.
  expect("libcls_journal.so",           ".so",    false, "journal",        "canonical");
  expect("libcls_journal.so.1",         ".so",    false, "<skip>",         "linux-versioned-rejected-by-suffix");
  expect("libcls_journal.so.1.0.0",     ".so",    false, "<skip>",         "linux-fullversioned-rejected");
  expect("libcls_2pc_queue.so",         ".so",    false, "2pc_queue",      "digit+underscore");
  expect("libcls_rgw_gc.so",            ".so",    false, "rgw_gc",         "underscore");

  std::printf(g_fail == 0 ? "\nCLS NAME-FILTER TESTS PASSED\n"
                          : "\n%d CLS NAME-FILTER CHECK(S) FAILED\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}

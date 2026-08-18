// SPDX-License-Identifier: MIT
// cpp-mount-watch — Linux filesystem mount & usage monitor (/proc/mounts + statvfs)
// Build: g++ -std=c++23 -O2 -Wall -Wextra src/main.cpp -o mount-watch
// Usage: ./mount-watch [--watch N] [--json] [--demo] [--no-color] [--all] [--timeout T] [--selftest]

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <sys/statvfs.h>

// ── Mutable ANSI palette (--no-color empties every code) ──
static std::string RST = "\x1b[0m";
static std::string BOLD = "\x1b[1m";
static std::string DIM = "\x1b[2m";
static std::string CYAN = "\x1b[36m";
static std::string GREEN = "\x1b[32m";
static std::string YELLOW = "\x1b[33m";
static std::string RED = "\x1b[31m";
static std::string MAGENTA = "\x1b[35m";

struct Mount {
  std::string device;
  std::string mountpoint;
  std::string fstype;
  std::string options;
  bool ro = false;
  bool excluded = false;
  bool ok = false;  // statvfs succeeded
  unsigned long long total = 0, used = 0, avail = 0;
  double use_pct = 0.0;
  long long delta = 0;  // avail delta vs previous snapshot (watch mode)
};

// Pseudo / synthetic filesystems skipped unless --all
static const std::vector<std::string> kSkipTypes = {
    "proc", "sysfs", "devpts", "devtmpfs", "cgroup", "cgroup2", "pstore",
    "securityfs", "debugfs", "tracefs", "fusectl", "configfs", "bpf",
    "autofs", "mqueue", "efivarfs", "rpc_pipefs", "binfmt_misc",
    "hugetlbfs", "nsfs", "selinuxfs", "sockfs", "pipefs", "ramfs",
    "overlay", "squashfs", "cramfs"};

static bool g_color = true;
static bool g_all = false;

static void set_palette(bool on) {
  g_color = on;
  if (on) {
    RST = "\x1b[0m"; BOLD = "\x1b[1m"; DIM = "\x1b[2m"; CYAN = "\x1b[36m";
    GREEN = "\x1b[32m"; YELLOW = "\x1b[33m"; RED = "\x1b[31m"; MAGENTA = "\x1b[35m";
  } else {
    RST = BOLD = DIM = CYAN = GREEN = YELLOW = RED = MAGENTA = "";
  }
}

// ── Helpers ──

static std::string unescape_mount(std::string_view s) {
  // /proc/mounts escapes space \040, tab \011, newline \012, backslash \134
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 3 < s.size() &&
        s[i + 1] >= '0' && s[i + 1] <= '3' &&
        s[i + 2] >= '0' && s[i + 2] <= '7' &&
        s[i + 3] >= '0' && s[i + 3] <= '7') {
      int v = (s[i + 1] - '0') * 64 + (s[i + 2] - '0') * 8 + (s[i + 3] - '0');
      out.push_back(static_cast<char>(v));
      i += 3;
    } else {
      out.push_back(s[i]);
    }
  }
  return out;
}

static bool fstype_skipped(const std::string& t) {
  return std::find(kSkipTypes.begin(), kSkipTypes.end(), t) != kSkipTypes.end();
}

[[nodiscard]] static std::string human_bytes(double v) {
  const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
  int u = 0;
  while (v >= 1024.0 && u < 5) { v /= 1024.0; ++u; }
  char buf[32];
  if (u == 0) std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(v));
  else std::snprintf(buf, sizeof(buf), "%.2f %s", v, units[u]);
  return buf;
}

[[nodiscard]] static std::string fmt_timestamp() {
  std::time_t now = std::time(nullptr);
  std::tm tmv{};
  localtime_r(&now, &tmv);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
  return buf;
}

// ── Parsing /proc/mounts ──

static std::vector<Mount> read_mounts() {
  std::vector<Mount> out;
  std::ifstream f("/proc/mounts");
  std::string line;
  while (std::getline(f, line)) {
    std::istringstream ss(line);
    Mount m;
    std::string dev, mp, ft, opts, dump_s, pass_s;
    if (!(ss >> dev >> mp >> ft >> opts >> dump_s >> pass_s)) continue;
    m.device = unescape_mount(dev);
    m.mountpoint = unescape_mount(mp);
    m.fstype = ft;
    m.options = opts;
    m.ro = opts.find("ro") != std::string::npos;
    if (fstype_skipped(m.fstype) && !g_all) m.excluded = true;
    out.push_back(std::move(m));
  }
  return out;
}

static void statvfs_mount(Mount& m) {
  struct statvfs st;
  if (::statvfs(m.mountpoint.c_str(), &st) != 0) {
    m.ok = false;
    return;
  }
  unsigned long long frsize = st.f_frsize ? st.f_frsize : 512;
  m.total = st.f_blocks * frsize;
  m.used = (st.f_blocks - st.f_bfree) * frsize;
  m.avail = st.f_bavail * frsize;
  m.use_pct = m.total > 0 ? 100.0 * static_cast<double>(m.used) / static_cast<double>(m.total) : 0.0;
  m.ok = true;
}

// ── Snapshot ──

struct Snapshot {
  std::vector<Mount> mounts;
  unsigned long long total_size = 0, total_used = 0, total_avail = 0;
  size_t count = 0;
};

static Snapshot take_snapshot() {
  Snapshot snap;
  snap.mounts = read_mounts();
  for (auto& m : snap.mounts) {
    if (m.excluded) continue;
    statvfs_mount(m);
    if (m.ok) {
      snap.total_size += m.total;
      snap.total_used += m.used;
      snap.total_avail += m.avail;
      ++snap.count;
    }
  }
  std::sort(snap.mounts.begin(), snap.mounts.end(),
            [](const Mount& a, const Mount& b) {
              if (a.excluded != b.excluded) return a.excluded < b.excluded;
              return a.mountpoint < b.mountpoint;
            });
  return snap;
}

// ── Output ──

[[nodiscard]] static std::string pct_color(double pct) {
  if (!g_color) return "";
  if (pct >= 90.0) return RED;
  if (pct >= 75.0) return YELLOW;
  return GREEN;
}

static std::string fmt_mount_line(const Mount& m, bool with_delta) {
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%5.1f%%", m.use_pct);
  std::string dev = (m.device == "/dev/root") ? "rootfs" : m.device;
  std::string delta_s;
  if (with_delta && m.delta != 0) {
    delta_s = std::format("({}{})", m.delta > 0 ? "+" : "", std::to_string(m.delta));
  }
  std::string ro_s = m.ro ? "RO" : "";
  if (!g_color) {
    return std::format("  {:<16} {:<22} {:<9} {:>10} {:>10} {:>10} {:>6} {}{}\n",
                       dev, m.mountpoint, m.fstype,
                       human_bytes(m.total), human_bytes(m.used), human_bytes(m.avail),
                       buf, delta_s, ro_s);
  }
  std::string color = pct_color(m.use_pct);
  return std::format("  {0:<16} {1:<22} {2:<9} {3} {4:>10} {5} {6:>10} {7} {8:>10} {9}{10}{11} {12}{13}\n",
                     dev, m.mountpoint, m.fstype,
                     DIM, human_bytes(m.total),
                     CYAN, human_bytes(m.used),
                     DIM, human_bytes(m.avail),
                     color, buf, RST,
                     delta_s.empty() ? "" : CYAN + delta_s,
                     ro_s.empty() ? "" : YELLOW + ro_s + RST);
}

static std::string sep_line(int n = 82) {
  return std::string(n, '-');
}

static void print_snapshot(const Snapshot& snap, bool with_delta) {
  std::cout << "\n  " << BOLD << "mount-watch" << RST
            << " — filesystems " << DIM << "(/proc/mounts + statvfs)" << RST
            << "  snapshot " << fmt_timestamp() << "\n";
  std::cout << std::format("  {:<16} {:<22} {:<9} {:>10} {:>10} {:>10} {:>6} {}\n",
                           "device", "mountpoint", "type", "size", "used", "avail", "use%", "");
  std::cout << "  " << sep_line() << "\n";
  for (const auto& m : snap.mounts) {
    if (m.excluded) {
      std::cout << DIM << "  " << std::format("{:<16} {:<22} {:<9} (synthetic — hidden, use --all)",
                                              m.device, m.mountpoint, m.fstype)
                << RST << "\n";
      continue;
    }
    std::cout << fmt_mount_line(m, with_delta);
  }
  std::cout << "  " << sep_line() << "\n";
  if (g_color) {
    std::cout << std::format("  {0}TOTAL{1} {2}  {3}used{4} {5}  {6}avail{7} {8}  on {9} filesystems\n",
                             BOLD, RST, human_bytes(snap.total_size),
                             RED, human_bytes(snap.total_used),
                             GREEN, human_bytes(snap.total_avail), RST, "", snap.count);
  } else {
    std::cout << std::format("  TOTAL {}  used {}  avail {}  on {} filesystems\n",
                             human_bytes(snap.total_size), human_bytes(snap.total_used),
                             human_bytes(snap.total_avail), snap.count);
  }
}

// ── JSON ──

static std::string mount_json(const Mount& m) {
  if (!m.ok) {
    return std::format("{{\"device\":\"{}\",\"mountpoint\":\"{}\",\"fstype\":\"{}\",\"ok\":false}}",
                       m.device, m.mountpoint, m.fstype);
  }
  return std::format("{{\"device\":\"{}\",\"mountpoint\":\"{}\",\"fstype\":\"{}\",\"ro\":{},\"ok\":true,\"total\":{},\"used\":{},\"avail\":{},\"use_pct\":{:.1f}}}",
                     m.device, m.mountpoint, m.fstype, m.ro ? "true" : "false",
                     m.total, m.used, m.avail, m.use_pct);
}

static void print_json(const Snapshot& snap) {
  std::cout << "{\n";
  std::cout << std::format("  \"ts\": {},\n", std::time(nullptr));
  std::cout << std::format("  \"filesystems\": {},\n", snap.count);
  std::cout << std::format("  \"total_bytes\": {},\n", snap.total_size);
  std::cout << std::format("  \"used_bytes\": {},\n", snap.total_used);
  std::cout << std::format("  \"avail_bytes\": {},\n", snap.total_avail);
  std::cout << "  \"mounts\": [\n";
  bool first = true;
  for (const auto& m : snap.mounts) {
    if (m.excluded) continue;
    std::cout << (first ? "" : ",\n") << "    " << mount_json(m);
    first = false;
  }
  std::cout << "\n  ]\n}\n";
}

// ── Demo ──

static Snapshot demo_snapshot() {
  Snapshot snap;
  auto add = [&](std::string dev, std::string mp, std::string ft,
                 unsigned long long total_gib, double use_pct, bool ro = false) {
    Mount m;
    m.device = std::move(dev);
    m.mountpoint = std::move(mp);
    m.fstype = std::move(ft);
    m.ro = ro;
    m.ok = true;
    m.total = total_gib * 1024ull * 1024ull * 1024ull;
    m.used = static_cast<unsigned long long>(static_cast<double>(m.total) * use_pct / 100.0);
    m.avail = m.total - m.used;
    m.use_pct = use_pct;
    snap.mounts.push_back(std::move(m));
    snap.total_size += m.total;
    snap.total_used += m.used;
    snap.total_avail += m.avail;
    ++snap.count;
  };
  add("/dev/nvme0n1p2", "/", "ext4", 476, 42.5);
  add("/dev/nvme0n1p3", "/home", "ext4", 930, 67.3);
  add("/dev/sdb1", "/data", "xfs", 3725, 81.8);
  add("/dev/nvme0n1p1", "/boot/efi", "vfat", 1, 3.2, true);
  add("tmpfs", "/dev/shm", "tmpfs", 8, 12.4);
  add("/dev/vg-var", "/var", "ext4", 98, 44.9);
  { Mount m; m.device = "proc"; m.mountpoint = "/proc"; m.fstype = "proc";
    m.excluded = true; snap.mounts.push_back(std::move(m)); }
  { Mount m; m.device = "overlay"; m.mountpoint = "/app"; m.fstype = "overlay";
    m.excluded = true; snap.mounts.push_back(std::move(m)); }
  std::sort(snap.mounts.begin(), snap.mounts.end(),
            [](const Mount& a, const Mount& b) { return a.excluded < b.excluded; });
  return snap;
}

// ── Selftest ──

struct TestResult { int passed = 0; int failed = 0; };

static void check(TestResult& r, bool cond, const std::string& name) {
  if (cond) { ++r.passed; std::cout << "  " << GREEN << "PASS" << RST << "  " << name << "\n"; }
  else { ++r.failed; std::cout << "  " << RED << "FAIL" << RST << "  " << name << "\n"; }
}

static int run_selftest() {
  TestResult r;
  std::cout << "  mount-watch selftest\n";

  // 1. unescape octal
  check(r, unescape_mount("/dev/sda1\\040x") == "/dev/sda1 x", "unescape: \\040 -> space");
  check(r, unescape_mount("a\\134b") == "a\\b", "unescape: \\134 -> backslash");
  check(r, unescape_mount("plain") == "plain", "unescape: string unchanged");

  // 2. ro detection
  {
    Mount m;
    m.options = "rw,relatime";
    m.ro = m.options.find("ro") != std::string::npos;
    check(r, m.ro == false, "parse: ro=false for rw,relatime");
    m.options = "ro,relatime";
    m.ro = m.options.find("ro") != std::string::npos;
    check(r, m.ro == true, "parse: ro=true for ro,relatime");
  }

  // 3. filter
  {
    std::vector<std::string> types{"proc", "sysfs", "devpts", "ext4", "xfs", "tmpfs", "overlay"};
    size_t kept = 0;
    for (auto& t : types) if (!fstype_skipped(t)) ++kept;
    check(r, kept == 3, "filter: ext4/xfs/tmpfs kept, pseudo+overlay skipped");
    check(r, fstype_skipped("cgroup2"), "filter: cgroup2 skipped");
    check(r, !fstype_skipped("btrfs"), "filter: btrfs kept");
  }

  // 4. real statvfs on /
  {
    struct statvfs st;
    check(r, ::statvfs("/", &st) == 0 && st.f_blocks > 0, "statvfs: / readable with blocks > 0");
  }

  // 5. human units
  check(r, human_bytes(0) == "0 B", "human: 0 B");
  check(r, human_bytes(1536) == "1.50 KiB", "human: 1536 B -> 1.50 KiB");
  check(r, human_bytes(1073741824) == "1.00 GiB", "human: 1 GiB exact");

  // 6. use_pct math
  {
    Mount m; m.total = 1000; m.used = 250;
    m.use_pct = m.total > 0 ? 100.0 * m.used / m.total : 0.0;
    check(r, std::abs(m.use_pct - 25.0) < 0.001, "pct: 250/1000 -> 25%");
  }

  // 7. delta arithmetic
  {
    Mount a, b;
    a.avail = 1000; b.avail = 850;
    long long d = static_cast<long long>(b.avail) - static_cast<long long>(a.avail);
    check(r, d == -150, "delta: 850-1000 = -150");
    check(r, std::to_string(d) == "-150", "delta: signed string");
  }

  // 8. JSON quoting — keys wrapped in double quotes
  {
    Mount m; m.device = "/dev/sda1"; m.mountpoint = "/mnt"; m.fstype = "ext4";
    m.ok = true; m.ro = false; m.total = 100; m.used = 40; m.avail = 60; m.use_pct = 40.0;
    std::string j = mount_json(m);
    check(r, j.find("\"device\":") != std::string::npos, "json: device key quoted");
    check(r, j.find("\"mountpoint\":") != std::string::npos, "json: mountpoint key quoted");
    check(r, j.find("\"use_pct\":") != std::string::npos, "json: use_pct key quoted");
    check(r, j.find("\"ok\":") != std::string::npos, "json: ok key quoted");
  }

  // 9. real /proc/mounts read
  {
    Snapshot snap = take_snapshot();
    check(r, snap.mounts.size() >= 1, "real: /proc/mounts has >= 1 mount");
    check(r, snap.count >= 1, "real: >= 1 non-excluded filesystem");
    check(r, snap.total_size > 0 && snap.total_used > 0, "real: aggregate totals > 0");
  }

  // 10. JSON output well-formed (balanced braces)
  {
    Snapshot snap = demo_snapshot();
    std::ostringstream os;
    auto old = std::cout.rdbuf(os.rdbuf());
    print_json(snap);
    std::cout.rdbuf(old);
    std::string j = os.str();
    int open = 0, close = 0;
    for (char c : j) { if (c == '{') ++open; if (c == '}') ++close; }
    check(r, open == close && open >= 2, "json: braces balanced in demo snapshot");
    check(r, j.find("\"mounts\":") != std::string::npos, "json: mounts array present");
  }

  // 11. demo snapshot consistency
  {
    Snapshot snap = demo_snapshot();
    unsigned long long sum = 0, sumu = 0;
    size_t n = 0;
    for (const auto& m : snap.mounts) {
      if (m.excluded) continue;
      sum += m.total; sumu += m.used; ++n;
    }
    check(r, n == snap.count, "demo: count matches");
    check(r, sum == snap.total_size && sumu == snap.total_used, "demo: totals consistent");
  }

  // 12. palette toggling (--no-color empties every code)
  {
    set_palette(false);
    bool empty = RST.empty() && BOLD.empty() && DIM.empty() && CYAN.empty() &&
                 GREEN.empty() && YELLOW.empty() && RED.empty() && MAGENTA.empty();
    check(r, empty, "no-color: all palette entries empty after set_palette(false)");
    set_palette(true);
    bool restored = !RST.empty() && !CYAN.empty() && !RED.empty();
    check(r, restored, "color: palette restored after set_palette(true)");
  }

  // 13. table rendering
  {
    Mount m; m.device = "/dev/sda1"; m.mountpoint = "/mnt/data"; m.fstype = "ext4";
    m.ok = true; m.total = 100; m.used = 50; m.avail = 50; m.use_pct = 50.0;
    std::string line = fmt_mount_line(m, false);
    check(r, line.find("/mnt/data") != std::string::npos, "table: mountpoint rendered");
  }

  // 14. watch delta formatting
  {
    Mount m; m.delta = -2048; m.use_pct = 50.0; m.total = 100; m.used = 50; m.avail = 50; m.ok = true;
    std::string line = fmt_mount_line(m, true);
    check(r, line.find("(-2048)") != std::string::npos, "table: negative delta rendered");
  }

  std::cout << "  " << BOLD << r.passed << " passed, " << r.failed << " failed" << RST << "\n";
  return r.failed == 0 ? 0 : 1;
}

// ── Main ──

static void usage() {
  std::cout <<
    "Usage: mount-watch [options]\n"
    "  Linux filesystem mount & usage monitor (/proc/mounts + statvfs)\n\n"
    "Options:\n"
    "  --watch N     refresh every N seconds (Ctrl+C to stop)\n"
    "  --timeout T   auto-exit after T seconds in watch mode\n"
    "  --json        emit one JSON snapshot\n"
    "  --demo        use synthetic data (containers/VMs without real mounts)\n"
    "  --all         show pseudo/synthetic filesystems too\n"
    "  --no-color    disable ANSI colors\n"
    "  --selftest    run self-tests\n"
    "  -h, --help    this help\n";
}

int main(int argc, char** argv) {
  int watch_secs = 0;
  int timeout_secs = 0;
  bool json = false, demo = false, selftest = false;

  for (int i = 1; i < argc; ++i) {
    std::string_view a{argv[i]};
    if (a == "--watch" || a == "-w") {
      if (i + 1 < argc) {
        ++i;
        auto [ptr, ec] = std::from_chars(argv[i], argv[i] + std::strlen(argv[i]), watch_secs);
        if (ec != std::errc()) { std::cerr << "mount-watch: invalid --watch value\n"; return 2; }
      }
    } else if (a == "--timeout" || a == "-t") {
      if (i + 1 < argc) {
        ++i;
        auto [ptr, ec] = std::from_chars(argv[i], argv[i] + std::strlen(argv[i]), timeout_secs);
        if (ec != std::errc()) { std::cerr << "mount-watch: invalid --timeout value\n"; return 2; }
      }
    } else if (a == "--json") {
      json = true;
    } else if (a == "--demo") {
      demo = true;
    } else if (a == "--all") {
      g_all = true;
    } else if (a == "--no-color" || a == "-c") {
      set_palette(false);
    } else if (a == "--selftest") {
      selftest = true;
    } else if (a == "--help" || a == "-h") {
      usage();
      return 0;
    } else {
      std::cerr << "mount-watch: unknown option '" << a << "'\n";
      usage();
      return 2;
    }
  }

  if (selftest) return run_selftest();

  if (demo) {
    Snapshot snap = demo_snapshot();
    if (json) { print_json(snap); return 0; }
    print_snapshot(snap, false);
    if (watch_secs > 0) {
      std::map<std::string, unsigned long long> prev;
      int elapsed = 0;
      bool first = true;
      while (timeout_secs == 0 || elapsed < timeout_secs) {
        if (!first) { std::this_thread::sleep_for(std::chrono::seconds(watch_secs)); elapsed += watch_secs; }
        first = false;
        Snapshot s2 = demo_snapshot();
        for (auto& m : s2.mounts) {
          if (m.excluded || !m.ok) continue;
          if (m.mountpoint == "/" || m.mountpoint == "/var") {
            unsigned long long drift = static_cast<unsigned long long>(elapsed) * 1024ull * 256ull;
            if (m.avail > drift) { m.avail -= drift; m.used += drift; }
            m.use_pct = m.total > 0 ? 100.0 * m.used / m.total : 0.0;
          }
          auto it = prev.find(m.mountpoint);
          if (it != prev.end()) m.delta = static_cast<long long>(m.avail) - static_cast<long long>(it->second);
          prev[m.mountpoint] = m.avail;
        }
        std::cout << "\x1b[2J\x1b[H";
        print_snapshot(s2, true);
        std::cout << std::flush;
      }
    }
    return 0;
  }

  if (json) {
    print_json(take_snapshot());
    return 0;
  }

  if (watch_secs > 0) {
    std::map<std::string, unsigned long long> prev;
    int elapsed = 0;
    bool first = true;
    while (timeout_secs == 0 || elapsed < timeout_secs) {
      if (!first) { std::this_thread::sleep_for(std::chrono::seconds(watch_secs)); elapsed += watch_secs; }
      first = false;
      Snapshot s2 = take_snapshot();
      for (auto& m : s2.mounts) {
        if (m.excluded || !m.ok) continue;
        auto it = prev.find(m.mountpoint);
        if (it != prev.end()) m.delta = static_cast<long long>(m.avail) - static_cast<long long>(it->second);
        prev[m.mountpoint] = m.avail;
      }
      std::cout << "\x1b[2J\x1b[H";
      print_snapshot(s2, true);
      std::cout << std::flush;
    }
    return 0;
  }

  print_snapshot(take_snapshot(), false);
  return 0;
}
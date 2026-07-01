#!/usr/bin/env python3
"""
dashboard_server.py — KB Analytics TLS Dashboard Server

Reads /proc/kb_stats from the kernel module and serves a live
HTTPS dashboard. Data is never written to disk — only held in memory.

Run:
    python3 dashboard_server.py
Then open:
    https://<pi-ip>:5000
"""

import ssl
import json
import time
import threading
import re
import os
from http.server import HTTPServer, BaseHTTPRequestHandler
from datetime import datetime

# ── Config ────────────────────────────────────────────────────────────────────
PROC_STATS   = "/proc/kb_stats"
CERT_FILE    = "certs/server.crt"
KEY_FILE     = "certs/server.key"
HOST         = "0.0.0.0"
PORT         = 5000
POLL_INTERVAL = 2  # seconds between /proc reads

# ── In-memory stats store (never touches disk) ────────────────────────────────
_stats_lock = threading.Lock()
_latest     = {
    "total_keypresses": 0,
    "typing_speed_kpm": 0,
    "avg_interval_us":  0,
    "hotkey_combos":    0,
    "top_keys":         [],   # [{"key": "A", "count": 42}, ...]
    "all_keys":         [],
    "last_updated":     None,
    "module_loaded":    False,
}

# ── Background poller: reads /proc/kb_stats every POLL_INTERVAL seconds ───────
def parse_proc_stats(raw: str) -> dict:
    data = {
        "total_keypresses": 0,
        "typing_speed_kpm": 0,
        "avg_interval_us":  0,
        "hotkey_combos":    0,
        "top_keys":         [],
        "all_keys":         [],
    }

    section = None
    for line in raw.splitlines():
        line = line.strip()

        if "Total key presses" in line:
            m = re.search(r":\s*(\d+)", line)
            if m: data["total_keypresses"] = int(m.group(1))

        elif "Typing speed (kpm)" in line:
            m = re.search(r":\s*(\d+)", line)
            if m: data["typing_speed_kpm"] = int(m.group(1))

        elif "Avg interval" in line:
            m = re.search(r":\s*(\d+)", line)
            if m: data["avg_interval_us"] = int(m.group(1))

        elif "Hotkey combos" in line:
            m = re.search(r":\s*(\d+)", line)
            if m: data["hotkey_combos"] = int(m.group(1))

        elif "Most-Used Keys" in line:
            section = "top"
        elif "All Key Press Counts" in line:
            section = "all"
        elif line.startswith("-"):
            pass  # separator
        elif section and ":" in line:
            parts = line.split(":")
            if len(parts) == 2:
                key   = parts[0].strip()
                count = parts[1].strip()
                try:
                    entry = {"key": key, "count": int(count)}
                    if section == "top":
                        data["top_keys"].append(entry)
                    else:
                        data["all_keys"].append(entry)
                except ValueError:
                    pass

    return data


def stats_poller():
    while True:
        try:
            if os.path.exists(PROC_STATS):
                with open(PROC_STATS, "r") as f:
                    raw = f.read()
                parsed = parse_proc_stats(raw)
                parsed["last_updated"] = datetime.now().strftime("%H:%M:%S")
                parsed["module_loaded"] = True
                with _stats_lock:
                    _latest.update(parsed)
            else:
                with _stats_lock:
                    _latest["module_loaded"] = False
                    _latest["last_updated"]  = datetime.now().strftime("%H:%M:%S")
        except Exception as e:
            print(f"[poller] error reading {PROC_STATS}: {e}")
        time.sleep(POLL_INTERVAL)


# ── HTTP request handler ──────────────────────────────────────────────────────
class DashboardHandler(BaseHTTPRequestHandler):

    def log_message(self, fmt, *args):
        # Suppress default per-request stdout noise
        pass

    def send_json(self, data, status=200):
        body = json.dumps(data).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def send_html(self, html: str):
        body = html.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/api/stats":
            with _stats_lock:
                data = dict(_latest)
            self.send_json(data)

        elif self.path in ("/", "/index.html"):
            self.send_html(DASHBOARD_HTML)

        else:
            self.send_response(404)
            self.end_headers()


# ── Dashboard HTML (single-file, no external dependencies except CDN) ─────────
DASHBOARD_HTML = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>KB Analytics Dashboard</title>
<script src="https://cdnjs.cloudflare.com/ajax/libs/Chart.js/4.4.1/chart.umd.min.js"></script>
<style>
  :root {
    --bg:     #0f1117;
    --card:   #1a1d27;
    --border: #2a2d3a;
    --accent: #6c63ff;
    --green:  #00d084;
    --yellow: #ffd166;
    --text:   #e2e8f0;
    --muted:  #8892a4;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body {
    background: var(--bg); color: var(--text);
    font-family: 'Segoe UI', system-ui, sans-serif;
    min-height: 100vh; padding: 24px;
  }
  header {
    display: flex; justify-content: space-between; align-items: center;
    margin-bottom: 28px;
  }
  header h1 { font-size: 1.5rem; font-weight: 700; }
  header h1 span { color: var(--accent); }
  #status-badge {
    padding: 6px 14px; border-radius: 20px; font-size: 0.8rem;
    font-weight: 600; letter-spacing: .5px;
  }
  .online  { background: #00d08422; color: var(--green); border: 1px solid var(--green); }
  .offline { background: #ff4d4d22; color: #ff4d4d;     border: 1px solid #ff4d4d; }

  .grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
    gap: 16px; margin-bottom: 24px;
  }
  .card {
    background: var(--card); border: 1px solid var(--border);
    border-radius: 12px; padding: 20px;
  }
  .card .label { font-size: .75rem; color: var(--muted); text-transform: uppercase; letter-spacing: .8px; margin-bottom: 8px; }
  .card .value { font-size: 2rem; font-weight: 700; }
  .card .unit  { font-size: .85rem; color: var(--muted); margin-left: 4px; }

  .charts { display: grid; grid-template-columns: 1fr 1fr; gap: 16px; margin-bottom: 24px; }
  @media (max-width: 700px) { .charts { grid-template-columns: 1fr; } }

  .chart-card { background: var(--card); border: 1px solid var(--border); border-radius: 12px; padding: 20px; }
  .chart-card h2 { font-size: .9rem; color: var(--muted); margin-bottom: 16px; text-transform: uppercase; letter-spacing: .6px; }

  .key-table { width: 100%; border-collapse: collapse; font-size: .9rem; }
  .key-table th { color: var(--muted); font-weight: 600; text-align: left; padding: 6px 10px; border-bottom: 1px solid var(--border); }
  .key-table td { padding: 7px 10px; border-bottom: 1px solid #ffffff08; }
  .key-table tr:last-child td { border-bottom: none; }
  .bar-wrap { background: #ffffff0a; border-radius: 4px; height: 8px; min-width: 60px; }
  .bar-fill  { background: var(--accent); border-radius: 4px; height: 8px; transition: width .4s; }

  footer { text-align: center; color: var(--muted); font-size: .78rem; margin-top: 8px; }
  #last-update { color: var(--muted); font-size: .8rem; }

  .tls-badge {
    display: inline-flex; align-items: center; gap: 6px;
    background: #00d08415; border: 1px solid #00d08440;
    color: var(--green); border-radius: 20px;
    padding: 4px 12px; font-size: .75rem; font-weight: 600;
  }
</style>
</head>
<body>

<header>
  <div>
    <h1>KB <span>Analytics</span> Dashboard</h1>
    <div style="margin-top:6px; display:flex; gap:10px; align-items:center;">
      <span class="tls-badge">🔒 TLS Secured</span>
      <span id="last-update">Connecting...</span>
    </div>
  </div>
  <div id="status-badge" class="offline">● MODULE OFFLINE</div>
</header>

<!-- Stat cards -->
<div class="grid">
  <div class="card">
    <div class="label">Total Key Presses</div>
    <div class="value" id="total">—</div>
  </div>
  <div class="card">
    <div class="label">Typing Speed</div>
    <div class="value" id="speed">—<span class="unit">kpm</span></div>
  </div>
  <div class="card">
    <div class="label">Avg Key Interval</div>
    <div class="value" id="interval">—<span class="unit">µs</span></div>
  </div>
  <div class="card">
    <div class="label">Hotkey Combos</div>
    <div class="value" id="hotkeys">—</div>
  </div>
</div>

<!-- Charts row -->
<div class="charts">
  <div class="chart-card">
    <h2>Typing Speed Over Time (kpm)</h2>
    <canvas id="speedChart" height="160"></canvas>
  </div>
  <div class="chart-card">
    <h2>Key Press Activity</h2>
    <canvas id="pressChart" height="160"></canvas>
  </div>
</div>

<!-- Top keys table -->
<div class="card" style="margin-bottom:24px;">
  <h2 style="font-size:.9rem; color:var(--muted); text-transform:uppercase; letter-spacing:.6px; margin-bottom:16px;">Top Keys</h2>
  <table class="key-table">
    <thead><tr><th>#</th><th>Key</th><th>Presses</th><th>Share</th></tr></thead>
    <tbody id="top-keys-body"></tbody>
  </table>
</div>

<footer>Data held in memory only — nothing written to disk &nbsp;|&nbsp; Refreshes every 2s</footer>

<script>
const speedHistory  = [];
const pressHistory  = [];
const timeLabels    = [];
const MAX_POINTS    = 30;

// ── Chart: typing speed timeline ──────────────────────────────────────────
const speedCtx = document.getElementById('speedChart').getContext('2d');
const speedChart = new Chart(speedCtx, {
  type: 'line',
  data: {
    labels: timeLabels,
    datasets: [{
      label: 'kpm',
      data: speedHistory,
      borderColor: '#6c63ff',
      backgroundColor: '#6c63ff22',
      fill: true,
      tension: 0.4,
      pointRadius: 2,
    }]
  },
  options: {
    animation: false,
    plugins: { legend: { display: false } },
    scales: {
      x: { ticks: { color: '#8892a4', maxTicksLimit: 6 }, grid: { color: '#2a2d3a' } },
      y: { ticks: { color: '#8892a4' }, grid: { color: '#2a2d3a' }, beginAtZero: true }
    }
  }
});

// ── Chart: total key presses timeline ─────────────────────────────────────
const pressCtx = document.getElementById('pressChart').getContext('2d');
const pressChart = new Chart(pressCtx, {
  type: 'bar',
  data: {
    labels: timeLabels,
    datasets: [{
      label: 'Total presses',
      data: pressHistory,
      backgroundColor: '#00d08455',
      borderColor: '#00d084',
      borderWidth: 1,
    }]
  },
  options: {
    animation: false,
    plugins: { legend: { display: false } },
    scales: {
      x: { ticks: { color: '#8892a4', maxTicksLimit: 6 }, grid: { color: '#2a2d3a' } },
      y: { ticks: { color: '#8892a4' }, grid: { color: '#2a2d3a' }, beginAtZero: true }
    }
  }
});

// ── Top-keys table renderer ───────────────────────────────────────────────
function renderTopKeys(keys) {
  const tbody = document.getElementById('top-keys-body');
  if (!keys || keys.length === 0) {
    tbody.innerHTML = '<tr><td colspan="4" style="color:var(--muted);padding:12px">No key data yet — start typing!</td></tr>';
    return;
  }
  const maxCount = keys[0].count || 1;
  tbody.innerHTML = keys.slice(0, 10).map((k, i) => {
    const pct = Math.round((k.count / maxCount) * 100);
    return `<tr>
      <td style="color:var(--muted)">${i + 1}</td>
      <td><strong>${k.key}</strong></td>
      <td>${k.count.toLocaleString()}</td>
      <td style="width:120px">
        <div class="bar-wrap"><div class="bar-fill" style="width:${pct}%"></div></div>
      </td>
    </tr>`;
  }).join('');
}

// ── Poll /api/stats every 2 seconds ──────────────────────────────────────
async function fetchStats() {
  try {
    const res  = await fetch('/api/stats');
    const data = await res.json();

    // Status badge
    const badge = document.getElementById('status-badge');
    if (data.module_loaded) {
      badge.textContent = '● MODULE ONLINE';
      badge.className = 'online';
    } else {
      badge.textContent = '● MODULE OFFLINE';
      badge.className = 'offline';
    }

    // Stat cards
    document.getElementById('total').textContent    = data.total_keypresses.toLocaleString();
    document.getElementById('speed').innerHTML      = `${data.typing_speed_kpm}<span class="unit">kpm</span>`;
    document.getElementById('interval').innerHTML   = `${data.avg_interval_us.toLocaleString()}<span class="unit">µs</span>`;
    document.getElementById('hotkeys').textContent  = data.hotkey_combos.toLocaleString();
    document.getElementById('last-update').textContent = `Last updated: ${data.last_updated}`;

    // Time-series data
    const label = data.last_updated || new Date().toLocaleTimeString();
    if (timeLabels.length >= MAX_POINTS) {
      timeLabels.shift(); speedHistory.shift(); pressHistory.shift();
    }
    timeLabels.push(label);
    speedHistory.push(data.typing_speed_kpm);
    pressHistory.push(data.total_keypresses);
    speedChart.update();
    pressChart.update();

    // Top keys
    renderTopKeys(data.top_keys);

  } catch (e) {
    document.getElementById('last-update').textContent = 'Connection error — retrying...';
  }
}

fetchStats();
setInterval(fetchStats, 2000);
</script>
</body>
</html>
"""


# ── TLS setup + server start ──────────────────────────────────────────────────
def main():
    print("=" * 55)
    print("  KB Analytics — TLS Dashboard Server")
    print("=" * 55)

    # Check certs exist
    if not os.path.exists(CERT_FILE) or not os.path.exists(KEY_FILE):
        print(f"\n[ERROR] TLS certificates not found.")
        print(f"  Expected: {CERT_FILE}  and  {KEY_FILE}")
        print(f"  Run:  bash gen_certs.sh   to generate them first.\n")
        raise SystemExit(1)

    # Start background poller
    poller = threading.Thread(target=stats_poller, daemon=True)
    poller.start()
    print(f"[OK] Stats poller started (reads {PROC_STATS} every {POLL_INTERVAL}s)")

    # Wrap server with TLS
    server = HTTPServer((HOST, PORT), DashboardHandler)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(certfile=CERT_FILE, keyfile=KEY_FILE)
    server.socket = ctx.wrap_socket(server.socket, server_side=True)

    print(f"[OK] TLS context loaded  ({CERT_FILE})")
    print(f"\n  Dashboard: https://0.0.0.0:{PORT}")
    print(f"  Open on your laptop: https://<pi-ip>:{PORT}")
    print(f"\n  (Accept the self-signed cert warning in your browser)")
    print(f"  Ctrl+C to stop\n")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[INFO] Server stopped.")


if __name__ == "__main__":
    main()

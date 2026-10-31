// Linux Service & Process Supervisor — Frontend Application
// Connects to the embedded C HTTP REST API server on port 8080.

const API_BASE = (window.location.protocol === 'http:' && (window.location.port === '8080' || window.location.port === '80'))
  ? ''
  : 'http://localhost:8080';

// DOM Elements
const navButtons = [...document.querySelectorAll('.nav-link')];
const pages = [...document.querySelectorAll('.page')];
const crumb = document.getElementById('crumbCurrent');
const sidebar = document.getElementById('sidebar');

// State
let g_supervisorData = null;
let g_isBackendConnected = false;
let g_logsPaused = false;
let g_activeLogSource = '';
let g_logLinesCache = [];
let g_initialConfigLoaded = false;
let g_lastCrashCounts = {};
let g_lastRestartCounts = {};
let g_multiServiceAutoRun = true;
let g_chaosInterval = null;

// -------------------------------------------------------------
// Navigation
// -------------------------------------------------------------
function showPage(name) {
  const target = document.getElementById(`page-${name}`);
  if (!target) return;
  pages.forEach(page => page.classList.toggle('active', page === target));
  navButtons.forEach(button => button.classList.toggle('active', button.dataset.page === name));
  const activeButton = navButtons.find(button => button.dataset.page === name);
  crumb.textContent = activeButton ? activeButton.textContent.trim() : 'Dashboard';
  sidebar.classList.remove('open');
  window.scrollTo({ top: 0, behavior: 'smooth' });

  if (name === 'configuration' && !g_initialConfigLoaded) {
    loadConfiguration();
  }
  if (name === 'logs') {
    fetchLogs();
  }
}

window.addEventListener('hashchange', () => {
  const name = location.hash.slice(1);
  if (name && document.getElementById(`page-${name}`)) showPage(name);
});

// -------------------------------------------------------------
// Toast Notifications
// -------------------------------------------------------------
function showToast(message, type = 'success') {
  const container = document.getElementById('toastContainer');
  if (!container) return;
  const toast = document.createElement('div');
  toast.className = `toast ${type}`;
  const icon = type === 'success' ? '✓' : type === 'error' ? '✕' : 'ℹ';
  toast.innerHTML = `<span style="font-weight:700;">${icon}</span><span>${escapeHtml(message)}</span>`;
  container.appendChild(toast);
  setTimeout(() => {
    toast.style.opacity = '0';
    toast.style.transform = 'translateY(10px)';
    setTimeout(() => toast.remove(), 300);
  }, 3500);
}

function escapeHtml(str) {
  if (!str) return '';
  return String(str)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#039;');
}

// -------------------------------------------------------------
// API Actions
// -------------------------------------------------------------
async function triggerAction(action, service = '', btnElement = null) {
  if (btnElement) {
    btnElement.disabled = true;
    btnElement.style.opacity = '0.5';
    btnElement.style.pointerEvents = 'none';
  }
  try {
    const resp = await fetch(`${API_BASE}/api/action`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ action, service })
    });
    const data = await resp.json();
    if (data.success) {
      showToast(data.message || `Action ${action} successful`, 'success');
      // Instant refresh
      pollStatus();
      fetchLogs();
    } else {
      showToast(data.message || `Action failed`, 'error');
    }
  } catch (err) {
    showToast(`Error communicating with backend: ${err.message}`, 'error');
  } finally {
    if (btnElement) {
      btnElement.disabled = false;
      btnElement.style.opacity = '';
      btnElement.style.pointerEvents = '';
    }
  }
}

// -------------------------------------------------------------
// Status Polling
// -------------------------------------------------------------
async function pollStatus() {
  try {
    const resp = await fetch(`${API_BASE}/api/status`, { cache: 'no-store' });
    if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
    const data = await resp.json();
    g_supervisorData = data;
    setConnectionState(true, data);

    // Track restarts and crashes across all services smoothly
    if (data.services) {
      data.services.forEach(s => {
        const prevCrashes = g_lastCrashCounts[s.name];
        const prevRestarts = g_lastRestartCounts[s.name];
        const currRestarts = (s.total_restarts !== undefined ? s.total_restarts : s.restart_count);
        if (prevCrashes !== undefined && s.crash_count > prevCrashes) {
          // Subtle crash notice
          showToast(`⚡ Service '${s.name}' recovered from crash (exit status: ${s.last_exit_status})`, 'info');
        }
        g_lastCrashCounts[s.name] = s.crash_count;
        g_lastRestartCounts[s.name] = currRestarts;
      });
    }

    renderDashboard(data);
    renderServicesTable(data);
    renderResources(data);
    renderArchitecture(data);
  } catch (err) {
    setConnectionState(false, null);
  }
}

function setConnectionState(connected, data) {
  g_isBackendConnected = connected;

  const dot = document.getElementById('sidebarDot');
  const bottom = document.getElementById('sidebarBottom');
  const title = document.getElementById('sidebarTitle');
  const sub = document.getElementById('sidebarSubtitle');
  const livePill = document.getElementById('livePill');
  const notice = document.getElementById('connNotice');
  const noticeIcon = document.getElementById('noticeIcon');
  const noticeTitle = document.getElementById('noticeTitle');
  const noticeDesc = document.getElementById('noticeDesc');

  if (connected && data) {
    if (dot) dot.classList.add('connected');
    if (bottom) bottom.classList.add('connected');
    if (title) title.textContent = 'C Supervisor Online';
    if (sub) sub.textContent = `PID: ${data.supervisor.pid} · Port: ${data.supervisor.http_port}`;
    if (livePill) {
      livePill.className = 'preview-pill live';
      livePill.innerHTML = '<i></i> LIVE CONNECTED';
    }
    if (notice && !notice.dataset.manuallyDismissed) {
      notice.style.background = '#eaf8f0';
      notice.style.borderColor = '#c0ecd3';
      notice.style.color = '#115e3b';
      if (noticeIcon) {
        noticeIcon.textContent = '✓';
        noticeIcon.style.background = '#b8e8cb';
      }
      if (noticeTitle) noticeTitle.textContent = 'Live C Supervisor Connected';
      if (noticeDesc) noticeDesc.textContent = `Successfully communicating with C supervisor (PID: ${data.supervisor.pid}) on 0.0.0.0:${data.supervisor.http_port}. Control buttons and telemetry are fully active.`;
    }
  } else {
    if (dot) dot.classList.remove('connected');
    if (bottom) bottom.classList.remove('connected');
    if (title) title.textContent = 'Backend Offline';
    if (sub) sub.textContent = 'Trying port 8080...';
    if (livePill) {
      livePill.className = 'preview-pill';
      livePill.innerHTML = '<i></i> DISCONNECTED';
    }
    if (notice) {
      notice.hidden = false;
      notice.style.background = '#fef2f2';
      notice.style.borderColor = '#fecaca';
      notice.style.color = '#991b1b';
      if (noticeIcon) {
        noticeIcon.textContent = '!';
        noticeIcon.style.background = '#fca5a5';
      }
      if (noticeTitle) noticeTitle.textContent = 'C Supervisor Backend Offline';
      if (noticeDesc) noticeDesc.textContent = 'The C supervisor backend is not reachable on port 8080. Start dev.js or run.sh in WSL to bring it online.';
    }
    const dashChip = document.getElementById('dashStatusChip');
    if (dashChip) {
      dashChip.className = 'status-chip neutral';
      dashChip.innerHTML = '<i></i> Offline';
    }
    const srvChip = document.getElementById('servicesStatusChip');
    if (srvChip) {
      srvChip.className = 'status-chip neutral';
      srvChip.innerHTML = '<i></i> Offline';
    }
  }
}

// -------------------------------------------------------------
// Render Dashboard
// -------------------------------------------------------------
function renderDashboard(data) {
  const sup = data.supervisor;

  const statManaged = document.getElementById('statManaged');
  const statRunning = document.getElementById('statRunning');
  const statRestarts = document.getElementById('statRestarts');
  const statResources = document.getElementById('statResources');
  const chip = document.getElementById('dashStatusChip');

  if (statManaged) statManaged.textContent = sup.total_services;
  if (statRunning) statRunning.textContent = sup.running_services;
  if (statRestarts) statRestarts.textContent = `${sup.total_restarts} / ${sup.total_crashes}`;
  if (statResources) {
    const supMb = (sup.resources.supervisor_rss_kb / 1024).toFixed(1);
    const chMb = (sup.resources.children_max_rss_kb / 1024).toFixed(1);
    statResources.textContent = `${supMb} MB / ${chMb} MB`;
  }
  if (chip) {
    chip.className = 'status-chip online';
    chip.innerHTML = `<i></i> Online · PID ${sup.pid}`;
  }

  const lastPoll = document.getElementById('lastPollTime');
  if (lastPoll) {
    const d = new Date();
    lastPoll.textContent = `LAST SYNC: ${d.toLocaleTimeString()}`;
  }

  // Dashboard Overview Panel
  const overviewContainer = document.getElementById('dashServicesContainer');
  if (overviewContainer && data.services) {
    let tbody = overviewContainer.querySelector('tbody');
    if (!tbody) {
      overviewContainer.innerHTML = '<div class="table-wrap"><table><thead><tr><th>Service</th><th>PID</th><th>State</th><th>Uptime</th><th>Heartbeat</th><th>Quick Actions</th></tr></thead><tbody id="dashTableBody"></tbody></table></div>';
      tbody = overviewContainer.querySelector('tbody');
    }
    updateTableRows(tbody, data.services, true);
  }
}

// -------------------------------------------------------------
// Render Heartbeat Cell
// -------------------------------------------------------------
function renderHeartbeatCell(s) {
  if (!s.heartbeat_enabled) {
    return `<span class="hb-cell disabled"><span class="hb-dot inactive"></span><span class="hb-text">No IPC watchdog</span></span>`;
  }
  if (s.state !== 'RUNNING' && s.state !== 'UNHEALTHY') {
    return `<span class="hb-cell stopped"><span class="hb-dot inactive"></span><span class="hb-text">${s.state}</span></span>`;
  }
  const ago = s.last_heartbeat_ago !== undefined ? s.last_heartbeat_ago : 0;
  const timeout = s.heartbeat_timeout_sec || 6;
  if (s.state === 'UNHEALTHY' || ago >= timeout) {
    return `<span class="hb-cell stalled" title="Watchdog timeout exceeded! (${ago}s > ${timeout}s)">
      <span class="hb-dot stalled"></span>
      <span class="hb-text"><strong>⚠️ Stalled (${ago}s)</strong></span>
    </span>`;
  }
  return `<span class="hb-cell active" title="IPC pipe(3) pulse received ${ago}s ago (timeout: ${timeout}s)">
    <span class="hb-dot pulse"></span>
    <span class="hb-text"><strong>Pulse (${ago}s ago)</strong> <small>${timeout}s</small></span>
  </span>`;
}

// -------------------------------------------------------------
// Update Table Rows In-Place (Smooth, flicker-free DOM patching)
// -------------------------------------------------------------
function updateTableRows(tbody, services, isDashboard) {
  if (!tbody || !services) return;

  const existingRows = {};
  [...tbody.querySelectorAll('tr[data-service]')].forEach(tr => {
    existingRows[tr.dataset.service] = tr;
  });

  const activeNames = new Set();

  services.forEach(s => {
    activeNames.add(s.name);
    let tr = existingRows[s.name];
    const isRunning = s.state === 'RUNNING';
    const isPaused = s.state === 'PAUSED';
    const isStopped = s.state === 'STOPPED';
    const rssMb = s.rss_kb > 0 ? `${(s.rss_kb / 1024).toFixed(1)} MB` : '-';
    const uptimeStr = formatUptime(s.uptime_sec);
    const restartsStr = `<strong>${s.total_restarts !== undefined ? s.total_restarts : s.restart_count}</strong> <span style="font-size:10px;color:var(--text-muted);">/ ${s.crash_count}</span>`;
    const hbHtml = renderHeartbeatCell(s);

    if (!tr) {
      tr = document.createElement('tr');
      tr.dataset.service = s.name;
      tr.dataset.lastState = s.state;

      if (isDashboard) {
        tr.innerHTML = `
          <td><strong>${escapeHtml(s.name)}</strong><span class="table-sub">${escapeHtml(s.command)}</span></td>
          <td><code class="cell-pid">${s.pid > 0 ? s.pid : '-'}</code></td>
          <td><span class="badge-status badge-${s.state}">${s.state}</span></td>
          <td class="cell-uptime">${uptimeStr}</td>
          <td class="cell-hb">${hbHtml}</td>
          <td>
            <div class="action-btns">
              ${(!isRunning && !isPaused) ? `<button class="btn-act start" onclick="triggerAction('start','${s.name}',this)">Start</button>` : ''}
              ${(isRunning || isPaused || s.state === 'UNHEALTHY') ? `<button class="btn-act stop" onclick="triggerAction('stop','${s.name}',this)">Stop</button>` : ''}
              <button class="btn-act restart" onclick="triggerAction('restart','${s.name}',this)">Restart</button>
            </div>
          </td>
        `;
      } else {
        tr.innerHTML = `
          <td>
            <strong>${escapeHtml(s.name)}</strong>
            <span class="table-sub">${escapeHtml(s.command)}</span>
            ${s.depends_on ? `<span style="font-size:9px;color:#3b82f6;">depends on: ${escapeHtml(s.depends_on)}</span>` : ''}
          </td>
          <td><code class="cell-pid">${s.pid > 0 ? s.pid : '-'}</code></td>
          <td><code class="cell-pgid">${s.pgid > 0 ? s.pgid : '-'}</code></td>
          <td><span class="badge-status badge-${s.state}">${s.state}</span></td>
          <td class="cell-restarts">${restartsStr}</td>
          <td class="cell-rss">${rssMb}</td>
          <td class="cell-hb">${hbHtml}</td>
          <td>
            <div class="action-btns">
              ${(!isRunning && !isPaused) ? `<button class="btn-act start" title="Start service (fork+exec)" onclick="triggerAction('start','${s.name}',this)">▶ Start</button>` : ''}
              ${(isRunning || s.state === 'UNHEALTHY') ? `<button class="btn-act stop" title="Stop service (SIGTERM)" onclick="triggerAction('stop','${s.name}',this)">⏹ Stop</button>` : ''}
              <button class="btn-act restart" title="Controlled Restart" onclick="triggerAction('restart','${s.name}',this)">↻ Restart</button>
              ${isRunning ? `<button class="btn-act pause" title="Pause process (SIGSTOP)" onclick="triggerAction('pause','${s.name}',this)">⏸ Pause</button>` : ''}
              ${isPaused ? `<button class="btn-act resume" title="Resume process (SIGCONT)" onclick="triggerAction('resume','${s.name}',this)">⏵ Resume</button>` : ''}
              ${(!isStopped) ? `<button class="btn-act kill" title="Force Kill (SIGKILL)" onclick="triggerAction('kill','${s.name}',this)">✕ Kill</button>` : ''}
            </div>
          </td>
        `;
      }
      tbody.appendChild(tr);
    } else {
      // In-place updates for existing row
      const pidEl = tr.querySelector('.cell-pid');
      if (pidEl) {
        const val = s.pid > 0 ? String(s.pid) : '-';
        if (pidEl.textContent !== val) pidEl.textContent = val;
      }

      const pgidEl = tr.querySelector('.cell-pgid');
      if (pgidEl) {
        const val = s.pgid > 0 ? String(s.pgid) : '-';
        if (pgidEl.textContent !== val) pgidEl.textContent = val;
      }

      const badge = tr.querySelector('.badge-status');
      if (badge && (badge.textContent !== s.state || !badge.classList.contains(`badge-${s.state}`))) {
        badge.className = `badge-status badge-${s.state}`;
        badge.textContent = s.state;
      }

      const uptimeEl = tr.querySelector('.cell-uptime');
      if (uptimeEl && uptimeEl.textContent !== uptimeStr) {
        uptimeEl.textContent = uptimeStr;
      }

      const restartsEl = tr.querySelector('.cell-restarts');
      if (restartsEl) {
        restartsEl.innerHTML = restartsStr;
      }

      const rssEl = tr.querySelector('.cell-rss');
      if (rssEl && rssEl.textContent !== rssMb) {
        rssEl.textContent = rssMb;
      }

      const hbEl = tr.querySelector('.cell-hb');
      if (hbEl) {
        hbEl.innerHTML = hbHtml;
      }

      // Update action buttons only if state changed
      if (tr.dataset.lastState !== s.state) {
        tr.dataset.lastState = s.state;
        const btnContainer = tr.querySelector('.action-btns');
        if (btnContainer) {
          if (isDashboard) {
            btnContainer.innerHTML = `
              ${(!isRunning && !isPaused) ? `<button class="btn-act start" onclick="triggerAction('start','${s.name}',this)">Start</button>` : ''}
              ${(isRunning || isPaused || s.state === 'UNHEALTHY') ? `<button class="btn-act stop" onclick="triggerAction('stop','${s.name}',this)">Stop</button>` : ''}
              <button class="btn-act restart" onclick="triggerAction('restart','${s.name}',this)">Restart</button>
            `;
          } else {
            btnContainer.innerHTML = `
              ${(!isRunning && !isPaused) ? `<button class="btn-act start" title="Start service (fork+exec)" onclick="triggerAction('start','${s.name}',this)">▶ Start</button>` : ''}
              ${(isRunning || s.state === 'UNHEALTHY') ? `<button class="btn-act stop" title="Stop service (SIGTERM)" onclick="triggerAction('stop','${s.name}',this)">⏹ Stop</button>` : ''}
              <button class="btn-act restart" title="Controlled Restart" onclick="triggerAction('restart','${s.name}',this)">↻ Restart</button>
              ${isRunning ? `<button class="btn-act pause" title="Pause process (SIGSTOP)" onclick="triggerAction('pause','${s.name}',this)">⏸ Pause</button>` : ''}
              ${isPaused ? `<button class="btn-act resume" title="Resume process (SIGCONT)" onclick="triggerAction('resume','${s.name}',this)">⏵ Resume</button>` : ''}
              ${(!isStopped) ? `<button class="btn-act kill" title="Force Kill (SIGKILL)" onclick="triggerAction('kill','${s.name}',this)">✕ Kill</button>` : ''}
            `;
          }
        }
      }
    }
  });

  // Remove rows no longer present
  Object.keys(existingRows).forEach(name => {
    if (!activeNames.has(name)) {
      existingRows[name].remove();
    }
  });
}

// -------------------------------------------------------------
// Render Services Table
// -------------------------------------------------------------
function renderServicesTable(data) {
  const tbody = document.getElementById('servicesTableBody');
  const countPill = document.getElementById('servicesCountPill');
  const chip = document.getElementById('servicesStatusChip');
  const searchInput = document.getElementById('serviceSearch');
  const query = searchInput ? searchInput.value.toLowerCase().trim() : '';

  if (chip) {
    chip.className = 'status-chip online';
    chip.innerHTML = '<i></i> Live Process Control Active';
  }

  if (!tbody || !data.services) return;

  const filtered = data.services.filter(s =>
    s.name.toLowerCase().includes(query) || s.command.toLowerCase().includes(query)
  );

  if (countPill) countPill.textContent = `${filtered.length} of ${data.services.length} services`;

  if (filtered.length === 0) {
    tbody.innerHTML = `<tr><td colspan="8"><div class="table-empty"><span>◈</span><strong>No services matching "${escapeHtml(query)}"</strong></div></td></tr>`;
    return;
  }

  // Remove placeholder empty state if present
  const emptyRow = tbody.querySelector('.table-empty');
  if (emptyRow) tbody.innerHTML = '';

  updateTableRows(tbody, filtered, false);
  renderHeartbeatCards(data.services);
}

// -------------------------------------------------------------
// Render Heartbeat Summary Cards In-Place
// -------------------------------------------------------------
function renderHeartbeatCards(services) {
  const hbGrid = document.getElementById('hbSummaryGrid');
  if (!hbGrid || !services) return;

  const existingCards = {};
  [...hbGrid.querySelectorAll('.hb-card[data-service]')].forEach(c => {
    existingCards[c.dataset.service] = c;
  });

  const activeNames = new Set();

  services.forEach(s => {
    activeNames.add(s.name);
    const ago = s.last_heartbeat_ago !== undefined ? s.last_heartbeat_ago : 0;
    const timeout = s.heartbeat_timeout_sec || 6;
    const isHb = s.heartbeat_enabled;

    let card = existingCards[s.name];
    if (!card) {
      card = document.createElement('div');
      card.className = 'hb-card';
      card.dataset.service = s.name;
      card.innerHTML = `
        <div class="hb-card-head">
          <span class="hb-card-title">${escapeHtml(s.name)}</span>
          <span class="badge-status badge-${s.state}">${s.state}</span>
        </div>
        <div class="hb-card-meta">
          <span>PID: <code class="card-pid">${s.pid > 0 ? s.pid : '-'}</code></span>
          <span class="hb-card-pipe">pipe(3) IPC</span>
        </div>
        <div class="card-hb-container" style="margin-top: 4px;">
          ${renderHeartbeatCell(s)}
        </div>
        <div style="margin-top: 6px; display: flex; justify-content: space-between; align-items: center;">
          <span style="font-size: 10px; color: #94a3b8;">${isHb ? `Watchdog: ${timeout}s` : 'Heartbeat disabled'}</span>
          <button class="btn-act" style="font-size: 9px;" onclick="viewServiceLogs('${s.name}')">View log →</button>
        </div>
      `;
      hbGrid.appendChild(card);
    } else {
      const badge = card.querySelector('.badge-status');
      if (badge && (badge.textContent !== s.state || !badge.classList.contains(`badge-${s.state}`))) {
        badge.className = `badge-status badge-${s.state}`;
        badge.textContent = s.state;
      }
      const pidEl = card.querySelector('.card-pid');
      if (pidEl) {
        const val = s.pid > 0 ? String(s.pid) : '-';
        if (pidEl.textContent !== val) pidEl.textContent = val;
      }
      const hbCont = card.querySelector('.card-hb-container');
      if (hbCont) {
        hbCont.innerHTML = renderHeartbeatCell(s);
      }
    }
  });

  Object.keys(existingCards).forEach(name => {
    if (!activeNames.has(name)) existingCards[name].remove();
  });
}

// -------------------------------------------------------------
// Render Resources
// -------------------------------------------------------------
function renderResources(data) {
  const sup = data.supervisor;
  const resCpuVal = document.getElementById('resCpuVal');
  const resCpuSub = document.getElementById('resCpuSub');
  const resMemVal = document.getElementById('resMemVal');
  const resMemSub = document.getElementById('resMemSub');
  const resLoadText = document.getElementById('resLoadText');
  const chip = document.getElementById('resStatusChip');

  if (chip) {
    chip.className = 'status-chip online';
    chip.innerHTML = '<i></i> Live Telemetry Connected';
  }

  if (resCpuVal) {
    const totalCpu = (sup.resources.children_user_cpu + sup.resources.children_sys_cpu).toFixed(2);
    resCpuVal.innerHTML = `${totalCpu} <small>sec</small>`;
  }
  if (resCpuSub) {
    resCpuSub.textContent = `User: ${sup.resources.children_user_cpu.toFixed(2)}s | Sys: ${sup.resources.children_sys_cpu.toFixed(2)}s | Self: ${sup.resources.self_user_cpu.toFixed(2)}s`;
  }
  if (resMemVal) {
    const memMb = (sup.resources.children_max_rss_kb / 1024).toFixed(1);
    resMemVal.innerHTML = `${memMb} <small>MB</small>`;
  }
  if (resMemSub) {
    const supMb = (sup.resources.supervisor_rss_kb / 1024).toFixed(1);
    resMemSub.textContent = `Children Max RSS: ${(sup.resources.children_max_rss_kb / 1024).toFixed(1)} MB | Supervisor Resident: ${supMb} MB`;
  }
  if (resLoadText && sup.load_avg) {
    resLoadText.textContent = `1 min: ${sup.load_avg[0].toFixed(2)}   ·   5 min: ${sup.load_avg[1].toFixed(2)}   ·   15 min: ${sup.load_avg[2].toFixed(2)}`;
  }

  // Per process table
  const tbody = document.getElementById('resProcTableBody');
  if (tbody && data.services) {
    let html = '';
    data.services.forEach(s => {
      const rss = s.rss_kb > 0 ? `${(s.rss_kb / 1024).toFixed(2)} MB (${s.rss_kb} KB)` : '-';
      html += `<tr>
        <td><strong>${escapeHtml(s.name)}</strong></td>
        <td><code>${s.pid > 0 ? s.pid : '-'}</code></td>
        <td><span class="badge-status badge-${s.state}">${s.state}</span></td>
        <td>${rss}</td>
        <td>${formatUptime(s.uptime_sec)}</td>
      </tr>`;
    });
    tbody.innerHTML = html;
  }
}

// -------------------------------------------------------------
// Render Architecture
// -------------------------------------------------------------
function renderArchitecture(data) {
  const front = document.getElementById('archFrontendStatus');
  const back = document.getElementById('archBackendStatus');
  const sup = document.getElementById('archSupervisorStatus');
  const chip = document.getElementById('archStatusChip');

  if (chip) {
    chip.className = 'status-chip online';
    chip.innerHTML = '<i></i> Live Integration Active';
  }
  if (front) {
    front.textContent = 'ONLINE (BROWSER)';
    front.className = 'arch-live-badge active';
  }
  if (back) {
    back.textContent = `PORT ${data.supervisor.http_port} ACTIVE`;
    back.className = 'arch-live-badge active';
  }
  if (sup) {
    sup.textContent = `PID ${data.supervisor.pid} RUNNING`;
    sup.className = 'arch-live-badge active';
  }
}

// -------------------------------------------------------------
// Logs Viewer
// -------------------------------------------------------------
async function fetchLogs() {
  if (g_logsPaused) return;

  const select = document.getElementById('logSourceSelect');
  const source = select ? select.value : '';
  g_activeLogSource = source;

  try {
    const url = `${API_BASE}/api/logs?service=${encodeURIComponent(source)}&lines=120`;
    const resp = await fetch(url, { cache: 'no-store' });
    if (!resp.ok) return;
    const data = await resp.json();
    g_logLinesCache = data.lines || [];

    const pathPill = document.getElementById('logActivePath');
    const countPill = document.getElementById('logLineCountPill');
    const chip = document.getElementById('logsStatusChip');

    if (pathPill) pathPill.textContent = data.path || 'logs/supervisor.log';
    if (countPill) countPill.textContent = `${g_logLinesCache.length} lines`;
    if (chip) {
      chip.className = 'status-chip online';
      chip.innerHTML = '<i></i> Live Tail Active';
    }

    renderLogsOutput();
    renderDashboardEvents(g_logLinesCache);
  } catch (err) {
    // Ignore transient network errors
  }
}

function renderLogsOutput() {
  const terminal = document.getElementById('logTerminal');
  const searchInput = document.getElementById('logSearch');
  const query = searchInput ? searchInput.value.toLowerCase().trim() : '';

  if (!terminal) return;

  const lines = query
    ? g_logLinesCache.filter(l => l.toLowerCase().includes(query))
    : g_logLinesCache;

  if (lines.length === 0) {
    terminal.innerHTML = `<div style="color:#64748b;text-align:center;padding:40px;">No log lines available.</div>`;
    return;
  }

  const html = lines.map(line => formatLogLine(line)).join('');
  const shouldScroll = terminal.scrollHeight - terminal.scrollTop - terminal.clientHeight < 120;
  terminal.innerHTML = html;
  if (shouldScroll) {
    terminal.scrollTop = terminal.scrollHeight;
  }
}

function formatLogLine(raw) {
  // Typical supervisor format: [YYYY-MM-DD HH:MM:SS] [LEVEL] [PID: xxx] message
  const match = raw.match(/^(\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\])\s+(\[.*?\])\s+(\[PID:\s*\d+\])\s+(.*)$/);
  if (match) {
    const time = match[1];
    const level = match[2];
    const pid = match[3];
    const msg = match[4];

    let lvlClass = 'log-tag-info';
    if (level.includes('WARN')) lvlClass = 'log-tag-warn';
    else if (level.includes('ERROR') || level.includes('FATAL')) lvlClass = 'log-tag-error';

    return `<div class="log-line">
      <span class="log-tag-time">${escapeHtml(time)}</span>
      <span class="${lvlClass}">${escapeHtml(level)}</span>
      <span class="log-tag-pid">${escapeHtml(pid)}</span>
      <span style="color:#cbd5e1;">${escapeHtml(msg)}</span>
    </div>`;
  }

  // Worker service format: [service] message (e.g. [heartbeat] heartbeat 45 at ...)
  const workerMatch = raw.match(/^\[([a-zA-Z0-9_-]+)\]\s*(.*)$/);
  if (workerMatch) {
    const tag = workerMatch[1].toLowerCase();
    const msg = workerMatch[2];
    let tagClass = 'log-tag-hb';
    if (tag === 'metrics') tagClass = 'log-tag-metrics';
    else if (tag === 'logger') tagClass = 'log-tag-logger';
    else if (tag === 'audit') tagClass = 'log-tag-audit';
    else if (tag === 'flaky') tagClass = 'log-tag-flaky';
    else if (tag === 'hung') tagClass = 'log-tag-hung';

    return `<div class="log-line">
      <span class="${tagClass}">[${escapeHtml(workerMatch[1])}]</span>
      <span style="color:#e2e8f0;margin-left:6px;">${escapeHtml(msg)}</span>
    </div>`;
  }

  return `<div class="log-line"><span style="color:#94a3b8;">${escapeHtml(raw)}</span></div>`;
}

function viewServiceLogs(svcName) {
  const select = document.getElementById('logSourceSelect');
  if (select) {
    select.value = svcName;
  }
  showPage('logs');
  fetchLogs();
}

function renderDashboardEvents(lines) {
  const container = document.getElementById('dashEventsContainer');
  if (!container || !lines || lines.length === 0) return;

  const recent = lines.slice(-6).reverse();
  let html = '<div class="dash-events-list" style="display:flex;flex-direction:column;gap:8px;">';
  recent.forEach(line => {
    html += `<div style="padding:8px 10px;background:#f8fafc;border:1px solid #e2e8f0;border-radius:6px;font-size:11px;font-family:monospace;">
      ${escapeHtml(line)}
    </div>`;
  });
  html += '</div>';
  container.innerHTML = html;
}

// -------------------------------------------------------------
// Configuration Editor
// -------------------------------------------------------------
async function loadConfiguration() {
  const textarea = document.getElementById('configEditor');
  const chip = document.getElementById('cfgStatusChip');
  const pathPill = document.getElementById('cfgPathPill');

  try {
    const resp = await fetch(`${API_BASE}/api/config`, { cache: 'no-store' });
    if (!resp.ok) throw new Error('Cannot load config');
    const data = await resp.json();
    if (textarea) textarea.value = data.raw || '';
    if (pathPill) pathPill.textContent = data.path || 'config/services.conf';
    if (chip) {
      chip.className = 'status-chip online';
      chip.innerHTML = '<i></i> Loaded';
    }
    g_initialConfigLoaded = true;
  } catch (err) {
    if (chip) {
      chip.className = 'status-chip error';
      chip.innerHTML = '<i></i> Failed to load';
    }
    showToast(`Failed to load config: ${err.message}`, 'error');
  }
}

async function saveConfiguration() {
  const textarea = document.getElementById('configEditor');
  const msg = document.getElementById('configMsg');
  if (!textarea) return;

  const raw = textarea.value;
  try {
    if (msg) msg.textContent = 'Saving & reloading...';
    const resp = await fetch(`${API_BASE}/api/config`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ raw })
    });
    const data = await resp.json();
    if (data.success) {
      showToast('Configuration saved & reloaded via SIGHUP!', 'success');
      if (msg) msg.textContent = 'Saved & reloaded successfully.';
      pollStatus();
    } else {
      showToast(data.message || 'Error saving configuration', 'error');
      if (msg) msg.textContent = 'Save failed.';
    }
  } catch (err) {
    showToast(`Network error: ${err.message}`, 'error');
    if (msg) msg.textContent = 'Error.';
  }
}

// -------------------------------------------------------------
// Utilities
// -------------------------------------------------------------
function formatUptime(seconds) {
  if (!seconds || seconds <= 0) return '-';
  const h = Math.floor(seconds / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  const s = seconds % 60;
  return `${String(h).padStart(2, '0')}:${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}`;
}

// -------------------------------------------------------------
// Global exports for HTML onclick handlers
// -------------------------------------------------------------
window.triggerAction = triggerAction;
window.viewServiceLogs = viewServiceLogs;
window.showPage = showPage;

// -------------------------------------------------------------
// Event Listeners Setup
// -------------------------------------------------------------
function initApp() {
  // Navigation & generic data-go buttons
  document.querySelectorAll('.nav-link').forEach(button => {
    button.addEventListener('click', () => showPage(button.dataset.page));
  });
  document.querySelectorAll('[data-go]').forEach(button => {
    button.addEventListener('click', () => showPage(button.dataset.go));
  });

  const mobMenu = document.getElementById('mobileMenu');
  if (mobMenu) mobMenu.addEventListener('click', () => sidebar.classList.toggle('open'));

  const dismissBtn = document.getElementById('dismissNotice');
  if (dismissBtn) {
    dismissBtn.addEventListener('click', () => {
      const notice = document.getElementById('connNotice');
      if (notice) {
        notice.hidden = true;
        notice.dataset.manuallyDismissed = 'true';
      }
    });
  }

  // Dashboard Action buttons
  const dStartAll = document.getElementById('dashBtnStartAll');
  if (dStartAll) dStartAll.addEventListener('click', () => triggerAction('start_all'));

  const dStopAll = document.getElementById('dashBtnStopAll');
  if (dStopAll) dStopAll.addEventListener('click', () => triggerAction('stop_all'));

  const dStartWait = document.getElementById('dashBtnStartWaiting');
  if (dStartWait) dStartWait.addEventListener('click', () => triggerAction('approve_waiting'));

  const dReload = document.getElementById('dashBtnReload');
  if (dReload) dReload.addEventListener('click', () => triggerAction('reload'));

  const dShutdown = document.getElementById('dashBtnShutdown');
  if (dShutdown) {
    dShutdown.addEventListener('click', () => {
      if (confirm('Are you sure you want to shut down the C supervisor backend process?')) {
        triggerAction('shutdown');
      }
    });
  }

  // Services Page Toolbar buttons
  const sStartAll = document.getElementById('srvBtnStartAll');
  if (sStartAll) sStartAll.addEventListener('click', () => triggerAction('start_all'));

  const sStopAll = document.getElementById('srvBtnStopAll');
  if (sStopAll) sStopAll.addEventListener('click', () => triggerAction('stop_all'));

  const sRandomRestart = document.getElementById('srvBtnRandomRestart');
  if (sRandomRestart) {
    sRandomRestart.addEventListener('click', () => {
      triggerAction('random_restart');
      showToast('🎲 Requested random service restart...', 'info');
    });
  }

  const sChaosMode = document.getElementById('srvBtnChaosMode');
  if (sChaosMode) {
    sChaosMode.addEventListener('click', () => {
      if (g_chaosInterval) {
        clearInterval(g_chaosInterval);
        g_chaosInterval = null;
        sChaosMode.textContent = '⚡ Auto-Chaos: OFF';
        sChaosMode.style.background = '';
        sChaosMode.style.borderColor = '';
        sChaosMode.style.color = '';
        showToast('Auto-Chaos mode disabled', 'info');
      } else {
        triggerAction('random_restart');
        g_chaosInterval = setInterval(() => {
          triggerAction('random_restart');
        }, 12000);
        sChaosMode.textContent = '⚡ Auto-Chaos: ACTIVE (12s)';
        sChaosMode.style.background = 'rgba(239, 68, 68, 0.2)';
        sChaosMode.style.borderColor = '#ef4444';
        sChaosMode.style.color = '#f87171';
        showToast('⚡ Auto-Chaos active! Random service restarting every 12s', 'info');
      }
    });
  }

  const sAutoRun = document.getElementById('srvBtnAutoRun');
  if (sAutoRun) {
    sAutoRun.addEventListener('click', () => {
      triggerAction('start_all');
      setTimeout(() => triggerAction('approve_waiting'), 600);
      showToast('⚡ Running multiple services now...', 'success');
    });
  }

  const sStartWait = document.getElementById('srvBtnStartWaiting');
  if (sStartWait) sStartWait.addEventListener('click', () => triggerAction('approve_waiting'));

  const sReload = document.getElementById('srvBtnReload');
  if (sReload) sReload.addEventListener('click', () => triggerAction('reload'));

  const sSearch = document.getElementById('serviceSearch');
  if (sSearch) sSearch.addEventListener('input', () => {
    if (g_supervisorData) renderServicesTable(g_supervisorData);
  });

  // Logs Toolbar
  const logSelect = document.getElementById('logSourceSelect');
  if (logSelect) logSelect.addEventListener('change', () => fetchLogs());

  const logSearch = document.getElementById('logSearch');
  if (logSearch) logSearch.addEventListener('input', () => renderLogsOutput());

  const btnPause = document.getElementById('btnPauseLogs');
  if (btnPause) {
    btnPause.addEventListener('click', () => {
      g_logsPaused = !g_logsPaused;
      btnPause.textContent = g_logsPaused ? '▶ Resume' : '⏸ Pause';
      if (!g_logsPaused) fetchLogs();
    });
  }

  const btnClear = document.getElementById('btnClearLogs');
  if (btnClear) {
    btnClear.addEventListener('click', () => {
      const terminal = document.getElementById('logTerminal');
      if (terminal) terminal.innerHTML = '<div style="color:#64748b;text-align:center;padding:40px;">Log display cleared.</div>';
    });
  }

  const btnRefreshLogs = document.getElementById('btnRefreshLogs');
  if (btnRefreshLogs) btnRefreshLogs.addEventListener('click', () => fetchLogs());

  // Config actions
  const btnSaveCfg = document.getElementById('btnSaveConfig');
  if (btnSaveCfg) btnSaveCfg.addEventListener('click', saveConfiguration);

  const btnResetCfg = document.getElementById('btnResetConfig');
  if (btnResetCfg) btnResetCfg.addEventListener('click', loadConfiguration);

  // Initial Poll & Timers
  pollStatus();
  fetchLogs();
  setInterval(pollStatus, 1500);
  setInterval(fetchLogs, 2500);
}

if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', initApp);
} else {
  initApp();
}

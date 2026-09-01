(function () {
  'use strict';

  const esc = (s) => String(s == null ? '' : s).replace(/[&<>"']/g,
    (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

  const qs = (sel) => document.querySelector(sel);

  function toast(msg, type) {
    const wrap = qs('#toasts');
    if (!wrap) return;
    const el = document.createElement('div');
    el.className = 'toast ' + (type || '');
    el.textContent = msg;
    wrap.appendChild(el);
    setTimeout(() => {
      el.style.opacity = '0';
      el.style.transform = 'translateY(-8px)';
      setTimeout(() => el.remove(), 260);
    }, 4200);
  }

  function initClock() {
    const el = qs('#clock');
    if (!el) return;
    const tick = () => {
      el.textContent = new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });
    };
    tick();
    setInterval(tick, 1000);
  }

  function initSidebar() {
    const toggle = qs('#sidebar-toggle');
    if (!toggle) return;
    const close = () => document.body.classList.remove('nav-open');
    toggle.addEventListener('click', () => document.body.classList.toggle('nav-open'));
    const backdrop = qs('#sidebar-backdrop');
    if (backdrop) backdrop.addEventListener('click', close);
    document.querySelectorAll('.nav-link').forEach((l) => {
      l.addEventListener('click', () => { if (window.innerWidth <= 900) close(); });
    });
  }

  function initFlash() {
    document.querySelectorAll('.flash').forEach((el) => {
      const btn = el.querySelector('.dismiss');
      if (btn) btn.addEventListener('click', () => el.remove());
      setTimeout(() => {
        el.style.opacity = '0';
        setTimeout(() => el.remove(), 320);
      }, 6000);
    });
  }

  function initConfirmForms() {
    document.querySelectorAll('form[data-confirm]').forEach((form) => {
      form.addEventListener('submit', (e) => {
        const msg = form.getAttribute('data-confirm');
        if (msg && !window.confirm(msg)) {
          e.preventDefault();
          return;
        }
        const btn = form.querySelector('button[type=submit]');
        if (btn) {
          btn.disabled = true;
          btn.textContent = 'Sending...';
        }
      });
    });
  }

  function fmtBytes(b) {
    if (b == null) return '';
    if (b >= 1048576) return (b / 1048576).toFixed(1) + ' MB';
    if (b >= 1024) return (b / 1024).toFixed(0) + ' KB';
    return b + ' B';
  }

  function fmtUptime(s) {
    s = Math.floor(s || 0);
    const d = Math.floor(s / 86400);
    const h = Math.floor((s % 86400) / 3600);
    const m = Math.floor((s % 3600) / 60);
    if (d) return d + 'd ' + h + 'h';
    if (h) return h + 'h ' + m + 'm';
    return m + 'm';
  }

  function statusChips(j) {
    const parts = [];
    if (j.ip) parts.push('<span class="chip">IP ' + esc(j.ip) + '</span>');
    parts.push('<span class="badge ' + (j.sprayActive ? 'badge-amber' : 'badge-no') + '">Spray ' +
      (j.sprayActive ? 'ACTIVE' : 'off') + '</span>');
    if (typeof j.lastConfidence === 'number') {
      parts.push('<span class="chip">Conf ' + j.lastConfidence.toFixed(2) + '</span>');
    }
    if (j.model) {
      parts.push('<span class="chip">' + esc(j.model) + '</span>');
    }
    if (typeof j.fps === 'number' && j.fps > 0) {
      parts.push('<span class="chip">' + j.fps + ' FPS</span>');
    }
    if (typeof j.heap === 'number' && j.heap > 0) {
      parts.push('<span class="chip">Heap ' + fmtBytes(j.heap) + '</span>');
    }
    if (typeof j.uptimeS === 'number') {
      parts.push('<span class="chip">Up ' + fmtUptime(j.uptimeS) + '</span>');
    }
    return parts.join('');
  }

  function initStatusPoll(opts) {
    const chipsEl = qs(opts.chipsEl);
    const bannerEl = opts.bannerEl ? qs(opts.bannerEl) : null;
    if (!chipsEl) return;
    async function tick() {
      try {
        const res = await fetch(opts.url);
        if (!res.ok) throw new Error(res.status);
        const j = await res.json();
        chipsEl.innerHTML = statusChips(j);
        if (bannerEl) bannerEl.hidden = true;
      } catch (err) {
        chipsEl.innerHTML = '<span class="chip chip-off">Device unreachable</span>';
        if (bannerEl) bannerEl.hidden = false;
      }
    }
    tick();
    setInterval(tick, opts.interval || 5000);
  }

  function confRow(r) {
    const conf = Number(r.confidence || 0);
    const pct = Math.round(conf * 100);
    const trig = r.triggered ? 'Yes' : 'No';
    const badge = trig === 'Yes' ? 'badge-yes' : 'badge-no';
    const verified = r.verified
      ? '<span class="chip chip-verified">✓ ' + (Number(r.yolo_conf) || 0).toFixed(2) + '</span>'
      : '<span class="muted">—</span>';
    return '<tr>' +
      '<td class="num">' + r.id + '</td>' +
      '<td>' + esc(r.event_time || '') + '</td>' +
      '<td><div class="conf"><span class="conf-text">' + conf.toFixed(2) +
      '</span><span class="conf-bar"><i style="width:' + pct + '%"></i></span></div></td>' +
      '<td><span class="badge ' + badge + '">' + trig + '</span></td>' +
      '<td class="num">' + (Number(r.spray_duration) || 0) + '</td>' +
      '<td><span class="chip">' + esc(r.source_ip || '') + '</span></td>' +
      '<td><span class="chip">' + esc(r.model_version || 'fomo:v1') + '</span></td>' +
      '<td>' + verified + '</td>' +
      '<td class="muted">' + esc(r.notes || '') + '</td>' +
      '</tr>';
  }

  function initDashboardRefresh(url, interval) {
    async function tick() {
      try {
        const res = await fetch(url);
        if (!res.ok) throw new Error(res.status);
        const j = await res.json();
        const set = (sel, v) => { const el = qs(sel); if (el) el.textContent = v; };
        set('#stat-total', j.total);
        set('#stat-triggered', j.triggered);
        set('#stat-today', j.today);
        set('#stat-avg', (Number(j.avgConf) || 0).toFixed(2));
        const tb = qs('#recent-tbody');
        if (tb) {
          if (Array.isArray(j.recent) && j.recent.length) {
            tb.innerHTML = j.recent.map(confRow).join('');
          } else {
            tb.innerHTML = '<tr><td colspan="9" class="empty">No detections yet.</td></tr>';
          }
        }
      } catch (e) { /* keep last view */ }
    }
    tick();
    setInterval(tick, interval || 10000);
  }

  window.__dash = { toast, initStatusPoll, initDashboardRefresh };

  document.addEventListener('DOMContentLoaded', () => {
    initClock();
    initSidebar();
    initFlash();
    initConfirmForms();
  });
})();

<?php $pageTitle = isset($pageTitle) ? $pageTitle : 'Dashboard'; ?>
<?php $pageSlug = isset($pageSlug) ? $pageSlug : 'dashboard'; ?>
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title><?= htmlspecialchars($pageTitle) ?> · Pigpen Feces Detector</title>
<link rel="stylesheet" href="<?= BASE_URL ?>/assets/css/style.css">
</head>
<body>
<button class="sidebar-backdrop" id="sidebar-backdrop" type="button" aria-label="Close menu"></button>

<aside class="sidebar">
  <div class="brand">
    <div class="logo">
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 2.7C12 2.7 5.5 9.6 5.5 14.5a6.5 6.5 0 0 0 13 0C18.5 9.6 12 2.7 12 2.7z"/></svg>
    </div>
    <div class="brand-text">Pigpen<span>Feces Detector · YOLOv8</span></div>
  </div>

  <nav class="nav">
    <a class="nav-link <?= $pageSlug === 'dashboard' ? 'active' : '' ?>" href="<?= BASE_URL ?>/index.php">
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="3" width="7" height="7" rx="1.5"/><rect x="14" y="3" width="7" height="7" rx="1.5"/><rect x="3" y="14" width="7" height="7" rx="1.5"/><rect x="14" y="14" width="7" height="7" rx="1.5"/></svg>
      <span>Dashboard</span>
    </a>
    <a class="nav-link <?= $pageSlug === 'history' ? 'active' : '' ?>" href="<?= BASE_URL ?>/history.php">
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><line x1="8" y1="6" x2="21" y2="6"/><line x1="8" y1="12" x2="21" y2="12"/><line x1="8" y1="18" x2="21" y2="18"/><circle cx="4" cy="6" r="1.2" fill="currentColor"/><circle cx="4" cy="12" r="1.2" fill="currentColor"/><circle cx="4" cy="18" r="1.2" fill="currentColor"/></svg>
      <span>History</span>
    </a>
    <a class="nav-link <?= $pageSlug === 'live' ? 'active' : '' ?>" href="<?= BASE_URL ?>/live.php">
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="9"/><polygon points="10 8.5 16 12 10 15.5" fill="currentColor"/></svg>
      <span>Live</span>
    </a>
    <a class="nav-link <?= $pageSlug === 'control' ? 'active' : '' ?>" href="<?= BASE_URL ?>/control.php">
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><line x1="4" y1="7" x2="20" y2="7"/><circle cx="9" cy="7" r="2.2" fill="currentColor"/><line x1="4" y1="17" x2="20" y2="17"/><circle cx="15" cy="17" r="2.2" fill="currentColor"/></svg>
      <span>Control</span>
    </a>
  </nav>

  <div class="sidebar-foot">
    <a class="nav-link" href="<?= htmlspecialchars(stream_url()) ?>" target="_blank" rel="noopener">
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M3 8a2 2 0 0 1 2-2h10a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/><path d="M17 10l4-2.5v9L17 14"/></svg>
      <span>Camera stream</span>
    </a>
  </div>
</aside>

<div class="main">
  <header class="topbar">
    <button class="icon-btn" id="sidebar-toggle" type="button" aria-label="Toggle menu">
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><line x1="3" y1="6" x2="21" y2="6"/><line x1="3" y1="12" x2="21" y2="12"/><line x1="3" y1="18" x2="21" y2="18"/></svg>
    </button>
    <h1 class="page-title"><?= htmlspecialchars($pageTitle) ?></h1>
    <div class="topbar-right"><span class="clock" id="clock"></span></div>
  </header>

  <main class="content">

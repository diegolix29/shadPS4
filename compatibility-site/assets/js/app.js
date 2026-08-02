(() => {
  'use strict';

  const INDEX_URL = 'data/site-index.json';
  const RELEASES_URL = 'data/releases.json';
  const GAME_URL = cusaId => `data/games/${encodeURIComponent(cusaId)}.json`;
  const PLACEHOLDER = 'assets/placeholder.svg';
  const STATUS_ORDER = { playable: 0, ingame: 1, menus: 2, boots: 3, nothing: 4, unknown: 5 };
  const STATUS_LABEL = { playable: 'Playable', ingame: 'Ingame', menus: 'Menus', boots: 'Boots', nothing: 'Nothing', unknown: 'Unknown' };

  const state = {
    index: null,
    releases: [],
    releaseMap: new Map(),
    games: [],
    details: new Map(),
    query: '',
    release: 'all',
    status: 'all',
    device: 'all',
    driver: 'all',
    gpu: 'all',
    sort: 'recent',
    visibleLimit: 48
  };

  const els = {
    themeToggle: document.querySelector('#theme-toggle'),
    search: document.querySelector('#search'),
    release: document.querySelector('#release-filter'),
    status: document.querySelector('#status-filter'),
    device: document.querySelector('#device-filter'),
    driver: document.querySelector('#driver-filter'),
    gpu: document.querySelector('#gpu-filter'),
    sort: document.querySelector('#sort-filter'),
    reset: document.querySelector('#reset-filters'),
    grid: document.querySelector('#game-grid'),
    empty: document.querySelector('#empty-state'),
    emptyTitle: document.querySelector('#empty-title'),
    emptyCopy: document.querySelector('#empty-copy'),
    resultCount: document.querySelector('#result-count'),
    activeSummary: document.querySelector('#active-filter-summary'),
    meta: document.querySelector('#database-meta'),
    total: document.querySelector('#stat-total'),
    playable: document.querySelector('#stat-playable'),
    ingame: document.querySelector('#stat-ingame'),
    devices: document.querySelector('#stat-devices'),
    template: document.querySelector('#game-card-template'),
    dialog: document.querySelector('#game-dialog'),
    dialogContent: document.querySelector('#dialog-content'),
    dialogClose: document.querySelector('#dialog-close'),
    loadMore: document.querySelector('#load-more')
  };

  function text(value, fallback = 'Not recorded') {
    const normalized = value === null || value === undefined ? '' : String(value).trim();
    return normalized || fallback;
  }

  function normalizeStatus(value) {
    const normalized = String(value || 'unknown').toLowerCase();
    return Object.hasOwn(STATUS_ORDER, normalized) ? normalized : 'unknown';
  }

  function parseDate(value) {
    const parsed = new Date(value || 0);
    return Number.isNaN(parsed.getTime()) ? new Date(0) : parsed;
  }

  function formatDate(value, options = { year: 'numeric', month: 'short', day: 'numeric' }) {
    const parsed = parseDate(value);
    return parsed.getTime() ? new Intl.DateTimeFormat(undefined, options).format(parsed) : 'Unknown date';
  }

  function releaseInfo(tag) {
    return state.releaseMap.get(tag) || { tag, name: tag, url: '' };
  }

  function releaseLabel(tag) {
    const release = releaseInfo(tag);
    return text(release.name || release.tag, tag || 'Unknown release');
  }

  function driverKey(report) {
    const driver = report?.driver || {};
    return [driver.type, driver.name, driver.version, driver.build].filter(Boolean).join('|').toLowerCase();
  }

  function driverLabel(report) {
    const driver = report?.driver || {};
    return text(driver.display || [driver.name || driver.type, driver.version, driver.build].filter(Boolean).join(' '), 'Unknown driver');
  }

  function deviceName(report) {
    return text(report?.device?.label, [report?.device?.manufacturer, report?.device?.model].filter(Boolean).join(' ') || 'Unknown device');
  }

  function averageFps(report) {
    const value = Number(report?.performance?.averageFps);
    return Number.isFinite(value) ? value : -1;
  }

  function formatFps(report) {
    const value = averageFps(report);
    return value >= 0 ? `${value.toFixed(value >= 10 ? 1 : 2)} FPS` : 'Not measured';
  }

  function candidateReports(game) {
    return (game.reports || []).filter(report => {
      if (state.release !== 'all' && report.releaseTag !== state.release) return false;
      if (state.status !== 'all' && normalizeStatus(report.status) !== state.status) return false;
      if (state.device !== 'all' && deviceName(report) !== state.device) return false;
      if (state.driver !== 'all' && driverKey(report) !== state.driver) return false;
      if (state.gpu !== 'all' && text(report.device?.gpu, '') !== state.gpu) return false;
      return true;
    });
  }

  function representativeReport(game) {
    const candidates = candidateReports(game);
    return candidates.sort((a, b) => parseDate(b.testedAt) - parseDate(a.testedAt))[0] || null;
  }

  function searchable(game) {
    const reports = game.reports || [];
    return [game.title, game.cusaId, game.region, game.publisher,
      ...reports.flatMap(report => [report.releaseTag, report.status, report.summary, deviceName(report), report.device?.soc, report.device?.gpu, driverLabel(report)])]
      .filter(Boolean).join(' ').toLowerCase();
  }

  function setTheme(theme) {
    document.documentElement.dataset.theme = theme;
    localStorage.setItem('bachata-compat-theme', theme);
    els.themeToggle.setAttribute('aria-label', `Switch to ${theme === 'dark' ? 'light' : 'dark'} theme`);
  }

  function initializeTheme() {
    const saved = localStorage.getItem('bachata-compat-theme');
    const preferred = matchMedia('(prefers-color-scheme: light)').matches ? 'light' : 'dark';
    setTheme(saved || preferred);
    els.themeToggle.addEventListener('click', () => setTheme(document.documentElement.dataset.theme === 'dark' ? 'light' : 'dark'));
  }

  function addOption(select, value, label) {
    const option = document.createElement('option');
    option.value = value;
    option.textContent = label;
    select.append(option);
  }

  function populateFilters() {
    state.releases.forEach(release => addOption(els.release, release.tag, `${release.name || release.tag}${release.latest ? ' · Latest' : ''}`));
    const reports = state.games.flatMap(game => game.reports || []);
    [...new Set(reports.map(deviceName))].sort().forEach(value => addOption(els.device, value, value));
    const drivers = new Map();
    reports.forEach(report => drivers.set(driverKey(report), driverLabel(report)));
    [...drivers.entries()].filter(([key]) => key).sort((a, b) => a[1].localeCompare(b[1])).forEach(([key, label]) => addOption(els.driver, key, label));
    [...new Set(reports.map(report => text(report.device?.gpu, '')).filter(Boolean))].sort().forEach(value => addOption(els.gpu, value, value));
    const requestedRelease = new URL(location.href).searchParams.get('release');
    if (requestedRelease && (requestedRelease === 'all' || state.releaseMap.has(requestedRelease))) {
      state.release = requestedRelease;
      els.release.value = requestedRelease;
    }
  }

  function updateStats() {
    const stats = state.index?.stats || {};
    els.total.textContent = stats.games ?? state.games.length;
    els.playable.textContent = stats.playable ?? state.games.filter(game => game.bestStatus === 'playable').length;
    els.ingame.textContent = stats.ingame ?? state.games.filter(game => game.bestStatus === 'ingame').length;
    els.devices.textContent = stats.devices ?? new Set(state.games.flatMap(game => (game.reports || []).map(deviceName))).size;
  }

  function filteredGames() {
    const query = state.query.trim().toLowerCase();
    const values = state.games
      .filter(game => candidateReports(game).length > 0)
      .filter(game => !query || searchable(game).includes(query))
      .map(game => ({ game, report: representativeReport(game) }));
    values.sort((a, b) => {
      if (state.sort === 'title') return a.game.title.localeCompare(b.game.title);
      if (state.sort === 'status') return STATUS_ORDER[normalizeStatus(a.report?.status)] - STATUS_ORDER[normalizeStatus(b.report?.status)] || a.game.title.localeCompare(b.game.title);
      if (state.sort === 'fps') return averageFps(b.report) - averageFps(a.report) || a.game.title.localeCompare(b.game.title);
      return parseDate(b.report?.testedAt) - parseDate(a.report?.testedAt) || a.game.title.localeCompare(b.game.title);
    });
    return values;
  }

  function createCard({ game, report }) {
    const fragment = els.template.content.cloneNode(true);
    const button = fragment.querySelector('.game-card-button');
    const issueLink = fragment.querySelector('.game-issue-link');
    const image = fragment.querySelector('.game-image');
    const status = normalizeStatus(report?.status || game.bestStatus);
    image.src = text(report?.thumbnail || game.thumbnail, PLACEHOLDER);
    image.alt = `${game.title} compatibility screenshot`;
    image.addEventListener('error', () => { image.src = PLACEHOLDER; }, { once: true });
    const badge = fragment.querySelector('.status-badge');
    badge.className = `status-badge ${status}`;
    badge.textContent = STATUS_LABEL[status];
    const count = fragment.querySelector('.screenshot-count');
    count.hidden = false;
    count.textContent = `${game.reportCount} ${game.reportCount === 1 ? 'report' : 'reports'}`;
    fragment.querySelector('h3').textContent = game.title;
    fragment.querySelector('.serial').textContent = game.cusaId;
    fragment.querySelector('.game-note').textContent = text(report?.summary, `${game.reportCount} reports across ${game.deviceCount} devices.`);
    fragment.querySelector('.release').textContent = releaseLabel(report?.releaseTag);
    fragment.querySelector('.device').textContent = deviceName(report);
    fragment.querySelector('.driver').textContent = driverLabel(report);
    fragment.querySelector('.fps').textContent = formatFps(report);
    button.setAttribute('aria-label', `Open all ${game.reportCount} ${game.reportCount === 1 ? 'report' : 'reports'} for ${game.title}`);
    button.addEventListener('click', () => openGame(game.cusaId, report?.reportId));
    if (issueLink) {
      if (game.issueUrl) {
        issueLink.href = game.issueUrl;
        issueLink.textContent = game.issueNumber ? `Open discussion #${game.issueNumber} ↗` : 'Open game discussion ↗';
        issueLink.setAttribute('aria-label', `Open the GitHub discussion for ${game.title}`);
      } else {
        issueLink.hidden = true;
      }
    }
    return fragment;
  }

  function activeFilterText() {
    const values = [];
    if (state.release !== 'all') values.push(releaseLabel(state.release));
    if (state.status !== 'all') values.push(STATUS_LABEL[state.status]);
    if (state.device !== 'all') values.push(state.device);
    if (state.driver !== 'all') values.push(els.driver.selectedOptions[0]?.textContent);
    if (state.gpu !== 'all') values.push(state.gpu);
    return values.filter(Boolean).join(' · ');
  }

  function render() {
    updateStats();
    const values = filteredGames();
    const visible = values.slice(0, state.visibleLimit);
    els.grid.replaceChildren(...visible.map(createCard));
    els.grid.hidden = values.length === 0;
    els.grid.setAttribute('aria-busy', 'false');
    els.empty.hidden = values.length !== 0;
    els.loadMore.hidden = visible.length >= values.length;
    els.loadMore.textContent = `Load more games · ${values.length - visible.length} remaining`;
    els.resultCount.textContent = `${values.length} ${values.length === 1 ? 'game' : 'games'} · ${values.reduce((sum, value) => sum + value.game.reportCount, 0)} reports`;
    els.activeSummary.textContent = activeFilterText();
    const generated = state.index?.generatedAt ? `Generated ${formatDate(state.index.generatedAt, { year: 'numeric', month: 'short', day: 'numeric', hour: 'numeric', minute: '2-digit' })}` : '';
    els.meta.textContent = `${state.games.length} CUSA entries · ${state.index?.stats?.reports ?? 0} reports${generated ? ` · ${generated}` : ''}`;
  }

  function definitionList(items) {
    const list = document.createElement('dl');
    list.className = 'detail-list';
    items.filter(([, value]) => value !== null && value !== undefined && String(value).trim()).forEach(([label, value]) => {
      const row = document.createElement('div');
      const dt = document.createElement('dt'); dt.textContent = label;
      const dd = document.createElement('dd'); dd.textContent = value;
      row.append(dt, dd); list.append(row);
    });
    return list;
  }

  function section(title, content) {
    const wrapper = document.createElement('section');
    wrapper.className = 'dialog-section';
    const heading = document.createElement('h3'); heading.textContent = title;
    wrapper.append(heading, content);
    return wrapper;
  }

  function reportFor(detail, requestedId) {
    const reports = detail.reports || [];
    if (requestedId) {
      const exact = reports.find(report => report.reportId === requestedId);
      if (exact) return exact;
    }
    const summary = state.games.find(game => game.cusaId === detail.game.cusaId);
    const selected = summary ? representativeReport(summary) : null;
    return reports.find(report => report.reportId === selected?.reportId) || reports[0];
  }

  async function loadGame(cusaId) {
    if (state.details.has(cusaId)) return state.details.get(cusaId);
    const detail = await fetchJson(GAME_URL(cusaId));
    if (!detail?.game || !Array.isArray(detail.reports)) throw new Error(`Invalid game detail: ${cusaId}`);
    state.details.set(cusaId, detail);
    return detail;
  }

  function makeReportSelector(detail, activeReport) {
    const wrapper = document.createElement('label');
    wrapper.className = 'report-selector';
    const label = document.createElement('span'); label.textContent = 'Viewing report';
    const select = document.createElement('select');
    detail.reports.forEach(report => {
      const option = document.createElement('option');
      option.value = report.reportId;
      option.textContent = `${report.release.tag} · ${STATUS_LABEL[normalizeStatus(report.status)]} · ${deviceName(report)} · ${driverLabel(report)} · ${formatDate(report.testedAt)}`;
      option.selected = report.reportId === activeReport.reportId;
      select.append(option);
    });
    select.addEventListener('change', () => renderDialog(detail, detail.reports.find(report => report.reportId === select.value), true));
    wrapper.append(label, select);
    return wrapper;
  }

  function renderDialog(detail, report, updateUrl = true) {
    const game = detail.game;
    const status = normalizeStatus(report.status);
    const screenshots = report.evidence?.screenshots || [];
    const release = releaseInfo(report.release?.tag);
    const hero = document.createElement('div'); hero.className = 'dialog-hero';
    const image = document.createElement('img'); image.src = screenshots.at(-1)?.url || PLACEHOLDER; image.alt = `${game.title} screenshot`; image.addEventListener('error', () => { image.src = PLACEHOLDER; }, { once: true });
    const heading = document.createElement('div'); heading.className = 'dialog-heading';
    const badge = document.createElement('span'); badge.className = `status-badge ${status}`; badge.textContent = STATUS_LABEL[status];
    const title = document.createElement('h2'); title.id = 'dialog-title'; title.textContent = game.title;
    const subtitle = document.createElement('p'); subtitle.textContent = [game.cusaId, game.region, game.publisher].filter(Boolean).join(' · ');
    heading.append(badge, title, subtitle, makeReportSelector(detail, report));
    if (game.issueUrl) {
      const issue = document.createElement('a'); issue.className = 'button button-secondary'; issue.href = game.issueUrl; issue.target = '_blank'; issue.rel = 'noreferrer'; issue.textContent = `Discuss in issue #${game.issueNumber}`;
      heading.append(issue);
    }
    hero.append(image, heading);

    const body = document.createElement('div'); body.className = 'dialog-body';
    const grid = document.createElement('div'); grid.className = 'dialog-grid';
    const main = document.createElement('div'); const side = document.createElement('aside');
    const notes = document.createElement('p'); notes.textContent = text(report.notes || report.summary, 'No detailed notes were recorded.');
    main.append(section(`Test notes · ${report.release.tag}`, notes));
    if (Array.isArray(report.issues) && report.issues.length) {
      const list = document.createElement('ul'); list.className = 'issue-list';
      report.issues.forEach(value => { const li = document.createElement('li'); li.textContent = value; list.append(li); });
      main.append(section('Known issues', list));
    }
    if (screenshots.length) {
      const gallery = document.createElement('div'); gallery.className = 'screenshot-gallery';
      screenshots.forEach((shot, index) => {
        const link = document.createElement('a'); link.href = shot.url; link.target = '_blank'; link.rel = 'noreferrer'; link.title = text(shot.caption, `Screenshot ${index + 1}`);
        const shotImage = document.createElement('img'); shotImage.loading = 'lazy'; shotImage.src = shot.url; shotImage.alt = text(shot.caption, `Screenshot ${index + 1}`); shotImage.addEventListener('error', () => { shotImage.src = PLACEHOLDER; }, { once: true });
        link.append(shotImage); gallery.append(link);
      });
      main.append(section('Screenshots', gallery));
    }

    const performance = report.performance || {}; const device = report.device || {}; const driver = report.driver || {};
    side.append(section('Test environment', definitionList([
      ['Release', releaseLabel(report.release.tag)], ['Selected device', deviceName(report)], ['SoC', device.soc], ['GPU', device.gpu],
      ['Android', device.androidVersion], ['RAM', device.ramGb ? `${device.ramGb} GB` : ''], ['Driver type', driver.type], ['Driver', driver.name],
      ['Driver version', driver.version], ['Driver build', driver.build], ['Driver source', driver.source], ['Guest backend', report.guestBackend],
      ['Installed version', report.emulatorVersion], ['Commit', report.release.commit?.slice(0, 12)], ['Tested by', report.tester], ['Tested', formatDate(report.testedAt)]
    ])));
    side.append(section('Performance', definitionList([
      ['Average FPS', Number.isFinite(Number(performance.averageFps)) ? performance.averageFps : ''],
      ['FPS range', Number.isFinite(Number(performance.minimumFps)) && Number.isFinite(Number(performance.maximumFps)) ? `${performance.minimumFps}–${performance.maximumFps}` : ''],
      ['Frame pacing', performance.framePacing], ['Resolution scale', report.settings?.resolutionScale ? `${report.settings.resolutionScale}×` : ''],
      ['Runtime', performance.testDurationSeconds ? `${performance.testDurationSeconds}s` : '']
    ])));
    const evidence = [];
    if (release.url || report.release.url) evidence.push([`${report.release.tag} release`, release.url || report.release.url]);
    (report.evidence?.logs || []).forEach(log => evidence.push([text(log.label, 'Session log'), log.url]));
    if (evidence.length) {
      const links = document.createElement('div'); links.className = 'link-list';
      evidence.forEach(([label, url]) => { const link = document.createElement('a'); link.className = 'evidence-link'; link.href = url; link.target = '_blank'; link.rel = 'noreferrer'; const left = document.createElement('span'); left.textContent = label; const right = document.createElement('span'); right.textContent = 'Open ↗'; link.append(left, right); links.append(link); });
      side.append(section('Evidence', links));
    }

    const history = document.createElement('div'); history.className = 'report-history';
    detail.reports.forEach(entry => {
      const button = document.createElement('button'); button.type = 'button'; button.className = `report-row${entry.reportId === report.reportId ? ' active' : ''}`;
      const left = document.createElement('span'); left.className = 'report-row-copy';
      const strong = document.createElement('strong'); strong.textContent = `${entry.release.tag} · ${formatDate(entry.testedAt)}`;
      const environment = document.createElement('small'); environment.textContent = `${deviceName(entry)} · ${driverLabel(entry)}`;
      left.append(strong, environment);
      const right = document.createElement('span'); right.className = `status-text ${normalizeStatus(entry.status)}`; right.textContent = STATUS_LABEL[normalizeStatus(entry.status)];
      button.append(left, right); button.addEventListener('click', () => renderDialog(detail, entry, true)); history.append(button);
    });
    main.append(section(`All reports for ${game.cusaId}`, history));
    grid.append(main, side); body.append(grid); els.dialogContent.replaceChildren(hero, body);
    if (!els.dialog.open) els.dialog.showModal();
    document.body.classList.add('dialog-open');
    if (updateUrl) {
      const url = new URL(location.href); url.searchParams.set('game', game.cusaId); url.searchParams.set('report', report.reportId);
      window.history.replaceState({}, '', url);
    }
  }

  async function openGame(cusaId, reportId, updateUrl = true) {
    els.dialogContent.innerHTML = '<div class="dialog-loading"><strong>Loading reports…</strong></div>';
    if (!els.dialog.open) els.dialog.showModal();
    document.body.classList.add('dialog-open');
    try {
      const detail = await loadGame(cusaId);
      const report = reportFor(detail, reportId);
      if (!report) throw new Error('No reports are available for this game');
      renderDialog(detail, report, updateUrl);
    } catch (error) {
      console.error(error);
      els.dialogContent.innerHTML = '<div class="dialog-loading"><strong>Could not load this game.</strong><p>Refresh the page or try again later.</p></div>';
    }
  }

  function closeGame(updateUrl = true) {
    if (els.dialog.open) els.dialog.close();
    document.body.classList.remove('dialog-open');
    if (updateUrl) { const url = new URL(location.href); url.searchParams.delete('game'); url.searchParams.delete('report'); window.history.replaceState({}, '', url); }
  }

  function openGameFromUrl() {
    const url = new URL(location.href); const cusaId = url.searchParams.get('game');
    if (cusaId && state.games.some(game => game.cusaId.toLowerCase() === cusaId.toLowerCase())) openGame(cusaId.toUpperCase(), url.searchParams.get('report'), false);
  }

  function updateReleaseUrl() {
    const url = new URL(location.href);
    if (state.release === 'all') url.searchParams.delete('release'); else url.searchParams.set('release', state.release);
    window.history.replaceState({}, '', url);
  }

  function bindFilters() {
    els.search.addEventListener('input', event => { state.query = event.target.value; state.visibleLimit = 48; render(); });
    els.release.addEventListener('change', event => { state.release = event.target.value; state.visibleLimit = 48; updateReleaseUrl(); render(); });
    els.status.addEventListener('change', event => { state.status = event.target.value; state.visibleLimit = 48; render(); });
    els.device.addEventListener('change', event => { state.device = event.target.value; state.visibleLimit = 48; render(); });
    els.driver.addEventListener('change', event => { state.driver = event.target.value; state.visibleLimit = 48; render(); });
    els.gpu.addEventListener('change', event => { state.gpu = event.target.value; state.visibleLimit = 48; render(); });
    els.sort.addEventListener('change', event => { state.sort = event.target.value; state.visibleLimit = 48; render(); });
    els.reset.addEventListener('click', () => {
      state.query = ''; state.release = state.status = state.device = state.driver = state.gpu = 'all'; state.sort = 'recent'; state.visibleLimit = 48;
      els.search.value = ''; els.release.value = els.status.value = els.device.value = els.driver.value = els.gpu.value = 'all'; els.sort.value = 'recent';
      updateReleaseUrl(); render();
    });
    els.loadMore.addEventListener('click', () => { state.visibleLimit += 48; render(); });
    els.dialogClose.addEventListener('click', () => closeGame());
    els.dialog.addEventListener('cancel', event => { event.preventDefault(); closeGame(); });
    els.dialog.addEventListener('click', event => { if (event.target === els.dialog) closeGame(); });
  }

  async function fetchJson(url) {
    const response = await fetch(url, { cache: 'no-store' });
    if (!response.ok) throw new Error(`${url}: HTTP ${response.status}`);
    return response.json();
  }

  async function loadDatabase() {
    try {
      const [index, releaseIndex] = await Promise.all([fetchJson(INDEX_URL), fetchJson(RELEASES_URL)]);
      if (!index || !Array.isArray(index.games)) throw new Error('site-index games must be an array');
      if (!releaseIndex || !Array.isArray(releaseIndex.releases)) throw new Error('releases must be an array');
      state.index = index; state.games = index.games; state.releases = releaseIndex.releases;
      state.releaseMap = new Map(state.releases.map(release => [release.tag, release]));
      populateFilters(); render(); openGameFromUrl();
    } catch (error) {
      console.error('Compatibility database failed to load:', error);
      els.grid.setAttribute('aria-busy', 'false'); els.grid.hidden = true; els.empty.hidden = false;
      els.emptyTitle.textContent = 'Compatibility data could not be loaded';
      els.emptyCopy.textContent = 'Confirm the Pages workflow generated data/site-index.json and the per-game JSON files.';
      els.meta.textContent = 'Database unavailable';
      els.total.textContent = els.playable.textContent = els.ingame.textContent = els.devices.textContent = '—';
    }
  }

  initializeTheme(); bindFilters(); loadDatabase();
})();

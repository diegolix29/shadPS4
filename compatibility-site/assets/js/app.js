(() => {
  'use strict';

  const DATA_URL = 'data/games.json';
  const RELEASES_URL = 'data/releases.json';
  const PLACEHOLDER = 'assets/placeholder.svg';
  const STATUS_ORDER = { playable: 0, ingame: 1, menus: 2, boots: 3, nothing: 4, unknown: 5 };
  const STATUS_LABEL = { playable: 'Playable', ingame: 'Ingame', menus: 'Menus', boots: 'Boots', nothing: 'Nothing', unknown: 'Unknown' };

  const state = {
    database: null,
    releases: [],
    releaseMap: new Map(),
    games: [],
    query: '',
    release: 'all',
    status: 'all',
    device: 'all',
    driver: 'all',
    gpu: 'all',
    sort: 'recent'
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
    count: document.querySelector('#result-count'),
    activeSummary: document.querySelector('#active-filter-summary'),
    meta: document.querySelector('#database-meta'),
    total: document.querySelector('#stat-total'),
    playable: document.querySelector('#stat-playable'),
    ingame: document.querySelector('#stat-ingame'),
    devices: document.querySelector('#stat-devices'),
    template: document.querySelector('#game-card-template'),
    dialog: document.querySelector('#game-dialog'),
    dialogClose: document.querySelector('#dialog-close'),
    dialogContent: document.querySelector('#dialog-content')
  };

  function text(value, fallback = 'Not recorded') {
    const output = String(value ?? '').trim();
    return output || fallback;
  }

  function normalizeStatus(value) {
    const status = String(value || 'unknown').toLowerCase();
    return Object.hasOwn(STATUS_ORDER, status) ? status : 'unknown';
  }

  function parseDate(value) {
    const date = new Date(value || 0);
    return Number.isNaN(date.getTime()) ? new Date(0) : date;
  }

  function formatDate(value, options = { year: 'numeric', month: 'short', day: 'numeric' }) {
    const date = parseDate(value);
    if (date.getTime() === 0) return 'Unknown';
    return new Intl.DateTimeFormat(undefined, options).format(date);
  }

  function allTests(game) {
    const tests = Array.isArray(game.tests) ? [...game.tests] : [];
    return tests.sort((a, b) => parseDate(b.testedAt) - parseDate(a.testedAt));
  }

  function deviceName(test) {
    const label = text(test?.device?.label, '');
    if (label) return label;
    return `${text(test?.device?.manufacturer, '')} ${text(test?.device?.model, '')}`.trim() || 'Unknown device';
  }

  function gpuFamily(test) {
    const gpu = text(test?.device?.gpu, 'Unknown');
    if (/adreno/i.test(gpu)) return 'Adreno';
    if (/mali/i.test(gpu)) return 'Mali';
    if (/xclipse/i.test(gpu)) return 'Xclipse';
    if (/immortalis/i.test(gpu)) return 'Immortalis';
    return gpu;
  }

  function driverKey(test) {
    const renderer = test?.renderer || {};
    const type = text(renderer.driverType, 'custom').toLowerCase();
    if (type === 'turnip') return `turnip:${text(renderer.turnipVersion, 'unknown')}`;
    return `${type}:${text(renderer.driverVersion || renderer.driver, 'unknown')}`;
  }

  function driverLabel(test) {
    const renderer = test?.renderer || {};
    if (renderer.driverType === 'turnip') {
      const build = renderer.turnipBuild ? ` (${renderer.turnipBuild})` : '';
      return `Turnip ${text(renderer.turnipVersion, 'unknown')}${build}`;
    }
    if (renderer.driverType === 'system') return `System · ${text(renderer.driverVersion || renderer.driver)}`;
    return [renderer.driver, renderer.driverVersion].filter(Boolean).join(' ') || 'Custom driver';
  }

  function releaseInfo(tag) {
    return state.releaseMap.get(tag) || { tag, name: tag, url: '', prerelease: false, latest: false };
  }

  function releaseLabel(tag) {
    const release = releaseInfo(tag);
    return `${release.tag || tag}${release.latest ? ' · Latest' : ''}${release.prerelease ? ' · Pre-release' : ''}`;
  }

  function candidateTests(game) {
    return allTests(game).filter(test => {
      if (state.release !== 'all' && test.releaseTag !== state.release) return false;
      if (state.device !== 'all' && deviceName(test) !== state.device) return false;
      if (state.driver !== 'all' && driverKey(test) !== state.driver) return false;
      if (state.gpu !== 'all' && gpuFamily(test) !== state.gpu) return false;
      return true;
    });
  }

  function selectedTest(game) {
    return candidateTests(game)[0] || null;
  }

  function formatFps(performance) {
    if (!performance || typeof performance !== 'object') return '—';
    if (Number.isFinite(Number(performance.averageFps))) {
      const value = Number(performance.averageFps);
      return `${value.toFixed(value % 1 ? 1 : 0)} FPS`;
    }
    const min = Number(performance.minFps);
    const max = Number(performance.maxFps);
    if (Number.isFinite(min) && Number.isFinite(max)) return `${min}–${max} FPS`;
    return '—';
  }

  function averageFps(test) {
    const value = Number(test?.performance?.averageFps);
    return Number.isFinite(value) ? value : -1;
  }

  function screenshotPath(item) {
    if (typeof item === 'string') return item;
    return item && typeof item === 'object' ? item.path : '';
  }

  function screenshotCaption(item, index) {
    if (item && typeof item === 'object' && item.caption) return item.caption;
    return `Screenshot ${index + 1}`;
  }

  function searchableGame(game, test) {
    return [
      game.title, game.serial, game.region, game.publisher,
      test.status, test.gameVersion, test.releaseTag, test.emulatorVersion, test.commit,
      test.device?.label, test.device?.manufacturer, test.device?.model, test.device?.soc,
      test.device?.gpu, test.device?.androidVersion, test.renderer?.driverType,
      test.renderer?.driver, test.renderer?.driverVersion, test.renderer?.turnipVersion,
      test.renderer?.turnipBuild, test.renderer?.turnipSource, test.notes,
      ...(Array.isArray(test.issues) ? test.issues : [])
    ].filter(Boolean).join(' ').toLowerCase();
  }

  function setTheme(theme) {
    document.documentElement.dataset.theme = theme;
    localStorage.setItem('bachata-theme', theme);
    els.themeToggle.setAttribute('aria-label', theme === 'dark' ? 'Switch to light theme' : 'Switch to dark theme');
  }

  function initializeTheme() {
    const stored = localStorage.getItem('bachata-theme');
    const preferred = matchMedia('(prefers-color-scheme: light)').matches ? 'light' : 'dark';
    setTheme(stored || preferred);
    els.themeToggle.addEventListener('click', () => setTheme(document.documentElement.dataset.theme === 'dark' ? 'light' : 'dark'));
  }

  function addOption(select, value, label) {
    const option = document.createElement('option');
    option.value = value;
    option.textContent = label;
    select.append(option);
  }

  function populateFilters() {
    for (const release of state.releases) addOption(els.release, release.tag, releaseLabel(release.tag));
    const tests = state.games.flatMap(allTests);
    const devices = [...new Set(tests.map(deviceName).filter(name => name !== 'Unknown device'))].sort((a, b) => a.localeCompare(b));
    const drivers = new Map();
    for (const test of tests) drivers.set(driverKey(test), driverLabel(test));
    const gpus = [...new Set(tests.map(gpuFamily).filter(Boolean))].sort((a, b) => a.localeCompare(b));
    for (const value of devices) addOption(els.device, value, value);
    for (const [key, label] of [...drivers.entries()].sort((a, b) => a[1].localeCompare(b[1]))) addOption(els.driver, key, label);
    for (const value of gpus) addOption(els.gpu, value, value);

    const requested = new URL(location.href).searchParams.get('release');
    const latest = state.releases.find(release => release.latest)?.tag;
    state.release = requested && state.releaseMap.has(requested) ? requested : (latest || 'all');
    els.release.value = state.release;
  }

  function environmentTests() {
    return state.games.map(game => selectedTest(game)).filter(Boolean);
  }

  function updateStats() {
    const tests = environmentTests();
    els.total.textContent = String(tests.length);
    els.playable.textContent = String(tests.filter(test => normalizeStatus(test.status) === 'playable').length);
    els.ingame.textContent = String(tests.filter(test => normalizeStatus(test.status) === 'ingame').length);
    els.devices.textContent = String(new Set(tests.map(deviceName).filter(name => name !== 'Unknown device')).size);
  }

  function filteredGames() {
    const query = state.query.trim().toLowerCase();
    const rows = [];
    for (const game of state.games) {
      const test = selectedTest(game);
      if (!test) continue;
      if (query && !searchableGame(game, test).includes(query)) continue;
      if (state.status !== 'all' && normalizeStatus(test.status) !== state.status) continue;
      rows.push({ game, test });
    }
    rows.sort((a, b) => {
      if (state.sort === 'title') return text(a.game.title).localeCompare(text(b.game.title));
      if (state.sort === 'status') {
        const difference = STATUS_ORDER[normalizeStatus(a.test.status)] - STATUS_ORDER[normalizeStatus(b.test.status)];
        return difference || text(a.game.title).localeCompare(text(b.game.title));
      }
      if (state.sort === 'fps') return averageFps(b.test) - averageFps(a.test) || text(a.game.title).localeCompare(text(b.game.title));
      return parseDate(b.test.testedAt) - parseDate(a.test.testedAt) || text(a.game.title).localeCompare(text(b.game.title));
    });
    return rows;
  }

  function createCard({ game, test }) {
    const fragment = els.template.content.cloneNode(true);
    const status = normalizeStatus(test.status);
    const screenshots = Array.isArray(test.screenshots) ? test.screenshots.filter(screenshotPath) : [];
    const button = fragment.querySelector('.game-card-button');
    const image = fragment.querySelector('.game-image');
    const badge = fragment.querySelector('.status-badge');
    const screenshotCount = fragment.querySelector('.screenshot-count');

    image.src = screenshots.length ? screenshotPath(screenshots[0]) : PLACEHOLDER;
    image.alt = screenshots.length ? `${text(game.title)} gameplay screenshot` : '';
    image.addEventListener('error', () => { image.src = PLACEHOLDER; image.alt = ''; }, { once: true });
    badge.classList.add(status);
    badge.textContent = STATUS_LABEL[status];
    if (screenshots.length > 1) {
      screenshotCount.hidden = false;
      screenshotCount.textContent = `${screenshots.length} shots`;
    }

    fragment.querySelector('h3').textContent = text(game.title, 'Untitled game');
    fragment.querySelector('.serial').textContent = text(game.serial, 'NO ID');
    fragment.querySelector('.game-note').textContent = `${text(test.summary || test.notes, 'No test notes.')} · Tested ${formatDate(test.testedAt, { year: 'numeric', month: 'short' })}`;
    fragment.querySelector('.release').textContent = text(test.releaseTag);
    fragment.querySelector('.device').textContent = deviceName(test);
    fragment.querySelector('.driver').textContent = driverLabel(test);
    fragment.querySelector('.fps').textContent = formatFps(test.performance);

    button.setAttribute('aria-label', `Open ${text(game.title)} details for ${text(test.releaseTag)}`);
    button.addEventListener('click', () => openGame(game, test));
    return fragment;
  }

  function updateReleaseUrl() {
    const url = new URL(location.href);
    if (state.release === 'all') url.searchParams.delete('release');
    else url.searchParams.set('release', state.release);
    window.history.replaceState({}, '', url);
  }

  function render() {
    const rows = filteredGames();
    updateStats();
    els.grid.replaceChildren(...rows.map(createCard));
    els.grid.setAttribute('aria-busy', 'false');
    els.count.textContent = `${rows.length} ${rows.length === 1 ? 'game' : 'games'}`;

    const active = [];
    if (state.release !== 'all') active.push(state.release);
    if (state.status !== 'all') active.push(STATUS_LABEL[state.status]);
    if (state.device !== 'all') active.push(state.device);
    if (state.driver !== 'all') active.push(els.driver.selectedOptions[0]?.textContent || state.driver);
    if (state.gpu !== 'all') active.push(state.gpu);
    if (state.query.trim()) active.push(`“${state.query.trim()}”`);
    els.activeSummary.textContent = active.length ? `Filtered by ${active.join(' · ')}` : '';

    els.empty.hidden = rows.length !== 0;
    els.grid.hidden = rows.length === 0;
    if (rows.length === 0) {
      const hasReports = state.games.some(game => allTests(game).length);
      if (!hasReports) {
        els.emptyTitle.textContent = state.release === 'all' ? 'No compatibility reports yet' : `No reports for ${state.release} yet`;
        els.emptyCopy.textContent = 'Use the included agent skill to test an official release on a selected device and driver, capture evidence, and add the first JSON report.';
      } else {
        els.emptyTitle.textContent = 'No matching reports';
        els.emptyCopy.textContent = 'Try another release, device, Turnip version, GPU, status, or search term.';
      }
    }

    const releaseMeta = state.release === 'all' ? 'All GitHub releases' : releaseLabel(state.release);
    els.meta.textContent = `${releaseMeta} · JSON schema v${text(state.database?.schemaVersion, '2')} · Updated ${formatDate(state.database?.lastUpdated, { year: 'numeric', month: 'short', day: 'numeric' })}`;
  }

  function definitionList(items) {
    const dl = document.createElement('dl');
    dl.className = 'detail-list';
    for (const [label, value] of items) {
      if (value === undefined || value === null || value === '') continue;
      const wrap = document.createElement('div');
      const dt = document.createElement('dt');
      const dd = document.createElement('dd');
      dt.textContent = label;
      dd.textContent = text(value);
      wrap.append(dt, dd);
      dl.append(wrap);
    }
    return dl;
  }

  function section(title, content) {
    const wrap = document.createElement('section');
    wrap.className = 'dialog-section';
    const heading = document.createElement('h3');
    heading.textContent = title;
    wrap.append(heading, content);
    return wrap;
  }

  function openGame(game, test, updateUrl = true) {
    const tests = allTests(game);
    const status = normalizeStatus(test.status);
    const screenshots = Array.isArray(test.screenshots) ? test.screenshots.filter(screenshotPath) : [];
    const heroImage = screenshots.length ? screenshotPath(screenshots[0]) : PLACEHOLDER;
    const release = releaseInfo(test.releaseTag);

    const hero = document.createElement('div');
    hero.className = 'dialog-hero';
    const image = document.createElement('img');
    image.src = heroImage;
    image.alt = screenshots.length ? `${text(game.title)} compatibility screenshot` : '';
    image.addEventListener('error', () => { image.src = PLACEHOLDER; image.alt = ''; }, { once: true });
    const heading = document.createElement('div');
    heading.className = 'dialog-heading';
    const badge = document.createElement('span');
    badge.className = `status-badge ${status}`;
    badge.textContent = STATUS_LABEL[status];
    const title = document.createElement('h2');
    title.id = 'dialog-title';
    title.textContent = text(game.title, 'Untitled game');
    const subtitle = document.createElement('p');
    subtitle.textContent = [game.serial, game.region, test.gameVersion ? `Game ${test.gameVersion}` : '', test.releaseTag].filter(Boolean).join(' · ');
    heading.append(badge, title, subtitle);
    hero.append(image, heading);

    const body = document.createElement('div');
    body.className = 'dialog-body';
    const grid = document.createElement('div');
    grid.className = 'dialog-grid';
    const main = document.createElement('div');
    const side = document.createElement('aside');

    const notes = document.createElement('p');
    notes.textContent = text(test.notes || test.summary, 'No detailed notes were recorded.');
    main.append(section(`Test notes · ${test.releaseTag}`, notes));

    if (Array.isArray(test.issues) && test.issues.length) {
      const list = document.createElement('ul');
      list.className = 'issue-list';
      for (const issue of test.issues) {
        const li = document.createElement('li');
        li.textContent = text(issue);
        list.append(li);
      }
      main.append(section('Known issues', list));
    }

    if (screenshots.length) {
      const gallery = document.createElement('div');
      gallery.className = 'screenshot-gallery';
      screenshots.forEach((shot, index) => {
        const path = screenshotPath(shot);
        const link = document.createElement('a');
        link.href = path;
        link.target = '_blank';
        link.rel = 'noreferrer';
        link.title = screenshotCaption(shot, index);
        const shotImage = document.createElement('img');
        shotImage.loading = 'lazy';
        shotImage.src = path;
        shotImage.alt = screenshotCaption(shot, index);
        shotImage.addEventListener('error', () => { shotImage.src = PLACEHOLDER; }, { once: true });
        link.append(shotImage);
        gallery.append(link);
      });
      main.append(section('Screenshots', gallery));
    }

    const performance = test.performance || {};
    const device = test.device || {};
    const renderer = test.renderer || {};
    side.append(section('Test environment', definitionList([
      ['Release', releaseLabel(test.releaseTag)],
      ['Selected device', deviceName(test)],
      ['SoC', device.soc],
      ['GPU', device.gpu],
      ['Android', device.androidVersion],
      ['RAM', device.ramGb ? `${device.ramGb} GB` : ''],
      ['Driver type', renderer.driverType],
      ['Driver', renderer.driver],
      ['Driver version', renderer.driverVersion],
      ['Turnip version', renderer.turnipVersion],
      ['Turnip build', renderer.turnipBuild],
      ['Turnip source', renderer.turnipSource],
      ['Guest backend', test.guestBackend],
      ['Installed version', test.emulatorVersion],
      ['Commit', test.commit ? String(test.commit).slice(0, 12) : ''],
      ['Tested', formatDate(test.testedAt)]
    ])));

    side.append(section('Performance', definitionList([
      ['Average FPS', Number.isFinite(Number(performance.averageFps)) ? `${performance.averageFps}` : ''],
      ['FPS range', Number.isFinite(Number(performance.minFps)) && Number.isFinite(Number(performance.maxFps)) ? `${performance.minFps}–${performance.maxFps}` : ''],
      ['Frame pacing', performance.framePacing],
      ['Resolution scale', renderer.resolutionScale ? `${renderer.resolutionScale}×` : ''],
      ['Runtime', performance.testDurationSeconds ? `${performance.testDurationSeconds}s` : '']
    ])));

    const evidence = [];
    for (const item of Array.isArray(test.logs) ? test.logs : []) {
      const path = typeof item === 'string' ? item : item?.path;
      if (path) evidence.push([typeof item === 'object' ? text(item.label, 'Session log') : 'Session log', path]);
    }
    if (release.url) evidence.unshift([`${test.releaseTag} release`, release.url]);
    if (test.videoUrl) evidence.push(['Video evidence', test.videoUrl]);
    if (test.reportUrl) evidence.push(['External report', test.reportUrl]);
    if (evidence.length) {
      const links = document.createElement('div');
      links.className = 'link-list';
      for (const [label, url] of evidence) {
        const link = document.createElement('a');
        link.className = 'evidence-link';
        link.href = url;
        link.target = '_blank';
        link.rel = 'noreferrer';
        const left = document.createElement('span');
        left.textContent = label;
        const right = document.createElement('span');
        right.textContent = 'Open ↗';
        link.append(left, right);
        links.append(link);
      }
      side.append(section('Evidence', links));
    }

    if (tests.length > 1) {
      const reportHistory = document.createElement('div');
      reportHistory.className = 'report-history';
      for (const entry of tests) {
        const row = document.createElement('div');
        row.className = 'report-row';
        const left = document.createElement('span');
        left.className = 'report-row-copy';
        const strong = document.createElement('strong');
        strong.textContent = `${entry.releaseTag} · ${formatDate(entry.testedAt, { year: 'numeric', month: 'short', day: 'numeric' })}`;
        const environment = document.createElement('small');
        environment.textContent = `${deviceName(entry)} · ${driverLabel(entry)}`;
        left.append(strong, environment);
        const right = document.createElement('span');
        const entryStatus = normalizeStatus(entry.status);
        right.className = `status-text ${entryStatus}`;
        right.textContent = STATUS_LABEL[entryStatus];
        row.append(left, right);
        reportHistory.append(row);
      }
      main.append(section('Report history by release', reportHistory));
    }

    grid.append(main, side);
    body.append(grid);
    els.dialogContent.replaceChildren(hero, body);
    if (!els.dialog.open) els.dialog.showModal();
    els.dialogClose.focus({ preventScroll: true });
    document.body.classList.add('dialog-open');
    if (updateUrl) {
      const url = new URL(location.href);
      url.searchParams.set('game', text(game.id || game.serial).toLowerCase());
      url.searchParams.set('test', `${test.releaseTag}:${test.testedAt}`);
      window.history.replaceState({}, '', url);
    }
  }

  function closeGame(updateUrl = true) {
    if (els.dialog.open) els.dialog.close();
    document.body.classList.remove('dialog-open');
    if (updateUrl) {
      const url = new URL(location.href);
      url.searchParams.delete('game');
      url.searchParams.delete('test');
      window.history.replaceState({}, '', url);
    }
  }

  function openGameFromUrl() {
    const url = new URL(location.href);
    const id = url.searchParams.get('game');
    if (!id) return;
    const game = state.games.find(item => [item.id, item.serial].filter(Boolean).some(value => String(value).toLowerCase() === id.toLowerCase()));
    if (!game) return;
    const signature = url.searchParams.get('test');
    const test = signature
      ? allTests(game).find(entry => `${entry.releaseTag}:${entry.testedAt}` === signature)
      : selectedTest(game) || allTests(game)[0];
    if (test) openGame(game, test, false);
  }

  function bindFilters() {
    els.search.addEventListener('input', event => { state.query = event.target.value; render(); });
    els.release.addEventListener('change', event => { state.release = event.target.value; updateReleaseUrl(); render(); });
    els.status.addEventListener('change', event => { state.status = event.target.value; render(); });
    els.device.addEventListener('change', event => { state.device = event.target.value; render(); });
    els.driver.addEventListener('change', event => { state.driver = event.target.value; render(); });
    els.gpu.addEventListener('change', event => { state.gpu = event.target.value; render(); });
    els.sort.addEventListener('change', event => { state.sort = event.target.value; render(); });
    els.reset.addEventListener('click', () => {
      state.query = '';
      state.release = state.releases.find(release => release.latest)?.tag || 'all';
      state.status = state.device = state.driver = state.gpu = 'all';
      state.sort = 'recent';
      els.search.value = '';
      els.release.value = state.release;
      els.status.value = els.device.value = els.driver.value = els.gpu.value = 'all';
      els.sort.value = 'recent';
      updateReleaseUrl();
      render();
    });
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
      const [database, releaseIndex] = await Promise.all([fetchJson(DATA_URL), fetchJson(RELEASES_URL)]);
      if (!database || !Array.isArray(database.games)) throw new Error('games must be an array');
      if (!releaseIndex || !Array.isArray(releaseIndex.releases)) throw new Error('releases must be an array');
      state.database = database;
      state.games = database.games;
      state.releases = releaseIndex.releases;
      state.releaseMap = new Map(state.releases.map(release => [release.tag, release]));
      populateFilters();
      render();
      openGameFromUrl();
    } catch (error) {
      console.error('Compatibility database failed to load:', error);
      els.grid.setAttribute('aria-busy', 'false');
      els.grid.hidden = true;
      els.empty.hidden = false;
      els.emptyTitle.textContent = 'Compatibility data could not be loaded';
      els.emptyCopy.textContent = 'Serve this folder over HTTP and confirm data/games.json and data/releases.json contain valid JSON.';
      els.meta.textContent = 'Database unavailable';
      els.total.textContent = els.playable.textContent = els.ingame.textContent = els.devices.textContent = '—';
    }
  }

  initializeTheme();
  bindFilters();
  loadDatabase();
})();

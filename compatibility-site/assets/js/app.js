(() => {
  'use strict';

  const DATA_URL = 'data/games.json';
  const PLACEHOLDER = 'assets/placeholder.svg';
  const STATUS_ORDER = { playable: 0, ingame: 1, menus: 2, boots: 3, nothing: 4, unknown: 5 };
  const STATUS_LABEL = { playable: 'Playable', ingame: 'Ingame', menus: 'Menus', boots: 'Boots', nothing: 'Nothing', unknown: 'Unknown' };

  const state = {
    database: null,
    games: [],
    query: '',
    status: 'all',
    gpu: 'all',
    sort: 'recent'
  };

  const els = {
    themeToggle: document.querySelector('#theme-toggle'),
    search: document.querySelector('#search'),
    status: document.querySelector('#status-filter'),
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

  function normalizeStatus(value) {
    const status = String(value || 'unknown').toLowerCase();
    return Object.hasOwn(STATUS_ORDER, status) ? status : 'unknown';
  }

  function parseDate(value) {
    const date = new Date(value || 0);
    return Number.isNaN(date.getTime()) ? new Date(0) : date;
  }

  function latestTest(game) {
    const tests = Array.isArray(game.tests) ? [...game.tests] : [];
    tests.sort((a, b) => parseDate(b.testedAt) - parseDate(a.testedAt));
    return tests[0] || {};
  }

  function allTests(game) {
    const tests = Array.isArray(game.tests) ? [...game.tests] : [];
    return tests.sort((a, b) => parseDate(b.testedAt) - parseDate(a.testedAt));
  }

  function text(value, fallback = 'Not recorded') {
    const output = String(value ?? '').trim();
    return output || fallback;
  }

  function formatDate(value, options = { year: 'numeric', month: 'short', day: 'numeric' }) {
    const date = parseDate(value);
    if (date.getTime() === 0) return 'Unknown';
    return new Intl.DateTimeFormat(undefined, options).format(date);
  }

  function formatFps(performance) {
    if (!performance || typeof performance !== 'object') return '—';
    if (Number.isFinite(Number(performance.averageFps))) return `${Number(performance.averageFps).toFixed(Number(performance.averageFps) % 1 ? 1 : 0)} FPS`;
    const min = Number(performance.minFps);
    const max = Number(performance.maxFps);
    if (Number.isFinite(min) && Number.isFinite(max)) return `${min}–${max} FPS`;
    return '—';
  }

  function averageFps(game) {
    const value = Number(latestTest(game).performance?.averageFps);
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

  function deviceName(test) {
    const manufacturer = text(test.device?.manufacturer, '');
    const model = text(test.device?.model, '');
    return `${manufacturer} ${model}`.trim() || 'Unknown device';
  }

  function gpuFamily(test) {
    const gpu = text(test.device?.gpu, 'Unknown');
    if (/adreno/i.test(gpu)) return 'Adreno';
    if (/mali/i.test(gpu)) return 'Mali';
    if (/xclipse/i.test(gpu)) return 'Xclipse';
    if (/immortalis/i.test(gpu)) return 'Immortalis';
    return gpu;
  }

  function searchableGame(game) {
    const test = latestTest(game);
    return [
      game.title, game.serial, game.region, game.publisher,
      test.status, test.gameVersion, test.emulatorVersion, test.commit,
      test.device?.manufacturer, test.device?.model, test.device?.soc, test.device?.gpu,
      test.device?.androidVersion, test.renderer?.driver, test.renderer?.driverVersion,
      test.notes, ...(Array.isArray(test.issues) ? test.issues : [])
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

  function populateGpuFilter() {
    const families = [...new Set(state.games.map(game => gpuFamily(latestTest(game))).filter(Boolean))].sort((a, b) => a.localeCompare(b));
    for (const family of families) {
      const option = document.createElement('option');
      option.value = family;
      option.textContent = family;
      els.gpu.append(option);
    }
  }

  function updateStats() {
    const latest = state.games.map(latestTest);
    const uniqueDevices = new Set(latest.map(deviceName).filter(name => name !== 'Unknown device'));
    els.total.textContent = String(state.games.length);
    els.playable.textContent = String(latest.filter(test => normalizeStatus(test.status) === 'playable').length);
    els.ingame.textContent = String(latest.filter(test => normalizeStatus(test.status) === 'ingame').length);
    els.devices.textContent = String(uniqueDevices.size);
  }

  function filteredGames() {
    const query = state.query.trim().toLowerCase();
    const games = state.games.filter(game => {
      const test = latestTest(game);
      if (query && !searchableGame(game).includes(query)) return false;
      if (state.status !== 'all' && normalizeStatus(test.status) !== state.status) return false;
      if (state.gpu !== 'all' && gpuFamily(test) !== state.gpu) return false;
      return true;
    });

    games.sort((a, b) => {
      const aTest = latestTest(a);
      const bTest = latestTest(b);
      if (state.sort === 'title') return text(a.title).localeCompare(text(b.title));
      if (state.sort === 'status') {
        const statusDiff = STATUS_ORDER[normalizeStatus(aTest.status)] - STATUS_ORDER[normalizeStatus(bTest.status)];
        return statusDiff || text(a.title).localeCompare(text(b.title));
      }
      if (state.sort === 'fps') return averageFps(b) - averageFps(a) || text(a.title).localeCompare(text(b.title));
      return parseDate(bTest.testedAt) - parseDate(aTest.testedAt) || text(a.title).localeCompare(text(b.title));
    });

    return games;
  }

  function createCard(game) {
    const fragment = els.template.content.cloneNode(true);
    const test = latestTest(game);
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
    fragment.querySelector('.game-note').textContent = text(test.summary || test.notes, 'No test notes have been added yet.');
    fragment.querySelector('.device').textContent = deviceName(test);
    fragment.querySelector('.fps').textContent = formatFps(test.performance);
    fragment.querySelector('.tested').textContent = formatDate(test.testedAt, { year: 'numeric', month: 'short' });

    button.setAttribute('aria-label', `Open compatibility details for ${text(game.title)}`);
    button.addEventListener('click', () => openGame(game));
    return fragment;
  }

  function render() {
    const games = filteredGames();
    els.grid.replaceChildren(...games.map(createCard));
    els.grid.setAttribute('aria-busy', 'false');
    els.count.textContent = `${games.length} ${games.length === 1 ? 'game' : 'games'}`;

    const active = [];
    if (state.status !== 'all') active.push(STATUS_LABEL[state.status]);
    if (state.gpu !== 'all') active.push(state.gpu);
    if (state.query.trim()) active.push(`“${state.query.trim()}”`);
    els.activeSummary.textContent = active.length ? `Filtered by ${active.join(' · ')}` : '';

    const noData = state.games.length === 0;
    els.empty.hidden = games.length !== 0;
    els.grid.hidden = games.length === 0;
    if (games.length === 0) {
      els.emptyTitle.textContent = noData ? 'No compatibility reports yet' : 'No matching reports';
      els.emptyCopy.textContent = noData
        ? 'The JSON database is ready. Run the included agent skill to test a game, capture evidence, and create the first entry.'
        : 'Try changing the search or filters.';
    }
  }

  function definitionList(items) {
    const dl = document.createElement('dl');
    dl.className = 'detail-list';
    for (const [label, value] of items) {
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

  function openGame(game, updateUrl = true) {
    const test = latestTest(game);
    const tests = allTests(game);
    const status = normalizeStatus(test.status);
    const screenshots = Array.isArray(test.screenshots) ? test.screenshots.filter(screenshotPath) : [];
    const heroImage = screenshots.length ? screenshotPath(screenshots[0]) : PLACEHOLDER;

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
    subtitle.textContent = [game.serial, game.region, test.gameVersion ? `Game ${test.gameVersion}` : ''].filter(Boolean).join(' · ');
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
    main.append(section('Latest test notes', notes));

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
      ['Device', deviceName(test)],
      ['SoC', device.soc],
      ['GPU', device.gpu],
      ['Android', device.androidVersion],
      ['RAM', device.ramGb ? `${device.ramGb} GB` : ''],
      ['Driver', [renderer.driver, renderer.driverVersion].filter(Boolean).join(' ')],
      ['Backend', test.guestBackend],
      ['Emulator', test.emulatorVersion],
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
    const logs = Array.isArray(test.logs) ? test.logs : [];
    for (const item of logs) {
      const path = typeof item === 'string' ? item : item?.path;
      if (!path) continue;
      evidence.push([typeof item === 'object' ? text(item.label, 'Session log') : 'Session log', path]);
    }
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
      const history = document.createElement('div');
      history.className = 'report-history';
      for (const entry of tests) {
        const row = document.createElement('div');
        row.className = 'report-row';
        const left = document.createElement('span');
        const strong = document.createElement('strong');
        strong.textContent = formatDate(entry.testedAt, { year: 'numeric', month: 'short', day: 'numeric' });
        left.append(strong, document.createTextNode(` · ${text(entry.emulatorVersion, 'unknown build')}`));
        const right = document.createElement('span');
        const entryStatus = normalizeStatus(entry.status);
        right.className = `status-text ${entryStatus}`;
        right.textContent = STATUS_LABEL[entryStatus];
        row.append(left, right);
        history.append(row);
      }
      main.append(section('Report history', history));
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
      history.replaceState({}, '', url);
    }
  }

  function closeGame(updateUrl = true) {
    if (els.dialog.open) els.dialog.close();
    document.body.classList.remove('dialog-open');
    if (updateUrl) {
      const url = new URL(location.href);
      url.searchParams.delete('game');
      history.replaceState({}, '', url);
    }
  }

  function openGameFromUrl() {
    const id = new URL(location.href).searchParams.get('game');
    if (!id) return;
    const game = state.games.find(item => [item.id, item.serial].filter(Boolean).some(value => String(value).toLowerCase() === id.toLowerCase()));
    if (game) openGame(game, false);
  }

  function bindFilters() {
    els.search.addEventListener('input', event => { state.query = event.target.value; render(); });
    els.status.addEventListener('change', event => { state.status = event.target.value; render(); });
    els.gpu.addEventListener('change', event => { state.gpu = event.target.value; render(); });
    els.sort.addEventListener('change', event => { state.sort = event.target.value; render(); });
    els.reset.addEventListener('click', () => {
      state.query = '';
      state.status = 'all';
      state.gpu = 'all';
      state.sort = 'recent';
      els.search.value = '';
      els.status.value = 'all';
      els.gpu.value = 'all';
      els.sort.value = 'recent';
      render();
    });
    els.dialogClose.addEventListener('click', () => closeGame());
    els.dialog.addEventListener('cancel', event => { event.preventDefault(); closeGame(); });
    els.dialog.addEventListener('click', event => { if (event.target === els.dialog) closeGame(); });
  }

  async function loadDatabase() {
    try {
      const response = await fetch(DATA_URL, { cache: 'no-store' });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const database = await response.json();
      if (!database || !Array.isArray(database.games)) throw new Error('games must be an array');
      state.database = database;
      state.games = database.games;
      updateStats();
      populateGpuFilter();
      render();
      els.meta.textContent = `JSON schema v${text(database.schemaVersion, '1')} · Updated ${formatDate(database.lastUpdated, { year: 'numeric', month: 'short', day: 'numeric' })}`;
      openGameFromUrl();
    } catch (error) {
      console.error('Compatibility database failed to load:', error);
      els.grid.setAttribute('aria-busy', 'false');
      els.grid.hidden = true;
      els.empty.hidden = false;
      els.emptyTitle.textContent = 'Compatibility data could not be loaded';
      els.emptyCopy.textContent = 'Serve this folder over HTTP and confirm data/games.json contains valid JSON.';
      els.meta.textContent = 'Database unavailable';
      els.total.textContent = els.playable.textContent = els.ingame.textContent = els.devices.textContent = '—';
    }
  }

  initializeTheme();
  bindFilters();
  loadDatabase();
})();

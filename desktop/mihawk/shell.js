/* SPDX-License-Identifier: GPL-3.0-or-later
 * Mihawk desktop adaptation for Xbox, based on the MihawkOS shell prototype's
 * desktop/window/taskbar design. This focused frontend has no demonstration
 * filesystem or simulated programs: native operations use the UWP host bridge.
 * Copyright (c) 2026 the contributors. See packaged upstream notices. */
(function () {
  'use strict';

  var pending = {}, requestNumber = 0, windows = [], nextWindow = 0, topZ = 10;
  var roots = [], hostInfo = null, connected = false, connecting = false;
  var desktop = document.getElementById('desktop-icons');
  var workspace = document.getElementById('windows');
  var startMenu = document.getElementById('start-menu');
  var startButton = document.getElementById('start-button');
  var settings = { theme: 'light', accent: '#006fc1', wallpaper: true };
  try {
    var stored = JSON.parse(localStorage.getItem('mihawk-xbox-settings') || 'null');
    if (stored) {
      if (stored.theme === 'dark' || stored.theme === 'light') settings.theme = stored.theme;
      if (/^#[0-9a-f]{6}$/i.test(stored.accent)) settings.accent = stored.accent;
      if (typeof stored.wallpaper === 'boolean') settings.wallpaper = stored.wallpaper;
    }
  } catch (ignored) {}

  var apps = [
    { id: 'library', name: 'Game Library', icon: 'games', glyph: '\u25b6', description: 'Your saved Windows programs and launch settings' },
    { id: 'files', name: 'Xbox Files', icon: 'folder', glyph: '\u25a3', description: 'Browse native Xbox storage and USB folders' },
    { id: 'notepad', name: 'Notepad', icon: 'app-notepad', glyph: '\u270e', description: 'Open and save text files' },
    { id: 'calculator', name: 'Calculator', icon: 'app-calculator', glyph: '\u229e', description: 'Everyday arithmetic' },
    { id: 'browser', name: 'Browser', icon: 'app-browser', glyph: '\u25ce', description: 'Open a website in the native browser view' },
    { id: 'settings', name: 'Personalization', icon: 'app-settings', glyph: '\u2699', description: 'Theme, accent and wallpaper' },
    { id: 'transfer', name: 'LAN Transfer', icon: 'downloads', glyph: '\u21c4', description: 'Send files from a phone or computer' },
    { id: 'about', name: 'About', icon: 'this-pc', glyph: 'i', description: 'Desktop and connection details' }
  ];

  function el(tag, className, text) {
    var node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined) node.textContent = text;
    return node;
  }
  function clear(node) { while (node.firstChild) node.removeChild(node.firstChild); }
  function add(parent, node) { parent.appendChild(node); return node; }
  function hasClass(node, name) { return node && node.classList && node.classList.contains(name); }
  function ancestor(node, name) {
    while (node && node !== document) { if (hasClass(node, name)) return node; node = node.parentNode; }
    return null;
  }
  function button(text, fn, className) {
    var node = el('button', className || '', text); node.type = 'button';
    node.addEventListener('click', fn); return node;
  }
  function appById(id) { for (var i = 0; i < apps.length; i++) if (apps[i].id === id) return apps[i]; return apps[0]; }
  function icon(info) {
    var node = el('span', 'app-icon'), fallback = el('span', 'icon-fallback', info.glyph || '\u25a3');
    node.setAttribute('aria-hidden', 'true');
    if (info.icon) {
      var img = el('img'); img.alt = ''; img.src = 'assets/icons/' + info.icon + '.svg';
      img.addEventListener('error', function () { node.classList.add('icon-missing'); });
      add(node, img); add(node, fallback);
    } else { add(node, el('span', '', info.glyph || '\u25a3')); }
    return node;
  }
  function errorText(error) {
    if (typeof error === 'string') return error;
    if (error && error.message) return String(error.message);
    if (error && error.error) return errorText(error.error);
    return 'The native operation failed.';
  }
  function toast(title, message) {
    var node = el('div', 'toast'); add(node, el('strong', '', title)); add(node, el('div', '', message));
    add(document.getElementById('toasts'), node);
    setTimeout(function () { if (node.parentNode) node.parentNode.removeChild(node); }, 7000);
  }
  function modernBridge() {
    return window.chrome && window.chrome.webview && window.chrome.webview.postMessage ? window.chrome.webview : null;
  }
  function bridgeAvailable() { return !!(modernBridge() || (window.external && window.external.notify)); }
  function rpc(method, params, timeout) {
    return new Promise(function (resolve, reject) {
      if (!bridgeAvailable()) { reject(new Error('Open this desktop in the Xbox application to access files and native apps.')); return; }
      var id = 'm' + (++requestNumber);
      var timer = setTimeout(function () {
        if (!pending[id]) return;
        delete pending[id]; reject(new Error('The Xbox host did not answer ' + method + '. Try again.'));
      }, timeout || 30000);
      pending[id] = { resolve: resolve, reject: reject, timer: timer };
      try {
        var serialized = JSON.stringify({ id: id, method: method, params: params || {} });
        var modern = modernBridge();
        if (modern) modern.postMessage(serialized); else window.external.notify(serialized);
      }
      catch (error) { clearTimeout(timer); delete pending[id]; reject(error); }
    });
  }
  window.MihawkNativeReply = function (payload) {
    var response;
    try { response = typeof payload === 'string' ? JSON.parse(payload) : payload; }
    catch (ignored) { return; }
    if (!response || !pending[response.id]) return;
    var task = pending[response.id]; delete pending[response.id]; clearTimeout(task.timer);
    if (response.ok) task.resolve(response.result);
    else task.reject(new Error(errorText(response.error)));
  };
  var webviewMessages = modernBridge();
  if (webviewMessages && webviewMessages.addEventListener) {
    webviewMessages.addEventListener('message', function (event) { window.MihawkNativeReply(event.data); });
  }
  function setConnection(ok, label) {
    connected = ok;
    var status = document.getElementById('connection-status');
    status.className = 'connection-status ' + (ok ? 'connected' : 'disconnected');
    document.getElementById('connection-label').textContent = label;
    document.getElementById('start-platform').textContent = ok ? (hostInfo.platform || 'Xbox') : 'Native host unavailable';
  }
  function connect() {
    if (connecting) return Promise.resolve();
    connecting = true;
    rpc('ready', {
      userAgent: navigator.userAgent,
      viewportWidth: window.innerWidth,
      viewportHeight: window.innerHeight,
      devicePixelRatio: window.devicePixelRatio || 1
    }).catch(function () {});
    return rpc('hello', {}).then(function (info) {
      if (!info || !Array.isArray(info.roots)) throw new Error('The host returned an invalid workspace response.');
      hostInfo = info; roots = info.roots; setConnection(true, 'Xbox connected');
      for (var i = 0; i < windows.length; i++) if (windows[i].onConnected) windows[i].onConnected();
    }).catch(function (error) {
      setConnection(false, bridgeAvailable() ? 'Connection unavailable' : 'Browser preview');
      hostInfo = { platform: 'Native host unavailable', version: '', error: errorText(error) };
    }).then(function () { connecting = false; });
  }

  function applySettings() {
    document.body.classList.toggle('dark', settings.theme === 'dark');
    document.body.classList.toggle('solid-wallpaper', !settings.wallpaper);
    document.documentElement.setAttribute('data-theme', settings.theme);
    document.body.style.setProperty('--accent', settings.accent);
    try { localStorage.setItem('mihawk-xbox-settings', JSON.stringify(settings)); } catch (ignored) {}
  }
  function showStart(show) {
    var open = show === undefined ? hasClass(startMenu, 'hidden') : show;
    startMenu.classList.toggle('hidden', !open); startButton.setAttribute('aria-expanded', String(open));
    startButton.classList.toggle('active', open);
    if (open) { document.getElementById('start-search').value = ''; renderStart(''); document.getElementById('start-search').focus(); }
  }
  function appButton(app, className) {
    var node = button('', function () { showStart(false); openApp(app.id); }, className);
    node.setAttribute('data-app', app.id); node.setAttribute('title', app.name); node.setAttribute('aria-label', app.name);
    add(node, icon(app));
    if (className !== 'taskbar-button') add(node, el('span', '', app.name));
    return node;
  }
  function renderStart(query) {
    var list = document.getElementById('start-apps'); clear(list);
    for (var i = 0; i < apps.length; i++) {
      if ((apps[i].name + ' ' + apps[i].description).toLowerCase().indexOf(query.toLowerCase()) !== -1)
        add(list, appButton(apps[i], 'start-app'));
    }
    if (!list.firstChild) add(list, el('p', 'muted', 'No matching applications.'));
  }
  function refreshTaskbar() {
    var running = document.getElementById('running-apps'); clear(running);
    for (var i = 0; i < windows.length; i++) (function (win) {
      var node = button('', function () {
        if (!win.minimized && win.active) minimize(win); else focusWindow(win);
      }, 'taskbar-button running' + (win.active && !win.minimized ? ' active' : ''));
      node.title = win.title; node.setAttribute('aria-label', win.title); node.setAttribute('data-task-window', win.id);
      add(node, icon(appById(win.kind))); add(running, node);
    }(windows[i]));
  }
  function focusWindow(win) {
    for (var i = 0; i < windows.length; i++) {
      windows[i].active = windows[i] === win;
      windows[i].node.classList.toggle('active', windows[i] === win);
    }
    win.minimized = false; win.active = true; win.node.classList.remove('hidden');
    win.node.style.zIndex = String(++topZ);
    if (!win.node.contains(document.activeElement)) {
      if (win.onFocus) win.onFocus(); else win.node.focus();
    }
    refreshTaskbar();
  }
  function minimize(win) {
    win.minimized = true; win.active = false; win.node.classList.add('hidden'); win.node.classList.remove('active'); refreshTaskbar();
  }
  function sizeWindow(win) {
    var width = workspace.clientWidth, height = workspace.clientHeight;
    if (win.maximized) {
      win.node.style.left = '0px'; win.node.style.top = '0px'; win.node.style.width = width + 'px'; win.node.style.height = height + 'px';
    } else {
      win.width = Math.min(win.width, width); win.height = Math.min(win.height, height);
      win.x = Math.max(0, Math.min(win.x, width - win.width));
      win.y = Math.max(0, Math.min(win.y, height - win.height));
      win.node.style.left = win.x + 'px'; win.node.style.top = win.y + 'px';
      win.node.style.width = win.width + 'px'; win.node.style.height = win.height + 'px';
    }
    win.node.classList.toggle('maximized', win.maximized);
    win.maxButton.title = win.maximized ? 'Restore' : 'Maximize';
    win.maxButton.setAttribute('aria-label', win.maxButton.title);
  }
  function setTitle(win, title) { win.title = title; win.titleNode.textContent = title; refreshTaskbar(); }
  function setStatus(win, text, isError) {
    win.status.textContent = text; win.status.classList.toggle('error', !!isError);
  }
  function finishClose(win) {
    var index = windows.indexOf(win); if (index === -1) return;
    windows.splice(index, 1); win.closed = true;
    if (win.onClose) win.onClose();
    if (win.node.parentNode) win.node.parentNode.removeChild(win.node);
    if (windows.length) focusWindow(windows[windows.length - 1]); else refreshTaskbar();
  }
  function closeWindow(win) {
    if (win.beforeClose) { win.beforeClose(function () { finishClose(win); }); }
    else finishClose(win);
  }
  function dragNode(handle, win, resizing) {
    var drag = null;
    handle.addEventListener('pointerdown', function (event) {
      if (event.button !== 0 || ancestor(event.target, 'caption') || win.maximized) return;
      focusWindow(win);
      drag = { px: event.clientX, py: event.clientY, x: win.x, y: win.y, w: win.width, h: win.height };
      if (handle.setPointerCapture) handle.setPointerCapture(event.pointerId);
      event.preventDefault();
    });
    handle.addEventListener('pointermove', function (event) {
      if (!drag) return;
      var dx = event.clientX - drag.px, dy = event.clientY - drag.py;
      if (resizing) {
        win.width = Math.max(320, Math.min(workspace.clientWidth - win.x, drag.w + dx));
        win.height = Math.max(230, Math.min(workspace.clientHeight - win.y, drag.h + dy));
      } else {
        win.x = Math.max(0, Math.min(workspace.clientWidth - 130, drag.x + dx));
        win.y = Math.max(0, Math.min(workspace.clientHeight - 38, drag.y + dy));
      }
      sizeWindow(win);
    });
    handle.addEventListener('pointerup', function () { drag = null; });
    handle.addEventListener('pointercancel', function () { drag = null; });
    handle.addEventListener('lostpointercapture', function () { drag = null; });
  }
  function createWindow(kind, title, width, height) {
    var app = appById(kind), n = ++nextWindow;
    var win = { id: 'window-' + n, kind: kind, title: title, x: 110 + (n % 6) * 26, y: 38 + (n % 6) * 24,
      width: width || 720, height: height || 490, minimized: false, maximized: false, active: false, closed: false };
    win.node = el('section', 'window'); win.node.setAttribute('data-window-id', win.id); win.node.setAttribute('data-window-kind', kind);
    win.node.tabIndex = -1;
    win.node.setAttribute('role', 'region'); win.node.setAttribute('aria-label', title);
    var titlebar = add(win.node, el('header', 'titlebar')); add(titlebar, icon(app));
    win.titleNode = add(titlebar, el('span', 'window-title', title));
    var caption = add(titlebar, el('div', 'caption'));
    var minButton = add(caption, button('\u2212', function () { minimize(win); })); minButton.title = 'Minimize'; minButton.setAttribute('aria-label', 'Minimize');
    win.maxButton = add(caption, button('\u25a1', function () { win.maximized = !win.maximized; sizeWindow(win); }));
    var closeButton = add(caption, button('\u00d7', function () { closeWindow(win); }, 'close')); closeButton.title = 'Close'; closeButton.setAttribute('aria-label', 'Close');
    win.body = add(win.node, el('div', 'window-body'));
    win.status = add(win.node, el('div', 'window-status'));
    var grip = add(win.node, el('div', 'resize-grip')); grip.setAttribute('aria-hidden', 'true');
    win.node.addEventListener('pointerdown', function () { if (!win.active) focusWindow(win); });
    titlebar.addEventListener('dblclick', function (event) { if (!ancestor(event.target, 'caption')) { win.maximized = !win.maximized; sizeWindow(win); } });
    dragNode(titlebar, win, false); dragNode(grip, win, true);
    windows.push(win); add(workspace, win.node); sizeWindow(win); focusWindow(win);
    return win;
  }
  function modal(title, message, choices, inputValue) {
    return new Promise(function (resolve) {
      var scrim = el('div', 'modal-scrim'), box = add(scrim, el('section', 'modal'));
      box.setAttribute('role', 'dialog'); box.setAttribute('aria-modal', 'true'); box.setAttribute('aria-label', title);
      add(box, el('h2', '', title)); add(box, el('p', '', message));
      var input = null;
      if (inputValue !== undefined) { input = add(box, el('input')); input.type = 'text'; input.value = inputValue; input.style.width = '100%'; input.style.marginBottom = '16px'; input.setAttribute('aria-label', 'Name'); }
      var actions = add(box, el('div', 'modal-actions'));
      function done(value) { if (scrim.parentNode) scrim.parentNode.removeChild(scrim); resolve(input && value === 'ok' ? input.value : value); }
      for (var i = 0; i < choices.length; i++) (function (choice) { add(actions, button(choice.label, function () { done(choice.value); }, choice.primary ? 'primary' : '')); }(choices[i]));
      scrim.addEventListener('keydown', function (event) {
        if (event.key === 'Escape' || event.keyCode === 27) { event.preventDefault(); done('cancel'); }
        if (input && (event.key === 'Enter' || event.keyCode === 13)) { event.preventDefault(); done('ok'); }
      });
      add(document.getElementById('modal-layer'), scrim);
      setTimeout(function () { if (input) { input.focus(); input.select(); } else if (actions.firstChild) actions.firstChild.focus(); }, 0);
    });
  }
  function askName(title, message, initial) {
    return modal(title, message, [{ label: 'Cancel', value: 'cancel' }, { label: 'Create', value: 'ok', primary: true }], initial);
  }
  function validName(name) { return name && name !== '.' && name !== '..' && !/[\\/:*?"<>|\x00-\x1f]/.test(name); }
  function humanSize(value) {
    if (typeof value !== 'number' || !isFinite(value)) return '';
    if (value < 1024) return value + ' B';
    if (value < 1048576) return (value / 1024).toFixed(1) + ' KB';
    return (value / 1048576).toFixed(1) + ' MB';
  }
  function folderLabel(folder) { return folder.id === 'workspace' ? 'Xbox Files' : folder.name; }
  function ensureRoot(entry) {
    if (!entry || !entry.id) return;
    for (var i = 0; i < roots.length; i++) if (roots[i].id === entry.id) return;
    roots.push({ id: entry.id, name: entry.name || 'Selected folder', writable: entry.writable !== false });
  }

  function openFiles(initial, chooseFile, options) {
    options = options || {};
    var win = createWindow('files', 'Xbox Files', 830, 535), selected = null, entries = [], trail = [], loading = false, generation = 0;
    var toolbar = add(win.body, el('div', 'toolbar'));
    var openButton = add(toolbar, button(chooseFile ? (options.chooseLabel || 'Open text file') : 'Open', openSelected, 'primary'));
    var libraryButton = add(toolbar, button('Add to Library', function () {
      if (!selected || !/\.exe$/i.test(selected.name)) return;
      libraryButton.disabled = true; setStatus(win, 'Saving program to Game Library…');
      rpc('libraryAdd', { file: selected.id }).then(function (game) {
        setStatus(win, 'Added ' + (game && game.name ? game.name : selected.name) + ' to Game Library.');
        refreshLibraries();
      }).catch(function (error) { setStatus(win, errorText(error), true); }).then(controls);
    }));
    if (chooseFile) libraryButton.classList.add('hidden');
    var newFolder = add(toolbar, button('New folder', function () { makeItem(true); }));
    var newText = add(toolbar, button('New text file', function () { makeItem(false); }));
    add(toolbar, button('Refresh', function () { loadFolder(trail[trail.length - 1], false); }));
    add(toolbar, el('span', 'toolbar-spacer'));
    var importButton = add(toolbar, button('Add folder / USB', function () {
      importButton.disabled = true; setStatus(win, 'Choose a folder in the Xbox picker…');
      rpc('pickFolder', {}, 300000).then(function (entry) {
        if (!entry || !entry.id) { setStatus(win, 'No folder selected.'); return; }
        ensureRoot(entry); renderRoots(); trail = []; loadFolder(entry, true);
      }).catch(function (error) { setStatus(win, errorText(error), true); }).then(function () { importButton.disabled = false; });
    }));
    var area = add(win.body, el('div', 'file-workspace'));
    var sidebar = add(area, el('nav', 'file-sidebar')); sidebar.setAttribute('aria-label', 'Locations');
    var main = add(area, el('div', 'file-main')), path = add(main, el('nav', 'file-path'));
    path.setAttribute('aria-label', 'Current folder');
    var columns = add(main, el('div', 'file-columns')); add(columns, el('span', 'file-name', 'Name')); add(columns, el('span', 'file-type', 'Type')); add(columns, el('span', 'file-size', 'Size'));
    var list = add(main, el('div', 'file-list')); list.setAttribute('aria-label', 'Folder contents');
    function current() { return trail.length ? trail[trail.length - 1] : null; }
    function controls() {
      var folder = current(); openButton.disabled = !selected || loading;
      libraryButton.disabled = !connected || loading || !selected || selected.kind !== 'file' || !/\.exe$/i.test(selected.name);
      newFolder.disabled = newText.disabled = !folder || folder.writable === false || loading || !connected;
      importButton.disabled = !connected || loading;
    }
    function renderRoots() {
      clear(sidebar);
      for (var i = 0; i < roots.length; i++) (function (root) {
        var node = button('', function () { trail = []; loadFolder(root, true); });
        add(node, icon({ icon: root.id === 'workspace' ? 'folder' : 'drive' })); add(node, el('span', '', folderLabel(root)));
        if (trail.length && trail[0].id === root.id) node.classList.add('selected');
        add(sidebar, node);
      }(roots[i]));
    }
    function renderPath() {
      clear(path);
      for (var i = 0; i < trail.length; i++) (function (folder, index) {
        if (index) add(path, el('span', 'muted', '\u203a'));
        add(path, button(folderLabel(folder), function () { trail = trail.slice(0, index + 1); loadFolder(folder, false); }));
      }(trail[i], i));
      setTitle(win, current() && current().id !== 'workspace' ? current().name + ' \u2014 Xbox Files' : 'Xbox Files'); renderRoots();
    }
    function select(entry, row) {
      selected = entry;
      var previous = list.querySelectorAll('.selected'); for (var i = 0; i < previous.length; i++) previous[i].classList.remove('selected');
      if (row) row.classList.add('selected'); controls();
    }
    function renderEntries(nextOffset, token) {
      clear(list);
      entries.sort(function (a, b) { if (a.kind !== b.kind) return a.kind === 'folder' ? -1 : 1; return String(a.name).localeCompare(String(b.name)); });
      for (var i = 0; i < entries.length; i++) (function (entry) {
        var row = button('', function () { select(entry, row); }, 'file-row');
        row.setAttribute('data-file-id', entry.id); row.title = entry.name;
        add(row, icon({ icon: entry.kind === 'folder' ? 'folder' : 'document' }));
        add(row, el('span', 'file-name', entry.name));
        add(row, el('span', 'file-type', entry.kind === 'folder' ? 'Folder' : 'File'));
        add(row, el('span', 'file-size', entry.kind === 'folder' ? '' : humanSize(entry.size)));
        row.addEventListener('dblclick', function () { select(entry, row); openSelected(); });
        row.addEventListener('keydown', function (event) { if (event.key === 'Enter' || event.keyCode === 13) { event.preventDefault(); select(entry, row); openSelected(); } });
        add(list, row);
      }(entries[i]));
      if (!entries.length) {
        var empty = add(list, el('div', 'empty-state')); add(empty, el('strong', '', 'This folder is empty'));
        add(empty, el('span', '', current() && current().writable !== false ? 'Create a folder or a text file to get started.' : 'There are no files in this location.'));
      }
      if (typeof nextOffset === 'number' && nextOffset >= 0) {
        add(list, button('Load more files', function (event) { event.target.disabled = true; fetchPage(nextOffset, token); }));
      }
      setStatus(win, entries.length + ' item' + (entries.length === 1 ? '' : 's') + (current() && current().writable === false ? ' \u00b7 Read only' : ''));
    }
    function fetchPage(offset, token) {
      var folder = current(); if (!folder) return;
      loading = true; controls();
      rpc('list', { folder: folder.id, offset: offset }).then(function (result) {
        if (win.closed || token !== generation) return;
        if (!result || !Array.isArray(result.entries)) throw new Error('The host returned an invalid folder listing.');
        for (var i = 0; i < result.entries.length; i++) entries.push(result.entries[i]);
        loading = false; renderEntries(result.nextOffset, token); controls();
      }).catch(function (error) {
        if (win.closed || token !== generation) return;
        loading = false; clear(list); var empty = add(list, el('div', 'empty-state'));
        add(empty, el('strong', '', 'Could not open this folder')); add(empty, el('div', '', errorText(error)));
        add(empty, button('Try again', function () { loadFolder(current(), false); }));
        setStatus(win, errorText(error), true); controls();
      });
    }
    function loadFolder(folder, push) {
      if (!folder) return; generation++; entries = []; selected = null;
      if (push) trail.push(folder); else if (!trail.length) trail = [folder];
      renderPath(); clear(list); add(list, el('div', 'empty-state', 'Reading folder…')); setStatus(win, 'Reading files from Xbox storage…');
      fetchPage(0, generation);
    }
    function openSelected() {
      if (!selected) return;
      if (selected.kind === 'folder') { loadFolder(selected, true); return; }
      if (chooseFile) {
        if (options.filter && !options.filter(selected)) { setStatus(win, options.filterMessage || 'Choose a supported file.', true); return; }
        chooseFile(selected, current()); finishClose(win); return;
      }
      if (/\.exe$/i.test(selected.name)) {
        setStatus(win, 'Preparing ' + selected.name + ' in the native runtime…');
        rpc('run', { file: selected.id }, 120000).then(function (result) {
          setStatus(win, result && result.started ? 'Execution dispatched. The native view shows the actual result.' : 'Native runtime request completed.');
        }).catch(function (error) { setStatus(win, errorText(error), true); }); return;
      }
      if (/\.(txt|log|ini|cfg|json|md|csv|xml|ya?ml|js|css|html?)$/i.test(selected.name)) { openNotepad(selected, current()); return; }
      setStatus(win, 'Open text files in Notepad or an .exe in the experimental native runtime. This file type has no handler.', true);
    }
    function makeItem(folder) {
      var location = current(); if (!location || loading) return;
      askName(folder ? 'New folder' : 'New text file', 'Create in ' + location.name + '.', folder ? 'New folder' : 'New document.txt').then(function (name) {
        if (name === 'cancel') return;
        name = String(name).trim();
        if (!validName(name)) { setStatus(win, 'Enter a name without slashes or reserved filename characters.', true); return; }
        setStatus(win, 'Creating ' + name + '…');
        rpc(folder ? 'mkdir' : 'create', folder ? { folder: location.id, name: name } : { folder: location.id, name: name, text: '' }).then(function () {
          loadFolder(location, false);
        }).catch(function (error) { setStatus(win, errorText(error), true); });
      });
    }
    win.onConnected = function () {
      renderRoots(); if (!trail.length && roots.length) loadFolder(roots[0], true); controls();
    };
    if (connected && roots.length) {
      var first = initial || roots[0]; loadFolder(first, true);
    } else {
      var empty = add(list, el('div', 'empty-state')); add(empty, el('strong', '', 'Xbox Files need the native host'));
      add(empty, el('div', '', 'The desktop is not connected to native storage. No sample files are shown.'));
      add(empty, button('Reconnect', function () { connect(); })); setStatus(win, 'Native host unavailable', true); controls();
    }
    return win;
  }

  function openNotepad(file, parent) {
    var win = createWindow('notepad', file ? file.name + ' \u2014 Notepad' : 'Untitled \u2014 Notepad', 750, 520);
    var currentFile = file || null, target = parent || null, savedText = '', busy = false, loadError = false;
    var toolbar = add(win.body, el('div', 'toolbar'));
    add(toolbar, button('New', function () { openNotepad(); }));
    add(toolbar, button('Open…', function () { openFiles(null, function (entry, folder) { openNotepad(entry, folder); }); }));
    var saveButton = add(toolbar, button('Save', function () { save(false); }, 'primary'));
    var saveAsButton = add(toolbar, button('Save as…', function () { save(true); }));
    var saveForm = add(win.body, el('div', 'save-form hidden'));
    var label = add(saveForm, el('label', '', 'File name')); var nameInput = add(saveForm, el('input')); nameInput.type = 'text';
    nameInput.value = file ? file.name : 'Untitled.txt'; nameInput.setAttribute('aria-label', 'File name');
    label.setAttribute('for', win.id + '-filename'); nameInput.id = win.id + '-filename';
    var destination = add(saveForm, el('select')); destination.setAttribute('aria-label', 'Save location');
    var confirmSave = add(saveForm, button('Save file', function () { saveNew(); }, 'primary'));
    add(saveForm, button('Cancel', function () { saveForm.classList.add('hidden'); }));
    add(saveForm, el('div', 'form-hint', 'Files are saved to the selected native Xbox folder.'));
    var editor = add(win.body, el('textarea', 'editor')); editor.spellcheck = false; editor.setAttribute('aria-label', 'Text editor');
    win.onFocus = function () { if (!editor.disabled) editor.focus(); };
    function dirty() { return editor.value !== savedText; }
    function update() {
      setTitle(win, (dirty() ? '* ' : '') + (currentFile ? currentFile.name : 'Untitled') + ' \u2014 Notepad');
      saveButton.disabled = busy || loadError || !connected || !!(currentFile && currentFile.writable === false);
      saveAsButton.disabled = confirmSave.disabled = busy || loadError || !connected;
      if (!busy) setStatus(win, (dirty() ? 'Unsaved changes' : (currentFile ? 'Saved' : 'New document')) + ' \u00b7 ' + editor.value.length + ' characters');
    }
    function destinations() {
      clear(destination);
      var choices = roots.slice(0);
      if (target) {
        var found = false; for (var i = 0; i < choices.length; i++) if (choices[i].id === target.id) found = true;
        if (!found) choices.unshift(target);
      }
      for (var j = 0; j < choices.length; j++) if (choices[j].writable !== false) {
        var option = el('option', '', folderLabel(choices[j])); option.value = choices[j].id; add(destination, option);
        if (target && target.id === choices[j].id) destination.value = target.id;
      }
    }
    function save(as) {
      if (busy) return Promise.resolve(false);
      if (!currentFile || as) {
        destinations(); saveForm.classList.remove('hidden'); nameInput.focus(); nameInput.select();
        return Promise.resolve(false);
      }
      var text = editor.value; busy = true; update(); setStatus(win, 'Saving to Xbox storage…');
      return rpc('write', { file: currentFile.id, text: text }, 60000).then(function () {
        savedText = text; busy = false; update(); return true;
      }).catch(function (error) { busy = false; update(); setStatus(win, 'Save failed: ' + errorText(error) + ' Your changes remain in this window.', true); return false; });
    }
    function saveNew() {
      if (busy) return;
      var name = nameInput.value.trim();
      if (!validName(name)) { setStatus(win, 'Enter a valid file name.', true); return; }
      if (!destination.value) { setStatus(win, 'No writable save location is available.', true); return; }
      var text = editor.value; busy = true; update(); setStatus(win, 'Creating file in Xbox storage…');
      rpc('create', { folder: destination.value, name: name, text: text }, 60000).then(function (entry) {
        if (!entry || !entry.id) throw new Error('The host did not return the saved file.');
        currentFile = entry; savedText = text; busy = false; saveForm.classList.add('hidden'); update();
      }).catch(function (error) { busy = false; update(); setStatus(win, 'Save failed: ' + errorText(error) + ' Your changes remain in this window.', true); });
    }
    editor.addEventListener('input', update);
    editor.addEventListener('keydown', function (event) {
      if (event.ctrlKey && (event.key === 's' || event.key === 'S' || event.keyCode === 83)) { event.preventDefault(); save(event.shiftKey); }
    });
    win.beforeClose = function (done) {
      if (busy) { setStatus(win, 'Wait for the file operation to finish before closing.'); return; }
      if (!dirty()) { done(); return; }
      modal('Unsaved changes', 'Keep this window open to save your changes, or discard them and close.', [
        { label: 'Keep editing', value: 'cancel', primary: true }, { label: 'Discard and close', value: 'discard' }
      ]).then(function (answer) { if (answer === 'discard') done(); });
    };
    win.onConnected = update;
    if (file) {
      busy = true; editor.disabled = true; setStatus(win, 'Reading ' + file.name + '…'); update();
      rpc('read', { file: file.id }, 60000).then(function (result) {
        if (!result || typeof result.text !== 'string') throw new Error('The host returned no text content.');
        editor.value = savedText = result.text; busy = false; editor.disabled = false; update(); editor.focus();
      }).catch(function (error) {
        busy = false; loadError = true; editor.disabled = true; saveButton.disabled = saveAsButton.disabled = true;
        setStatus(win, 'Could not read file: ' + errorText(error), true);
      });
    } else { update(); editor.focus(); }
    return win;
  }

  function openCalculator() {
    var win = createWindow('calculator', 'Calculator', 350, 480), value = '0', storedValue = null, operation = null, replace = true;
    var panel = add(win.body, el('div', 'calculator'));
    var expression = add(panel, el('div', 'calc-expression'));
    var display = add(panel, el('div', 'calc-display', '0')); display.setAttribute('aria-live', 'polite');
    var keys = add(panel, el('div', 'calc-keys'));
    var labels = ['CE', 'C', '\u232b', '\u00f7', '7', '8', '9', '\u00d7', '4', '5', '6', '\u2212', '1', '2', '3', '+', '\u00b1', '0', '.', '='];
    function render() { display.textContent = value; expression.textContent = operation === null ? '' : storedValue + ' ' + operation; }
    function apply(a, b, op) {
      if (op === '+') return a + b; if (op === '\u2212') return a - b;
      if (op === '\u00d7') return a * b; return b === 0 ? NaN : a / b;
    }
    function press(key) {
      if (/^[0-9]$/.test(key)) { value = replace || value === '0' || value === 'Error' ? key : value + key; replace = false; }
      else if (key === '.') { if (replace || value === 'Error') { value = '0'; replace = false; } if (value.indexOf('.') === -1) value += '.'; }
      else if (key === 'C') { value = '0'; operation = storedValue = null; replace = true; }
      else if (key === 'CE') { value = '0'; replace = true; }
      else if (key === '\u232b') { if (!replace && value !== 'Error') value = value.length > 1 ? value.slice(0, -1) : '0'; if (value === '-') value = '0'; }
      else if (key === '\u00b1') { if (value !== '0' && value !== 'Error') value = value.charAt(0) === '-' ? value.slice(1) : '-' + value; }
      else {
        var number = Number(value);
        if (!isFinite(number)) { value = '0'; storedValue = operation = null; replace = true; render(); return; }
        if (operation !== null && !replace) {
          number = apply(storedValue, number, operation);
          value = isFinite(number) ? String(Number(number.toPrecision(12))) : 'Error';
        }
        if (key === '=' || value === 'Error') { storedValue = operation = null; }
        else { storedValue = number; operation = key; }
        replace = true;
      }
      render();
    }
    for (var i = 0; i < labels.length; i++) (function (key) {
      add(keys, button(key, function () { press(key); }, key === '=' ? 'equals' : (/^[0-9]$/.test(key) ? 'number' : '')));
    }(labels[i]));
    win.node.addEventListener('keydown', function (event) {
      var key = event.key || '', mapping = { '*': '\u00d7', '/': '\u00f7', '-': '\u2212', Enter: '=', Backspace: '\u232b', Escape: 'C' };
      if (mapping[key]) key = mapping[key];
      if (/^[0-9.+]$/.test(key) || key === '=' || key === 'C' || key === '\u00d7' || key === '\u00f7' || key === '\u2212' || key === '\u232b') { event.preventDefault(); press(key); }
    });
    setStatus(win, 'Standard calculator'); return win;
  }

  function refreshLibraries() {
    for (var i = 0; i < windows.length; i++) if (windows[i].kind === 'library' && windows[i].refreshLibrary) windows[i].refreshLibrary();
  }
  function openLibrary() {
    var win = createWindow('library', 'Game Library', 940, 600), games = [], selected = null, loading = false, request = 0;
    var toolbar = add(win.body, el('div', 'toolbar'));
    var addButton = add(toolbar, button('Add a program…', function () {
      if (draftDirty()) { setStatus(win, 'Save the changed launch settings before adding another program.', true); return; }
      openFiles(null, function (file) {
        setStatus(win, 'Adding ' + file.name + '…');
        rpc('libraryAdd', { file: file.id }).then(function (game) {
          selected = game; load(); refreshLibrariesExceptSelf();
        }).catch(function (error) { setStatus(win, errorText(error), true); });
      }, { chooseLabel: 'Add to Library', filter: function (file) { return /\.exe$/i.test(file.name); }, filterMessage: 'Select a Windows .exe program to add to your library.' });
    }, 'primary'));
    add(toolbar, button('Refresh', load));
    add(toolbar, el('span', 'toolbar-spacer')); add(toolbar, el('span', 'muted library-caption', 'Saved programs \u00b7 Native runtime'));
    var area = add(win.body, el('div', 'library-workspace'));
    var grid = add(area, el('div', 'library-grid')); grid.setAttribute('aria-label', 'Saved programs');
    var details = add(area, el('aside', 'library-details')); details.setAttribute('aria-label', 'Program settings');
    var titleInput = null, notesInput = null, inputToggle = null, saveButton = null;
    function draftDirty() {
      return selected && titleInput && (titleInput.value !== selected.name || notesInput.value !== (selected.notes || '') || inputToggle.checked !== (selected.inputEnabled !== false));
    }
    function refreshLibrariesExceptSelf() {
      for (var i = 0; i < windows.length; i++) if (windows[i] !== win && windows[i].refreshLibrary) windows[i].refreshLibrary();
    }
    function cover(game, className) {
      var node = el('div', className || 'library-cover');
      add(node, icon(appById('library')));
      if (game.cover && /^data:image\/(png|jpe?g|webp|gif|bmp);base64,/i.test(game.cover)) {
        var image = el('img'); image.alt = ''; image.src = game.cover;
        image.addEventListener('error', function () { if (image.parentNode) image.parentNode.removeChild(image); });
        add(node, image);
      }
      return node;
    }
    function renderCards() {
      clear(grid);
      if (!games.length) {
        var empty = add(grid, el('div', 'empty-state')); add(empty, el('strong', '', connected ? 'Your library starts here' : 'Connect to your Xbox library'));
        add(empty, el('div', '', connected ? 'Add an .exe from Xbox Files or a selected USB folder. Programs remain on their original storage.' : 'Saved programs are read from the native host. No sample games are shown.'));
        return;
      }
      for (var i = 0; i < games.length; i++) (function (game) {
        var card = button('', function () { selectGame(game); }, 'library-card' + (selected && selected.id === game.id ? ' selected' : ''));
        card.setAttribute('data-game-id', game.id); add(card, cover(game));
        add(card, el('strong', '', game.name));
        add(card, el('span', 'muted', game.available === false ? 'Storage unavailable' : 'Saved Windows program'));
        add(grid, card);
      }(games[i]));
    }
    function selectGame(game) {
      if (selected && selected.id === game.id) return;
      if (selected && selected.id !== game.id && draftDirty()) {
        modal('Unsaved launch settings', 'Save your changes before selecting another program, or discard them.', [
          { label: 'Keep editing', value: 'cancel', primary: true }, { label: 'Discard changes', value: 'discard' }
        ]).then(function (answer) { if (answer === 'discard') { selected = game; renderCards(); renderDetails(); } });
        return;
      }
      selected = game; renderCards(); renderDetails();
    }
    function renderDetails() {
      clear(details); titleInput = notesInput = inputToggle = saveButton = null;
      if (!selected) { add(details, el('p', 'muted', 'Select a program to edit its cover and launch settings.')); return; }
      var game = selected;
      add(details, cover(game, 'library-detail-cover'));
      var nameLabel = add(details, el('label', '', 'Display name'));
      titleInput = add(details, el('input')); titleInput.type = 'text'; titleInput.maxLength = 120; titleInput.value = game.name;
      titleInput.setAttribute('aria-label', 'Display name'); nameLabel.setAttribute('for', win.id + '-game-name'); titleInput.id = win.id + '-game-name';
      if (game.executable) add(details, el('div', 'library-executable muted', game.executable));
      var notesLabel = add(details, el('label', '', 'Notes'));
      notesInput = add(details, el('textarea', 'library-notes')); notesInput.value = game.notes || ''; notesInput.maxLength = 4096;
      notesInput.setAttribute('aria-label', 'Program notes'); notesLabel.setAttribute('for', win.id + '-game-notes'); notesInput.id = win.id + '-game-notes';
      var inputLabel = add(details, el('label', 'library-input-toggle'));
      inputToggle = add(inputLabel, el('input')); inputToggle.type = 'checkbox'; inputToggle.checked = game.inputEnabled !== false;
      add(inputLabel, el('span', '', 'Forward input to this program'));
      var actions = add(details, el('div', 'library-actions'));
      saveButton = add(actions, button('Save settings', saveGame));
      var launch = add(actions, button('Launch', function () {
        if (draftDirty()) { setStatus(win, 'Save the changed launch settings before launching.', true); return; }
        launch.disabled = true; setStatus(win, 'Preparing ' + game.name + '…');
        rpc('libraryRun', { game: game.id }, 120000).then(function (result) {
          setStatus(win, result && result.started ? 'Execution dispatched. The native view shows the actual result.' : 'Native launch request completed.');
        }).catch(function (error) { setStatus(win, errorText(error), true); }).then(function () { launch.disabled = game.available === false; });
      }, 'primary')); launch.disabled = game.available === false || !connected;
      var changeCover = add(details, button('Choose cover image…', function () {
        openFiles(null, function (file) {
          setStatus(win, 'Preparing cover image…');
          rpc('libraryCover', { game: game.id, file: file.id }, 60000).then(function () { load(); refreshLibrariesExceptSelf(); })
            .catch(function (error) { setStatus(win, errorText(error), true); });
        }, { chooseLabel: 'Use as cover', filter: function (file) { return /\.(png|jpe?g|bmp|gif|webp)$/i.test(file.name); }, filterMessage: 'Choose a PNG, JPEG, BMP, GIF or WebP image.' });
      })); changeCover.disabled = !connected;
      add(details, button('Remove from library', function () {
        modal('Remove ' + game.name + '?', 'This removes the saved library entry. The original program files remain on their storage.', [
          { label: 'Cancel', value: 'cancel', primary: true }, { label: 'Remove entry', value: 'remove' }
        ]).then(function (answer) {
          if (answer !== 'remove') return;
          rpc('libraryRemove', { game: game.id }).then(function () { selected = null; load(); refreshLibrariesExceptSelf(); })
            .catch(function (error) { setStatus(win, errorText(error), true); });
        });
      }, 'library-remove'));
      if (game.available === false) add(details, el('p', 'library-note', 'Reconnect the original storage and refresh the library.'));
      add(details, el('p', 'library-note muted', 'A saved entry does not establish compatibility. Launch shows the native runtime result.'));
    }
    function saveGame() {
      if (!selected || !titleInput || !titleInput.value.trim()) { setStatus(win, 'Enter a display name.', true); return; }
      var id = selected.id, name = titleInput.value.trim(), notes = notesInput.value, enabled = inputToggle.checked;
      saveButton.disabled = true; setStatus(win, 'Saving launch settings…');
      rpc('libraryUpdate', { game: id, name: name, notes: notes, inputEnabled: enabled }).then(function (game) {
        if (!game || !game.id) throw new Error('The host did not return the saved program.');
        selected = game;
        for (var i = 0; i < games.length; i++) if (games[i].id === id) games[i] = game;
        renderCards(); renderDetails(); setStatus(win, 'Launch settings saved.'); refreshLibrariesExceptSelf();
      }).catch(function (error) { if (saveButton) saveButton.disabled = false; setStatus(win, 'Settings were not saved: ' + errorText(error), true); });
    }
    function load() {
      if (win.closed || loading) return;
      addButton.disabled = !connected;
      if (!connected) { renderCards(); renderDetails(); setStatus(win, 'Native host unavailable', true); return; }
      loading = true; var token = ++request, oldId = selected && selected.id, preserveDraft = draftDirty();
      setStatus(win, 'Reading saved programs…');
      rpc('libraryList', {}).then(function (result) {
        if (win.closed || token !== request) return;
        if (!result || !Array.isArray(result.games)) throw new Error('The host returned an invalid library.');
        games = result.games; var next = null;
        for (var i = 0; i < games.length; i++) if (games[i].id === oldId) next = games[i];
        selected = next || (games.length ? games[0] : null);
        renderCards();
        if (!preserveDraft || !next) renderDetails();
        setStatus(win, games.length + ' saved program' + (games.length === 1 ? '' : 's') + (preserveDraft && next ? ' \u00b7 Unsaved launch settings retained' : ''));
      }).catch(function (error) { setStatus(win, errorText(error), true); }).then(function () { loading = false; });
    }
    win.refreshLibrary = load; win.onConnected = load;
    win.beforeClose = function (done) {
      if (!draftDirty()) { done(); return; }
      modal('Unsaved launch settings', 'Keep this window open to save your changes, or discard them and close.', [
        { label: 'Keep editing', value: 'cancel', primary: true }, { label: 'Discard and close', value: 'discard' }
      ]).then(function (answer) { if (answer === 'discard') done(); });
    };
    addButton.disabled = !connected; load(); return win;
  }

  function openTransfer() {
    var win = createWindow('transfer', 'LAN Transfer', 700, 540), busy = false, timer = null;
    var panel = add(win.body, el('div', 'content-pad'));
    add(panel, el('h1', '', 'Send files to your Xbox'));
    add(panel, el('p', 'muted', 'Start the transfer service, then open the pairing link on a phone or computer on the same local network.'));
    var actions = add(panel, el('div', 'transfer-actions'));
    var start = add(actions, button('Start transfer service', function () { perform('lanStart'); }, 'primary'));
    var stop = add(actions, button('Stop service', function () { perform('lanStop'); })); stop.disabled = true;
    add(actions, button('Refresh status', function () { perform('lanStatus'); }));
    var info = add(panel, el('div', 'info-card transfer-info'));
    var stateLabel = add(info, el('strong', '', 'Reading transfer status…'));
    var details = add(info, el('div', 'transfer-details hidden'));
    add(details, el('label', '', 'Pairing link'));
    var pairing = add(details, el('input')); pairing.type = 'text'; pairing.readOnly = true; pairing.setAttribute('aria-label', 'Pairing link');
    add(details, el('label', '', 'Pairing code'));
    var code = add(details, el('input', 'pairing-code')); code.type = 'text'; code.readOnly = true; code.setAttribute('aria-label', 'Pairing code');
    var address = add(details, el('p', 'muted'));
    var stats = add(panel, el('div', 'transfer-stats'));
    var warning = add(panel, el('p', 'transfer-error hidden')); warning.setAttribute('role', 'status');
    add(panel, el('p', 'muted', 'Uploads go to Xbox Files / LAN Inbox. Maximum file size: 16 MiB. Received programs never launch automatically.'));
    add(panel, el('p', 'muted', 'The pairing link gives access to the transfer inbox. Share it only with the device you are using, and stop the service when finished.'));
    add(panel, button('Open Xbox Files', function () { openFiles(); }));
    function count(value) { return typeof value === 'number' && isFinite(value) && value >= 0 ? String(value) : '\u2014'; }
    function bytes(value) { return typeof value === 'number' && isFinite(value) && value >= 0 ? humanSize(value) : '\u2014'; }
    function render(result) {
      if (!result || typeof result.running !== 'boolean') throw new Error('The host returned an invalid transfer status.');
      var running = result.running;
      stateLabel.textContent = running ? 'Transfer service is running' : 'Transfer service is stopped';
      start.disabled = running || busy || !connected; stop.disabled = !running || busy;
      details.classList.toggle('hidden', !running);
      pairing.value = running && typeof result.pairingUrl === 'string' ? result.pairingUrl : '';
      code.value = running && typeof result.token === 'string' ? result.token : '';
      address.textContent = running && typeof result.url === 'string' ? result.url : '';
      clear(stats);
      add(stats, el('span', '', count(result.uploadedFiles) + ' files received \u00b7 ' + bytes(result.uploadedBytes)));
      add(stats, el('span', '', count(result.downloadedFiles) + ' files downloaded \u00b7 ' + bytes(result.downloadedBytes)));
      warning.textContent = result.lastError || ''; warning.classList.toggle('hidden', !result.lastError);
      setStatus(win, running ? 'Authenticated local transfer service active' : 'Start the service when you want to transfer files.');
    }
    function perform(method) {
      if (busy || win.closed) return; busy = true; start.disabled = stop.disabled = true;
      rpc(method, {}, 30000).then(function (result) { busy = false; render(result); }).catch(function (error) {
        busy = false; details.classList.add('hidden'); pairing.value = code.value = ''; address.textContent = '';
        stateLabel.textContent = 'Transfer status unavailable'; start.disabled = !connected; stop.disabled = false;
        setStatus(win, errorText(error), true);
      });
    }
    win.onConnected = function () { perform('lanStatus'); };
    win.onClose = function () { if (timer) clearInterval(timer); };
    perform('lanStatus'); timer = setInterval(function () { perform('lanStatus'); }, 3000);
    return win;
  }

  function openSettings() {
    var win = createWindow('settings', 'Personalization', 620, 445), panel = add(win.body, el('div', 'content-pad'));
    add(panel, el('h1', '', 'Make it yours')); add(panel, el('p', 'muted', 'These settings personalize your Mihawk desktop.'));
    var row = add(panel, el('div', 'setting-row')); add(row, el('label', '', 'Appearance'));
    var theme = add(row, el('select')); theme.setAttribute('aria-label', 'Appearance');
    var light = el('option', '', 'Light'); light.value = 'light'; add(theme, light); var dark = el('option', '', 'Dark'); dark.value = 'dark'; add(theme, dark); theme.value = settings.theme;
    theme.addEventListener('change', function () { settings.theme = theme.value; applySettings(); });
    row = add(panel, el('div', 'setting-row')); add(row, el('label', '', 'Accent color'));
    var swatches = add(row, el('div', 'swatches')), colors = ['#006fc1', '#126b54', '#7444a8', '#a63a63', '#9a541c'];
    for (var i = 0; i < colors.length; i++) (function (color) {
      var node = button('', function () {
        settings.accent = color; applySettings();
        var all = swatches.querySelectorAll('button'); for (var j = 0; j < all.length; j++) all[j].classList.toggle('selected', all[j] === node);
      }, 'swatch' + (settings.accent === color ? ' selected' : ''));
      node.style.background = color; node.setAttribute('aria-label', 'Accent ' + color); add(swatches, node);
    }(colors[i]));
    row = add(panel, el('div', 'setting-row')); add(row, el('label', '', 'Background'));
    var background = add(row, el('select')); background.setAttribute('aria-label', 'Background');
    var meadow = el('option', '', 'Aero Meadow'); meadow.value = 'wallpaper'; add(background, meadow);
    var solid = el('option', '', 'Solid accent'); solid.value = 'solid'; add(background, solid); background.value = settings.wallpaper ? 'wallpaper' : 'solid';
    background.addEventListener('change', function () { settings.wallpaper = background.value === 'wallpaper'; applySettings(); });
    add(panel, el('p', 'muted', 'Aero Meadow is original MihawkOS artwork.'));
    setStatus(win, 'Preferences save automatically on this device.'); return win;
  }
  function openBrowser() {
    var win = createWindow('browser', 'Browser', 650, 365), panel = add(win.body, el('div', 'content-pad'));
    add(panel, icon(appById('browser'))); add(panel, el('h1', '', 'Open a website'));
    add(panel, el('p', 'muted', 'The native browser view opens above the desktop. Use its Back control to return.'));
    var form = add(panel, el('form', 'browser-form')), input = add(form, el('input')); input.type = 'text'; input.value = 'https://duckduckgo.com'; input.setAttribute('aria-label', 'Website address');
    var go = add(form, button('Open', function () {}, 'primary')); go.type = 'submit';
    form.addEventListener('submit', function (event) {
      event.preventDefault(); var url = input.value.trim(); if (!/^https?:\/\//i.test(url)) url = 'https://' + url;
      if (!/^https?:\/\/[^\s/]+/i.test(url)) { setStatus(win, 'Enter an http or https website address.', true); return; }
      go.disabled = true; setStatus(win, 'Opening native browser…');
      rpc('browser', { url: url }, 120000).then(function () { go.disabled = false; setStatus(win, 'Native browser request completed.'); }).catch(function (error) { go.disabled = false; setStatus(win, errorText(error), true); });
    });
    setStatus(win, connected ? 'Ready' : 'Native host unavailable'); return win;
  }
  function openAbout() {
    var win = createWindow('about', 'About Mihawk for Xbox', 650, 495), panel = add(win.body, el('div', 'content-pad'));
    var mark = add(panel, el('img')); mark.src = 'assets/start.svg'; mark.alt = 'Mihawk'; mark.width = 48; mark.height = 48; mark.style.marginBottom = '14px';
    add(panel, el('h1', '', 'Mihawk for Xbox'));
    add(panel, el('p', 'muted', 'Your Xbox Tools desktop for files, saved programs, browsing and local transfers.'));
    function card(title, text) { var node = add(panel, el('div', 'info-card')); add(node, el('strong', '', title)); add(node, el('span', '', text)); }
    card('Native host', connected ? (hostInfo.platform || 'Xbox') + (hostInfo.version ? ' \u00b7 ' + hostInfo.version : '') : (hostInfo && hostInfo.error ? hostInfo.error : 'Not connected'));
    card('Your files', 'Xbox Files and Notepad use native Xbox storage. Add a folder or USB location through the system picker.');
    card('Game Library and transfers', 'Save programs with a cover, notes and input forwarding settings. LAN Transfer receives files into LAN Inbox from your paired phone or computer.');
    card('Windows programs', 'The experimental .exe bridge reports each program’s actual startup result. A native x64 Wine desktop is not implemented.');
    card('Credits and license', 'MihawkOS by mihawk-99; Xbox adaptation by SlaveIT contributors. Frontend source: GPL-3.0-or-later. Original icons, fonts and artwork retain their component licenses in the package.');
    add(panel, button('Reconnect native host', function () { connect().then(function () { toast('Native host', connected ? 'Connected to Xbox storage.' : 'The host is still unavailable.'); }); }));
    setStatus(win, 'Runs inside a UWP app; Xbox Home remains available.'); return win;
  }
  function openApp(id) {
    if (id === 'library') return openLibrary(); if (id === 'transfer') return openTransfer();
    if (id === 'files') return openFiles(); if (id === 'notepad') return openNotepad();
    if (id === 'calculator') return openCalculator(); if (id === 'settings') return openSettings();
    if (id === 'browser') return openBrowser(); return openAbout();
  }

  function updateClock() {
    var now = new Date();
    document.getElementById('clock-time').textContent = now.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
    document.getElementById('clock-date').textContent = now.toLocaleDateString();
  }
  for (var i = 0; i < apps.length; i++) add(desktop, appButton(apps[i], 'desktop-icon'));
  var pinned = document.getElementById('pinned-apps');
  add(pinned, appButton(appById('library'), 'taskbar-button'));
  add(pinned, appButton(appById('files'), 'taskbar-button')); add(pinned, appButton(appById('notepad'), 'taskbar-button')); add(pinned, appButton(appById('browser'), 'taskbar-button'));
  startButton.addEventListener('click', function () { showStart(); });
  document.getElementById('start-search').addEventListener('input', function (event) { renderStart(event.target.value); });
  document.getElementById('start-about').addEventListener('click', function () { showStart(false); openAbout(); });
  document.getElementById('connection-status').addEventListener('click', openAbout);
  document.getElementById('show-desktop').addEventListener('click', function () {
    var visible = false; for (var j = 0; j < windows.length; j++) if (!windows[j].minimized) visible = true;
    for (var k = 0; k < windows.length; k++) { if (visible) minimize(windows[k]); else focusWindow(windows[k]); }
  });
  document.addEventListener('pointerdown', function (event) {
    if (!hasClass(startMenu, 'hidden') && !startMenu.contains(event.target) && !startButton.contains(event.target)) showStart(false);
  });
  document.addEventListener('keydown', function (event) {
    if (event.ctrlKey && (event.key === 'Escape' || event.keyCode === 27)) { event.preventDefault(); showStart(); }
    else if ((event.key === 'Escape' || event.keyCode === 27) && !hasClass(startMenu, 'hidden')) showStart(false);
  });
  window.addEventListener('resize', function () { for (var j = 0; j < windows.length; j++) sizeWindow(windows[j]); });
  window.MihawkShell = { openApp: openApp, connect: connect };
  applySettings(); renderStart(''); updateClock(); setInterval(updateClock, 1000); connect();
}());

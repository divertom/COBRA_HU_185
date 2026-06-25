(function () {
  'use strict';

  var TIRE_POSITIONS = ['NO', 'LF', 'RF', 'LR', 'RR'];

  var timeEl = document.getElementById('phone-time');
  var dateEl = document.getElementById('phone-date');
  var tempToggle = document.getElementById('temp-toggle');
  var pressureToggle = document.getElementById('pressure-toggle');
  var unitsStatus = document.getElementById('units-status');
  var setTimeBtn = document.getElementById('btn-set-time');
  var rtcStatus = document.getElementById('rtc-status');
  var calibrateBtn = document.getElementById('btn-calibrate');
  var scanBtn = document.getElementById('btn-scan');
  var scanResult = document.getElementById('scan-result');
  var reorderList = document.getElementById('reorder-list');
  var pagesStatus = document.getElementById('pages-status');
  var tpmsList = document.getElementById('tpms-sensor-list');
  var tpmsScanStatus = document.getElementById('tpms-scan-status');
  var tpmsScanStart = document.getElementById('btn-tpms-scan-start');
  var tpmsScanStop = document.getElementById('btn-tpms-scan-stop');

  var dragItem = null;
  var pagesSaveTimer = null;
  var tpmsPollTimer = null;
  var unitsSaveTimer = null;
  var openTireMenu = null;

  var state = {
    tempUnit: 'C',
    pressureUnit: 'kpa',
    scanActive: false,
    primaryGattMac: null,
    sensors: []
  };

  function pad(n) {
    return n < 10 ? '0' + n : String(n);
  }

  function updatePhoneClock() {
    var now = new Date();
    if (timeEl) {
      timeEl.textContent =
        pad(now.getHours()) + ':' +
        pad(now.getMinutes()) + ':' +
        pad(now.getSeconds());
    }
    if (dateEl) {
      dateEl.textContent = now.toLocaleDateString(undefined, {
        day: 'numeric',
        month: 'short',
        year: 'numeric'
      });
    }
  }

  function setStatus(el, text, isError) {
    if (!el) {
      return;
    }
    el.textContent = text || '';
    el.classList.toggle('is-error', !!isError);
    el.classList.toggle('is-ok', !!text && !isError);
  }

  function setToggleGroup(toggleEl, unit, attr) {
    if (!toggleEl) {
      return;
    }
    var buttons = toggleEl.querySelectorAll('button');
    for (var i = 0; i < buttons.length; i++) {
      var btn = buttons[i];
      var selected = btn.getAttribute(attr) === unit;
      btn.classList.toggle('active', selected);
      btn.setAttribute('aria-pressed', selected ? 'true' : 'false');
    }
  }

  function refreshReorderNumbers() {
    if (!reorderList) {
      return;
    }
    var items = reorderList.querySelectorAll('.reorder-item:not(.reorder-loading)');
    for (var i = 0; i < items.length; i++) {
      var num = items[i].querySelector('.item-num');
      if (num) {
        num.textContent = String(i + 1);
      }
    }
  }

  function getReorderIds() {
    var items = reorderList.querySelectorAll('.reorder-item[data-page]');
    var ids = [];
    for (var i = 0; i < items.length; i++) {
      ids.push(items[i].getAttribute('data-page'));
    }
    return ids;
  }

  function buildReorderItem(page, index) {
    var li = document.createElement('li');
    li.className = 'reorder-item';
    li.setAttribute('draggable', 'true');
    li.setAttribute('data-page', page.id);
    li.innerHTML =
      '<svg class="drag-handle" aria-hidden="true"><use href="#icon-drag"></use></svg>' +
      '<span class="item-num">' + String(index + 1) + '</span>' +
      '<span class="item-name"></span>';
    li.querySelector('.item-name').textContent = page.name;
    return li;
  }

  function renderReorderList(pages) {
    if (!reorderList) {
      return;
    }
    reorderList.innerHTML = '';
    for (var i = 0; i < pages.length; i++) {
      reorderList.appendChild(buildReorderItem(pages[i], i));
    }
  }

  function parsePagesPayload(data) {
    if (Array.isArray(data)) {
      return data;
    }
    if (data && data.ok && Array.isArray(data.pages)) {
      return data.pages;
    }
    return null;
  }

  function fetchJson(url, options) {
    return fetch(url, options).then(function (res) {
      var type = (res.headers.get('Content-Type') || '').toLowerCase();
      if (!res.ok) {
        throw new Error('HTTP ' + res.status);
      }
      if (type.indexOf('json') < 0) {
        throw new Error('expected JSON');
      }
      return res.json();
    });
  }

  function loadPageOrder() {
    fetchJson('/api/pages/order')
      .then(function (data) {
        var pages = parsePagesPayload(data);
        if (!pages || pages.length === 0) {
          throw new Error('invalid response');
        }
        renderReorderList(pages);
        setStatus(pagesStatus, '', false);
      })
      .catch(function (err) {
        if (reorderList) {
          reorderList.innerHTML =
            '<li class="reorder-item reorder-loading">Could not load pages</li>';
        }
        setStatus(pagesStatus, err.message || 'Failed to load page order', true);
      });
  }

  function savePageOrder() {
    var ids = getReorderIds();
    if (ids.length === 0) {
      return;
    }
    setStatus(pagesStatus, 'Saving…', false);
    fetchJson('/api/pages/order', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(ids)
    })
      .then(function (data) {
        if (!data.ok) {
          throw new Error(data.error || 'save failed');
        }
        setStatus(pagesStatus, 'Saved', false);
        if (pagesSaveTimer) {
          clearTimeout(pagesSaveTimer);
        }
        pagesSaveTimer = setTimeout(function () {
          setStatus(pagesStatus, '', false);
        }, 2000);
      })
      .catch(function (err) {
        setStatus(pagesStatus, err.message || 'Save failed', true);
      });
  }

  function saveUnits() {
    fetchJson('/api/settings/units', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        temp_unit: state.tempUnit,
        pressure_unit: state.pressureUnit
      })
    })
      .then(function (data) {
        if (!data.ok) {
          throw new Error(data.error || 'save failed');
        }
        setStatus(unitsStatus, 'Saved', false);
        if (unitsSaveTimer) {
          clearTimeout(unitsSaveTimer);
        }
        unitsSaveTimer = setTimeout(function () {
          setStatus(unitsStatus, '', false);
        }, 2000);
        loadTpms();
      })
      .catch(function (err) {
        setStatus(unitsStatus, err.message || 'Save failed', true);
      });
  }

  function loadUnits() {
    return fetchJson('/api/settings/units')
      .then(function (data) {
        if (!data.ok) {
          throw new Error(data.error || 'load failed');
        }
        state.tempUnit = data.temp_unit === 'F' ? 'F' : 'C';
        state.pressureUnit = data.pressure_unit === 'psi' ? 'psi' : 'kpa';
        setToggleGroup(tempToggle, state.tempUnit, 'data-unit');
        setToggleGroup(pressureToggle, state.pressureUnit, 'data-unit');
      });
  }

  function pressureUnitLabel() {
    return state.pressureUnit === 'psi' ? 'psi' : 'kPa';
  }

  function tempUnitLabel() {
    return state.tempUnit === 'F' ? '°F' : '°C';
  }

  function formatTelem(sensor) {
    if (sensor.telemetry_state === 'none' || sensor.pressure == null) {
      return '-- ' + pressureUnitLabel() + '  -- ' + tempUnitLabel();
    }
    var p = Math.round(sensor.pressure * 10) / 10;
    var t = Math.round(sensor.temperature * 10) / 10;
    return String(p) + ' ' + pressureUnitLabel() + '  ' + String(t) + ' ' + tempUnitLabel();
  }

  function statusClass(status) {
    if (status === 'live') {
      return 'status-live';
    }
    if (status === 'recent') {
      return 'status-recent';
    }
    return 'status-stale';
  }

  function telemClass(telemetryState) {
    if (telemetryState === 'stable') {
      return 'telem-stable';
    }
    if (telemetryState === 'active') {
      return 'telem-active';
    }
    return 'telem-none';
  }

  function assignedPositions(excludeMac) {
    var map = {};
    for (var i = 0; i < state.sensors.length; i++) {
      var s = state.sensors[i];
      if (s.mac !== excludeMac && s.position && s.position !== 'NO') {
        map[s.position] = true;
      }
    }
    return map;
  }

  function closeAllTireMenus() {
    if (!tpmsList) {
      return;
    }
    var open = tpmsList.querySelectorAll('.tire-dropdown.open');
    for (var i = 0; i < open.length; i++) {
      open[i].classList.remove('open');
      var row = open[i].closest('.tpms-sensor-row');
      if (row) {
        row.classList.remove('tpms-row-menu-open');
      }
      var toggle = open[i].querySelector('.tire-dropdown-btn');
      if (toggle) {
        toggle.setAttribute('aria-expanded', 'false');
      }
    }
    openTireMenu = null;
  }

  function getTpmsRowMac(row) {
    if (!row) {
      return null;
    }
    return row.getAttribute('data-mac');
  }

  function buildTireMenu(sensor, assigned) {
    var wrap = document.createElement('div');
    wrap.className = 'tire-dropdown';

    var btn = document.createElement('button');
    btn.type = 'button';
    btn.className = 'tire-dropdown-btn';
    btn.textContent = sensor.position || 'NO';
    btn.setAttribute('aria-haspopup', 'listbox');
    btn.setAttribute('aria-expanded', 'false');

    var menu = document.createElement('div');
    menu.className = 'tire-dropdown-menu';
    menu.setAttribute('role', 'listbox');

    for (var i = 0; i < TIRE_POSITIONS.length; i++) {
      var pos = TIRE_POSITIONS[i];
      var opt = document.createElement('button');
      opt.type = 'button';
      opt.className = 'tire-option';
      opt.setAttribute('role', 'option');
      opt.textContent = pos;
      if (pos === sensor.position) {
        opt.classList.add('tire-option-assigned');
      } else if (pos !== 'NO' && assigned[pos]) {
        opt.classList.add('tire-option-assigned');
      } else {
        opt.classList.add('tire-option-free');
      }
      opt.setAttribute('data-position', pos);
      menu.appendChild(opt);
    }

    wrap.appendChild(btn);
    wrap.appendChild(menu);
    return wrap;
  }

  function updateForgetButtons() {
    if (!tpmsList) {
      return;
    }
    var buttons = tpmsList.querySelectorAll('.btn-forget');
    for (var i = 0; i < buttons.length; i++) {
      buttons[i].disabled = state.scanActive;
      buttons[i].title = state.scanActive ? 'Stop scan before removing a sensor' : '';
    }
  }

  function setPrimaryGatt(mac) {
    fetchJson('/api/tpms/sensors/primary', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ mac: mac })
    })
      .then(function (data) {
        if (!data.ok) {
          throw new Error(data.error || 'failed');
        }
        setStatus(tpmsScanStatus, 'Live source updated', false);
        loadTpms();
      })
      .catch(function (err) {
        setStatus(tpmsScanStatus, err.message || 'Set live source failed', true);
        loadTpms();
      });
  }

  function handleTpmsListClick(event) {
    var primaryBtn = event.target.closest('.btn-primary-gatt');
    if (primaryBtn) {
      event.preventDefault();
      event.stopPropagation();
      if (state.scanActive || primaryBtn.disabled) {
        return;
      }
      var primaryRow = primaryBtn.closest('.tpms-sensor-row[data-mac]');
      var primaryMac = getTpmsRowMac(primaryRow);
      if (primaryMac) {
        setPrimaryGatt(primaryMac);
      }
      return;
    }

    var forgetBtn = event.target.closest('.btn-forget');
    if (forgetBtn) {
      event.preventDefault();
      event.stopPropagation();
      if (state.scanActive || forgetBtn.disabled) {
        return;
      }
      var forgetRow = forgetBtn.closest('.tpms-sensor-row[data-mac]');
      var forgetMac = getTpmsRowMac(forgetRow);
      if (forgetMac) {
        forgetSensor(forgetMac);
      }
      return;
    }

    var toggleBtn = event.target.closest('.tire-dropdown-btn');
    if (toggleBtn) {
      event.stopPropagation();
      var wrap = toggleBtn.closest('.tire-dropdown');
      if (!wrap) {
        return;
      }
      var wasOpen = wrap.classList.contains('open');
      closeAllTireMenus();
      if (!wasOpen) {
        wrap.classList.add('open');
        openTireMenu = wrap;
        toggleBtn.setAttribute('aria-expanded', 'true');
        var row = wrap.closest('.tpms-sensor-row');
        if (row) {
          row.classList.add('tpms-row-menu-open');
        }
      }
    }
  }

  function handleTpmsListMousedown(event) {
    var optBtn = event.target.closest('.tire-option');
    if (!optBtn) {
      return;
    }
    event.preventDefault();
    event.stopPropagation();
    var row = optBtn.closest('.tpms-sensor-row[data-mac]');
    var mac = getTpmsRowMac(row);
    var newPos = optBtn.getAttribute('data-position');
    if (!mac || !newPos) {
      return;
    }
    var toggleBtn = row ? row.querySelector('.tire-dropdown-btn') : null;
    if (toggleBtn) {
      toggleBtn.textContent = newPos;
    }
    closeAllTireMenus();
    setSensorPosition(mac, newPos);
  }

  function setSensorPosition(mac, position) {
    fetchJson('/api/tpms/sensors/position', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ mac: mac, position: position })
    })
      .then(function (data) {
        if (!data.ok) {
          throw new Error(data.error || 'failed');
        }
        openTireMenu = null;
        loadTpms();
      })
      .catch(function (err) {
        setStatus(tpmsScanStatus, err.message || 'Assign failed', true);
        loadTpms();
      });
  }

  function forgetSensor(mac) {
    fetchJson('/api/tpms/sensors/forget', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ mac: mac })
    })
      .then(function (data) {
        if (!data.ok) {
          throw new Error(data.error || 'failed');
        }
        openTireMenu = null;
        loadTpms();
      })
      .catch(function (err) {
        setStatus(tpmsScanStatus, err.message || 'Forget failed', true);
      });
  }

  function updateTpmsListInPlace() {
    if (!tpmsList || !state.sensors) {
      return;
    }
    var rows = tpmsList.querySelectorAll('.tpms-sensor-row[data-mac]');
    for (var i = 0; i < rows.length; i++) {
      var li = rows[i];
      var mac = li.getAttribute('data-mac');
      var sensor = null;
      for (var j = 0; j < state.sensors.length; j++) {
        if (state.sensors[j].mac === mac) {
          sensor = state.sensors[j];
          break;
        }
      }
      if (!sensor) {
        continue;
      }
      li.className = 'tpms-sensor-row ' + statusClass(sensor.status);
      li.setAttribute('data-mac', sensor.mac);
      var macEl = li.querySelector('.tpms-sensor-mac');
      if (macEl) {
        var label = sensor.display_name ? sensor.display_name + ' · ' + sensor.mac : sensor.mac;
        if (sensor.is_gatt_active) {
          label += ' · Reading';
        } else if (sensor.is_primary_gatt) {
          label += ' · Live';
        }
        macEl.textContent = label;
      }
      if (sensor.is_gatt_active) {
        li.classList.add('gatt-active');
      } else {
        li.classList.remove('gatt-active');
      }
      var primaryBtn = li.querySelector('.btn-primary-gatt');
      if (primaryBtn) {
        primaryBtn.disabled = state.scanActive || !!sensor.is_primary_gatt;
        primaryBtn.classList.toggle('is-active', !!sensor.is_primary_gatt);
      }
      var telemEl = li.querySelector('.tpms-telem');
      if (telemEl) {
        telemEl.className = 'tpms-telem ' + telemClass(sensor.telemetry_state);
        telemEl.textContent = formatTelem(sensor);
      }
      var tireBtn = li.querySelector('.tire-dropdown-btn');
      if (tireBtn) {
        tireBtn.textContent = sensor.position || 'NO';
      }
    }
  }

  function renderTpmsList() {
    if (!tpmsList) {
      return;
    }

    if (!state.sensors || state.sensors.length === 0) {
      tpmsList.innerHTML =
        '<li class="tpms-sensor-row tpms-empty">No sensors yet. Start scan to discover new sensors.</li>';
      return;
    }

    tpmsList.innerHTML = '';
    for (var i = 0; i < state.sensors.length; i++) {
      var sensor = state.sensors[i];
      var li = document.createElement('li');
      li.className = 'tpms-sensor-row ' + statusClass(sensor.status);
      li.setAttribute('data-mac', sensor.mac);

      var macEl = document.createElement('div');
      macEl.className = 'tpms-sensor-mac';
      var label = sensor.display_name ? sensor.display_name + ' · ' + sensor.mac : sensor.mac;
      if (sensor.is_gatt_active) {
        label += ' · Reading';
      } else if (sensor.is_primary_gatt) {
        label += ' · Live';
      }
      macEl.textContent = label;
      if (sensor.is_gatt_active) {
        li.classList.add('gatt-active');
      }

      var telemEl = document.createElement('div');
      telemEl.className = 'tpms-telem ' + telemClass(sensor.telemetry_state);
      telemEl.textContent = formatTelem(sensor);

      var assigned = assignedPositions(sensor.mac);
      var tireMenu = buildTireMenu(sensor, assigned);

      var primaryBtn = null;
      if (sensor.is_gateway) {
        primaryBtn = document.createElement('button');
        primaryBtn.type = 'button';
        primaryBtn.className = 'btn-primary-gatt';
        primaryBtn.textContent = sensor.is_primary_gatt ? 'Live' : 'Set live';
        primaryBtn.disabled = state.scanActive || !!sensor.is_primary_gatt;
        if (sensor.is_primary_gatt) {
          primaryBtn.classList.add('is-active');
        }
      }

      var forgetBtn = document.createElement('button');
      forgetBtn.type = 'button';
      forgetBtn.className = 'btn-forget';
      forgetBtn.textContent = 'Forget';

      li.appendChild(macEl);
      li.appendChild(telemEl);
      li.appendChild(tireMenu);
      if (primaryBtn) {
        li.appendChild(primaryBtn);
      }
      li.appendChild(forgetBtn);
      tpmsList.appendChild(li);
    }
    updateForgetButtons();
  }

  function updateScanButtons() {
    if (tpmsScanStart) {
      tpmsScanStart.disabled = state.scanActive;
    }
    if (tpmsScanStop) {
      tpmsScanStop.disabled = !state.scanActive;
    }
    if (state.scanActive) {
      var cur = tpmsScanStatus ? tpmsScanStatus.textContent : '';
      if (!tpmsScanStatus || !tpmsScanStatus.classList.contains('is-error')) {
        if (cur === '' || cur.indexOf('Scanning') >= 0) {
          setStatus(tpmsScanStatus, 'Scanning for new sensors…', false);
        }
      }
    } else if (tpmsScanStatus && !tpmsScanStatus.classList.contains('is-error')) {
      var idle = tpmsScanStatus.textContent || '';
      if (idle.indexOf('Scanning') >= 0 || idle === 'Removed') {
        if (state.rotationActive) {
          setStatus(tpmsScanStatus, 'Rotating GATT reads across sensors…', false);
        } else {
          setStatus(tpmsScanStatus, '', false);
        }
      } else if (state.rotationActive && (idle === '' || idle.indexOf('Rotating') >= 0)) {
        setStatus(tpmsScanStatus, 'Rotating GATT reads across sensors…', false);
      }
    }
    updateForgetButtons();
  }

  function loadTpms() {
    return fetchJson('/api/tpms')
      .then(function (data) {
        if (!data.ok) {
          throw new Error(data.error || 'load failed');
        }
        state.scanActive = !!data.scan_active;
        state.rotationActive = !!data.rotation_active;
        state.primaryGattMac = data.primary_gatt_mac || null;
        state.sensors = Array.isArray(data.sensors) ? data.sensors : [];
        if (data.sensors.length > 0 && data.sensors[0].temp_unit) {
          state.tempUnit = data.sensors[0].temp_unit === 'F' ? 'F' : 'C';
          state.pressureUnit = data.sensors[0].pressure_unit === 'psi' ? 'psi' : 'kpa';
          setToggleGroup(tempToggle, state.tempUnit, 'data-unit');
          setToggleGroup(pressureToggle, state.pressureUnit, 'data-unit');
        }
        updateScanButtons();
        if (openTireMenu) {
          updateTpmsListInPlace();
        } else {
          renderTpmsList();
        }
      })
      .catch(function (err) {
        if (tpmsList) {
          tpmsList.innerHTML =
            '<li class="tpms-sensor-row tpms-empty">Could not load sensors</li>';
        }
        setStatus(tpmsScanStatus, err.message || 'Load failed', true);
      });
  }

  function setTpmsScan(active) {
    fetchJson('/api/tpms/scan', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ active: active })
    })
      .then(function (data) {
        if (!data.ok) {
          throw new Error(data.error || 'scan failed');
        }
        state.scanActive = active;
        updateScanButtons();
        loadTpms();
      })
      .catch(function (err) {
        setStatus(tpmsScanStatus, err.message || 'Scan failed', true);
      });
  }

  function startTpmsPolling() {
    if (tpmsPollTimer) {
      clearInterval(tpmsPollTimer);
    }
    tpmsPollTimer = setInterval(function () {
      if (document.visibilityState === 'visible') {
        loadTpms();
      }
    }, 2000);
  }

  if (tempToggle) {
    tempToggle.addEventListener('click', function (event) {
      var btn = event.target.closest('button[data-unit]');
      if (!btn) {
        return;
      }
      state.tempUnit = btn.getAttribute('data-unit');
      setToggleGroup(tempToggle, state.tempUnit, 'data-unit');
      saveUnits();
    });
  }

  if (pressureToggle) {
    pressureToggle.addEventListener('click', function (event) {
      var btn = event.target.closest('button[data-unit]');
      if (!btn) {
        return;
      }
      state.pressureUnit = btn.getAttribute('data-unit');
      setToggleGroup(pressureToggle, state.pressureUnit, 'data-unit');
      saveUnits();
    });
  }

  if (setTimeBtn) {
    setTimeBtn.addEventListener('click', function () {
      var now = new Date();
      var payload = {
        year: now.getFullYear(),
        month: now.getMonth() + 1,
        day: now.getDate(),
        hour: now.getHours(),
        minute: now.getMinutes(),
        second: now.getSeconds()
      };

      setStatus(rtcStatus, 'Syncing…', false);
      setTimeBtn.disabled = true;

      fetchJson('/api/rtc/set', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload)
      })
        .then(function (data) {
          if (!data.ok) {
            throw new Error(data.error || 'sync failed');
          }
          return fetchJson('/api/rtc');
        })
        .then(function (data) {
          if (data.ok) {
            setStatus(
              rtcStatus,
              'Time synced (' +
                data.year + '-' + pad(data.month) + '-' + pad(data.day) + ' ' +
                pad(data.hour) + ':' + pad(data.minute) + ':' + pad(data.second) + ')',
              false
            );
          } else {
            setStatus(rtcStatus, 'Time set', false);
          }
        })
        .catch(function (err) {
          setStatus(rtcStatus, err.message || 'Sync failed', true);
        })
        .finally(function () {
          setTimeBtn.disabled = false;
        });
    });
  }

  if (calibrateBtn) {
    calibrateBtn.addEventListener('click', function () {
      console.log('[Service] Start Calibration — future API: POST /api/accelerometer/calibrate');
    });
  }

  if (scanBtn) {
    scanBtn.addEventListener('click', function () {
      if (scanResult) {
        scanResult.textContent = 'Scanning…';
      }
      window.setTimeout(function () {
        if (scanResult) {
          scanResult.textContent = 'No devices found';
        }
      }, 1200);
    });
  }

  if (tpmsScanStart) {
    tpmsScanStart.addEventListener('click', function () {
      setTpmsScan(true);
    });
  }

  if (tpmsScanStop) {
    tpmsScanStop.addEventListener('click', function () {
      setTpmsScan(false);
    });
  }

  if (tpmsList) {
    tpmsList.addEventListener('click', handleTpmsListClick);
    tpmsList.addEventListener('mousedown', handleTpmsListMousedown);
  }

  document.addEventListener('click', function (event) {
    if (event.target.closest('.tire-dropdown-btn') || event.target.closest('.tire-option')) {
      return;
    }
    closeAllTireMenus();
  });

  if (reorderList) {
    reorderList.addEventListener('dragstart', function (event) {
      var item = event.target.closest('.reorder-item[data-page]');
      if (!item) {
        return;
      }
      dragItem = item;
      item.classList.add('dragging');
      event.dataTransfer.effectAllowed = 'move';
    });

    reorderList.addEventListener('dragend', function () {
      if (dragItem) {
        dragItem.classList.remove('dragging');
        dragItem = null;
        refreshReorderNumbers();
        savePageOrder();
      }
    });

    reorderList.addEventListener('dragover', function (event) {
      event.preventDefault();
      var target = event.target.closest('.reorder-item[data-page]');
      if (!target || !dragItem || target === dragItem) {
        return;
      }
      var rect = target.getBoundingClientRect();
      var after = event.clientY > rect.top + rect.height / 2;
      reorderList.insertBefore(dragItem, after ? target.nextSibling : target);
    });
  }

  if (window.__cobraClockTimer) {
    clearInterval(window.__cobraClockTimer);
  }
  updatePhoneClock();
  window.setInterval(updatePhoneClock, 1000);

  loadPageOrder();
  loadUnits().then(loadTpms).catch(function () {
    loadTpms();
  });
  startTpmsPolling();
})();

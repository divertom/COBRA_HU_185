(function () {
  'use strict';

  var timeEl = document.getElementById('phone-time');
  var dateEl = document.getElementById('phone-date');
  var tempToggle = document.getElementById('temp-toggle');
  var setTimeBtn = document.getElementById('btn-set-time');
  var rtcStatus = document.getElementById('rtc-status');
  var calibrateBtn = document.getElementById('btn-calibrate');
  var scanBtn = document.getElementById('btn-scan');
  var scanResult = document.getElementById('scan-result');
  var reorderList = document.getElementById('reorder-list');
  var pagesStatus = document.getElementById('pages-status');
  var dragItem = null;
  var pagesSaveTimer = null;

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

  function setTempUnit(unit) {
    if (!tempToggle) {
      return;
    }
    var buttons = tempToggle.querySelectorAll('button');
    for (var i = 0; i < buttons.length; i++) {
      var btn = buttons[i];
      var selected = btn.getAttribute('data-unit') === unit;
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

  if (tempToggle) {
    tempToggle.addEventListener('click', function (event) {
      var btn = event.target.closest('button[data-unit]');
      if (!btn) {
        return;
      }
      setTempUnit(btn.getAttribute('data-unit'));
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
})();

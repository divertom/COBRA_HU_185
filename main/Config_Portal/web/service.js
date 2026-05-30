(function () {
  'use strict';

  var timeEl = document.getElementById('phone-time');
  var dateEl = document.getElementById('phone-date');
  var tempToggle = document.getElementById('temp-toggle');
  var setTimeBtn = document.getElementById('btn-set-time');
  var calibrateBtn = document.getElementById('btn-calibrate');
  var scanBtn = document.getElementById('btn-scan');
  var scanResult = document.getElementById('scan-result');
  var reorderList = document.getElementById('reorder-list');

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

  if (tempToggle) {
    tempToggle.addEventListener('click', function (event) {
      var btn = event.target.closest('button[data-unit]');
      if (!btn) {
        return;
      }
      setTempUnit(btn.getAttribute('data-unit'));
      // TODO: POST /api/settings/temp-unit with selected unit
    });
  }

  if (setTimeBtn) {
    setTimeBtn.addEventListener('click', function () {
      // TODO: POST /api/rtc/set with phone timestamp
      console.log('[Service] Set Time clicked — future API: POST /api/rtc/set');
    });
  }

  if (calibrateBtn) {
    calibrateBtn.addEventListener('click', function () {
      // TODO: POST /api/accelerometer/calibrate
      console.log('[Service] Start Calibration clicked — future API: POST /api/accelerometer/calibrate');
    });
  }

  if (scanBtn) {
    scanBtn.addEventListener('click', function () {
      // TODO: POST /api/bt/scan and populate scan results
      if (scanResult) {
        scanResult.textContent = 'Scanning…';
      }
      console.log('[Service] Scan clicked — future API: POST /api/bt/scan');
      window.setTimeout(function () {
        if (scanResult) {
          scanResult.textContent = 'No devices found';
        }
      }, 1200);
    });
  }

  if (reorderList) {
    var dragItem = null;

    reorderList.addEventListener('dragstart', function (event) {
      var item = event.target.closest('.reorder-item');
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
        // TODO: POST /api/pages/order with new page sequence
      }
    });

    reorderList.addEventListener('dragover', function (event) {
      event.preventDefault();
      var target = event.target.closest('.reorder-item');
      if (!target || !dragItem || target === dragItem) {
        return;
      }
      var rect = target.getBoundingClientRect();
      var after = event.clientY > rect.top + rect.height / 2;
      reorderList.insertBefore(dragItem, after ? target.nextSibling : target);
    });
  }

  function refreshReorderNumbers() {
    if (!reorderList) {
      return;
    }
    var items = reorderList.querySelectorAll('.reorder-item');
    for (var i = 0; i < items.length; i++) {
      var num = items[i].querySelector('.item-num');
      if (num) {
        num.textContent = String(i + 1);
      }
    }
  }

  updatePhoneClock();
  window.setInterval(updatePhoneClock, 1000);
})();

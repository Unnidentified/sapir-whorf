/* Sapir-Whorf — renders the /history page from locally stored search history. */
(function () {
  'use strict';

  var HISTORY_KEY = 'symsearch.history';
  var ENABLED_KEY = 'symsearch.history.enabled';
  var TTL_KEY = 'symsearch.history.ttl';   /* days; 0 = keep forever */

  function lsGet(k) {
    try { return window.localStorage.getItem(k); } catch (e) { return null; }
  }

  function lsSet(k, v) {
    try { window.localStorage.setItem(k, v); } catch (e) {}
  }

  function isEnabled() {
    var v = lsGet(ENABLED_KEY);
    return v === null ? true : v === '1';
  }

  function ttlDays() {
    var v = parseInt(lsGet(TTL_KEY), 10);
    return isNaN(v) || v < 0 ? 0 : v;
  }

  function readHistory() {
    var raw = lsGet(HISTORY_KEY);
    if (!raw) return [];
    var arr;
    try { arr = JSON.parse(raw); } catch (e) { return []; }
    if (Object.prototype.toString.call(arr) !== '[object Array]') return [];
    var out = [];
    for (var i = 0; i < arr.length; i++) {
      var e = arr[i];
      if (typeof e === 'string') {
        if (e.trim()) out.push({ q: e, t: 0 });
      } else if (e && typeof e === 'object' && typeof e.q === 'string' && e.q.trim()) {
        out.push({ q: e.q, t: typeof e.t === 'number' ? e.t : 0 });
      }
    }
    return out;
  }

  function writeHistory(entries) {
    lsSet(HISTORY_KEY, JSON.stringify(entries));
  }

  function prune(entries) {
    var days = ttlDays();
    if (!days) return entries;
    var cutoff = Date.now() - days * 86400000;
    var out = [];
    for (var i = 0; i < entries.length; i++) {
      if (!entries[i].t || entries[i].t >= cutoff) out.push(entries[i]);
    }
    return out;
  }

  function formatTime(t) {
    if (!t) return 'earlier';
    var d = new Date(t);
    if (isNaN(d.getTime())) return 'earlier';
    var now = new Date();
    var sameDay = d.getFullYear() === now.getFullYear() &&
                  d.getMonth() === now.getMonth() &&
                  d.getDate() === now.getDate();
    var time = d.toLocaleTimeString(undefined, { hour: '2-digit', minute: '2-digit' });
    if (sameDay) return 'today ' + time;
    return d.toLocaleDateString(undefined, { day: '2-digit', month: 'short', year: 'numeric' }) +
           ' · ' + time;
  }

  function removeAt(index) {
    var entries = readHistory();
    entries.splice(index, 1);
    writeHistory(entries);
    render();
  }

  function render() {
    var list = document.getElementById('history-list');
    var empty = document.getElementById('history-empty');
    var clear = document.getElementById('history-clear');
    if (!list) return;

    var entries = prune(readHistory());
    writeHistory(entries);
    list.innerHTML = '';

    if (!entries.length) {
      if (empty) empty.hidden = false;
      if (clear) clear.hidden = true;
    } else {
      if (empty) empty.hidden = true;
      if (clear) clear.hidden = false;
    }

    for (var i = 0; i < entries.length; i++) {
      (function (entry, index) {
        var li = document.createElement('li');
        li.className = 'history-item';

        var link = document.createElement('a');
        link.className = 'history-query';
        link.href = '/search?q=' + encodeURIComponent(entry.q);
        link.textContent = entry.q;
        link.title = entry.q;

        var stamp = document.createElement('time');
        stamp.className = 'history-time';
        stamp.textContent = formatTime(entry.t);
        if (entry.t) stamp.setAttribute('datetime', new Date(entry.t).toISOString());

        var del = document.createElement('button');
        del.type = 'button';
        del.className = 'history-delete';
        del.textContent = 'Delete';
        del.setAttribute('aria-label', 'Delete ' + entry.q + ' from history');
        del.addEventListener('click', function () { removeAt(index); });

        li.appendChild(link);
        li.appendChild(stamp);
        li.appendChild(del);
        list.appendChild(li);
      })(entries[i], i);
    }

    var note = document.getElementById('history-off-note');
    if (!isEnabled()) {
      if (!note) {
        note = document.createElement('p');
        note.id = 'history-off-note';
        note.className = 'history-off-note';
        note.textContent = 'History recording is currently turned off.';
        list.parentNode.insertBefore(note, list.nextSibling);
      }
    } else if (note) {
      note.remove();
    }
  }

  function init() {
    var select = document.getElementById('history-autodelete');
    if (select) {
      select.value = String(ttlDays());
      select.addEventListener('change', function () {
        lsSet(TTL_KEY, String(parseInt(select.value, 10) || 0));
        render();
      });
    }

    render();

    var clear = document.getElementById('history-clear');
    if (clear) {
      clear.addEventListener('click', function () {
        try { window.localStorage.removeItem(HISTORY_KEY); } catch (e) {}
        render();
      });
    }
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }
})();

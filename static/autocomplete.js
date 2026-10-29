/* SymSearch — lightweight client-side autocomplete + search history.
 * No backend changes: suggestions come from locally stored search history.
 * Injects its own styles and a history on/off switch next to each search box. */
(function () {
  'use strict';

  var HISTORY_KEY = 'symsearch.history';
  var ENABLED_KEY = 'symsearch.history.enabled';
  var TTL_KEY = 'symsearch.history.ttl';   /* days; 0 = keep forever */
  var MAX_HISTORY = 25;
  var MAX_SHOWN = 8;

  function lsGet(k) {
    try { return window.localStorage.getItem(k); } catch (e) { return null; }
  }
  function lsSet(k, v) {
    try { window.localStorage.setItem(k, v); } catch (e) {}
  }

  function ttlDays() {
    var v = parseInt(lsGet(TTL_KEY), 10);
    return isNaN(v) || v < 0 ? 0 : v;
  }

  function pruneExpired(arr) {
    var days = ttlDays();
    if (!days) return arr;
    var cutoff = Date.now() - days * 86400000;
    return arr.filter(function (e) { return !e.t || e.t >= cutoff; });
  }

  function isEnabled() {
    var v = lsGet(ENABLED_KEY);
    return v === null ? true : v === '1';
  }
  function setEnabled(on) { lsSet(ENABLED_KEY, on ? '1' : '0'); }

  function normalize(entry) {
    if (typeof entry === 'string') {
      return entry.trim() ? { q: entry, t: 0 } : null;
    }
    if (entry && typeof entry === 'object' && typeof entry.q === 'string') {
      return {
        q: entry.q,
        t: typeof entry.t === 'number' ? entry.t : 0
      };
    }
    return null;
  }

  function getHistory() {
    var raw = lsGet(HISTORY_KEY);
    if (!raw) return [];
    try {
      var arr = JSON.parse(raw);
      if (Object.prototype.toString.call(arr) !== '[object Array]') return [];
      var out = [];
      for (var i = 0; i < arr.length; i++) {
        var e = normalize(arr[i]);
        if (e && e.q.trim() !== '') out.push(e);
      }
      return pruneExpired(out);
    } catch (e) { return []; }
  }
  function saveHistory(arr) {
    lsSet(HISTORY_KEY, JSON.stringify(arr.slice(0, MAX_HISTORY)));
  }

  function pushHistory(q) {
    q = (q || '').replace(/\s+/g, ' ').trim();
    if (!q || !isEnabled()) return;
    var h = getHistory().filter(function (x) {
      return x.q.toLowerCase() !== q.toLowerCase();
    });
    h.unshift({ q: q, t: Date.now() });
    saveHistory(h);
  }

  function matching(q) {
    if (!q) return [];
    var h = getHistory();
    var lq = q.toLowerCase();
    var starts = [], contains = [];
    for (var i = 0; i < h.length; i++) {
      var li = h[i].q.toLowerCase();
      if (li === lq) continue;
      if (li.indexOf(lq) === 0) starts.push(h[i].q);
      else if (li.indexOf(lq) !== -1) contains.push(h[i].q);
    }
    return starts.concat(contains).slice(0, MAX_SHOWN);
  }

  var CSS = [
    '.sym-ac-wrap{position:relative;display:block;width:100%}',
    '.sym-ac-list{position:absolute;left:0;right:0;top:100%;z-index:60;display:none;',
    'margin:4px 0 0;padding:0;list-style:none;max-height:280px;overflow-y:auto;text-align:left;',
    'background:var(--bg-card,#272a27);border:1px solid var(--border,#333633)}',
    '.sym-ac-list.sym-open{display:block}',
    '.sym-ac-item{padding:8px 12px;cursor:pointer;font-size:.82rem;line-height:1.4;text-align:left;',
    'color:var(--text-secondary,#b0b0ad);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}',
    '.sym-ac-item[aria-selected="true"],.sym-ac-item:hover{background:var(--border,#333633);',
    'color:var(--text-primary,#e6e3e0)}',
    '.sym-ac-switch{position:absolute;top:50%;right:10px;transform:translateY(-50%);',
    'width:32px;height:16px;padding:0;margin:0;cursor:pointer;background:transparent;',
    'border:1px solid var(--text-dim,#7d7d7b);border-radius:0;font:inherit;',
    'box-sizing:border-box;display:block;cursor:pointer}',
    '.sym-ac-knob{position:absolute;top:50%;left:3px;transform:translateY(-50%);',
    'width:8px;height:8px;background:var(--text-dim,#7d7d7b);',
    'transition:left .15s ease,background .15s ease}',
    '.sym-ac-switch[aria-checked="true"]{border-color:var(--accent,#c99a75)}',
    '.sym-ac-switch[aria-checked="true"] .sym-ac-knob{left:calc(100% - 11px);background:var(--accent,#c99a75)}',
    '.sym-ac-switch:focus-visible{outline:1px solid var(--accent,#c99a75);outline-offset:2px}'
  ].join('');

  function injectStyles() {
    if (document.getElementById('sym-ac-style')) return;
    var s = document.createElement('style');
    s.id = 'sym-ac-style';
    s.appendChild(document.createTextNode(CSS));
    (document.head || document.documentElement).appendChild(s);
  }

  function submitForm(form) {
    if (!form) return;
    if (form.requestSubmit) form.requestSubmit();
    else form.submit();
  }

  function attach(input) {
    if (!input || input.getAttribute('data-sym-ac') === '1') return;
    input.setAttribute('data-sym-ac', '1');

    var wrap = document.createElement('div');
    wrap.className = 'sym-ac-wrap';
    input.parentNode.insertBefore(wrap, input);
    wrap.appendChild(input);

    input.style.paddingRight = '52px';

    var list = document.createElement('ul');
    list.className = 'sym-ac-list';
    list.setAttribute('role', 'listbox');
    wrap.appendChild(list);

    /* a span, not a button: page/theme button rules must not restyle it */
    var sw = document.createElement('span');
    sw.className = 'sym-ac-switch';
    sw.setAttribute('role', 'switch');
    sw.setAttribute('tabindex', '0');
    sw.setAttribute('title', 'Toggle search history');
    sw.setAttribute('aria-label', 'Toggle search history');
    sw.innerHTML = '<span class="sym-ac-knob"></span>';
    wrap.appendChild(sw);

    var active = -1;

    function refreshSwitch() {
      sw.setAttribute('aria-checked', isEnabled() ? 'true' : 'false');
    }

    function close() {
      list.className = 'sym-ac-list';
      active = -1;
    }

    function open(items) {
      list.innerHTML = '';
      if (!items.length) { close(); return; }
      for (var i = 0; i < items.length; i++) {
        var li = document.createElement('li');
        li.className = 'sym-ac-item';
        li.setAttribute('role', 'option');
        li.setAttribute('aria-selected', 'false');
        li.textContent = items[i];
        li.addEventListener('mousedown', (function (value) {
          return function (ev) {
            ev.preventDefault();
            input.value = value;
            close();
            submitForm(input.form);
          };
        })(items[i]));
        list.appendChild(li);
      }
      list.className = 'sym-ac-list sym-open';
      active = -1;
    }

    function refresh() {
      /* only suggest once the user has actually typed something */
      if (!isEnabled() || input.value.trim() === '') { close(); return; }
      open(matching(input.value));
    }

    function move(delta) {
      var items = list.querySelectorAll('.sym-ac-item');
      if (!items.length) return;
      active = (active + delta + items.length) % items.length;
      for (var i = 0; i < items.length; i++) {
        items[i].setAttribute('aria-selected', i === active ? 'true' : 'false');
      }
      var el = items[active];
      if (el.offsetTop < list.scrollTop) list.scrollTop = el.offsetTop;
      else if (el.offsetTop + el.offsetHeight > list.scrollTop + list.clientHeight) {
        list.scrollTop = el.offsetTop + el.offsetHeight - list.clientHeight;
      }
    }

    input.addEventListener('input', refresh);
    input.addEventListener('focus', refresh);
    input.addEventListener('blur', function () { window.setTimeout(close, 120); });

    input.addEventListener('keydown', function (e) {
      var openNow = list.className.indexOf('sym-open') !== -1;
      if (!openNow) return;
      if (e.key === 'ArrowDown') { e.preventDefault(); move(1); }
      else if (e.key === 'ArrowUp') { e.preventDefault(); move(-1); }
      else if (e.key === 'Escape') { close(); }
      else if (e.key === 'Enter') {
        var items = list.querySelectorAll('.sym-ac-item');
        if (active >= 0 && items[active]) {
          e.preventDefault();
          input.value = items[active].textContent;
          close();
          submitForm(input.form);
        }
      }
    });

    function toggle() {
      setEnabled(!isEnabled());
      refreshSwitch();
      close();
      input.focus();
    }

    sw.addEventListener('click', toggle);
    sw.addEventListener('keydown', function (e) {
      if (e.key === 'Enter' || e.key === ' ' || e.key === 'Spacebar') {
        e.preventDefault();
        toggle();
      }
    });

    if (input.form) {
      input.form.addEventListener('submit', function (ev) {
        /* "/history" and friends are local pages, not web searches */
        var typed = (input.value || '').trim();
        if (/^\/[a-z-]+$/i.test(typed)) {
          ev.preventDefault();
          window.location.href = typed;
          return;
        }
        pushHistory(input.value);
      });
    }

    refreshSwitch();
  }

  function init() {
    injectStyles();
    var inputs = document.querySelectorAll('form input[name="q"]');
    for (var i = 0; i < inputs.length; i++) {
      var type = (inputs[i].getAttribute('type') || 'text').toLowerCase();
      if (type === 'text' || type === 'search') attach(inputs[i]);
    }
  }

  /* "/" focuses the search box, Esc leaves it — unless a field already has focus */
  document.addEventListener('keydown', function (e) {
    if (e.metaKey || e.ctrlKey || e.altKey)
      return;

    var el = document.activeElement;
    var tag = el && el.tagName ? el.tagName.toLowerCase() : '';
    var typing = tag === 'input' || tag === 'textarea' || tag === 'select' ||
                 (el && el.isContentEditable);

    if (e.key === 'Escape') {
      if (tag === 'input' && el.getAttribute && el.getAttribute('name') === 'q')
        el.blur();
      return;
    }

    if (e.key !== '/' || typing)
      return;

    var box = document.querySelector('form input[name="q"]');
    if (!box)
      return;

    e.preventDefault();
    box.focus();
    if (box.select) box.select();
  });

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }
})();

/* settings: two independent jobs, both optional - the page works without them,
   it is only slower to use. */
(function () {
    /* 1. keep the hex readout in step with each colour input */
    var colours = document.querySelectorAll('input[type="color"]');
    for (var i = 0; i < colours.length; i++) {
        (function (input) {
            var hex = document.querySelector(
                '.settings-colour-hex[data-for="' + input.id + '"]'
            );
            if (!hex) return;
            input.addEventListener('input', function () {
                hex.textContent = input.value.replace('#', '').toUpperCase();
                input.style.background = input.value;
            });
        })(colours[i]);
    }

    /* 2. K1.3: collapse the provider checkboxes into one field.
       Without this the form submits one off_<id> parameter per provider that is
       off, and beaker keeps at most 16 URL parameters - it stops parsing
       silently beyond that, so the seventeenth toggle would be lost and the
       provider it should have turned off would quietly come back on. The
       checkboxes keep their names so the form still works with scripts off;
       when this runs they are disabled at submit time so only "off" is sent. */
    var form = document.getElementById('engines-form');
    var field = document.getElementById('off-field');
    if (!form || !field) return;

    form.addEventListener('submit', function () {
        var boxes = form.querySelectorAll('input[name^="off_"]');
        var ids = [];
        for (var i = 0; i < boxes.length; i++) {
            if (boxes[i].checked && !boxes[i].disabled) {
                ids.push(boxes[i].name.slice(4));
            }
        }
        field.value = ids.join('|');
        for (var j = 0; j < boxes.length; j++) {
            boxes[j].disabled = true;
        }
    });
})();

/* 3. K3: the "Resumes" column counts down, from the server's own instant.
 *
 * The server renders the absolute time it will let the engine be contacted
 * again, plus its clock. The skew is taken once, so a client whose clock is
 * minutes wrong still counts to the right instant - and the number can never
 * disagree with the gate, because both read the same field.
 */
(function () {
    var table = document.getElementById('engine-status');
    if (!table) return;

    var rows = table.querySelectorAll('tbody tr');
    var nowServer = parseFloat(table.getAttribute('data-now') || '');
    var skew = isNaN(nowServer) ? 0 : nowServer - Date.now() / 1000;

    function serverNow() {
        return Date.now() / 1000 + skew;
    }

    function human(seconds) {
        if (seconds <= 0) return 'retrying…';
        var h = Math.floor(seconds / 3600);
        var m = Math.floor((seconds % 3600) / 60);
        var s = Math.floor(seconds % 60);
        function pad(n) { return (n < 10 ? '0' : '') + n; }
        if (h > 0) return h + 'h ' + pad(m) + 'm';
        if (m > 0) return m + 'm ' + pad(s) + 's';
        return s + 's';
    }

    function tick() {
        for (var i = 0; i < rows.length; i++) {
            var cell = rows[i].querySelector('td.resume');
            if (!cell) continue;
            var at = parseFloat(cell.getAttribute('data-resume-at') || '');
            if (isNaN(at)) continue;
            cell.textContent = human(at - serverNow());
        }
    }

    var timer = setInterval(tick, 1000);
    tick();

    /* When something is suspended, ask the server for the same rows every 12 s
     * and re-render them: a countdown that reaches zero and then keeps showing
     * "retrying…" for a suspended engine is the same lie as a stale duration.
     * Polling stops as soon as nothing is suspended. */
    var poll = null;

    function suspended() {
        return rows && table.querySelectorAll('tr[data-suspended="1"]').length > 0;
    }

    function refresh() {
        var xhr = new XMLHttpRequest();
        xhr.open('GET', '/settings/engine-status.json', true);
        xhr.onload = function () {
            if (xhr.status !== 200) return;
            var data;
            try { data = JSON.parse(xhr.responseText); } catch (e) { return; }
            if (!data || !data.engines) return;
            if (typeof data.now === 'number') skew = data.now - Date.now() / 1000;
            var byName = {};
            for (var i = 0; i < data.engines.length; i++) byName[data.engines[i].name] = data.engines[i];
            var anySuspended = false;
            for (var r = 0; r < rows.length; r++) {
                var name = rows[r].getAttribute('data-engine');
                var e = byName[name];
                if (!e) continue;
                var stateCell = rows[r].querySelector('td.state');
                var whyCell = rows[r].querySelector('td.why');
                var resumeCell = rows[r].querySelector('td.resume');
                if (stateCell) stateCell.textContent = e.state;
                if (whyCell) whyCell.textContent = e.why;
                if (resumeCell) {
                    if (e.resume_at > 0) {
                        resumeCell.setAttribute('data-resume-at', String(e.resume_at));
                        resumeCell.textContent = human(e.resume_at - serverNow());
                        anySuspended = true;
                    } else {
                        resumeCell.removeAttribute('data-resume-at');
                        resumeCell.textContent = e.why === 'none' ? '-' : 'ready to retry';
                    }
                }
                if (e.resume_at > 0) rows[r].setAttribute('data-suspended', '1');
                else rows[r].removeAttribute('data-suspended');
            }
            if (!anySuspended) stopPolling();
        };
        xhr.send();
    }

    function stopPolling() {
        if (poll) { clearInterval(poll); poll = null; }
    }

    if (suspended()) poll = setInterval(refresh, 12000);
})();

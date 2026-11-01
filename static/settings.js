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

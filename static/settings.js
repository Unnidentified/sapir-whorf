/* settings: keep the hex readout in step with each colour input */
(function () {
    var inputs = document.querySelectorAll('input[type="color"]');
    for (var i = 0; i < inputs.length; i++) {
        (function (input) {
            var hex = document.querySelector(
                '.settings-colour-hex[data-for="' + input.id + '"]'
            );
            if (!hex) return;
            input.addEventListener('input', function () {
                hex.textContent = input.value.replace('#', '').toUpperCase();
                input.style.background = input.value;
            });
        })(inputs[i]);
    }
})();

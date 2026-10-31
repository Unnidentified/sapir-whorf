/* J4.0: progressive enhancement, and only that.

   Every widget works with JavaScript off. The keypad is a plain GET form whose
   buttons carry name="key", the translate form is a GET, and every card is
   rendered server-side. This file removes the full page reload for those two
   interactions by fetching /w/<kind> and swapping the card in place.

   It builds no URL the server could not have built itself: the kind comes from
   the element's class, and the query comes from the form. If the fetch fails,
   nothing is intercepted twice and the plain form still submits. */
(function () {
  "use strict";

  var KINDS = [
    ["widget-calculator", "calculator"],
    ["widget-translate", "translate"],
    ["widget-place", "place"],
    ["widget-stock", "stock"],
  ];

  function kindOf(card) {
    for (var i = 0; i < KINDS.length; i++)
      if (card.classList.contains(KINDS[i][0])) return KINDS[i][1];
    return null;
  }

  function expressionOf(card) {
    var field = card.querySelector('input[name="q"]');
    if (field) return field.value || "";
    var shown = card.querySelector(".widget-calc-expr");
    return shown ? (shown.textContent || "").trim() : "";
  }

  /* fallback: if the fragment cannot be had, do what the form would have done.
     Preventing the submit and then failing silently is worse than no script at
     all - measured, when the service was running a build without /w and every
     keypad press did nothing. */
  function fallback(form) {
    form.dataset.widgetFallback = "1";
    form.submit();
  }

  function refresh(card, key, form) {
    var kind = kindOf(card);
    if (!kind) { if (form) fallback(form); return; }
    var url = "/w/" + kind + "?q=" + encodeURIComponent(expressionOf(card));
    if (key) url += "&key=" + encodeURIComponent(key);
    fetch(url, { headers: { "X-Requested-With": "widgets.js" } })
      .then(function (response) { return response.ok ? response.text() : ""; })
      .then(function (html) {
        if (!html) { if (form) fallback(form); return; }
        var box = document.createElement("div");
        box.innerHTML = html;
        var next = box.querySelector(".widget");
        if (next) card.parentNode.replaceChild(next, card);
      })
      .catch(function () { if (form) fallback(form); });
  }

  document.addEventListener("submit", function (event) {
    var form = event.target;
    if (!form || !form.classList) return;
    if (form.dataset && form.dataset.widgetFallback === "1") return;
    var isKeypad = form.classList.contains("widget-calc-form");
    var isWidgetForm = form.classList.contains("widget-form");
    if (!isKeypad && !isWidgetForm) return;
    var card = form.closest(".widget");
    if (!card) return;
    var key = "";
    if (isKeypad) {
      var submitter = event.submitter;
      if (!submitter || submitter.name !== "key") return; /* plain GET for AC too? no: still local */
      key = submitter.value || "";
      if (key === "AC") key = "AC";
    } else {
      var field = form.querySelector('input[name="q"]');
      if (field) {
        /* translate: the typed text replaces the query, so put it in the card
           before refreshing or expressionOf() would send the old one */
        var hidden = card.querySelector('input[name="q"]');
        if (hidden) hidden.value = field.value;
      }
    }
    event.preventDefault();
    refresh(card, key, form);
  });
})();

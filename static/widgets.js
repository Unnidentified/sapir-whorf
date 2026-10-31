/* Progressive enhancement for the answer cards and the knowledge panel.
   Everything here is optional: with JavaScript off the page still answers,
   still renders the panel (open), and every control is a plain link or a GET
   form. Nothing in this file talks to a third party. */
(function () {
  "use strict";

  /* J1: the panel is a <details> that is open in the markup, which is right
     for the wide layout where it sits in the rail. Below the rail breakpoint
     it moves above the results, and there it should start collapsed to its
     two-line summary. Keeping the default open means the no-JS page is still
     complete; this only tightens it where the room is. */
  var RAIL_BREAKPOINT = 1241;
  var panel = document.querySelector(".panel-compact");
  if (panel && window.matchMedia) {
    var mq = window.matchMedia("(max-width: " + (RAIL_BREAKPOINT - 1) + "px)");
    var apply = function () {
      panel.open = !mq.matches;
    };
    apply();
    if (mq.addEventListener) {
      mq.addEventListener("change", apply);
    } else if (mq.addListener) {
      mq.addListener(apply);
    }
  }
})();

/* K7: a real description for a result the engines would not describe.
 *
 * The page is rendered without it - no reader waits for a second request - and
 * this asks the server for each blank row afterwards. The browser never
 * contacts the result site: it talks to this server, which fetches under the
 * provider switch, the robots check and the byte budget.
 *
 * At most five rows per page, and one at a time: a burst would look like
 * scraping to the sites that are already unwilling to be read. */
(function () {
  "use strict";
  var MAX_ROWS = 5;
  var rows = document.querySelectorAll("[data-desc]");
  if (!rows.length) return;

  var budget = Math.min(rows.length, MAX_ROWS);
  var index = 0;

  function next() {
    if (index >= budget) return;
    var row = rows[index++];
    var href = row.getAttribute("data-desc");
    var target = row.querySelector("[data-snippet]");
    if (!href) return next();

    fetch(href, { credentials: "same-origin" })
      .then(function (response) {
        if (!response.ok) return null;
        return response.text();
      })
      .then(function (text) {
        /* the server may have filled a breadcrumb already; a real description
           replaces it, which is the whole point of asking */
        if (text && text.trim() && target) {
          target.textContent = text.trim();
          target.classList.add("desc-filled");
        }
      })
      .catch(function () { /* a missing description is not an error */ })
      .then(function () { next(); });
  }

  next();
})();

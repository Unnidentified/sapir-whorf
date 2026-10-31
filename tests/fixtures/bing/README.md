# Bing fixtures

| File | What it is |
|---|---|
| `good.html` | A real Bing SERP for `linux kernel`, ten organic `b_algo` results. |
| `junk-sap.html` | **The H2 finding.** A real 200 response for `best mechanical keyboard` whose ten `b_algo` results are SAP warehouse-error pages. Real markup, real titles, nothing to do with the query. |

Both are 200 with ten `b_algo` entries. Nothing in the status, the headers or
the markup distinguishes them — which is why the relevance floor exists and why
JUNK is a separate outcome from OK and EMPTY.

`good.html` was previously called `empty-shell.html`, which it never was: it
holds ten results. The name came from a probe where `grep -c` counted lines
rather than matches (H2/G1). The README and the test now agree with the file.

Note for anyone re-deriving the "Bing has no results" marker: every Bing page
contains `#b_notificationContainer`, so testing for the bare string `b_no`
matches every page. The real marker is `class="b_no"`.

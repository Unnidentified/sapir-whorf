# J4.0 + J4a — the widget layer and the calculator card

## The layer

`Answerer` carries one more field: the widget template for that kind. So a
card is a **template plus a name in the table**, not a second registry and not
HTML built in C — which is what made the old cards impossible to theme (J2).

```c
{"arithmetic", 1, is_calculator_query, answer_arithmetic, "calculator"},
{"units",      1, is_unit_conv_query,  answer_units,      NULL},
```

`answer_box_widget()` returns **data**, `WidgetData`, never markup: kind, title,
expression, result, note, ok. The template decides the look.

**Beaker cannot include a subdirectory** — `{{include "widgets/calculator.html"}}`
renders `<!-- Include blocked: path traversal not allowed -->`. So the naming
convention is `templates/widget_<kind>.html` rather than the directory the review
suggested. One file per kind either way.

## Placement

The card is the first element of the main column, above the results, and the
text strip above it is suppressed when a card exists (the card carries the
value; Google does the same). Measured: `2+2` → card with `4` and 10 results
below it, widget offset before the first result's.

## The no-JS keypad

The keypad is a plain `GET` form. Each key submits:

```
/search?q=<expression so far>&w=calculator&key=<one character>
```

and the server appends the character before the query is analysed or cached.
`w=calculator` is what makes it work: without it the card would vanish the
moment the expression stopped looking like arithmetic, which is every moment
while you type (`5`, `53`).

Only characters the keypad offers are accepted (`0-9 . + - * / ( ) ^ % !`, `÷ ×
− π`, `e`, `AC`, `sin(` … `sqrt(`, `=`). Anything else is dropped — the value
becomes the query, so `?key=<script>` must not reach it. Measured: `q=5&key=7`
leaves the expression at `5`.

`AC` resets to `0`, not to empty: an empty `q` redirects to the home page and
takes the card with it.

## Mid-entry is not an error

An expression ending on an operator, an open bracket, or the `=` just pressed has
no value yet, so the card shows none. Only a complete expression that fails to
parse says `Error`. Measured:

| request | expression | result |
|---|---|---|
| `q=2+2` | `2+2` | `4` |
| `q=2^10` | `2^10` | `1024` |
| `q=1/0` | `1/0` | `Infinity` |
| `q=5+*3&w=calculator` | `5+*+3` | `Error` |
| `q=(2+3&w=calculator` | `(2+3` | `Error` |
| `q=5&w=calculator&key=+` | `5+` | *(none)* |
| `q=5+&w=calculator&key=3` | `5+3` | `8` |
| `q=5+3&w=calculator&key==` | `5+3` | `8` |

## A bug this uncovered

The local answer and the card were set **inside `if (total_results > 0)`**, so
when every engine was rate-limited the page showed "some sources did not answer"
and threw away arithmetic it had already computed. It surfaced on a private
instance whose only engine was in a challenge pause: `2+2` rendered no card at
all. Both now resolve before the fan-out, on page 1, and `results_state` is
`answered` rather than `empty` when a local answer exists.

## Context slots

Adding three keys overflowed the 32-slot template context and beaker **drops a
key silently** — `TemplateContext is full. Cannot add key 'widget_kind'` — which
is why the card rendered its container and nothing else for a while.

Fixed by not spending the slots:

- the custom palette is one `custom_palette` CSS string on every page that only
  injects it (`context_set_custom_palette`), and the nine per-colour keys are
  set only on `/settings`, where the colour pickers need them;
- `answer_is_web` and `debug_funnel` were set and never rendered: removed;
- the card uses two keys, not four (the expression is `query`; the result slot
  doubles as the error slot).

`make render-check` fails on `TemplateContext is full` and on a key that no
template has, so this cannot come back quietly.

## Tests

`make widget-check` → `tests/test_j4_widget.sh`:

```
J4 calculator card: PASS (render, states, keypad, AC, rejected key, answers=off)
```

It asserts the card exists with ≥ 25 keys, the form carries `w=calculator`, the
strip is not doubled, the widget precedes the first result (not that results
exist — that depends on an engine's mood), the three parser states, three
keypad appends, `AC`, that a rejected key changes nothing, and that
`answers = off` renders no card.

The 13-expression parser table is in `tests/test_g4_answerbox.c`
(`make answer-check`).

## Not done

- **No other kind has a card.** Units, colour, currency and dictionary still
  render a text line. Each is a template plus one name in the table.
- **`/w/<kind>` and `static/widgets.js` are not built.** The JS is optional
  enhancement: the card already works without it. The endpoint and the
  `fetch()` swap are next, and they need the inbound limiter wiring the review
  asks for.
- **No trigonometry.** `sin`, `cos`, `tan` were on the keypad and are now gone
  rather than shipped broken: without a Deg/Rad switch they are ambiguous
  (Google's calculator is DEG, a mathematician expects radians), and a button
  that answers `Error` is worse than one that is not there. `ln`, `log`,
  `sqrt`, `abs`, `π` and `e` are implemented and unit-free. `%` and `x!` went
  the same way: they were on the keypad and in the parser's matcher but never in
  the evaluator. A Deg/Rad toggle plus `sin/cos/tan`, `%` as "percent of the
  preceding value" (Google reads `100 + 10%` as 110) and `!` remain J4a work.
- **No implicit multiplication.** `2(3+4)` and `2π` are not accepted; `2*π` is.

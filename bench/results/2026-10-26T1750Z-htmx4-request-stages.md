# Where a request through htmx 4 spends its time

## What was measured

One page in WebKitGTK 2.52.6 under Xvfb, driven by a small C++
program on webview/webview (no mruby). The page holds htmx 4.0.0, the
ascaridol extension, a button with `hx-get` and a target. A script
clicks the button and waits for the change of the target through a
MutationObserver. 50 requests warm up, then 300 are timed. Times are
microseconds per request, mean over the 300, offset from the click.

Two bridges: `real` is the bound C++ function. `fake` is a JS function
that returns a resolved Promise with the same HTML, so the time is
htmx and the browser alone.

The machine is a new VM against the file of 16:10: Intel Xeon @
2.10GHz, 4 cpus, kernel 6.18.44-fc-v153. Absolute figures are not
comparable with that file.

## Results

Stages, minified htmx, three rounds:

| stage (us after click) | fake | real |
|---|---|---|
| htmx:config:request | 110..193 | 143..183 |
| htmx:before:request | 177..310 | 260..283 |
| bridge returns | 183..330 | 1457..1710 |
| htmx:before:swap | 317..547 | 1670..1970 |
| target changed | 430..687 | 1840..2127 |

Inside htmx, fake bridge, unminified htmx with marks, three rounds:

| part | us |
|---|---|
| `#createRequestContext` (attribute lookups with inheritance) | 104..150 |
| sync strategy, request queue, abort listener | 64..83 |
| `#makeFragment` (DOMParser on the response) | 124..190 |
| `#insertContent` up to the change of the target | 140..173 |

Flows in one page, five rounds (us per request):

| flow | median of rounds |
|---|---|
| click handler, bridge, innerHTML (the old router) | 853 |
| the same with a fake bridge | 13 |
| htmx 4 with the bridge | 2103 |
| htmx 4 with a fake bridge | 530 |

Rendering: the same flows with `body{display:none}`, three rounds:
plain 307..493 against 747..983 shown; htmx with the bridge
1323..1463 against 2103..2397 shown.

Overlap: a click handler that spins 450 us before or after it calls the
bridge, hidden page, two rounds: 360/593 without spin, 1083/1203 with
the spin before, 1020/1133 with the spin after. The time of the spin
adds to the time of the bridge in both positions.

## Reading

- htmx 4 itself takes 430..690 us per request here. The two largest
  parts are the attribute lookups and the parse of the response into a
  new document.
- The bridge call does not run beside JavaScript. A spin after the call
  delays its answer by the time of the spin. So the time of htmx adds
  to the time of the bridge.
- Rendering takes more time with htmx than with the old router.
- The extension owns none of these parts.

## Context

webview/webview at the submodule commit of ascaridol 0d93074, g++ from
the distribution, WebKitGTK 2.52.6 (libwebkit2gtk-4.1), Xvfb with
software rendering (no DRI3). Run as root, not pinned, no renice.

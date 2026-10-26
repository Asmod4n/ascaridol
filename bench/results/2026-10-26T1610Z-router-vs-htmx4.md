# Router against htmx 4: time per request through the bridge

## What was measured

One button with a GET to a bound Ruby route. The route returns
`<span>N</span>`, and the page puts it into `#t`. A driver script clicks
the button and waits for the change of `#t` through a MutationObserver.
100 requests warm up, then 1000 sequential requests are timed with
`performance.now()`. The result is microseconds per request.

- old arm: ascaridol f70d987, rb-get router (`Ascaridol.html_router`).
- new arm: ascaridol 0d93074, htmx 4.0.0 with the ascaridol extension
  (`Ascaridol.htmx`).

Each arm is its own binary. The bench code is not in the tree. It is
written out below. Binaries `old2` and `new2` are byte copies of `old`
and `new` and give the A/A noise floor.

## Results (microseconds per request)

| run | old | old2 | new | new2 |
|---|---|---|---|---|
| 1 | 968 | 1032 | 2993 | 2887 |
| 2 | 991 | 1112 | 2885 | 3000 |
| 3 | 953 | 926 | 2676 | 2564 |
| 4 | 1088 | 967 | 2159 | 2326 |
| 5 | 872 | 895 | 2660 | 2515 |
| median | 968 | 967 | 2676 | 2564 |
| min..max | 872..1088 | 895..1112 | 2159..2993 | 2326..3000 |

A/A difference of the medians: 0.1 % (old against old2), 4.4 %
(new against new2). new against old: 2.76 times (2676 against 968).

An earlier run of the same binaries, old and new alternated, 5 each:
old 835 968 887 882 917 (median 887), new 2546 2475 2522 2260 2592
(median 2522).

## Context

- commit of the new arm: 0d93074, clean; commit of the old arm: f70d987.
- mruby: git HEAD d660d41, `conf.gembox 'default'`, no `enable_debug`,
  defines `MRB_UTF8_STRING MRB_HIGH_PROFILE MRB_USE_DEBUG_HOOK`,
  mruby default flags `-g -O3`, no `-march`.
- compiler: gcc 13.3.0 (Ubuntu 13.3.0-6ubuntu2~24.04.1); C++ library
  libstdc++ GLIBCXX_3.4.32; C library glibc 2.39-0ubuntu8.9.
- webview: WebKitGTK 2.52.6 (libwebkit2gtk-4.1), display Xvfb (xvfb-run).
- kernel: 6.18.44-fc-v153, Firecracker VM, block scheduler mq-deadline.
- processor: Intel Xeon @ 2.80GHz, 4 cpus, digest of flags 50b4012617c99e7c.
- user: root, `nice -n -5`, not pinned. Load average before the run 0.58.
- threads: the WebKit processes as WebKitGTK starts them.

Deviations from the bench rules: the run is root, the other processes
of the user were not reniced, and no cpu was held back.

## Diagnostic (one page, no htmx, same machine)

A page that calls the bound function directly: 386..508 us per call.
The same call plus `innerHTML`: 982..1111 us. `Response.text()` on the
result: 3..5 us. The same page with htmx 4 and a click: 3199..3745 us.
So the bridge costs the same in both arms, and the difference lies in
the request pipeline of htmx 4. Its cause is not found yet.

## Bench code

`bench_new.rb` (the old arm differs in `Ascaridol.html_router(:route)`
and `rb-get="/n" rb-target="#t"`):

    $n = 0
    def main
      Ascaridol.run(title: "bench", size: [400, 300]) do |w|
        w.bind(:route) { |m, p, params| $n += 1; "<span>#{$n}</span>" }
        w.html = "<!doctype html><html><head><meta charset=\"utf-8\">#{Ascaridol.htmx(:route)}</head><body><button id=\"b\" hx-get=\"/n\" hx-target=\"#t\">go</button><div id=\"t\"></div></body></html>"
      end
    end

The driver binds `__report`, installs a script through `init`, clicks
1100 times as described above and reports `us_per_request`.

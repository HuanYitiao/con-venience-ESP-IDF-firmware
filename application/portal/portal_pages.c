// Pages in flash, from the old firmware (wifi_config.cpp, set_time.cpp),
// plus a note for the WebView, which cannot pick files.

#include "portal_pages.h"

const char portal_index_page[] =
    "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\""
    "width=device-width, initial-scale=1\"><title>con-venience &middot; settings</title><style>:"
    "root{color-scheme:dark}*{box-sizing:border-box}body{margin:0;min-height:100vh;display:flex"
    ";align-items:center;justify-content:center;font-family:system-ui,-apple-system,\"Segoe UI\","
    "Roboto,sans-serif;background:#0f1115;color:#e8eaed}main{width:100%;max-width:22rem;padding"
    ":2rem 1.5rem}.brand{font-size:.8rem;letter-spacing:.18em;text-transform:uppercase;color:#6"
    "b7280;text-align:center;margin-bottom:.5rem}h1{font-size:1.6rem;text-align:center;margin:0"
    " 0 1.75rem}a.item{display:block;width:100%;padding:1.1rem 1.25rem;margin-top:.9rem;font-si"
    "ze:1.15rem;font-weight:600;text-decoration:none;color:#e8eaed;background:#1a1d24;border:1p"
    "x solid #2b2f38;border-radius:.75rem}a.item:active{background:#e8eaed;color:#0f1115}.note{"
    "font-size:.95rem;line-height:1.4;color:#fbbf24;background:#1a1d24;border:1px solid #4b3b12"
    ";border-radius:.75rem;padding:1rem 1.1rem;margin-bottom:.5rem}.note b{color:#e8eaed}</styl"
    "e></head><body><main><div class=\"brand\">con-venience</div><h1>Settings</h1>";

const char portal_index_webview_note[] =
    "<div class=\"note\">This window cannot pick photos. To set your profile, open <b>http://192."
    "168.4.1</b> in your browser. If the page does not load there, choose <b>Stay connected</b>"
    " when your phone asks about this Wi-Fi, or turn mobile data off.</div>";

const char portal_index_end[] =
    "<a class=\"item\" href=\"/profile\">Set Profile</a><a class=\"item\" href=\"/settime\">Set Time</a"
    "></main></body></html>";

const char portal_settime_page[] =
    "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\""
    "width=device-width, initial-scale=1\"><title>con-venience &middot; set time</title><style>:"
    "root{color-scheme:dark}*{box-sizing:border-box}body{margin:0;min-height:100vh;display:flex"
    ";align-items:center;justify-content:center;font-family:system-ui,-apple-system,\"Segoe UI\","
    "Roboto,sans-serif;background:#0f1115;color:#e8eaed}main{width:100%;max-width:22rem;padding"
    ":2rem 1.5rem;text-align:center}.brand{font-size:.8rem;letter-spacing:.18em;text-transform:"
    "uppercase;color:#6b7280;margin-bottom:1.5rem}.status{font-size:1.5rem;font-weight:600;marg"
    "in:0 0 .5rem}.clock{font-size:3rem;font-weight:700;font-variant-numeric:tabular-nums;lette"
    "r-spacing:.02em;margin:.5rem 0}.detail{font-size:.95rem;color:#9aa0a6;min-height:1.4em;mar"
    "gin:0}.ok .status{color:#34d399}.fail .status{color:#f87171}button{margin-top:2rem;width:1"
    "00%;padding:1rem;font-size:1.1rem;font-weight:600;color:#0f1115;background:#e8eaed;border:"
    "0;border-radius:.75rem}button:active{background:#b9bdc2}a.back{display:inline-block;margin"
    "-top:1.5rem;color:#6b7280;text-decoration:none;font-size:.9rem}[hidden]{display:none!impor"
    "tant}</style></head><body><main id=\"root\" class=\"pending\"><div class=\"brand\">con-venience<"
    "/div><h1 class=\"status\" id=\"status\">Syncing time&hellip;</h1><div class=\"clock\" id=\"clock\""
    ">--:--:--</div><p class=\"detail\" id=\"detail\"></p><button id=\"retry\" hidden>Sync again</but"
    "ton><a class=\"back\" href=\"/\">&lsaquo; Settings</a></main><script>var root=document.getElem"
    "entById(\"root\"),status=document.getElementById(\"status\"),clock=document.getElementById(\"cl"
    "ock\"),detail=document.getElementById(\"detail\"),retry=document.getElementById(\"retry\");func"
    "tion two(n){return(n<10?\"0\":\"\")+n}async function syncTime(){root.className=\"pending\";statu"
    "s.textContent=\"Syncing time\\u2026\";detail.textContent=\"\";retry.hidden=true;var now=new Dat"
    "e();var payload={year:now.getFullYear(),month:now.getMonth()+1,day:now.getDate(),hour:now."
    "getHours(),minute:now.getMinutes(),second:now.getSeconds()};clock.textContent=two(payload."
    "hour)+\":\"+two(payload.minute)+\":\"+two(payload.second);try{var res=await fetch(\"/settime\",{"
    "method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify(payload)});i"
    "f(!res.ok){throw new Error(\"HTTP \"+res.status)}root.className=\"ok\";status.textContent=\"Tim"
    "e set\";detail.textContent=payload.year+\"-\"+two(payload.month)+\"-\"+two(payload.day)}catch(e"
    "){root.className=\"fail\";status.textContent=\"Sync failed\";detail.textContent=String(e.messa"
    "ge||e);retry.hidden=false}}retry.addEventListener(\"click\",syncTime);window.addEventListene"
    "r(\"load\",syncTime);</script></body></html>";

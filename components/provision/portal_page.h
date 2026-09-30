/* Generated from the portal mockup; see tools/portal-preview.html. */
#pragma once

/* %s device id, %s link state */
static const char PORTAL_HEAD[] =
"<!doctype html><meta charset=utf-8><meta name=viewport content=\"width=device-width,initial-scale=1\">\n"
"<title>downlink setup</title>\n"
"<style>\n"
":root{--bg:#0f1115;--card:#171a21;--line:#262b36;--tx:#e8eaf0;--mut:#8b93a7;--acc:#3ddc97;--acc2:#2bb37c;--warn:#f0b35a}\n"
"*{box-sizing:border-box}html{background:var(--bg)}\n"
"body{margin:0;font:16px/1.45 -apple-system,system-ui,Segoe UI,Roboto,sans-serif;color:var(--tx);background:var(--bg);min-height:100vh}\n"
".wrap{max-width:26em;margin:0 auto;padding:1.25em 1em 3em}\n"
"header{display:flex;align-items:center;gap:.8em;margin:.5em 0 1.4em}\n"
".logo{width:40px;height:40px;border-radius:10px;background:var(--acc);display:grid;place-items:center;flex:none}\n"
".logo svg{width:22px;height:22px}\n"
"h1{font-size:1.15em;margin:0;font-weight:600}\n"
"header p{margin:.1em 0 0;color:var(--mut);font-size:.9em}\n"
".card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:1em 1em .4em;margin-bottom:1em}\n"
".card h2{font-size:.8em;letter-spacing:.06em;text-transform:uppercase;color:var(--mut);margin:0 0 .6em;font-weight:600}\n"
".net{display:flex;align-items:center;gap:.8em;padding:.7em .2em;border-top:1px solid var(--line);cursor:pointer}\n"
".net:first-of-type{border-top:0}\n"
".net input{accent-color:var(--acc);width:1.1em;height:1.1em;margin:0;flex:none}\n"
".net .n{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}\n"
".net .k{font-size:.75em;color:var(--acc);margin-left:.5em}\n"
".bars{display:flex;gap:2px;align-items:flex-end;height:14px;flex:none}\n"
".bars i{width:4px;background:var(--line);border-radius:1px}\n"
".bars i:nth-child(1){height:5px}.bars i:nth-child(2){height:8px}.bars i:nth-child(3){height:11px}.bars i:nth-child(4){height:14px}\n"
".s4 i{background:var(--acc)}.s3 i:nth-child(-n+3){background:var(--acc)}.s2 i:nth-child(-n+2){background:var(--warn)}.s1 i:nth-child(1){background:var(--warn)}\n"
"label.f{display:block;margin:.3em 0 .9em}\n"
"label.f span{display:block;font-size:.85em;color:var(--mut);margin-bottom:.35em}\n"
"input[type=text],input[type=password],input[type=number]{width:100%%;padding:.7em .8em;font-size:1em;border-radius:10px;border:1px solid var(--line);background:#0c0e12;color:var(--tx);outline:none}\n"
"input:focus{border-color:var(--acc)}\n"
".row{display:flex;gap:.8em}.row label{flex:1}\n"
"details{margin:.2em 0 .8em}summary{color:var(--mut);font-size:.9em;cursor:pointer;padding:.3em 0}\n"
"button{width:100%%;padding:.9em;font-size:1.05em;font-weight:600;border:0;border-radius:12px;background:var(--acc);color:#07110c;cursor:pointer}\n"
"button:active{background:var(--acc2)}\n"
".foot{color:var(--mut);font-size:.8em;text-align:center;margin-top:1.2em}\n"
".chk{display:flex;gap:.5em;align-items:center;font-size:.9em;color:var(--mut);margin:-.4em 0 .9em}\n"
".chk input{accent-color:var(--acc)}\n"
"</style>\n"
"<div class=wrap>\n"
"<header>\n"
" <div class=logo><svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#07110c\" stroke-width=\"2.2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><path d=\"M11 5 6 9H3v6h3l5 4z\"/><path d=\"M15.5 8.5a5 5 0 0 1 0 7\"/><path d=\"M18.5 5.5a9 9 0 0 1 0 13\"/></svg></div>\n"
" <div><h1>Stream player setup</h1><p>%s &middot; %s</p></div>\n"
"</header>\n"
"<form method=post action=/save>\n"
"<div class=card><h2>Choose a network</h2>\n"
"\n";

/* %s value, %s " checked" or "", %s name, %s " <span class=k>&#10003; saved</span>" or "", %d bars 1-4 */
static const char PORTAL_ROW[] =
"<label class=net><input type=radio name=ssid value=\"%s\"%s><span class=n>%s%s</span><span class=\"bars s%d\"><i></i><i></i><i></i><i></i></span></label>\n";

/* %s stream url, %s device id */
static const char PORTAL_TAIL[] =
" <label class=net><input type=radio name=ssid value=\"\"><span class=n>Other network&hellip;</span></label>\n"
"</div>\n"
"<div class=card>\n"
" <label class=f id=o hidden><span>Network name</span><input type=text name=ssid2 autocomplete=off autocapitalize=off></label>\n"
" <label class=f><span>Password</span><input type=password name=pass id=p autocomplete=off></label>\n"
" <label class=chk><input type=checkbox onchange=\"p.type=this.checked?'text':'password'\">Show password</label>\n"
" <label class=f><span>Stream URL (Ogg Opus)</span><input type=text name=url value=\"%s\" autocomplete=off autocapitalize=off spellcheck=false></label>\n"
" <details><summary>Advanced</summary>\n"
"  <div class=row>\n"
"   <label class=f><span>Priority (higher wins)</span><input type=number name=prio value=0 min=0 max=255></label>\n"
"   <label class=f><span>Device id</span><input type=text name=id value=\"%s\" pattern=\"[A-Za-z0-9_-]{1,24}\" autocapitalize=off></label>\n"
"  </div>\n"
" </details>\n"
"</div>\n"
"<button type=submit>Save and reboot</button>\n"
"</form>\n"
"<script>var f=document.forms[0];function u(){o.hidden=f.ssid.value!==''}f.addEventListener('change',u);u()</script>\n"
"<p class=foot>The player restarts after saving and this network disappears.<br>Portal closes on its own when idle.</p>\n"
"</div>\n"
"\n";

/* %s title, %s message */
static const char PORTAL_DONE[] =
"<!doctype html><meta charset=utf-8><meta name=viewport content=\"width=device-width,initial-scale=1\"><title>downlink</title>"
"<style>html{background:#0f1115}body{margin:0;font:16px/1.45 system-ui,sans-serif;color:#e8eaf0;background:#0f1115}.wrap{max-width:26em;margin:0 auto;padding:2em 1em}"
".card{background:#171a21;border:1px solid #262b36;border-radius:14px;padding:1.2em}h1{font-size:1.2em;margin:0 0 .5em}p{margin:.4em 0;color:#8b93a7}b{color:#3ddc97}a{color:#3ddc97}</style>"
"<div class=wrap><div class=card><h1>%s</h1><p>%s</p></div></div>\n";

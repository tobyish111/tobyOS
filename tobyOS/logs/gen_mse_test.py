import sys, base64

b64 = base64.b64encode(open(sys.argv[1], 'rb').read()).decode()

# ONE line, ONLY single quotes, no backslashes -> embeds directly in CDP JSON.
js = ("(function(){var S=function(s){window.__mse=s;};S('start');"
      "var bin=atob('%s');var u=new Uint8Array(bin.length);"
      "for(var i=0;i<bin.length;i++)u[i]=bin.charCodeAt(i);"
      "var v=document.createElement('video');v.autoplay=true;v.muted=true;v.loop=true;"
      "v.style.width='640px';v.style.height='480px';v.style.background='#402020';"
      "document.body.appendChild(v);window.__v=v;"
      "v.addEventListener('error',function(){S('velem-err '+(v.error?v.error.code:'?'));});"
      "if(!window.MediaSource){S('NO-MediaSource');return 'no-mse';}"
      "S('has-MediaSource sup='+MediaSource.isTypeSupported('video/webm;codecs=vp9'));"
      "var ms=new MediaSource();v.src=URL.createObjectURL(ms);S('src-set');"
      "ms.addEventListener('sourceopen',function(){S('sourceopen');"
      "try{var sb=ms.addSourceBuffer('video/webm;codecs=vp9');S('sb-added');"
      "sb.addEventListener('updateend',function(){"
      "S('updateend buf='+(v.buffered.length?v.buffered.end(0).toFixed(2):'none'));"
      "try{ms.endOfStream();}catch(e){}var p=v.play();"
      "if(p&&p.catch)p.catch(function(e){S('play-rej '+e);});});"
      "sb.addEventListener('error',function(){S('sb-error');});"
      "sb.appendBuffer(u);S('appending len='+u.length);}"
      "catch(e){S('exc '+e);}});"
      "return 'injected '+u.length;})()") % b64

bad = [c for c in ('"', chr(92), chr(10), chr(13)) if c in js]
assert not bad, "unsafe chars for JSON embedding: %r" % bad

open(sys.argv[2], 'w', newline='').write(js)
print("wrote", sys.argv[2], len(js), "bytes; media b64", len(b64))

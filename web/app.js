const $=id=>document.getElementById(id);
const sleep=ms=>new Promise(r=>setTimeout(r,ms));
const kb=b=>b>=1048576?(b/1048576).toFixed(2)+' МБ':Math.round(b/1024)+' КБ';
const dur=s=>{s=Math.max(0,Math.round(s));return s>=60?Math.floor(s/60)+' мин '+(s%60)+' с':s+' с'};
const ago=s=>s<60?s+' с':s<3600?Math.floor(s/60)+' мин':Math.floor(s/3600)+' ч';
const upfmt=s=>s>=86400?Math.floor(s/86400)+' д '+Math.floor(s%86400/3600)+' ч':s>=3600?Math.floor(s/3600)+' ч '+Math.floor(s%3600/60)+' мин':Math.floor(s/60)+' мин';
const ERRS={'timeout p1':'сенсор не ответил на старт','timeout p2':'сенсор перестал отвечать на чанки','timeout p3':'сенсор не подтвердил прошивку','no progress':'сенсор не принимает чанки','sensor fail':'сенсор сообщил об ошибке записи','read err':'не читается файл на боте','encrypt err':'ошибка шифрования чанка','no file':'файл на боте не открыт','новый файл':'сессия прервана загрузкой нового файла','manual':'прервано вручную','no RAM':'на сенсоре не хватило памяти','begin fail':'сенсор не смог открыть раздел прошивки','no first chunk':'сенсор не дождался первого чанка','stall timeout':'сенсор перестал получать данные','write fail':'сенсор не смог записать образ','size mismatch':'на сенсор пришло не столько байт','crc mismatch':'контрольная сумма образа не совпала','end fail':'сенсор забраковал образ при записи','board mismatch':'образ собран для другой платы','from bot':'сессию прервал бот'};
const errText=e=>ERRS[e]||e;
let file=null,poll=null,busy=false,vErr=false,sensors=[],info={},target='__self__';
const isSelf=()=>target=='__self__';
function st(t,c){$('st').textContent=t;$('st').className=c||'';vErr=false}
function bar(p,l,r){$('prog').hidden=false;$('pctv').textContent=Math.round(p);$('fill').style.width=p+'%';$('pl').textContent=l;$('pr').textContent=r||''}
function batHtml(p){return '<span class="bat'+(p<20?' low':'')+'"><i style="width:'+Math.max(0,Math.min(100,p))+'%"></i></span>'}
function refresh(){
  const need=isSelf()?'bin':'otaz';
  $('hint').textContent=file?'':'Перетащи .'+need+' сюда или нажми';
  $('file').accept='.'+need;
  const bad=!!file&&file.name.split('.').pop().toLowerCase()!=need;
  if(!busy){
    if(bad){st(isSelf()?'Для бота нужен .bin':'Для сенсора нужен .otaz','err');vErr=true}
    else if(vErr)st('','');
  }
  $('go').textContent=isSelf()?'Прошить бота':'Прошить '+target;
  $('go').disabled=busy||!file||bad;
  $('fwgo')&&($('fwgo').disabled=busy||isSelf());
  $('scfgbox').hidden=isSelf();
}
function setFile(f){
  if(!f)return;
  file=f;
  $('fname').textContent=f.name+' ('+kb(f.size)+')';
  const m=f.name.match(/_v(\d+\.\d+\.\d+)\./);
  $('fver').textContent=m?'версия '+m[1]:'';
  refresh();
}
function addTarget(id,name,meta,online,bat){
  const d=document.createElement('div');
  d.className='tgt'+(target==id?' sel':'');
  d.innerHTML='<span class="dot'+(online?' on':'')+'"></span>'
    +'<span class="grow"><span class="nm">'+name+'</span><div class="meta">'+meta+'</div></span>'
    +(bat>=0?batHtml(bat):'');
  d.onclick=()=>{target=id;renderTargets();refresh()};
  $('targets').appendChild(d);
}
function renderTargets(){
  $('targets').innerHTML='';
  addTarget('__self__',$('dev').textContent+' — этот бот',
            '<b>'+(info.env||'?')+'</b> · v'+(info.ver||'?')+' · файл .bin',true,
            info.bat>=0?info.bat:-1);
  for(const s of sensors){
    const meta='<b>'+(s.env||'?')+'</b> · '+(s.ver?'v'+s.ver:'версия ?')
      +' · '+(s.online?'онлайн':'был '+ago(s.seen_s)+' назад')
      +(s.rssi?' · '+s.rssi+' dBm':'');
    addTarget(s.name,s.name,meta,s.online,s.bat);
  }
  if(!sensors.length){
    const d=document.createElement('div');
    d.style.cssText='font-size:11px;color:#93a4c4;margin-bottom:6px';
    d.textContent='сенсоры ещё не выходили на связь';
    $('targets').appendChild(d);
  }
}
async function loadSensors(){
  try{sensors=await (await fetch('/sensors')).json()}catch(e){return}
  if(!isSelf()&&!sensors.some(s=>s.name==target))target='__self__';
  renderTargets();refresh();
}
async function loadInfo(){
  try{
    info=await (await fetch('/info')).json();
    $('info').innerHTML='<span class="dot'+(info.wifi?' on':'')+'"></span>WiFi'
      +' <span class="dot'+(info.mqtt?' on':'')+'"></span>MQTT · '+info.ip
      +' · '+upfmt(info.up)+' · '+info.temp.toFixed(0)+'°C · heap '+Math.round(info.heap/1024)+' КБ'
      +(info.bat>=0?' · '+batHtml(info.bat)+' '+info.bat+'% ('+info.volt.toFixed(2)+' V)':'');
    const fw=$('fw');
    if(info.fwready){
      fw.hidden=false;
      fw.innerHTML='На боте: <b>'+(info.fwname||'файл .otaz')+'</b><br>'+kb(info.fwsize)
        +' сжато, образ '+kb(info.fwimg)
        +' <button id="fwgo" class="sec sm" style="margin-top:6px">Прошить сохранённым</button>';
      $('fwgo').onclick=flashStored;
    }else fw.hidden=true;
    renderTargets();refresh();
  }catch(e){$('info').textContent='нет связи с ботом'}
}
function finish(){busy=false;$('ab').hidden=true;clearInterval(poll);poll=null;refresh()}
$('drop').onclick=()=>$('file').click();
$('drop').ondragover=e=>{e.preventDefault();$('drop').classList.add('hover')};
$('drop').ondragleave=()=>$('drop').classList.remove('hover');
$('drop').ondrop=e=>{e.preventDefault();$('drop').classList.remove('hover');setFile(e.dataTransfer.files[0])};
$('file').onchange=()=>setFile($('file').files[0]);
$('ab').onclick=()=>fetch('/ota/abort',{method:'POST'}).catch(()=>{});
$('ask').onclick=async()=>{
  $('ask').disabled=true;
  try{
    const r=await fetch('/sensors/hello',{method:'POST'});
    if(!r.ok)throw new Error(await r.text());
    const t0=Date.now();
    if(!busy)st('Запрос отправлен, ждём ответы…','ok');
    for(let i=0;i<3;i++){await sleep(3000);await loadSensors()}
    const n=sensors.filter(s=>s.seen_s<=(Date.now()-t0)/1000).length;
    if(!busy)st('Ответили: '+n+' из '+sensors.length,n?'ok':'err');
  }catch(e){if(!busy)st(e.message,'err')}
  $('ask').disabled=false;
};
let logPos=0,logTimer=null,logAll='',logLines=[],filterQ='';
const atBottom=()=>{const l=$('logs');return l.scrollTop+l.clientHeight>=l.scrollHeight-12};
function scrollBottom(){const l=$('logs');l.scrollTop=l.scrollHeight;$('down').hidden=true}
function lineClass(s){
  if(/(abort|fail|error|ошиб|timeout|mismatch|прерван)/i.test(s))return 'l-e';
  if(s.startsWith('[OTA'))return 'l-o';
  if(s.startsWith('[SNS'))return 'l-s';
  if(s.startsWith('[WEB')||s.startsWith('[MQTT'))return 'l-w';
  return '';
}
function appendLines(arr){
  const l=$('logs'),frag=document.createDocumentFragment();
  for(const s of arr){
    if(filterQ&&!s.toLowerCase().includes(filterQ))continue;
    const d=document.createElement('div');
    d.className=lineClass(s);
    d.textContent=s;
    frag.appendChild(d);
  }
  l.appendChild(frag);
  while(l.childElementCount>2000)l.removeChild(l.firstChild);
}
function renderLog(){$('logs').textContent='';appendLines(logLines);scrollBottom()}
function addChunk(t){
  const stick=atBottom();
  logAll+=t;
  if(logAll.length>120000)logAll=logAll.slice(-80000);
  const lines=t.split('\n').filter(s=>s.length>0);
  logLines.push(...lines);
  if(logLines.length>3000)logLines=logLines.slice(-2000);
  appendLines(lines);
  if(stick)scrollBottom();else $('down').hidden=false;
}
async function pullLog(){
  try{
    const r=await fetch('/logs/tail?from='+logPos);
    const t=await r.text();
    logPos=+r.headers.get('X-Log-Pos')||logPos;
    if(t)addChunk(t);
  }catch(e){}
}
async function initLog(){
  try{
    const r=await fetch('/logs');
    logPos=+r.headers.get('X-Log-Pos')||0;
    logAll=await r.text();
    logLines=logAll.split('\n').filter(s=>s.length>0);
    renderLog();
  }catch(e){$('logs').textContent='нет связи с ботом'}
  clearInterval(logTimer);
  logTimer=setInterval(pullLog,1500);
}
$('logs').onscroll=()=>{if(atBottom())$('down').hidden=true};
$('down').onclick=scrollBottom;
$('q').oninput=()=>{filterQ=$('q').value.trim().toLowerCase();renderLog()};
$('clr').onclick=()=>{logAll='';logLines=[];renderLog()};
$('dl').onclick=()=>{
  const a=document.createElement('a');
  a.href=URL.createObjectURL(new Blob([logAll],{type:'text/plain'}));
  a.download='meshbot-log.txt';
  a.click();
  URL.revokeObjectURL(a.href);
};
function upload(url){
  return new Promise((ok,fail)=>{
    const x=new XMLHttpRequest(),fd=new FormData();
    fd.append('fw',file);
    x.open('POST',url);
    x.upload.onprogress=e=>{if(e.lengthComputable)bar(Math.round(e.loaded*100/e.total),'Загрузка на бот',kb(e.loaded)+' из '+kb(e.total))};
    x.onload=()=>ok(x.responseText);
    x.onerror=()=>fail(new Error('нет связи с ботом'));
    x.send(fd);
  });
}
async function waitBot(){
  await sleep(4000);
  for(let i=0;i<30;i++){
    try{if((await fetch('/info')).ok){location.reload();return}}catch(e){}
    await sleep(2000);
  }
  st('Бот не вернулся за минуту — проверьте питание и WiFi','err');
}
function track(){
  let doneAt=0;
  poll=setInterval(async()=>{
    let j;
    try{j=await (await fetch('/ota/status')).json()}catch(e){return}
    const sec=j.elapsed_ms/1000;
    const stats='повторы: '+j.retrs+' · опросы: '+j.polls;
    if(j.phase==1){bar(0,'Ждём ответ сенсора…',dur(sec))}
    else if(j.phase==2){
      const p=j.total?j.sent*100/j.total:0,rate=sec>0?j.sent/sec:0;
      bar(p,kb(j.sent)+' из '+kb(j.total),rate>0?(rate/1024).toFixed(1)+' КБ/с · осталось '+dur((j.total-j.sent)/rate):'');
      st(j.retr?'Повторы подряд: '+j.retr:'','');
    }
    else if(j.phase==3){bar(100,'Сенсор проверяет прошивку…',dur(sec))}
    else if(j.phase==4){
      bar(100,'Передано за '+dur(sec),stats);
      if(j.back){st('Готово: '+j.target+' загрузился'+(j.ver?' с версией '+j.ver:''),'ok');finish();loadSensors()}
      else{
        doneAt=doneAt||Date.now();
        if(Date.now()-doneAt>120000){st('Прошивка принята, но сенсор пока не вышел на связь','err');finish()}
        else st('Прошивка принята, ждём перезагрузку сенсора…','ok');
      }
    }
    else{st(j.err?'Ошибка: '+errText(j.err)+' ('+stats+')':'Сессия завершена','err');finish()}
  },1000);
}
async function startSession(){
  const s=await fetch('/ota/start?target='+encodeURIComponent(target),{method:'POST'});
  if(!s.ok)throw new Error(await s.text());
  $('ab').hidden=false;
  bar(0,'Ждём ответ сенсора…','');
  track();
}
async function flashStored(){
  if(isSelf())return;
  busy=true;refresh();
  try{await startSession()}catch(e){st(e.message,'err');finish()}
}
$('go').onclick=async()=>{
  if(isSelf()&&!confirm('Прошить сам бот? Он перезагрузится, связь ненадолго пропадёт.'))return;
  busy=true;refresh();
  try{
    if(isSelf()){
      const r=await upload('/update');
      if(r.indexOf('OK')<0)throw new Error(r||'FAIL');
      bar(100,'Прошито','');st('Бот перезагружается, страница обновится сама…','ok');
      waitBot();
      return;
    }
    const r=await upload('/savefw');
    if(r.indexOf('OK')<0)throw new Error(r||'FAIL');
    await startSession();
  }catch(e){st(e.message,'err');finish()}
};
async function loadCfg(){
  let list;
  try{list=await (await fetch('/config')).json()}catch(e){return}
  const box=$('cfg');
  box.innerHTML='';
  for(const it of list){
    const row=document.createElement('div');
    row.className='cfgrow';
    const lab=document.createElement('span');
    lab.textContent=it.f;
    const inp=document.createElement('input');
    // секрет приходит маской: показываем её подсказкой, а значение оставляем пустым,
    // чтобы случайно не записать маску вместо пароля
    if(it.s){inp.placeholder=it.v;inp.value=''}
    else{inp.value=(it.v=='(пусто)')?'':it.v}
    inp.dataset.f=it.f;
    inp.oninput=()=>{inp.dataset.dirty='1';inp.classList.add('dirty')};
    row.appendChild(lab);row.appendChild(inp);
    box.appendChild(row);
  }
}
$('cfgsave').onclick=async()=>{
  const body=new URLSearchParams();
  let n=0;
  document.querySelectorAll('#cfg input').forEach(i=>{
    if(i.dataset.dirty){body.append(i.dataset.f,i.value);n++}
  });
  if(!n){st('Ничего не изменено','');return}
  if(!confirm('Сохранить изменённых полей: '+n+'? Бот перезагрузится.'))return;
  body.append('reboot','1');
  try{
    const r=await fetch('/config',{method:'POST',body});
    const t=await r.text();
    if(!r.ok)throw new Error(t);
    st(t+' — ждём возврата бота…','ok');
    waitBot();
  }catch(e){st(e.message,'err')}
};
const SCFG=['name','sns_name','sns_key','lora_freq','lora_bw','lora_sf','lora_cr',
            'lora_tx','lora_pre','lora_sync','tz','disp_bri','vext_on'];
function buildSensorCfg(){
  const box=$('scfg');
  if(box.childElementCount)return;
  for(const f of SCFG){
    const row=document.createElement('div');
    row.className='cfgrow';
    const lab=document.createElement('span');
    lab.textContent=f;
    const inp=document.createElement('input');
    inp.placeholder='не менять';
    inp.dataset.f=f;
    inp.oninput=()=>inp.classList.toggle('dirty',!!inp.value);
    row.appendChild(lab);row.appendChild(inp);
    box.appendChild(row);
  }
}
buildSensorCfg();
$('scfgget').onclick=async()=>{
  if(isSelf()){st('Сначала выберите сенсор','err');return}
  const body=new URLSearchParams();
  body.append('target',target);
  body.append('get','1');
  try{
    const r=await fetch('/sensors/config',{method:'POST',body});
    st(await r.text(),'ok');
  }catch(e){st(e.message,'err')}
};
$('scfgsend').onclick=async()=>{
  if(isSelf()){st('Сначала выберите сенсор','err');return}
  const body=new URLSearchParams();
  body.append('target',target);
  let n=0;
  document.querySelectorAll('#scfg input').forEach(i=>{
    if(i.value){body.append(i.dataset.f,i.value);n++}
  });
  const save=$('scfgsave').checked,reboot=$('scfgreboot').checked;
  if(!n&&!save&&!reboot){st('Нечего отправлять','');return}
  if(!confirm('Отправить '+n+' полей сенсору '+target+'?'
      +(save?' Настройки будут сохранены.':' Без сохранения — до перезагрузки.')
      +(reboot?' Сенсор перезагрузится.':'')))return;
  if(save)body.append('save','1');
  if(reboot)body.append('reboot','1');
  $('scfgsend').disabled=true;
  try{
    const r=await fetch('/sensors/config',{method:'POST',body});
    const t=await r.text();
    if(!r.ok)throw new Error(t);
    st(t,'ok');
    document.querySelectorAll('#scfg input').forEach(i=>{i.value='';i.classList.remove('dirty')});
  }catch(e){st(e.message,'err')}
  $('scfgsend').disabled=false;
};
loadInfo();
loadSensors();
loadCfg();
initLog();
setInterval(loadInfo,5000);
setInterval(loadSensors,20000);
fetch('/ota/status').then(r=>r.json()).then(j=>{if(j.phase>=1&&j.phase<=3){busy=true;$('ab').hidden=false;$('prog').hidden=false;refresh();track()}}).catch(()=>{});

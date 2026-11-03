// Capture the reviewed HTML without changing its layout or production data.
// Start a local Chrome debugging port, then: node scripts/capture-android-html-reference.mjs PORT [BOTTOM_INSET]
import fs from 'node:fs/promises';
import {fileURLToPath} from 'node:url';
import path from 'node:path';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const port=Number(process.argv[2]||9222);
const bottom=Number(process.argv[3]||0);
if(!Number.isFinite(bottom)||bottom<0||bottom>100)throw Error('Invalid bottom inset');
const targets=await(await fetch(`http://127.0.0.1:${port}/json`)).json();
const ws=new WebSocket(targets.find(t=>t.type==='page').webSocketDebuggerUrl);
await new Promise(r=>ws.addEventListener('open',r,{once:true}));
let seq=0;const pending=new Map();ws.addEventListener('message',e=>{const m=JSON.parse(e.data);if(m.id){const p=pending.get(m.id);pending.delete(m.id);m.error?p.reject(m.error):p.resolve(m.result)}});
const call=(method,params={})=>new Promise((resolve,reject)=>{const id=++seq;pending.set(id,{resolve,reject});ws.send(JSON.stringify({id,method,params}))});
const evaluate=async expression=>{const r=await call('Runtime.evaluate',{expression,returnByValue:true,awaitPromise:true});if(r.exceptionDetails)throw Error(JSON.stringify(r.exceptionDetails));return r.result.value};
const pause=ms=>new Promise(r=>setTimeout(r,ms));
const close=()=>ws.close();
const shot=async(path,clip)=>{const r=await call('Page.captureScreenshot',{format:'png',captureBeyondViewport:true,...(clip?{clip:{...clip,scale:1}}:{})});await fs.writeFile(path,Buffer.from(r.data,'base64'))};

const out=root+'/tmp/android-design/reference-388'+(bottom?'-inset-'+bottom:'');
await fs.mkdir(out,{recursive:true});
await call('Emulation.setDeviceMetricsOverride',{width:388,height:844,deviceScaleFactor:1,mobile:false});
await call('Page.navigate',{url:'file://'+root+'/docs/design/android-prototype.html'});
await pause(600);
await evaluate(`(() => {
const p=states.app.el;document.body.appendChild(p);
document.querySelectorAll('body > :not(.phone)').forEach(e=>e.style.display='none');
const style=document.createElement('style');style.textContent='body{background:transparent}.phone{width:388px!important;max-width:none!important;height:844px!important;border:0!important;border-radius:0!important;margin:0!important;box-shadow:none!important}.statusbar{height:24px;min-height:24px;padding-top:0}.camera{display:none}.systembar{display:${bottom?'grid':'none'};height:${bottom}px;min-height:${bottom}px}';document.head.appendChild(style);
hosts.forEach(h=>h.address+=':22');
})()`);
const statesToCapture=['hosts','host-menu','host-edit','host-add','host-search-empty','hosts-empty','settings','settings-appearance','settings-input','settings-files','settings-sessions','settings-security','settings-choice','settings-reset','ssh-config','about','files-local','files-selection','files-clipboard','files-remote-empty','files-menu','terminal','hosts-resume','sessions'];
for(const theme of ['light','dark'])for(const page of statesToCapture){
await evaluate(`(() => {
Object.assign(preferences,defaultPreferences,{appearance:${JSON.stringify(theme)}});
const s=states.app;Object.assign(s,{sheet:null,search:'',emptyHosts:false,sessions:[],closed:true,keyboard:false,selecting:false,selected:new Set(),clipboard:null,job:null,path:[],scope:'local',fileStore:{}});
const page=${JSON.stringify(page)};
if(page==='settings-choice'){s.page='settings-detail';s.settingsSection='appearance';s.sheet='pref-choice';s.editingPref='terminalTheme'}
else if(page==='settings-reset'){s.page='settings';s.sheet='reset-preferences'}
else if(page.startsWith('settings-')){s.settingsSection=page.slice(9);s.page='settings-detail'}
else if(page==='settings')s.page='settings';
else if(page==='ssh-config'){s.page='settings';s.sheet='config'}
else if(page==='about'){s.page='settings';s.sheet='about'}
else if(page==='host-edit'||page==='host-add'){s.page='hosts';s.menuHost='prod';s.sheet=page==='host-edit'?'edit-host':'add-host'}
else if(page.startsWith('files-')) {
s.page='files';const items=[{name:'core',size:'文件夹',dir:true},{name:'docs',size:'文件夹',dir:true},{name:'scripts',size:'文件夹',dir:true},{name:'build.log',size:'15 B',dir:false},{name:'README.md',size:'38 B',dir:false}];
s.fileStore[locationKey(fileLocation(s))]=items;
if(page==='files-selection'){s.selecting=true;s.selected=new Set(['README.md'])}
if(page==='files-clipboard')s.clipboard={from:{scope:'local',host:null,path:['source']},items:[items[4]]};
if(page==='files-remote-empty'){s.scope='remote';s.fileConnections=new Set()}
if(page==='files-menu')s.sheet='file-menu';
} else if(page==='terminal'||page==='sessions'){s.page='terminal';s.host='local';s.sessions=['local'];s.closed=false;if(page==='sessions')s.sheet='sessions'}
else {s.page='hosts';if(page==='host-menu'){s.menuHost='prod';s.sheet='host-menu'}if(page==='host-search-empty')s.search='no-such-host';if(page==='hosts-empty')s.emptyHosts=true;if(page==='hosts-resume'){s.host='local';s.sessions=['local'];s.closed=false}}
render(s);return s.page;
})()`);
await pause(100);
await shot(out+'/'+theme+'-'+page+'.png');
}
close();
console.log('Captured '+statesToCapture.length*2+' HTML references');

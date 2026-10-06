import assert from 'node:assert/strict';
import {spawn,spawnSync} from 'node:child_process';
import {mkdtempSync,mkdirSync,writeFileSync,readFileSync,rmSync,statSync,symlinkSync,existsSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join,dirname} from 'node:path';
import {createServer} from 'node:net';
import test from 'node:test';
const bin=process.env.HERDR_FOCUS_BIN!;
const fixture=process.env.HERDR_FOCUS_FIXTURE!;
const legacy=process.env.HERDR_FOCUS_LEGACY;
assert.ok(bin&&fixture,'Set HERDR_FOCUS_BIN and HERDR_FOCUS_FIXTURE');
function pure(mode:string,input:any=null,args:string[]=[],env:Record<string,string>={},status=0){const r=spawnSync(fixture,[mode,...args],{input:input===null?'':typeof input==='string'?input:JSON.stringify(input),encoding:'utf8',env:{...process.env,...env},maxBuffer:8_000_000});assert.ifError(r.error);assert.equal(r.status,status,r.stderr);return r.stdout?JSON.parse(r.stdout):null;}
const snap=()=>({workspaces:[{workspace_id:'w1',label:'dot',number:1},{workspace_id:'w3',label:'admin',number:3}],tabs:[{tab_id:'t1',workspace_id:'w1',label:'only'},{tab_id:'t2',workspace_id:'w3',label:'2 hadleigh review'},{tab_id:'t3',workspace_id:'w3',label:'other'}],agents:[{agent:'pi',display_agent:'pi · medium',agent_status:'working',pane_id:'p1',tab_id:'t2',workspace_id:'w3',state_change_seq:4},{agent:'pi',display_agent:'pi · medium',agent_status:'done',pane_id:'p2',tab_id:'t2',workspace_id:'w3',state_change_seq:9},{agent:'pi',agent_status:'idle',pane_id:'p3',tab_id:'t2',workspace_id:'w3',state_change_seq:3}]});
const body='pi finished: admin · 3 · 2 hadleigh review';
const toast=(text=body)=>pure('parse',null,['Ghostty',text]);
function resolve(snapshot:any,text=body){return pure('resolve',{snapshot,toast:toast(text)});}
function scratch(fn:(root:string)=>void){const root=mkdtempSync(join(tmpdir(),'hf-'));try{fn(root);}finally{rmSync(root,{recursive:true,force:true});}}
function save(path:string,data:any){mkdirSync(dirname(path),{recursive:true});writeFileSync(path,typeof data==='string'||Buffer.isBuffer(data)?data:JSON.stringify(data));}
function env(root:string){return {HOME:root,XDG_CONFIG_HOME:join(root,'config'),XDG_STATE_HOME:join(root,'state'),HERDR_SOCKET_PATH:join(root,'none'),PATH:join(root,'bin')+':'+process.env.PATH};}
function clients(root:string,data:any=[],mode='normal'){mkdirSync(join(root,'bin'),{recursive:true});save(join(root,'clients.json'),data);writeFileSync(join(root,'bin/hyprctl'),`#!${process.execPath}\nconst f=require('node:fs'),p=require('node:path');const args=process.argv.slice(2);f.appendFileSync(p.join(process.env.HOME,'actions'),JSON.stringify(args)+'\\n');if(args[0]==='clients'){${mode==='stall'?"setTimeout(()=>{},10000);":mode==='bad'?"process.stdout.write('bad');":mode==='nul'?"process.stdout.write(f.readFileSync(p.join(process.env.HOME,'clients.json')));process.stdout.write('\\0tail');":mode==='invalid'?"process.stdout.write(Buffer.from([255]));":mode==='exit'?"process.exit(4);":"process.stdout.write(f.readFileSync(p.join(process.env.HOME,'clients.json')));"}}${mode==='dispatch-stall'?"if(args[0]==='dispatch')setTimeout(()=>{},10000);":''}\n`,{mode:0o755});}
function actions(root:string){return existsSync(join(root,'actions'))?readFileSync(join(root,'actions'),'utf8').trim().split('\n').map(s=>JSON.parse(s)):[];}
async function command(exe:string,args:string[],environment:Record<string,string>){const c=spawn(exe,args,{env:{...process.env,...environment},stdio:['ignore','pipe','pipe']});let stdout='',stderr='';c.stdout.on('data',b=>stdout+=b);c.stderr.on('data',b=>stderr+=b);return await new Promise<{status:number|null,stdout:string,stderr:string}>((resolve,reject)=>{c.on('error',reject);c.on('close',status=>resolve({status,stdout,stderr}));});}
async function scenario(fn:(root:string,exe:string,calls:any[],peer:(path:string,snapshot:any,mode?:string)=>Promise<void>)=>Promise<void>){for(const exe of [bin,...legacy?[legacy]:[]]){const root=mkdtempSync(join(tmpdir(),'hf-rpc-')),calls:any[]=[],servers:any[]=[],connections=new Set<any>();try{clients(root);const peer=async(path:string,snapshot:any,mode='normal')=>{mkdirSync(dirname(path),{recursive:true});const server=createServer(s=>{connections.add(s);s.on('close',()=>connections.delete(s));let input='';s.on('error',()=>{});s.on('data',b=>{input+=b;if(!input.includes('\n'))return;const request=JSON.parse(input.split('\n')[0]);calls.push({path,...request});if(mode==='stall')return;if(mode==='empty'){s.end();return;}if(mode==='bad'){s.end('malformed\n');return;}const data=JSON.stringify(request.method==='session.snapshot'?{result:{snapshot}}:{result:{type:'ok'}});if(mode==='chunks'||mode==='progress'){let at=0;const timer=setInterval(()=>{if(s.destroyed||at>=data.length){clearInterval(timer);if(!s.destroyed)s.end('\nignored');return;}s.write(data.slice(at,at+(mode==='progress'?50:10000)));at+=mode==='progress'?50:10000;},mode==='progress'?600:15);}else s.end((mode==='bom'?'\ufeff':'')+data+(mode==='eof'?'':'\nignored'));});});servers.push(server);await new Promise<void>((r,j)=>{server.once('error',j);server.listen(path,r);});};await fn(root,exe,calls,peer);}finally{for(const s of connections)s.destroy();for(const s of servers)await new Promise<void>(r=>s.close(()=>r()));rmSync(root,{recursive:true,force:true});}}}
const args=(command='remember',text=body)=>[command,'--summary','Ghostty','--body',text,'--ts','12.5'];
const cache=(root:string)=>join(root,'state/herdr-notification-focus/targets.json');

test('toast formats, whitespace, events, Unicode decimal numbers and tab separators',()=>{
 assert.deepEqual(toast(),{agent:'pi',event:'finished',workspace_label:'admin',workspace_number:3,tab_label:'2 hadleigh review'});
 assert.equal(toast('pi needs attention: admin · 3').tab_label,null);
 assert.equal(toast('pi finished: dot · 1 · 1 race · later').tab_label,'1 race · later');
 assert.equal(toast('\u001cpi finished: admin · ３\u0085').workspace_number,3);
 assert.equal(pure('parse',null,['pi finished','admin · 3','com.Ghostty']).agent,'pi');
 assert.equal(toast('pi finished: admin · ²x'),null);
 for(const digit of ['²','①','፩','🄁'])pure('parse',null,['Ghostty','pi finished: admin · '+digit],{},1);
 for(const [summary,text] of [['Slack',body],['Ghostty','finished: admin · 3'],['Ghostty','pi finished: dot · -1'],['Ghostty','pi finished: dot · x'],['ghostty',body]])assert.equal(pure('parse',null,[summary,text]),null);
});
test('pane status preference, display/prefix matching and first equal-sequence tie',()=>{
 assert.equal(resolve(snap()).pane_id,'p2');
 for(const name of ['PI','pi · medium','pi custom model'])assert.equal(resolve(snap(),body.replace('pi ',name+' ')).pane_id,'p2');
 const s=snap();s.agents[0].agent_status='blocked';s.agents[0].state_change_seq=11;assert.equal(resolve(s,body.replace('finished','needs attention')).pane_id,'p1');
 s.agents[1].state_change_seq=3;assert.equal(resolve(s).pane_id,'p2');
 s.agents.forEach(a=>a.agent_status='working');assert.equal(resolve(s).pane_id,'p1');
});
test('workspace matching never follows another label and ambiguous tabs stay scoped',()=>{
 assert.equal(resolve(snap(),body.replace('admin','missing')),null);
 assert.deepEqual(resolve(snap(),body.replace('2 hadleigh review','renamed')),{workspace_id:'w3'});
 const s=snap();s.workspaces.push({workspace_id:'other',label:'admin',number:8});assert.equal(resolve(s).workspace_id,'w3');assert.equal(resolve(s,body.replace(' · 3',' · 7')),null);
 s.tabs.push({...s.tabs[1]});assert.deepEqual(resolve(s),{workspace_id:'w3'});
});
test('single-tab and no-tab status uniqueness retain pane/tab/workspace fallback',()=>{
 const s=snap();s.tabs=s.tabs.filter(t=>t.workspace_id==='w3'&&t.tab_id==='t2');assert.equal(resolve(s,'pi finished: admin · 3').pane_id,'p2');
 s.tabs=[];assert.deepEqual(resolve(s,'pi finished: admin · 3'),{workspace_id:'w3'});
 s.agents=s.agents.filter(a=>a.agent_status!=='idle');assert.equal(resolve(s,'pi finished: admin · 3').pane_id,'p2');
 s.agents=[];s.tabs=[{workspace_id:'w3',tab_id:'empty',label:'2 hadleigh review'}];assert.equal(resolve(s).tab_id,'empty');
});
test('wide integers, decimal separators, JSON ID types and invalid numeric records',()=>{
 const s=snap();(s.workspaces[1] as any).number=' +0_3 ';(s.agents[0] as any).state_change_seq='999999999999999999999999';s.agents[0].agent_status='done';assert.equal(resolve(s).pane_id,'p1');
 (s.agents[0] as any).state_change_seq='bad';pure('resolve',{snapshot:s,toast:toast()},[],{},1);
 const wide=JSON.stringify({snapshot:snap(),toast:toast()}).replace('"state_change_seq":9','"state_change_seq":18446744073709551615');assert.equal(pure('resolve',wide).pane_id,'p2');
 const n=snap();(n.workspaces[1] as any).workspace_id=3;(n.tabs[1] as any).workspace_id='3';assert.deepEqual(resolve(n),{workspace_id:3});
 pure('parse',null,['Ghostty','pi finished: admin · ²'],{},1);
});
test('unused malformed groups do not poison workspace fallbacks; selected malformed status fails',()=>{
 assert.equal(pure('resolve',{snapshot:{workspaces:[],tabs:'bad',agents:'bad'},toast:toast()}),null);
 assert.deepEqual(pure('resolve',{snapshot:{workspaces:snap().workspaces,tabs:[],agents:'bad'},toast:toast()}),{workspace_id:'w3'});
 const s=snap();(s.agents[1] as any).agent_status=[];pure('resolve',{snapshot:s,toast:toast()},[],{},1);
 pure('alive',{snapshot:{agents:[{pane_id:'p'},{pane_id:[]}]},target:{pane_id:'p'}},[],{},1);
});
test('alive checks use pane precedence across agents and panes without requiring status',()=>{
 const s=snap();assert.equal(pure('alive',{snapshot:s,target:{pane_id:'p2',workspace_id:'gone'}}),true);
 assert.equal(pure('alive',{snapshot:{panes:[{pane_id:'p'}]},target:{pane_id:'p'}}),true);
 assert.equal(pure('alive',{snapshot:s,target:{pane_id:'gone',tab_id:'t2'}}),false);
 assert.equal(pure('alive',{snapshot:s,target:{tab_id:'t2'}}),true);
 assert.equal(pure('alive',{snapshot:s,target:{workspace_id:'w3'}}),true);
});
test('focus payload prefers pane, tab, then workspace and preserves ID data',()=>{
 for(const [target,method,params] of [[{pane_id:'p',tab_id:'t'},'pane.focus',{pane_id:'p'}],[{pane_id:'',tab_id:'t'},'tab.focus',{tab_id:'t'}],[{workspace_id:3},'workspace.focus',{workspace_id:3}]])assert.deepEqual(pure('payload',target),{method,params});
 assert.equal(pure('payload',{}),null);
});
test('window selection prefers sender, ancestors, title, then first minimum history',()=>{
 const c=[{pid:1,class:'Ghostty',address:'one',title:'other',focusHistoryID:0},{pid:9,class:'Ghostty',address:'nine',title:'admin',focusHistoryID:4},{pid:3,class:'cliamp.ghostty',address:'three',focusHistoryID:-9}];
 const choose=(pid=0,ancestors:number[]=[],label='')=>pure('choose',{clients:c,pid,ancestors,label});
 assert.equal(choose(3).address,'three');assert.equal(choose(0,[4,9],'admin').address,'nine');assert.equal(choose(0,[],'admin').address,'nine');assert.equal(choose().address,'one');
 c[1].title='other';c[1].focusHistoryID=0;assert.equal(choose().address,'one');
 assert.equal(pure('choose',{clients:[{pid:8,class:'other',address:'eight'}],ancestors:[8]}).address,'eight');
});
test('cache upserts exact raw keys, retains unknown metadata, limits to 100 and publishes privately',()=>scratch(root=>{
 const path=join(root,'new/targets.json');save(path,{extra:{kept:true},entries:Array.from({length:110},(_,i)=>({key:String(i),pane_id:String(i)}))});
 pure('remember',{pane_id:'p',socket:'sock'},['Ghostty',body,'12.5',path]);let data=JSON.parse(readFileSync(path,'utf8'));assert.equal(data.entries.length,100);assert.equal(data.entries[0].key,'11');assert.deepEqual(data.extra,{kept:true});assert.equal(statSync(path).mode&0o777,0o600);assert.ok(!existsSync(join(root,'new/targets.tmp')));
 pure('remember',{pane_id:'new'},['Ghostty',body,'12.5',path]);data=JSON.parse(readFileSync(path,'utf8'));assert.equal(data.entries.length,100);assert.equal(pure('cache',null,['Ghostty',body,'12.5',path]).pane_id,'new');assert.equal(pure('cache',null,['Ghostty ',body,'12.5',path]),null);
 const fresh=join(root,'private/deep/targets.json');pure('remember',{workspace_id:'w'},['s','b','t',fresh]);assert.equal(statSync(dirname(fresh)).mode&0o777,0o700);
}));
test('malformed cache is reseeded, malformed entries fail, and invalid UTF-8 does not rewrite',()=>scratch(root=>{
 const path=join(root,'targets.json');for(const bad of ['bad','[]','{"entries":1}']){save(path,bad);pure('remember',{pane_id:'p'},['s','b','t',path]);assert.equal(JSON.parse(readFileSync(path,'utf8')).entries.length,1);}
 save(path,{entries:[4]});pure('cache',null,['s','b','t',path],{},1);
 save(path,Buffer.from([0xff]));pure('remember',{pane_id:'p'},['s','b','t',path],{},1);assert.deepEqual(readFileSync(path),Buffer.from([0xff]));
}));
test('socket discovery preserves override/default/sorted sessions, duplicates and lexical paths',()=>scratch(root=>{
 const e=env(root),base=join(root,'config/herdr');for(const p of ['herdr.sock','sessions/z/herdr.sock','sessions/a/herdr.sock','sessions/.hidden/herdr.sock'])save(join(base,p),'');
 assert.deepEqual(pure('paths',null,[],{...e,HERDR_SOCKET_PATH:join(base,'./herdr.sock')}),[join(base,'herdr.sock'),join(base,'sessions/.hidden/herdr.sock'),join(base,'sessions/a/herdr.sock'),join(base,'sessions/z/herdr.sock')]);
 save(join(root,'override'),'');assert.equal(pure('paths',null,[],{...e,HERDR_SOCKET_PATH:join(root,'override')})[0],join(root,'override'));
}));
test('proc stat parsing, ancestor cycles/limit and session-specific attachment with fallback',()=>scratch(root=>{
 const proc=join(root,'proc');save(join(proc,'9/stat'),'9 (name with ) parens) S 8 0');save(join(proc,'8/stat'),'8 (parent) S 9 0');assert.deepEqual(pure('ancestors',null,['9',proc]),[9,8]);assert.equal(pure('parent',null,['9',proc]),8);
 for(let n=1;n<=40;n++)save(join(proc,String(n),'stat'),`${n} (p) S ${n+1}`);assert.equal(pure('ancestors',null,['1',proc]).length,32);
 save(join(proc,'42/cmdline'),Buffer.from('herdr\0session\0attach\0default\0'));save(join(proc,'43/cmdline'),Buffer.from('herdr\0session\0attach\0work\0'));save(join(proc,'44/cmdline'),Buffer.from('other\0attach\0work\0'));
 assert.deepEqual(pure('attached',null,['/x/herdr.sock',proc]),[42]);assert.deepEqual(pure('attached',null,['/x/sessions/work/herdr.sock',proc]),[43]);assert.deepEqual(pure('attached',null,['/x/sessions/missing/herdr.sock',proc]).sort(),[42,43]);
}));
test('remember resolves through ordered sockets and persists the first target',async()=>scenario(async(root,exe,calls,peer)=>{
 const e=env(root),first=join(root,'first.sock');await peer(first,{workspaces:[]});await peer(join(root,'config/herdr/herdr.sock'),snap());const r=await command(exe,args(),{...e,HERDR_SOCKET_PATH:first});assert.equal(r.status,0,r.stderr);assert.equal(calls.length,2);assert.equal(calls[0].method,'session.snapshot');assert.equal(calls[0].id,'omapager-herdr-focus');const saved=JSON.parse(readFileSync(cache(root),'utf8')).entries[0];assert.equal(saved.pane_id,'p2');assert.equal(saved.socket,calls[1].path);assert.deepEqual(actions(root),[]);
}));
test('remember ignores unrelated toasts without contacting peers or compositor',async()=>scenario(async(root,exe,calls,peer)=>{
 const path=join(root,'config/herdr/herdr.sock');await peer(path,snap());const r=await command(exe,['remember','--summary','Slack','--body',body],env(root));assert.equal(r.status,0,r.stderr);assert.equal(calls.length,0);assert.equal(existsSync(cache(root)),false);assert.deepEqual(actions(root),[]);
}));
test('open reuses a live saved pane after state changes and focuses Herdr before raising Ghostty',async()=>scenario(async(root,exe,calls,peer)=>{
 const path=join(root,'saved.sock'),s=snap();s.agents[0].agent_status='blocked';await peer(path,s);save(cache(root),{entries:[{key:'12.5|Ghostty|'+body,pane_id:'p1',socket:path}]});clients(root,[{pid:7,class:'Ghostty',address:'0xabc',title:'admin'}]);const r=await command(exe,args('open'),env(root));assert.equal(r.status,0,r.stderr);assert.deepEqual(calls.map(c=>c.method),['session.snapshot','pane.focus']);assert.deepEqual(calls[1].params,{pane_id:'p1'});assert.deepEqual(actions(root),[['clients','-j'],['dispatch','focuswindow','address:0xabc']]);
}));
test('stale saved targets re-resolve there before considering another socket',async()=>scenario(async(root,exe,calls,peer)=>{
 const path=join(root,'saved.sock');await peer(path,snap());save(cache(root),{entries:[{key:'12.5|Ghostty|'+body,pane_id:'gone',socket:path}]});const r=await command(exe,args('open'),env(root));assert.equal(r.status,0,r.stderr);assert.deepEqual(calls.map(c=>c.method),['session.snapshot','pane.focus']);assert.equal(calls[1].params.pane_id,'p2');
}));
test('missing cached workspace falls back to other sessions, renamed tabs focus only workspace',async()=>scenario(async(root,exe,calls,peer)=>{
 const old=join(root,'old.sock'),current=join(root,'config/herdr/herdr.sock');await peer(old,{workspaces:[]});await peer(current,snap());save(cache(root),{entries:[{key:'12.5|Ghostty|'+body,workspace_id:'gone',socket:old}]});let r=await command(exe,args('open'),env(root));assert.equal(r.status,0,r.stderr);assert.equal(calls.at(-1).params.pane_id,'p2');calls.length=0;r=await command(exe,args('open',body.replace('2 hadleigh review','renamed')),env(root));assert.equal(r.status,0,r.stderr);assert.equal(calls.at(-1).method,'workspace.focus');
}));
test('unrelated open still selects the explicit sender and uses argv without a shell',async()=>scenario(async(root,exe,calls)=>{
 const address='0xabc; touch /tmp/not-a-command';clients(root,[{pid:7,class:'other',address},{pid:8,class:'Ghostty',address:'other'}]);const r=await command(exe,['open','--summary','Slack','--pid','7'],env(root));assert.equal(r.status,0,r.stderr);assert.equal(calls.length,0);assert.deepEqual(actions(root).at(-1),['dispatch','focuswindow','address:'+address]);
}));
test('fragmented/large replies, EOF framing, BOM and trailing messages retain protocol behavior',async()=>{
 for(const mode of ['chunks','eof','bom'])await scenario(async(root,exe,calls,peer)=>{const s:any=snap();s.padding='x'.repeat(200000);await peer(join(root,'config/herdr/herdr.sock'),s,mode);const r=await command(exe,args(),env(root));assert.equal(r.status,0,r.stderr);assert.equal(calls.length,1);assert.equal(JSON.parse(readFileSync(cache(root),'utf8')).entries[0].pane_id,'p2');});
});
test('progressing socket replies can exceed the two-second total duration',async()=>scenario(async(root,exe,calls,peer)=>{
 await peer(join(root,'config/herdr/herdr.sock'),{workspaces:[{workspace_id:'w3',label:'admin',number:3}],tabs:[],agents:[]},'progress');const before=Date.now(),r=await command(exe,args(),env(root));assert.equal(r.status,0,r.stderr);assert.ok(Date.now()-before>2000);assert.equal(JSON.parse(readFileSync(cache(root),'utf8')).entries[0].workspace_id,'w3');
}));
test('empty peers are harmless but malformed replies and connection errors stop resolution',async()=>{
 for(const mode of ['empty','bad'])await scenario(async(root,exe,calls,peer)=>{await peer(join(root,'config/herdr/herdr.sock'),snap(),mode);const r=await command(exe,args(),env(root));assert.equal(r.status,mode==='empty'?0:1,r.stderr);assert.equal(existsSync(cache(root)),false);});
 await scenario(async(root,exe)=>{save(join(root,'config/herdr/herdr.sock'),'not a socket');const r=await command(exe,args(),env(root));assert.equal(r.status,1);});
});
test('socket idle timeout is bounded and retains existing cache',async()=>scenario(async(root,exe,calls,peer)=>{
 await peer(join(root,'config/herdr/herdr.sock'),snap(),'stall');save(cache(root),{entries:[{key:'old',pane_id:'old'}]});const before=Date.now(),r=await command(exe,args(),env(root));assert.equal(r.status,1,r.stderr);assert.ok(Date.now()-before>=1800&&Date.now()-before<4500);assert.equal(JSON.parse(readFileSync(cache(root),'utf8')).entries[0].key,'old');
}));
test('Hyprland missing/malformed/nonzero/timeout client probes stay harmless; focus timeout fails',async()=>{
 for(const mode of ['bad','nul','invalid','exit','stall','dispatch-stall'])await scenario(async(root,exe)=>{clients(root,[{pid:7,class:'Ghostty',address:'a'}],mode);const r=await command(exe,['open','--pid','7'],env(root));assert.equal(r.status,mode==='dispatch-stall'||mode==='invalid'?1:0,r.stderr);});
});
test('embedded NUL compositor addresses fail before dispatch rather than truncating data',async()=>scenario(async(root,exe)=>{
 clients(root,[{pid:7,class:'Ghostty',address:'0xabc\u0000suffix'}]);const r=await command(exe,['open','--pid','7'],env(root));assert.equal(r.status,1,r.stderr);assert.deepEqual(actions(root),[['clients','-j']]);
}));
test('invalid digit-like sender PIDs fail instead of raising a fallback terminal',async()=>scenario(async(root,exe)=>{
 clients(root,[{pid:7,class:'Ghostty',address:'a'}]);const r=await command(exe,['open','--pid','²'],env(root));assert.equal(r.status,1,r.stderr);assert.deepEqual(actions(root),[]);
}));
test('CLI syntax and option abbreviations retain statuses without live actions',async()=>scenario(async(root,exe)=>{
 for(const [argv,status] of [[[],2],[['bad'],2],[['remember','--bad'],2],[['remember','--body'],2],[['remember','--pid','-g'],2],[['--help'],0],[['remember','--help'],0],[['remember','--sum=Slack','--bo=foo','--t=3','--p=-1'],0]]){const r=await command(exe,argv as string[],env(root));assert.equal(r.status,status,r.stderr);}
}));

import assert from "node:assert/strict";
import { spawn, spawnSync } from "node:child_process";
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { tmpdir } from "node:os";
import { test } from "node:test";

const binary = process.env.DAILY_WORKFLOW_BIN!, pty = process.env.DAILY_WORKFLOW_PTY!;
assert.ok(binary && pty, "Set DAILY_WORKFLOW_BIN and DAILY_WORKFLOW_PTY");
const legacy = process.env.DAILY_WORKFLOW_LEGACY;
const python = process.env.DAILY_WORKFLOW_PYTHON || process.env.PATH?.split(":").map(p=>join(p,"python3")).find(existsSync);
const bash = process.env.PATH?.split(":").map(p=>join(p,"bash")).find(existsSync)!;
const quoted = (s:string)=>"'"+s.replaceAll("'","'\\''")+"'";
const prefix=["--user","--scope","--collect","--quiet","--","ghostty"];
const usage="Usage: daily-workflow workspace|notes|agenda|cleanup\n";
const error="Daily action could not start. Check installed commands and user services.\n";
const banner="Delete old Nix generations and collect garbage for your user AND the system.\nThis removes rollback options. Sudo may request your password.\nType DELETE to run gcall, or Enter to cancel: ";
const close="\nPress Enter to close.";
const fake=`#!${process.execPath}
const fs=require('node:fs'),path=require('node:path'),name=path.basename(process.argv[1]);
const proc=fs.readFileSync('/proc/self/stat','utf8').split(') ')[1].split(' ');
const target=fd=>fs.readlinkSync('/proc/self/fd/'+fd);
let inherited=false;try{fs.fstatSync(98);inherited=true;}catch{}
const env={backend:process.env.MUX_BACKEND,attach:process.env.MUX_HERDR_ATTACH,herdr:process.env.HERDR_TEST,plain:process.env.HERDR,other:process.env.NOT_HERDR_TEST,marker:process.env.DW_MARKER};
const entry={name,args:process.argv.slice(2),cwd:process.cwd(),pid:process.pid,pgrp:Number(proc[2]),sid:Number(proc[3]),stdio:[0,1,2].map(target),env,herdrKeys:Object.keys(process.env).filter(k=>k.startsWith('HERDR_')),inherited};
if(process.env.DW_READ_STDIN==='1')entry.input=fs.readFileSync(0,'utf8');
fs.appendFileSync(process.env.DW_LOG,JSON.stringify(entry)+'\\n');
if(name!=='systemd-run')process.stdout.write('fixture-'+name+'\\n');
if(process.env.DW_LARGE)process.stdout.write('x'.repeat(Number(process.env.DW_LARGE)));
if(process.env.DW_STDERR)process.stderr.write(process.env.DW_STDERR);
setTimeout(()=>{if(process.env.DW_SIGNAL)process.kill(process.pid,process.env.DW_SIGNAL);else process.exitCode=Number(process.env.DW_STATUS||0);},Number(process.env.DW_DELAY||0));
`;
function setup(t:any){
  const root=mkdtempSync(join(tmpdir(),"daily-workflow-"));t.after(()=>rmSync(root,{recursive:true,force:true}));
  const home=join(root,"home"),bin=join(root,"bin"),log=join(root,"log.jsonl");mkdirSync(join(home,".bin"),{recursive:true});mkdirSync(bin);writeFileSync(log,"");
  for(const name of ["systemd-run","daily-agenda"])writeFileSync(join(bin,name),fake,{mode:0o755});writeFileSync(join(home,".bin","gcall"),fake,{mode:0o755});
  const env={...process.env,HOME:home,PATH:bin,DW_LOG:log,DW_MARKER:"test-marker",HERDR_TEST:"remove-me",HERDR_ANOTHER:"remove-me-too",HERDR_:"also-remove",HERDR:"keep-me",NOT_HERDR_TEST:"keep-other",MUX_BACKEND:"tmux",MUX_HERDR_ATTACH:"attach-value"};
  const programs=[binary];if(legacy){assert.ok(python && bash);const script=join(root,"legacy");writeFileSync(script,`#!${bash}\nexec ${quoted(python!)} ${quoted(legacy)} "$@"\n`,{mode:0o755});programs.push(script);}
  const run=(program:string,args:string[],options:any={})=>spawnSync(program,args,{encoding:"utf8",timeout:5000,input:"",cwd:root,env,...options});
  const rows=()=>readFileSync(log,"utf8").trim().split("\n").filter(Boolean).map(s=>JSON.parse(s));
  return {root,home,bin,log,env,programs,run,rows};
}
async function until(check:()=>boolean){for(let i=0;i<150;i++){if(check())return;await new Promise(r=>setTimeout(r,20));}assert.fail("expected command did not appear");}

test("invalid actions, options, injection and argument counts do not spawn",t=>{
  const s=setup(t);for(const program of s.programs)for(const args of [[],["--help"],["notes","extra"],["_cleanup","extra"],["agenda;touch marker"],[""],["_agenda "],["NOTES"]]){const r=s.run(program,args);assert.equal(r.status,64);assert.equal(r.stderr,usage);assert.equal(r.stdout,"");}assert.deepEqual(s.rows(),[]);
});

test("all public actions use fixed scoped argv, original cwd and silenced streams",async t=>{
  const s=setup(t);for(const program of s.programs)for(const action of ["workspace","notes","agenda","cleanup"]){const before=s.rows().length,r=s.run(program,[action]);assert.equal(r.status,0,r.stderr);assert.equal(r.stdout,"");assert.equal(r.stderr,"");await until(()=>s.rows().length>before);const e=s.rows()[before];
    const window=action==="agenda"?["--title=Today","--window-width=84","--window-height=24"]:action==="notes"?["--title=Today's notes","--window-width=100","--window-height=36"]:[];
    const command=action==="workspace"?["fish","-ic","mux start dot"]:action==="notes"?["fish","-ic","today"]:["daily-workflow",action==="agenda"?"_agenda":"_cleanup"];
    assert.deepEqual(e.args,[...prefix,...window,"--working-directory="+s.home,"-e",...command]);assert.equal(e.cwd,s.root);assert.equal(e.sid,e.pid);assert.equal(e.pgrp,e.pid);assert.deepEqual(e.stdio,["/dev/null","/dev/null","/dev/null"]);
  }
});

test("launch environment strips every HERDR_ key and overrides mux attachment only",async t=>{
  const s=setup(t);for(const program of s.programs){const before=s.rows().length;assert.equal(s.run(program,["notes"]).status,0);await until(()=>s.rows().length>before);const e=s.rows()[before];assert.deepEqual(e.herdrKeys,[]);assert.deepEqual(e.env,{backend:"herdr",plain:"keep-me",other:"keep-other",marker:"test-marker"});}
});

test("subprocess descriptors close and HOME remains one unsplit argument",async t=>{
  const s=setup(t);for(const program of s.programs){const before=s.rows().length;const home=join(s.root,"hôme; $(touch nope) 'quote' \"q\"");const r=spawnSync(pty,["fd",program,"workspace"],{encoding:"utf8",env:{...s.env,HOME:home},cwd:s.root});assert.equal(r.status,0,r.stderr);await until(()=>s.rows().length>before);const e=s.rows()[before];assert.equal(e.inherited,false);assert.equal(e.args.at(-5),"--working-directory="+home);assert.ok(!existsSync(join(s.root,"nope")));}
});

test("HOME Path normalization preserves relative paths and two leading slashes",async t=>{
  const s=setup(t);for(const program of s.programs)for(const [home,expected] of [["","/"],[".","."],["relative//./home/","relative/home"],["/"+s.home,"/"+s.home],["//"+s.home,s.home],[s.home+"/../home/",s.home+"/../home"]]){const before=s.rows().length;assert.equal(s.run(program,["workspace"],{env:{...s.env,HOME:home}}).status,0);await until(()=>s.rows().length>before);assert.ok(s.rows()[before].args.includes("--working-directory="+expected));}
});

test("missing scope launcher returns the bounded path-free error",t=>{
  const s=setup(t);rmSync(join(s.bin,"systemd-run"));for(const program of s.programs){const r=s.run(program,["notes"]);assert.equal(r.status,1);assert.equal(r.stdout,"");assert.equal(r.stderr,error);}
});

test("later scope command failure is not a synchronous launch failure",async t=>{
  const s=setup(t);for(const program of s.programs){const before=s.rows().length;assert.equal(s.run(program,["agenda"],{env:{...s.env,DW_STATUS:"9"}}).status,0);await until(()=>s.rows().length>before);}
});

test("internal agenda inherits environment/stdio, propagates status and waits for Enter",t=>{
  const s=setup(t);for(const program of s.programs)for(const status of [0,7,255]){const before=s.rows().length,r=s.run(program,["_agenda"],{input:"\n",env:{...s.env,DW_STATUS:String(status),DW_STDERR:"fixture-error\n"}});assert.equal(r.status,status,r.stderr);assert.equal(r.stdout,"fixture-daily-agenda\n"+close);assert.equal(r.stderr,"fixture-error\n");const e=s.rows()[before];assert.equal(e.name,"daily-agenda");assert.deepEqual(e.args,[]);assert.equal(e.cwd,s.root);assert.notEqual(e.sid,e.pid);assert.deepEqual(e.env,{backend:"tmux",attach:"attach-value",herdr:"remove-me",plain:"keep-me",other:"keep-other",marker:"test-marker"});assert.ok(e.herdrKeys.includes("HERDR_"));assert.ok(e.stdio.every((p:string)=>/^(pipe|socket):/.test(p)));}
});

test("internal child input is the original stdin",t=>{
  const s=setup(t);for(const program of s.programs){const before=s.rows().length,r=s.run(program,["_agenda"],{input:"child input\n",env:{...s.env,DW_READ_STDIN:"1"}});assert.equal(r.status,0,r.stderr);assert.equal(s.rows()[before].input,"child input\n");}
});

test("large child output streams without an adapter buffer or cap",t=>{
  const s=setup(t),size=5*1024*1024;for(const program of s.programs){const r=s.run(program,["_agenda"],{env:{...s.env,DW_LARGE:String(size)},maxBuffer:7*1024*1024});assert.equal(r.status,0,r.stderr);assert.equal(r.stdout.length,"fixture-daily-agenda\n".length+size+close.length);assert.ok(r.stdout.startsWith("fixture-daily-agenda\n")&&r.stdout.endsWith(close));assert.ok(/^x+$/.test(r.stdout.slice("fixture-daily-agenda\n".length,-close.length)));}
});

test("exact DELETE alone authorizes the home gcall helper, never a shell",t=>{
  const s=setup(t);for(const program of s.programs)for(const input of ["DELETE\n\n","DELETE"]){const before=s.rows().length,r=s.run(program,["_cleanup"],{input});assert.equal(r.status,0,r.stderr);assert.equal(r.stdout,banner+"fixture-gcall\n"+close);assert.equal(r.stderr,"");const e=s.rows()[before];assert.equal(e.name,"gcall");assert.deepEqual(e.args,[]);assert.equal(e.env.backend,"tmux");}
});

test("cleanup rejects whitespace, case, Unicode and embedded NUL confirmations",t=>{
  const s=setup(t);for(const program of s.programs)for(const input of ["\n\n","delete\n\n"," DELETE\n\n","DELETE \n\n","DELETE\t\n\n","DELETE\u0000junk\n\n","DELETE\u0000\n\n","ＤＥＬＥＴＥ\n\n","DELETE;touch nope\n\n","DELETE\r\n\r\n","DELETE\r\r"]){const r=s.run(program,["_cleanup"],{input});assert.equal(r.status,0,r.stderr);assert.equal(r.stdout,banner+"Cancelled. Nothing deleted.\n"+close);assert.equal(r.stderr,"");}assert.deepEqual(s.rows(),[]);assert.ok(!existsSync(join(s.root,"nope")));
});

test("malformed UTF-8 fails before confirmation or close",t=>{
  const s=setup(t);for(const program of s.programs){const r=s.run(program,["_cleanup"],{input:Buffer.from([0xff,...Buffer.from("DELETE\n\n")])});assert.equal(r.status,1);assert.equal(r.stdout,banner);if(program===binary)assert.equal(r.stderr,error);else assert.ok(r.stderr.includes("UnicodeDecodeError"));}assert.deepEqual(s.rows(),[]);
});

test("cleanup EOF and partial confirmation never authorize execution",t=>{
  const s=setup(t);for(const program of s.programs)for(const input of ["","DEL","DELETE\u0000"]){const r=s.run(program,["_cleanup"],{input});assert.equal(r.status,0,r.stderr);assert.equal(r.stdout,banner+(input?"Cancelled. Nothing deleted.\n":"")+close);}assert.deepEqual(s.rows(),[]);
});

test("gcall exit/signal status propagates and close still follows",t=>{
  const s=setup(t);for(const program of s.programs){for(const status of [2,17]){const r=s.run(program,["_cleanup"],{input:"DELETE\n\n",env:{...s.env,DW_STATUS:String(status)}});assert.equal(r.status,status,r.stderr);assert.equal(r.stdout,banner+"fixture-gcall\n"+close);}const r=s.run(program,["_cleanup"],{input:"DELETE\n\n",env:{...s.env,DW_SIGNAL:"SIGTERM"}});assert.equal(r.status,241,r.stderr);assert.equal(r.stdout,banner+"fixture-gcall\n"+close);}
});

test("missing internal commands report failure without a close prompt",t=>{
  const s=setup(t);rmSync(join(s.bin,"daily-agenda"));rmSync(join(s.home,".bin/gcall"));for(const program of s.programs)for(const action of ["_agenda","_cleanup"]){const r=s.run(program,[action],{input:"DELETE\n"});assert.equal(r.status,1);assert.equal(r.stderr,error);assert.equal(r.stdout,action==="_cleanup"?banner:"");}
});

test("cleanup stdin I/O errors fail closed rather than authorizing or cancelling",t=>{
  const s=setup(t);for(const program of s.programs){const r=spawnSync(pty,["input-error",program,"_cleanup"],{encoding:"utf8",cwd:s.root,env:s.env});assert.equal(r.status,1);if(program===binary){assert.equal(r.stderr,error);assert.equal(r.stdout,banner);}else{assert.ok(r.stderr.includes("<stdin> is a directory"));assert.equal(r.stdout,"");}}assert.deepEqual(s.rows(),[]);
});

for(const mode of ["key","ctrl-c","interrupt","queued"]){
  test(`agenda PTY ${mode}: raw masks and saved terminal restoration`,t=>{
    const s=setup(t);for(const program of s.programs){const r=spawnSync(pty,[mode,program,"_agenda"],{encoding:"utf8",timeout:9000,env:{...s.env,DW_DELAY:"60"},cwd:s.root});assert.equal(r.status,0,r.stderr+" "+r.stdout);const e=JSON.parse(r.stdout);assert.equal(e.status,0);assert.equal(e.raw,true);assert.equal(e.restored,true);assert.equal(e.queued_cleared,mode==="queued");const text=Buffer.from(e.output_b64,"base64").toString();assert.ok(text.includes("fixture-daily-agenda"));assert.ok(!text.includes("Press Enter"));}
  });
}
for(const mode of ["cleanup-cancel","cleanup-interrupt","cleanup-eof","cleanup-confirm"]){
  test(`cleanup PTY ${mode}: canonical confirmation, cancellation and close`,t=>{
    const s=setup(t);for(const program of s.programs){const before=s.rows().length,r=spawnSync(pty,[mode,program,"_cleanup"],{encoding:"utf8",timeout:9000,env:s.env,cwd:s.root});assert.equal(r.status,0,r.stderr+" "+r.stdout);const e=JSON.parse(r.stdout);assert.equal(e.status,0);assert.equal(e.canonical,true);assert.equal(e.restored,true);assert.equal(e.raw,false);const text=Buffer.from(e.output_b64,"base64").toString();assert.ok(text.includes("Press Enter to close."));assert.equal(s.rows().length-before,mode==="cleanup-confirm"?1:0);}
  });
}

test("interrupt while waiting for an internal child kills/reaps it and skips close",async t=>{
  const s=setup(t);for(const program of s.programs){const before=s.rows().length,child=spawn(program,["_agenda"],{env:{...s.env,DW_DELAY:"10000"},cwd:s.root,detached:true,stdio:["pipe","pipe","pipe"]});let out="";child.stdout!.on("data",b=>out+=b);child.stderr!.on("data",()=>{});const exited=new Promise<any>(r=>child.once("exit",(code,signal)=>r({code,signal})));t.after(()=>{try{process.kill(-child.pid!,"SIGKILL");}catch{}});await until(()=>s.rows().length>before);const pid=s.rows()[before].pid;child.kill("SIGINT");assert.deepEqual(await exited,{code:null,signal:"SIGINT"});assert.ok(!out.includes("Press Enter"));assert.throws(()=>process.kill(pid,0),{code:"ESRCH"});}
});

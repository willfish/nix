import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { chmodSync, existsSync, mkdirSync, mkdtempSync, readFileSync, renameSync, rmSync, symlinkSync, writeFileSync } from "node:fs";
import { join, resolve } from "node:path";
import { tmpdir } from "node:os";
import { test } from "node:test";

const binary = process.env.LAUNCHER_PROJECTS_BIN!, fixture = process.env.LAUNCHER_PROJECTS_FIXTURE!;
assert.ok(binary && fixture, "Set LAUNCHER_PROJECTS_BIN and LAUNCHER_PROJECTS_FIXTURE");
const legacy = process.env.LAUNCHER_PROJECTS_LEGACY;
const python = process.env.LAUNCHER_PROJECTS_PYTHON || process.env.PATH?.split(":").map(p => join(p, "python3")).find(existsSync);
function hash(path: string | Buffer) { return createHash("sha256").update("launcher-project\0").update(path).digest("hex"); }
const fake = `#!${process.execPath}
const fs=require('node:fs');
const proc=fs.readFileSync('/proc/self/stat','utf8').split(') ')[1].split(' ');
const target=fd=>fs.readlinkSync('/proc/self/fd/'+fd);
let inherited=false;try{fs.fstatSync(98);inherited=true;}catch{}
fs.appendFileSync(process.env.LP_LOG,JSON.stringify({args:process.argv.slice(2),cwd:process.cwd(),pid:process.pid,pgrp:Number(proc[2]),sid:Number(proc[3]),stdio:[0,1,2].map(target),inherited,marker:process.env.LP_MARKER})+'\\n');
`;
function setup(t: any) {
  const root=mkdtempSync(join(tmpdir(),"launcher-projects-"));
  t.after(()=>rmSync(root,{recursive:true,force:true}));
  const home=join(root,"home"),outside=join(root,"outside"),bin=join(root,"bin"),log=join(root,"log.jsonl");
  for(const p of [home,outside,bin])mkdirSync(p);
  mkdirSync(join(outside,".git"));writeFileSync(join(outside,"secret-marker"),"private-marker");
  writeFileSync(log,"");writeFileSync(join(bin,"systemd-run"),fake,{mode:0o755});
  const env={...process.env,HOME:home,LAUNCHER_HOME:home,PATH:bin,LP_LOG:log,LP_MARKER:"inherited-marker"};
  const run=(args: string[],options: any={})=>spawnSync(binary,args,{encoding:"utf8",timeout:5000,env,...options});
  const call=(args: string[])=>{const r=spawnSync(fixture,args,{encoding:"utf8",timeout:5000,env});assert.equal(r.status,0,r.stderr);return r.stdout.trim();};
  const repo=(relative:string,marker="dir")=>{const p=join(home,relative);mkdirSync(p,{recursive:true});if(marker==="dir")mkdirSync(join(p,".git"));else if(marker==="file")writeFileSync(join(p,".git"),"gitdir: /outside/private\n");else if(marker==="symlink")symlinkSync(join(outside,".git"),join(p,".git"));return p;};
  const list=(args: string[]=["list"],options: any={})=>{const r=run(args,options);assert.equal(r.status,0,r.stderr);const entries=JSON.parse(r.stdout);if(legacy){assert.ok(python);const old=spawnSync(python!,[legacy,...args],{encoding:"utf8",timeout:5000,env,...options});assert.equal(old.status,0,old.stderr);assert.equal(r.stdout,old.stdout);}return entries;};
  return {root,home,outside,bin,log,env,run,call,repo,list};
}
const actions=["terminal","editor","files"];
async function lines(log: string, count: number) { for(let i=0;i<100;i++){const rows=readFileSync(log,"utf8").trim().split("\n").filter(Boolean).map(x=>JSON.parse(x));if(rows.length>=count)return rows;await new Promise(r=>setTimeout(r,20));}assert.fail("detached command did not log"); }

test("stable catalogue shape, byte order, dotfiles and opaque SHA IDs",t=>{
  const s=setup(t);mkdirSync(join(s.home,".dotfiles"));s.repo("Repositories/zeta");s.repo("Repositories/org/alpha");
  const expected=[".dotfiles","Repositories/org/alpha","Repositories/zeta"],entries=s.list();
  assert.deepEqual(entries.map((e:any)=>e.Subtext),expected);
  for(let i=0;i<entries.length;i++){const e=entries[i];assert.deepEqual(Object.keys(e),["Text","Subtext","Value","Icon","Keywords"]);assert.equal(e.Icon,"folder");assert.equal(e.Value,hash(join(s.home,expected[i])));assert.match(e.Value,/^[a-f0-9]{64}$/);assert.equal(e.Text,expected[i].split("/").at(-1));}
  assert.deepEqual(entries[0].Keywords,[".dotfiles","dotfiles"]);assert.deepEqual(entries[1].Keywords,["Repositories","org","alpha"]);assert.deepEqual(s.list(),entries);
});

test("worktree marker and environment files are neither read nor executed",t=>{
  const s=setup(t),p=s.repo("Repositories/app","file");chmodSync(join(p,".git"),0);writeFileSync(join(p,".envrc"),`printf pwned > ${s.root}/pwned`,{mode:0o755});
  assert.deepEqual(s.list().map((e:any)=>e.Subtext),["Repositories/app"]);assert.equal(readFileSync(s.log,"utf8"),"");assert.ok(!existsSync(join(s.root,"pwned")));
});

test("all hidden/dependency names and symlink escapes are skipped",t=>{
  const s=setup(t);
  for(const name of [".hidden","node_modules","bower_components","vendor","venv","site-packages","__pycache__","third_party","third-party","deps","Pods","Carthage","elm-stuff","result","target","dist","build"])s.repo(`Repositories/${name}/repo`);
  s.repo("Repositories/marker-link","symlink");symlinkSync(s.outside,join(s.home,"Repositories","escape"));symlinkSync(join(s.home,"Repositories"),join(s.home,"Repositories","loop"));
  mkdirSync(join(s.home,"Repositories","group"));symlinkSync(s.outside,join(s.home,"Repositories","group","hop"));s.repo("Repositories/visible");
  assert.deepEqual(s.list().map((e:any)=>e.Subtext),["Repositories/visible"]);
});

test("symlinked catalogue roots are not followed",t=>{const s=setup(t);symlinkSync(s.outside,join(s.home,".dotfiles"));symlinkSync(s.outside,join(s.home,"Repositories"));assert.deepEqual(s.list(),[]);});

test("directory or regular file git markers only; no FIFOs or dangling links",t=>{
  const s=setup(t);s.repo("Repositories/file","file");s.repo("Repositories/directory");s.repo("Repositories/broken","none");symlinkSync(join(s.root,"missing"),join(s.home,"Repositories/broken/.git"));
  const pipe=s.repo("Repositories/pipe","none");const mkfifo=process.env.PATH?.split(":").map(p=>join(p,"mkfifo")).find(existsSync);assert.ok(mkfifo);assert.equal(spawnSync(mkfifo!,[join(pipe,".git")]).status,0);
  assert.deepEqual(s.list().map((e:any)=>e.Text),["directory","file"]);
});

test("depth three and existing repository boundary",t=>{const s=setup(t);s.repo("Repositories/a/b/c");s.repo("Repositories/x/y/z/deep");s.repo("Repositories/parent");s.repo("Repositories/parent/child");assert.deepEqual(s.list().map((e:any)=>e.Subtext),["Repositories/a/b/c","Repositories/parent"]);});

test("200 result cap includes dotfiles and selects sorted candidates",t=>{const s=setup(t);mkdirSync(join(s.home,".dotfiles"));for(let n=200;n>=0;n--)s.repo(`Repositories/p${String(n).padStart(3,"0")}`);const e=s.list();assert.equal(e.length,200);assert.equal(e[0].Subtext,".dotfiles");assert.equal(e.at(-1).Subtext,"Repositories/p198");});

test("result cap stops recursive traversal, including zero-result limits",t=>{const s=setup(t);s.repo("Repositories/a/inner");s.repo("Repositories/b/inner");s.repo("Repositories/c/inner");const result=JSON.parse(s.call(["discover",s.home,"3","1","4000"]));assert.equal(result.scans,2);assert.deepEqual(result.entries.map((e:any)=>e.Subtext),["Repositories/a/inner"]);const empty=JSON.parse(s.call(["discover",s.home,"3","0","4000"]));assert.deepEqual(empty,{entries:[],used:0,scans:0});});

test("global dirent budget counts skipped names/files but excludes dot entries",t=>{
  const s=setup(t);mkdirSync(join(s.home,"Repositories"));for(let i=0;i<12;i++)writeFileSync(join(s.home,"Repositories",`.hidden-${i}`),"");s.repo("Repositories/project");
  const result=JSON.parse(s.call(["discover",s.home,"3","200","3"]));assert.equal(result.used,3);assert.equal(result.scans,1);assert.ok(result.entries.length<=1);
  const zero=JSON.parse(s.call(["discover",s.home,"3","200","0"]));assert.equal(zero.used,0);assert.equal(zero.scans,0);assert.deepEqual(zero.entries,[]);
  const plain=setup(t);plain.repo("Repositories/p00");plain.repo("Repositories/p01");const exact=JSON.parse(plain.call(["discover",plain.home,"3","200","2"]));assert.equal(exact.used,2);assert.equal(exact.entries.length,2);
});

test("directory iteration failures discard partial catalogues and prevent launches",t=>{
  const s=setup(t);s.repo("Repositories/app");const id=s.list()[0].Value,env={...s.env,LP_READDIR_FAIL_AFTER:"3"};
  for(const [args,message] of [[["list"],"unable to list\n"],[["open",id,"files"],"unknown project\n"]] as [string[],string][]){const r=spawnSync(fixture,["cli",...args],{encoding:"utf8",env});assert.equal(r.status,1);assert.equal(r.stdout,"");assert.equal(r.stderr,message);}
  assert.equal(s.run(["list"],{env}).status,0);assert.equal(readFileSync(s.log,"utf8"),"");
});

test("production directory-entry limit bounds large catalogues",t=>{
  const s=setup(t);mkdirSync(join(s.home,"Repositories"));for(let i=0;i<4001;i++)writeFileSync(join(s.home,"Repositories",`.ignored-${i}`),"");s.repo("Repositories/project");const result=JSON.parse(s.call(["discover",s.home,"3","200","4000"]));assert.equal(result.used,4000);assert.equal(result.scans,1);assert.deepEqual(s.list(),result.entries);
});

test("global exhausted budget still emits already-read direct repos, not groups",t=>{const s=setup(t);s.repo("Repositories/a/inner");s.repo("Repositories/b");const result=JSON.parse(s.call(["discover",s.home,"3","200","2"]));assert.equal(result.used,2);assert.equal(result.scans,1);assert.deepEqual(result.entries.map((e:any)=>e.Subtext),["Repositories/b"]);});

test("missing, unreadable and non-directory roots give empty catalogues",t=>{const s=setup(t);assert.deepEqual(s.list(),[]);writeFileSync(join(s.home,"Repositories"),"not a directory");assert.deepEqual(s.list(),[]);rmSync(join(s.home,"Repositories"));s.repo("Repositories/no-access/child");chmodSync(join(s.home,"Repositories/no-access"),0);assert.deepEqual(s.list(),[]);chmodSync(join(s.home,"Repositories/no-access"),0o700);});

test("relative/home overrides use lexical absolute paths without symlink resolution",t=>{
  const s=setup(t);s.repo("Repositories/app");const other=join(s.root,"other");mkdirSync(other);mkdirSync(join(other,".dotfiles"));
  assert.equal(s.list(["--home",other,"list"])[0].Subtext,".dotfiles");assert.equal(s.list(["--home=other","list"],{cwd:s.root})[0].Subtext,".dotfiles");assert.equal(s.list(["--home","other/../home/.","list"],{cwd:s.root})[0].Value,hash(join(s.home,"Repositories/app")));
  assert.equal(s.list(["--home","","list"])[0].Text,"app");assert.equal(s.list(["list"],{env:{...s.env,LAUNCHER_HOME:""}})[0].Text,"app");
  symlinkSync(s.home,join(s.root,"alias"));const alias=s.list(["--home",join(s.root,"alias"),"list"]);assert.equal(alias[0].Value,hash(join(s.root,"alias/Repositories/app")));assert.notEqual(alias[0].Value,s.list()[0].Value);
});

test("two leading slashes and normalized project IDs preserve Path semantics",t=>{
  const s=setup(t);s.repo("Repositories/app");assert.equal(s.list(["--home","/"+s.home,"list"])[0].Value,hash("/"+join(s.home,"Repositories/app")));assert.equal(s.list(["--home","//"+s.home,"list"])[0].Value,hash(join(s.home,"Repositories/app")));
  for(const [raw,normalized] of [["a//./b/","a/b"],["a/../b","a/../b"],["","."],["//a///b/","//a/b"],["///a","/a"]])assert.equal(s.call(["id",raw]),hash(normalized));
  assert.equal(s.call(["absolute","../home/./"]),resolve("../home"));
});

test("Unicode/control text remains JSON data, byte sorting differs from locale",t=>{
  const s=setup(t),names=["Ω","é","Z","a","tab\tline\n\"quote\\:,"];
  for(const n of names)s.repo("Repositories/"+n);const entries=s.list();assert.deepEqual(entries.map((e:any)=>e.Text),[...names].sort((a,b)=>Buffer.compare(Buffer.from(a),Buffer.from(b))));
  for(const e of entries)assert.equal(e.Value,hash(join(s.home,"Repositories",e.Text)));
});

test("invalid filesystem UTF-8 uses question marks while hashing original bytes",t=>{
  const s=setup(t);mkdirSync(join(s.home,"Repositories"));
  const names=[Buffer.from([0x61,0xff]),Buffer.from([0x61,0xfe]),Buffer.from([0x62,0xe2,0x82]),Buffer.from([0x63,0xed,0xa0,0x80]),Buffer.from([0x64,0xf0,0x9f,0x98,0x80])];
  for(const name of names){const p=Buffer.concat([Buffer.from(join(s.home,"Repositories")+"/"),name]);mkdirSync(p);mkdirSync(Buffer.concat([p,Buffer.from("/.git")]));}
  const entries=s.list(),ordered=[...names].sort(Buffer.compare);assert.deepEqual(entries.map((e:any)=>e.Text),["a?","a?","b??","c???","d😀"]);
  for(let i=0;i<ordered.length;i++)assert.equal(entries[i].Value,hash(Buffer.concat([Buffer.from(join(s.home,"Repositories")+"/"),ordered[i]])));
});

test("keyword uniqueness uses raw components, not lossy display strings",t=>{
  const s=setup(t);const parent=Buffer.concat([Buffer.from(join(s.home,"Repositories")+"/"),Buffer.from([0x61,0xff])]),child=Buffer.concat([parent,Buffer.from("/"),Buffer.from([0x61,0xfe])]);mkdirSync(child,{recursive:true});mkdirSync(Buffer.concat([child,Buffer.from("/.git")]));assert.deepEqual(s.list()[0].Keywords,["Repositories","a?","a?"]);
});

test("dotfiles opens without a git marker",async t=>{const s=setup(t);mkdirSync(join(s.home,".dotfiles"));const r=s.run(["open",s.list()[0].Value,"files"]);assert.equal(r.status,0,r.stderr);assert.deepEqual((await lines(s.log,1))[0].args,["--user","--scope","--collect","--quiet","--","xdg-open",join(s.home,".dotfiles")]);});

test("all launch actions detach, use argv/cwd root and silence stdio",async t=>{
  const s=setup(t),name="proj; $(touch pwned) 'quote' \"dq\" --flag",path=s.repo("Repositories/"+name,"file"),id=s.list()[0].Value;
  for(let i=0;i<actions.length;i++){const action=actions[i],r=s.run(["open",id,action]);assert.equal(r.status,0,r.stderr);assert.equal(r.stdout,"");const e=(await lines(s.log,i+1))[i];const expected=action==="files"?["xdg-open",path]:["ghostty","--working-directory="+path,...(action==="editor"?["-e","nvim","."]:[])];assert.deepEqual(e.args,["--user","--scope","--collect","--quiet","--",...expected]);assert.equal(e.cwd,"/");assert.equal(e.sid,e.pid);assert.equal(e.pgrp,e.pid);assert.deepEqual(e.stdio,["/dev/null","/dev/null","/dev/null"]);assert.equal(e.marker,"inherited-marker");assert.equal(e.inherited,false);}
  assert.ok(!existsSync(join(s.root,"pwned")));
});

test("descriptor inheritance is closed even when opened without CLOEXEC",async t=>{const s=setup(t);s.repo("Repositories/app");s.call(["fd-open",s.home,s.list()[0].Value,"terminal"]);assert.equal((await lines(s.log,1))[0].inherited,false);});

test("unknown/invalid IDs, invalid actions and command injection never launch",t=>{
  const s=setup(t);s.repo("Repositories/app");const id=s.list()[0].Value;
  for(const bad of ["a", "a".repeat(63),"g".repeat(64),id.toUpperCase(),"f".repeat(64),s.outside,"../../etc/passwd",id+";touch marker"]){const r=s.run(["open",bad,"files"]);assert.equal(r.status,1);assert.equal(r.stderr,"unknown project\n");assert.equal(r.stdout,"");}
  for(const action of ["", "terminal;touch", "files extra"]){const r=s.run(["open",id,action]);assert.equal(r.status,1);assert.equal(r.stderr,"invalid action\n");}
  assert.equal(readFileSync(s.log,"utf8"),"");
});

test("vanished and replaced projects are rejected by fresh discovery",t=>{
  const s=setup(t),path=s.repo("Repositories/app"),id=s.list()[0].Value;renameSync(path,join(s.outside,"gone"));assert.equal(s.run(["open",id,"files"]).stderr,"unknown project\n");symlinkSync(join(s.outside,"gone"),path);assert.equal(s.run(["open",id,"files"]).stderr,"unknown project\n");rmSync(path);mkdirSync(path);assert.equal(s.run(["open",id,"files"]).stderr,"unknown project\n");assert.equal(readFileSync(s.log,"utf8"),"");
});

test("launchable rechecks each component, marker, depth and root boundary",t=>{
  const s=setup(t),p=s.repo("Repositories/a/b/c");assert.equal(s.call(["launchable",s.home,p]),"true");
  for(const bad of ["Repositories/a",s.home,s.outside,join(s.home,"Repositories"),join(s.home,"Repositories-extra/a"),s.home+"/Repositories/a/../a/b/c"])assert.equal(s.call(["launchable",s.home,bad]),"false");
  s.repo("Repositories/a/b/c/d");assert.equal(s.call(["launchable",s.home,join(p,"d")]),"false");s.repo("Repositories/.hidden");assert.equal(s.call(["launchable",s.home,join(s.home,"Repositories/.hidden")]),"false");
  renameSync(join(s.home,"Repositories/a"),join(s.outside,"a"));symlinkSync(join(s.outside,"a"),join(s.home,"Repositories/a"));assert.equal(s.call(["launchable",s.home,p]),"false");
});

test("spawn failure stays path-free",t=>{const s=setup(t);s.repo("Repositories/app");rmSync(join(s.bin,"systemd-run"));const r=s.run(["open",s.list()[0].Value,"terminal"]);assert.equal(r.status,1);assert.equal(r.stderr,"unable to launch\n");assert.equal(r.stdout,"");});

test("CLI argument/help statuses and override abbreviations",t=>{
  const s=setup(t);s.repo("Repositories/app");
  const cases: [string[],number][]=[ [[],2],[["--help"],0],[["list","--help"],0],[["open","--help"],0],[["unknown","--help"],2],[["list","extra"],2],[["list","--home",s.home],2],[["--home"],2],[["--home","--bad","list"],2],[["--bad","list"],2],[["open","x"],2],[["open","a","b","c"],2],[["--","list"],0],[["list","--"],2],[["open","--","a","b"],1],[["open","-1","terminal"],1],[["open","a","--bad"],2],[["--ho="+s.home,"list"],0],[["--hom",s.home,"list"],0],[["--h","list"],2] ];
  for(const [args,code] of cases){const r=s.run(args);assert.equal(r.status,code,JSON.stringify(args)+" "+r.stderr);if(legacy){const old=spawnSync(python!,[legacy,...args],{encoding:"utf8",env:s.env});assert.equal(r.status,old.status,JSON.stringify(args));}}
});

import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {chmodSync,copyFileSync,cpSync,existsSync,mkdirSync,mkdtempSync,readFileSync,rmSync,statSync,symlinkSync,writeFileSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {dirname,join} from 'node:path';
import test from 'node:test';

const tools=process.env.OMAPAGER_TOOLS_DIR!,icons=process.env.OMAPAGER_ICONS_DIR||tools,fixture=process.env.OMAPAGER_ICON_FIXTURE!,legacy=process.env.OMAPAGER_LEGACY_DIR;
const plugin=process.env.OMAPAGER_PLUGIN_SOURCE!,omarchy=process.env.OMAPAGER_OMARCHY_SOURCE!;
assert.ok(tools&&fixture&&plugin&&omarchy,'Set command/fixture and pinned source paths');
const python=process.env.OMAPAGER_PYTHON||'python3';
function run(exe:string,args:string[],options:any={}){const r=spawnSync(exe,args,{encoding:'utf8',timeout:15000,...options});assert.ifError(r.error);return r;}
function pure(fn:string,args:any[]=[],kwargs:any={},options:any={}){const req=JSON.stringify({fn,args,kwargs,...options.request});const r=run(fixture,[icons,...(options.upstream?[options.upstream]:[])],{input:req,...options});assert.equal(r.status,0,r.stderr);const value=JSON.parse(r.stdout);if(legacy&&!options.upstream&&fn!=='$origin'){const old=run(fixture,[legacy],{input:req,...options});assert.equal(old.status,0,old.stderr);assert.deepEqual(value,JSON.parse(old.stdout),req);}return value;}
function root(fn:(path:string)=>void){const path=mkdtempSync(join(tmpdir(),'omapager-c-'));try{fn(path);}finally{rmSync(path,{recursive:true,force:true});}}
function save(path:string,text:string|Buffer){mkdirSync(dirname(path),{recursive:true});writeFileSync(path,text);}
function command(name:string,path:string,extra:string[]=[],old=false){return old?run(python,[join(legacy!,name==='prepare-shell'?'prepare-shell.py':name+'.py'),path,...extra]):run(join(tools,'omapager-'+name),[path,...extra]);}
const color='shell/Commons/Color.qml',panel='shell/Ui/KeyboardPanel.qml';
function seed(path:string){for(const file of [color,panel])save(join(path,file),readFileSync(join(omarchy,file)));}
const themeAnchor='readonly property string currentThemePath: stateHome + "/omarchy/current/theme"';
const iconTheme='def from_icon_theme(names):\n    """Whatever the machine already has for this name."""\n    for name in names:';
const iconDesktop='if want in haystack.split("-") or ("-" + want + "-") in ("-" + haystack + "-"):';
const serviceText=readFileSync(join(plugin,'Service.qml'),'utf8');

// The retained Python module is an optional oracle outside the repository.
test('installed chat aliases retain raw/slug/alias order and exact deduplication',()=>{
 assert.deepEqual(pure('expand_names',[['Telegram Desktop','GitHub notifications','Whats-App','Discord','Telegram']]),['Telegram Desktop','telegram-desktop','org.telegram.desktop','telegram','GitHub notifications','github-notifications','github','Whats-App','whats-app','whatsapp','Discord','discord','Telegram']);
 assert.deepEqual(pure('expand_names',[[' GitHub ','github','GITHUB',null,false,0,'']]),['GitHub','github','GITHUB']);
});
test('names iterable, falsy values and string conversions match the native interpreter',()=>{
 for(const names of [null,false,0,[],{},'Ab',[1,true,{},['Telegram'],12.5],{Discord:true,GitHub:false}])pure('expand_names',[names]);
 assert.deepEqual(pure('expand_names',[12]),{error:'TypeError'});
});
test('Unicode lower expansion, character slicing, slug punctuation and embedded NUL are data',()=>{
 for(const value of ['İ WhatsApp','ẞ_KELVINK','foo--💥bar','...a..b--','a'.repeat(100)+'..z','🙂'.repeat(256)+'Telegram','İ'.repeat(256)+'X','a\u0000b','\u001cTelegram\u001f',null,false,123,0,{x:'İ'},['Telegram']])pure('_slug',[value]);
 assert.equal(pure('_slug',['foo--💥bar']),'foo---bar');assert.equal(pure('_slug',['a\u0000b']),'a-b');
});
test('deterministic Unicode alias corpus retains whole-result parity',()=>{
 let seed=71;const glyphs=['a','Z','-','.','_',' ','İ','ẞ','K','💥','🙂','\u0000','\u001c','\u00a0','\u2003','é','中','\ud800'];const names=[];
 for(let i=0;i<180;i++){let s='';for(let j=0;j<40;j++){seed=(Math.imul(seed,1664525)+1013904223)>>>0;s+=glyphs[seed%glyphs.length];}names.push(s);}
 pure('expand_names',[names]);
});
test('desktop matching uses dotted/underscore/Unicode whitespace tokens and complete hyphen sequences',()=>{
 for(const [want,hay,expected] of [['Telegram','org.telegram.desktop telegram',true],['Discord','discord',true],['app','whatsapp',false],['hub','github-notifications',false],['foo-bar','prefix foo._bar suffix',true],['foo-bar','foo x bar',false],['foo.bar','foo.bar',false],['foo.bar','foobar',true],['foo_bar','foo_bar',false],['tele-gram','tele\u001cgram',true],['abc','xabc abcxyz',false],['a-b','a b',false],['foo--bar','foo bar',true]] as [string,string,boolean][])assert.equal(pure('name_matches',[want,hay]),expected,JSON.stringify([want,hay]));
});
test('keyword calls and argument failures preserve the original function interface',()=>{
 assert.ok(pure('expand_names',[],{names:['GitHub']}).includes('github'));
 assert.equal(pure('name_matches',[],{want:'Telegram',haystack:'org.telegram.desktop'}),true);
 assert.equal(pure('_slug',[],{text:' WhatsApp '}),'whatsapp');
 for(const [fn,args,kwargs] of [['expand_names',[],{}],['expand_names',[[]],{names:[]}],['name_matches',['foo'],{}],['allowed_icon',[],{}],['_slug',[],{bad:1}]] as any[])assert.deepEqual(pure(fn,args,kwargs),{error:'TypeError'});
});
test('lexical common paths preserve equality, separator boundaries, bytes and ValueError fallback',()=>{
 for(const [path,base,expected] of [['/tmp/icons/a','/tmp/icons',true],['/tmp/icons','/tmp/icons',true],['/tmp/icons2/a','/tmp/icons',false],['relative/a','/absolute',false],['relative/a','relative',true],['//tmp/a','/tmp',true]] as any[])assert.equal(pure('_inside',[path,base]),expected);
 const bytes=(s:string)=>({$bytes:[...Buffer.from(s)]});assert.equal(pure('_inside',[bytes('/a/b'),bytes('/a')]),true);assert.deepEqual(pure('_inside',[bytes('/a/b'),'/a']),{error:'TypeError'});
});
test('regular files, base equality, pathlib objects and multiple bases remain allowed',()=>root(path=>{
 const base=join(path,'icons'),file=join(base,'telegram.svg');save(file,'svg');
 assert.equal(pure('allowed_icon',[file,[base]]),true);assert.equal(pure('allowed_icon',[file,[file]]),true);assert.equal(pure('allowed_icon',[{$path:file},[{$path:base}]]),true);
 assert.equal(pure('allowed_icon',[file,[join(path,'wrong'),base]]),true);assert.equal(pure('allowed_icon',[],{path:file,bases:[base]}),true);
}));
test('internal symlinks are allowed but external targets and prefix siblings are rejected',()=>root(path=>{
 const base=join(path,'icons'),target=join(base,'real.svg'),other=join(path,'outside.svg');save(target,'svg');save(other,'outside');symlinkSync(target,join(base,'inside.svg'));symlinkSync(other,join(base,'escape.svg'));save(join(path,'icons-sibling/a.svg'),'svg');
 assert.equal(pure('allowed_icon',[join(base,'inside.svg'),[base]]),true);assert.equal(pure('allowed_icon',[join(base,'escape.svg'),[base]]),false);assert.equal(pure('allowed_icon',[join(path,'icons-sibling/a.svg'),[base]]),false);
}));
test('directory-link traversal is confined relative to the selected real search root',()=>root(path=>{
 const base=join(path,'icons'),outside=join(path,'outside');mkdirSync(base);save(join(outside,'a.svg'),'svg');symlinkSync(outside,join(base,'escape'));symlinkSync(outside,join(path,'selected-root'));
 assert.equal(pure('allowed_icon',[join(base,'escape/a.svg'),[base]]),false);assert.equal(pure('allowed_icon',[join(path,'selected-root/a.svg'),[join(path,'selected-root')]]),true);
 assert.equal(pure('allowed_icon',[join(path,'selected-root/a.svg'),[join(path,'missing/../selected-root')]]),true);
}));
test('Nix profile links require logical discovery confinement, not just a store target',()=>root(path=>{
 const base=join(path,'icons');mkdirSync(base);const target=process.execPath;assert.ok(target.startsWith('/nix/store/'));symlinkSync(target,join(base,'store.svg'));
 assert.equal(pure('allowed_icon',[join(base,'store.svg'),[base]]),true);assert.equal(pure('allowed_icon',[target,[base]]),false);
}));
test('missing, broken, cyclic, directory, FIFO and NUL paths never become icons',()=>root(path=>{
 const base=join(path,'icons');mkdirSync(base);symlinkSync('missing',join(base,'broken'));symlinkSync('loop',join(base,'loop'));assert.equal(run('mkfifo',[join(base,'fifo')]).status,0);
 for(const file of ['missing','broken','loop','fifo'])assert.equal(pure('allowed_icon',[join(base,file),[base]]),false);
 assert.equal(pure('allowed_icon',[base,[base]]),false);assert.equal(pure('allowed_icon',[base+'\u0000x',[base]]),false);assert.equal(pure('allowed_icon',[null,null]),false);
 assert.deepEqual(pure('allowed_icon',[{},[]]),false);
}));
test('raw filesystem bytes and mixed-type errors preserve path semantics',()=>root(path=>{
 const base=join(path,'icons');mkdirSync(base);const file=Buffer.concat([Buffer.from(base+'/'),Buffer.from([255]),Buffer.from('.svg')]);writeFileSync(file,'svg');const bytes=(b:Buffer)=>({$bytes:[...b]});
 assert.equal(pure('allowed_icon',[bytes(file),[bytes(Buffer.from(base))]]),true);assert.deepEqual(pure('allowed_icon',[bytes(file),[base]]),{error:'TypeError'});
 const outside=join(path,'other');save(outside,'svg');const link=Buffer.from(base+'/escape');symlinkSync(outside,link);assert.deepEqual(pure('allowed_icon',[bytes(link),[bytes(Buffer.from(base))]]),{error:'TypeError'});
}));
test('repeated module operations retain results',()=>{
 assert.deepEqual(pure('expand_names',[['Telegram Desktop','GitHub']],{}, {request:{repeat:1000}}),['Telegram Desktop','telegram-desktop','org.telegram.desktop','telegram','GitHub','github']);
 assert.equal(pure('name_matches',['Telegram','org.telegram.desktop'],{}, {request:{repeat:1000}}),true);
});
test('compiled import origin is the native module rather than handwritten glue',()=>assert.equal(pure('$origin').endsWith('.so'),true));

test('pinned preparation produces byte-identical Color/KeyboardPanel patches',()=>root(path=>{
 const native=join(path,'native'),old=join(path,'legacy');seed(native);assert.equal(command('prepare-shell',native).status,0);
 if(legacy){seed(old);const r=command('prepare-shell',old,[],true);assert.equal(r.status,0,r.stderr);for(const file of [color,panel])assert.deepEqual(readFileSync(join(native,file)),readFileSync(join(old,file)));}
 assert.ok(readFileSync(join(native,color),'utf8').includes('HYPR_CONTROLS_THEME'));assert.ok(readFileSync(join(native,panel),'utf8').includes('Intersection.Subtract'));
}));
test('preparation retains first-only replacement, BOM/NUL and universal newlines',()=>root(path=>{
 for(const prefix of ['\ufeff','\u0000prefix\n']){const native=join(path,'native'),old=join(path,'legacy');for(const target of [native,old]){seed(target);for(const file of [color,panel]){let s=prefix+readFileSync(join(target,file),'utf8');if(file===color)s+='\n'+themeAnchor;s=s.replaceAll('\n','\r\n');save(join(target,file),s);}}
 assert.equal(command('prepare-shell',native).status,0);if(legacy){assert.equal(command('prepare-shell',old,[],true).status,0);for(const file of [color,panel])assert.deepEqual(readFileSync(join(native,file)),readFileSync(join(old,file)));}
 assert.equal(readFileSync(join(native,color),'utf8').split(themeAnchor).length-1,1);
 }
}));
test('missing color anchors abort before write; panel failures retain the successful color write',()=>root(path=>{
 for(const mutation of ['color','panel','absent']){const target=join(path,mutation);seed(target);if(mutation==='color')save(join(target,color),readFileSync(join(target,color),'utf8').replace(themeAnchor,'missing'));if(mutation==='panel')save(join(target,panel),'missing');if(mutation==='absent')rmSync(join(target,panel));const before=readFileSync(join(target,color));const r=command('prepare-shell',target);assert.equal(r.status,1);if(mutation==='color')assert.deepEqual(readFileSync(join(target,color)),before);else assert.ok(readFileSync(join(target,color),'utf8').includes('HYPR_CONTROLS_THEME'));}
}));
test('invalid UTF-8 fails at the original file boundary and preserves partial preparation ordering',()=>root(path=>{
 for(const file of [color,panel]){const target=join(path,file===color?'color':'panel');seed(target);save(join(target,file),Buffer.from([255]));const before=readFileSync(join(target,color));assert.equal(command('prepare-shell',target).status,1);if(file===color)assert.deepEqual(readFileSync(join(target,color)),before);else assert.ok(readFileSync(join(target,color),'utf8').includes('HYPR_CONTROLS_THEME'));}
}));
test('pinned icon patch has byte-identical output and first-only anchors',()=>root(path=>{
 for(const doubled of [false,true]){const raw=readFileSync(join(plugin,'bin/omapager-icon'),'utf8')+(doubled?'\n'+iconTheme+'\n'+iconDesktop:'');const a=join(path,'native'),b=join(path,'legacy');save(a,raw);save(b,raw);const r=command('patch-icon',a);assert.equal(r.status,0,r.stderr);if(legacy){assert.equal(command('patch-icon',b,[],true).status,0);assert.deepEqual(readFileSync(a),readFileSync(b));}assert.equal(readFileSync(a,'utf8').split(iconTheme).length-1,doubled?1:0);}
}));
test('icon patch failures do not publish partial edits; write preserves inode/mode and follows links',()=>root(path=>{
 const target=join(path,'target'),link=join(path,'link');save(target,iconTheme+'\nmissing second anchor');const before=readFileSync(target);assert.equal(command('patch-icon',target).status,1);assert.deepEqual(readFileSync(target),before);
 save(target,iconTheme+'\n'+iconDesktop);symlinkSync(target,link);const ino=statSync(target).ino,mode=statSync(target).mode;assert.equal(command('patch-icon',link).status,0);assert.equal(statSync(target).ino,ino);assert.equal(statSync(target).mode,mode);assert.ok(readFileSync(target,'utf8').includes('expand_names'));
}));
test('pinned Herdr patch has exact helper formatting, call ordering and byte parity',()=>root(path=>{
 const native=join(path,'native'),old=join(path,'legacy'),script='/nix/store/example/bin/herdr-notification-focus';save(native,serviceText);save(old,serviceText);const r=command('patch-herdr-focus',native,[script]);assert.equal(r.status,0,r.stderr);if(legacy){assert.equal(command('patch-herdr-focus',old,[script],true).status,0);assert.deepEqual(readFileSync(native),readFileSync(old));}
 const text=readFileSync(native,'utf8');assert.ok(text.includes('rememberRecent(row)\n    rememberHerdrTarget(row)'));assert.ok(text.includes('if (!handled && row && openHerdrTarget(row))'));assert.ok(text.includes('"'+script+'"'));
}));
test('Herdr missing anchors never partially write and malformed CLI fails',()=>root(path=>{
 for(const anchor of ['  function runExecArgv(argv) {','    rememberRecent(row)\n','        // Source first, link last. A Slack message quoting a link to\n']){const file=join(path,'service');const raw=serviceText.replace(anchor,'gone');save(file,raw);assert.equal(command('patch-herdr-focus',file,['/bin/focus']).status,1);assert.equal(readFileSync(file,'utf8'),raw);}
 assert.equal(run(join(tools,'omapager-patch-herdr-focus'),[]).status,1);assert.equal(run(join(tools,'omapager-patch-icon'),[]).status,1);
}));
test('Herdr patch preserves first-only anchors, UTF-8/newlines and script substitution',()=>root(path=>{
 const script='/a unicode/π"\\x';for(const raw of [serviceText+serviceText,'\ufeff'+serviceText.replaceAll('\n','\r'),serviceText+'\u0000tail']){const a=join(path,'a'),b=join(path,'b');save(a,raw);save(b,raw);assert.equal(command('patch-herdr-focus',a,[script]).status,0);if(legacy){assert.equal(command('patch-herdr-focus',b,[script],true).status,0);assert.deepEqual(readFileSync(a),readFileSync(b));}}
}));
test('packaged upstream worker retains isolated import and local icon resolution',{skip:!process.env.OMAPAGER_RUNTIME_BIN},()=>{
 const bin=process.env.OMAPAGER_RUNTIME_BIN!;
 root(path=>{const svg=join(path,'.local/share/icons/hicolor/scalable/apps/org.telegram.desktop.svg');save(svg,'<svg/>');const worker=join(bin,'omapager-run-helper');const r=run(python,[worker,'icon','--app-icon','Telegram Desktop','--why'],{env:{...process.env,HOME:path,OMAPAGER_REQUIRE_SANDBOX:'0',PYTHONPATH:'/nonexistent'},timeout:60000});assert.equal(r.status,0,r.stderr);assert.equal(r.stdout,svg+'\tfrom_icon_theme:hint');});
});
test('actual patched upstream icon functions import the compiled module and resolve installed aliases',()=>root(path=>{
 const bin=join(path,'bin');cpSync(join(plugin,'bin'),bin,{recursive:true});const icon=join(bin,'omapager-icon');chmodSync(bin,0o700);chmodSync(icon,0o700);save(icon,readFileSync(icon,'utf8').replace('from omapager_files import private_dir, write_bytes, write_json, read_json','from omapager_files import private_dir, write_bytes, write_json, read_json\nimport icon_paths'));assert.equal(command('patch-icon',icon).status,0);
 const file=join(path,'.local/share/icons/hicolor/scalable/apps/org.telegram.desktop.svg');save(file,'<svg/>');const options={upstream:icon,env:{...process.env,HOME:path}};assert.equal(pure('from_icon_theme',[['Telegram Desktop']],{},options),file);
 save(join(path,'.local/share/applications/org.telegram.desktop.desktop'),'[Desktop Entry]\nName=Telegram\nIcon=org.telegram.desktop\n');assert.equal(pure('from_desktop_entries',[['Telegram']],{},options),file);
}));

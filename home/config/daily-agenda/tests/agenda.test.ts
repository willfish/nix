import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {mkdtempSync,mkdirSync,writeFileSync,rmSync,chmodSync,symlinkSync,readFileSync,statSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import test from 'node:test';
const bin=process.env.DAILY_AGENDA_BIN!;
const fixture=process.env.DAILY_AGENDA_FIXTURE!;
const legacy=process.env.DAILY_AGENDA_LEGACY;
assert.ok(bin&&fixture,'Set DAILY_AGENDA_BIN and DAILY_AGENDA_FIXTURE');
const feed='https://calendar.google.com/calendar/ical/user%40example.com/private-fixture_token_0123456789/basic.ics';
function run(command:string,args:string[],input:string|Buffer='',env:Record<string,string>={}){if(command===bin&&process.env.DAILY_AGENDA_PARENT_FIXTURE&&!args[0]?.startsWith('--parse-')){command=process.env.DAILY_AGENDA_PARENT_FIXTURE;args=['cli',...args];}const r=spawnSync(command,args,{input,encoding:'utf8',env:{...process.env,TZ:'UTC',...env},maxBuffer:12_000_000,timeout:70_000});assert.ifError(r.error);return r;}
function ics(events:string[],properties=''){return `BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//Fixtures//EN\r\n${properties}${events.map(e=>`BEGIN:VEVENT\r\n${e.replaceAll('\n','\r\n')}\r\nEND:VEVENT\r\n`).join('')}END:VCALENDAR\r\n`;}
function parity(input:string|Buffer,args=['--parse-feed','2026-10-06','UTC'],expectedStatus=0){const native=run(bin,args,input);assert.equal(native.status,expectedStatus,native.stderr);if(legacy){const old=run(legacy,args,input);assert.equal(native.status,old.status,`Status differs: ${input}`);if(native.status===0)assert.deepEqual(JSON.parse(native.stdout),JSON.parse(old.stdout),`Calendar differs: ${input}`);}return native.status===0?JSON.parse(native.stdout):null;}
function scratch(fn:(root:string)=>void){const root=mkdtempSync(join(tmpdir(),'agenda-fixture-'));try{fn(root);}finally{rmSync(root,{recursive:true,force:true});}}
function env(root:string){return {HOME:root,XDG_CACHE_HOME:join(root,'cache'),XDG_STATE_HOME:join(root,'state'),DAILY_CALENDAR_CREDENTIALS:join(root,'missing')};}
const timed=(extra='',start='20261006T090000Z')=>`UID:event\nDTSTART:${start}\nDTEND:20261006T100000Z\nSUMMARY:Example${extra?'\n'+extra:''}`;

test('one-off, zero duration, all-day, duration and overlap windows',()=>{
 for(const event of [timed(), 'UID:zero\nDTSTART:20261006T090000Z', 'UID:all\nDTSTART;VALUE=DATE:20261006', 'UID:duration\nDTSTART:20261006T090000Z\nDURATION:PT2H', 'UID:overnight\nDTSTART:20261005T230000Z\nDTEND:20261006T010000Z', 'UID:exclusive\nDTSTART:20261005T230000Z\nDTEND:20261006T000000Z','UID:tomorrow\nDTSTART:20261007T000000Z'])parity(ics([event]));
});
test('multiple, cancelled, duplicate UID/start and sequence updates',()=>{
 for(const events of [[timed('STATUS:CANCELLED')],[timed(),timed()],[timed(),timed('SEQUENCE:2\nSUMMARY:Updated')],[timed(),timed('DTSTAMP:20261005T090000Z\nSUMMARY:Updated')],['DTSTART:20261006T090000Z\nSUMMARY:No UID','DTSTART:20261006T090000Z\nSUMMARY:Duplicate']])parity(ics(events));
});
test('recurrence rules cover daily, weekly, monthly, yearly and positions',()=>{
 for(const rule of ['FREQ=DAILY;COUNT=9','FREQ=WEEKLY;BYDAY=TU,TH;COUNT=30','FREQ=MONTHLY;BYMONTHDAY=6;COUNT=40','FREQ=MONTHLY;BYDAY=TU;BYSETPOS=1;COUNT=30','FREQ=YEARLY;BYMONTH=10;BYMONTHDAY=6;COUNT=6','FREQ=DAILY;INTERVAL=2;UNTIL=20261006T090000Z','FREQ=HOURLY;COUNT=12','FREQ=MINUTELY;INTERVAL=20;COUNT=4'])parity(ics([timed(`RRULE:${rule}`,'20251006T090000Z').replace('DTEND:20261006T100000Z','DURATION:PT1H')]));
});
test('RDATE unions, EXDATE/EXRULE exclusions and periods',()=>{
 for(const extra of ['RDATE:20261006T090000Z,20261006T150000Z','RDATE;TZID=America/New_York:20261006T150000','RRULE:FREQ=DAILY;COUNT=3\nEXDATE:20261006T090000Z','RRULE:FREQ=HOURLY;COUNT=6\nEXRULE:FREQ=HOURLY;INTERVAL=2;COUNT=3','RDATE;VALUE=PERIOD:20261006T150000Z/20261006T180000Z','RDATE;VALUE=PERIOD:20261006T150000Z/PT2H'])parity(ics([timed(extra)]));
});
test('mixed RDATE timezones, floating attachment and absolute duplicate starts',()=>{
 const master='UID:mixed\nDTSTART;TZID=America/New_York:20261006T090000\nDURATION:PT1H';
 for(const extra of ['RDATE:20261006T150000','RDATE:20261006T130000Z','RDATE;TZID=America/New_York;VALUE=PERIOD:20261006T150000/20261006T160000'])parity(ics([master+'\n'+extra]));
 assert.equal(parity(ics([master+'\nRDATE:20261006T130000Z'])).length,1);
 parity(ics([master.replace('DTSTART;TZID=America/New_York:','DTSTART:')+'\nRDATE;TZID=America/New_York:20261006T150000']));
 parity(ics([master+'\nRRULE:FREQ=DAILY;COUNT=4\nRDATE:20261008T150000Z']),['--parse-range','2026-10-06','2026-10-10','UTC']);
});
test('detached recurrence moves, cancellation and THISANDFUTURE',()=>{
 const master=timed('RRULE:FREQ=DAILY;COUNT=8','20261004T090000Z').replace('DTEND:20261006T100000Z','DURATION:PT1H');
 for(const override of ['UID:event\nRECURRENCE-ID:20261006T090000Z\nDTSTART:20261006T150000Z\nDTEND:20261006T170000Z\nSUMMARY:Moved','UID:event\nRECURRENCE-ID:20261006T090000Z\nDTSTART:20261006T090000Z\nSTATUS:CANCELLED','UID:event\nRECURRENCE-ID;RANGE=THISANDFUTURE:20261005T090000Z\nDTSTART:20261005T130000Z\nDURATION:PT2H\nSUMMARY:Shifted'])parity(ics([master,override]));
 parity(ics([master,'UID:event\nRECURRENCE-ID:20261010T090000Z\nDTSTART:20261006T160000Z\nDURATION:PT1H\nSUMMARY:Moved from future']));
});
test('recurrence aliases, excluded moves, collision precedence and range ordering',()=>{
 const master='UID:a\nDTSTART:20261004T090000Z\nRRULE:FREQ=DAILY;COUNT=8\nSUMMARY:Original';
 const moved='UID:a\nRECURRENCE-ID:20261006T090000Z\nDTSTART:20261007T090000Z\nSUMMARY:Moved';
 assert.equal(parity(ics([master,moved]),['--parse-feed','2026-10-07','UTC'])[0].summary,'Original');
 parity(ics([master,moved.replace('DTSTART:20261007T090000Z','DTSTART:20261006T150000Z')]),['--parse-range','2026-10-04','2026-10-09','UTC']);
 assert.deepEqual(parity(ics([master+'\nEXDATE:20261006T090000Z',moved]),['--parse-feed','2026-10-07','UTC']).map((e:any)=>e.summary),['Original']);
 const named=master.replace('DTSTART:20261004T090000Z','DTSTART;TZID=America/New_York:20261004T090000');
 const alternate='UID:a\nRECURRENCE-ID:20261006T130000Z\nDTSTART:20261006T150000Z\nSUMMARY:Moved';
 assert.equal(parity(ics([named,alternate])).length,1);
 for(const day of ['2026-10-06','2026-10-07'])parity(ics([named,alternate.replace('RECURRENCE-ID:','RECURRENCE-ID;RANGE=THISANDFUTURE:')]),['--parse-feed',day,'UTC']);
});
test('floating, IANA, attachment zones, UTC and daylight transitions',()=>{
 for(const [date,tz,props,event] of [
 ['2026-10-06','Europe/London','',timed().replaceAll('Z','')],
 ['2026-10-06','UTC','X-WR-TIMEZONE:America/New_York\r\n',timed().replaceAll('Z','')],
 ['2026-10-06','UTC','',timed().replaceAll('DTSTART:','DTSTART;TZID=America/New_York:').replaceAll('DTEND:','DTEND;TZID=America/New_York:').replaceAll('Z','')],
 ['2026-11-01','America/New_York','',`UID:fold\nDTSTART:20261101T013000\nDURATION:PT1H`],
 ['2026-03-08','America/New_York','',`UID:gap\nDTSTART:20260308T023000\nDURATION:PT1H`],
 ['2026-10-25','Europe/London','',`UID:dst\nDTSTART;TZID=Europe/London:20261024T093000\nDURATION:PT1H\nRRULE:FREQ=DAILY;COUNT=3`],
 ])parity(ics([event],props),['--parse-feed',date,tz]);
});
test('VTIMEZONE custom rules remain attached to the event',()=>{
 const tz='BEGIN:VTIMEZONE\r\nTZID:Fixture/Custom\r\nBEGIN:STANDARD\r\nDTSTART:19700101T000000\r\nTZOFFSETFROM:+0230\r\nTZOFFSETTO:+0230\r\nTZNAME:FIX\r\nEND:STANDARD\r\nEND:VTIMEZONE\r\n';
 parity(ics(['UID:custom\nDTSTART;TZID=Fixture/Custom:20261006T090000\nDURATION:PT1H'],tz));
});
test('Unicode, folded/escaped text, safe links and range metadata',()=>{
 const event=timed('LOCATION:Room\\, 2\nDESCRIPTION:Line 1\\nLine 2\nURL:https://calendar.google.com/event?eid=a').replace('SUMMARY:Example','SUMMARY:🌞 Résumé\\, planner\\; \\nnext\n line');
 parity(ics([event]));parity(ics([event]),['--parse-range','2026-10-01','2026-10-10','UTC']);
 for(const link of ['https://calendar.google.com/event?eid=x','https://calendar.google.com/calendar/ical/a/private-secret/basic.ics','http://calendar.google.com/event','https://example.com/'])parity(ics([timed('URL:'+link)]));
});
test('malformed components, windows and untrusted input are rejected',()=>{
 for(const event of ['SUMMARY:Missing start','DTSTART:garbage','DTSTART:20260230T090000Z','DTSTART:20261006T090000Z\nRRULE:COUNT=3','DTSTART:20261006T090000Z\nRRULE:FREQ=BOGUS','DTSTART:20261006T090000Z\nEXDATE:garbage','DTSTART;VALUE=DATE:20261006\nDTEND;VALUE=DATE:20261006'])parity(ics([event]),undefined,1);
 for(const raw of ['garbage','BEGIN:VCALENDAR\nVERSION:2.0\nBEGIN:VEVENT\nDTSTART:20261006T090000Z','BEGIN:VEVENT\nDTSTART:20261006T090000Z\nEND:VEVENT'])parity(raw,undefined,1);
 for(const args of [['--parse-feed','2026-10-06','Bogus/Zone'],['--parse-feed','2026-10-06','local'],['--parse-feed','2026-02-30','UTC'],['--parse-range','2026-10-06','2026-10-06','UTC'],['--parse-range','2026-01-01','2026-04-02','UTC']])parity(ics([timed()]),args,1);
 parity(Buffer.alloc(2_000_001,65),undefined,1);
});
test('occurrence bounds and bar range bounds are independent',()=>{
 const event='UID:many\nDTSTART:20261006T000000Z\nRRULE:FREQ=MINUTELY;COUNT=501';parity(ics([event]),undefined,1);parity(ics([event]),['--parse-range','2026-10-06','2026-10-07','UTC']);
});
test('credential grammar, naming, permissions, symlink targets and BOMs',()=>{scratch(root=>{
 const path=join(root,'secret');writeFileSync(path,'\ufeff '+feed+'\n',{mode:0o600});assert.equal(run(fixture,['feeds',path]).status,0);symlinkSync(path,join(root,'alias'));assert.equal(run(fixture,['feeds',join(root,'alias')]).status,0);chmodSync(path,0o644);assert.equal(run(fixture,['feeds',path]).status,1);chmodSync(path,0o600);
 for(const content of [{Work:feed,Family:feed},{'':feed},{['a'.repeat(41)]:feed},{'bad\u001b':feed},{Work:feed.replace('https:','http:')},{Work:feed, ...Object.fromEntries(Array.from({length:8},(_,i)=>['C'+i,feed]))}]){writeFileSync(path,JSON.stringify(content));assert.equal(run(fixture,['feeds',path]).status,Object.keys(content).length===2?0:1);}
 for(const url of [feed,feed.replace('user%40example.com','a%23b')])assert.equal(run(fixture,['url',url]).status,0);
 for(const url of [feed+'?x',feed+'#x',feed.replace('calendar.google.com','calendar.google.com.evil'),feed.replace('user%40example.com','a%2fb'),feed.replace('user%40example.com','a%2540b'),feed.replace('user%40example.com','..'),feed.replace('calendar.google.com','calendar.google.com:443')])assert.equal(run(fixture,['url',url]).status,1);
});});
test('credential files strip every Python whitespace character before feed validation',()=>{scratch(root=>{
 const path=join(root,'secret');
 for(const cp of [9,10,11,12,13,28,29,30,31,32,0x85,0xa0,0x1680,...Array.from({length:11},(_,i)=>0x2000+i),0x2028,0x2029,0x202f,0x205f,0x3000]) {
  const space=String.fromCodePoint(cp);writeFileSync(path,space+feed+space,{mode:0o600});
  const result=run(fixture,['feeds',path]);assert.equal(result.status,0,result.stderr);
  const e={...env(root),DAILY_CALENDAR_CREDENTIALS:path},native=run(bin,['--status'],'',e);
  assert.equal(native.status,0,native.stderr);assert.match(native.stdout,/Calendar feed: present/);
  if(legacy)assert.equal(native.stdout,run(legacy,['--status'],'',e).stdout);
 }
});});
test('notes sections, unchecked boxes, fences, deduplication and Unicode lines',()=>{scratch(root=>{
 const path=join(root,'notes');writeFileSync(path,'# Reminders\r\n- First\n- [ ] Work\n- [x] Done\n```\n- hidden\n```\n# Other\n- Not a reminder\n- [ ] Everywhere\n# Todos\n+ First\u2028* Café\n- \u001b[31mDanger\u0000here\n');
 const r=run(fixture,['notes',path]);assert.equal(r.status,0,r.stderr);assert.deepEqual(JSON.parse(r.stdout).rows,['First','Work','Everywhere','Café','[31mDangerhere']);
 writeFileSync(path,Buffer.from([0xff]));assert.equal(run(fixture,['notes',path]).status,1);
 writeFileSync(path,'# Reminders\n- '+'😃'.repeat(550_000));assert.equal(run(fixture,['notes',path]).status,0);
});});
test('cache-only display and status match legacy without external reads',()=>{scratch(root=>{
 const e=env(root),today=new Date().toISOString().slice(0,10);mkdirSync(join(root,'Notes',today),{recursive:true});writeFileSync(join(root,'Notes',today,'today.md'),'# Reminders\n- Brief task\n- [ ] A longer half-hyphenated reminder '.repeat(4));
 mkdirSync(join(root,'cache','daily-agenda'),{recursive:true});const path=join(root,'cache','daily-agenda','calendar.json');
 for(const value of [null,{day:'2000-01-01'},{day:today,fetched_at:new Date().toISOString(),events:[]},{day:today,fetched_at:today+'T10:00:00+00:00',events:[{summary:'Demo\u001b[31m',start:{date:today},end:{date:new Date(Date.now()+86400000).toISOString().slice(0,10)},calendar:'Work'}]},{day:today,fetched_at:new Date().toISOString(),events:'bad'}]){
 if(value===null)rmSync(path,{force:true});else writeFileSync(path,JSON.stringify(value));
 for(const args of [[],['--status'],['--refresh','--status']]){const n=run(bin,args,'',e);assert.equal(n.status,0);if(legacy){const old=run(legacy,args,'',e);assert.equal(n.stdout,old.stdout);assert.equal(n.status,old.status);}}
 }
});});
test('safe private state publication rejects leaf/parent links',()=>{scratch(root=>{
 const file=join(root,'state','calendar.json');assert.equal(run(fixture,['save',file],'{"version":1}').status,0);assert.equal(statSync(join(root,'state')).mode&0o777,0o700);assert.equal(statSync(file).mode&0o777,0o600);assert.equal(readFileSync(file,'utf8'),'{"version": 1}');
 rmSync(file);symlinkSync(join(root,'target'),file);assert.equal(run(fixture,['save',file],'{}').status,1);rmSync(file);rmSync(join(root,'state'),{recursive:true});mkdirSync(join(root,'target'));symlinkSync(join(root,'target'),join(root,'state'));assert.equal(run(fixture,['save',file],'{}').status,1);
});});
test('bar export identifiers, colors, spanning days, meeting hosts and redaction',()=>{
 const record={uid:'event',summary:'Title fixture_token_0123456789',start:{dateTime:'2026-10-06T22:00:00+00:00'},end:{dateTime:'2026-10-08T00:00:00+00:00'},location:'Room https://meet.google.com/abc-defg-hij).',description:feed,url:'https://calendar.google.com/event?eid=x'};
 const r=run(fixture,['bar','Work','UTC',feed],JSON.stringify([record]));assert.equal(r.status,0,r.stderr);const rows=JSON.parse(r.stdout);assert.equal(rows.length,2);assert.deepEqual(rows.map((r:any)=>r.dateKey),['2026-10-06','2026-10-07']);assert.equal(rows[0].title,'Title [redacted]');assert.equal(rows[0].meetingUrl,'https://meet.google.com/abc-defg-hij');assert.ok(!r.stdout.includes('fixture_token_'));assert.match(rows[0].id,/^[a-f0-9]{20}$/);
});
test('sandboxed child parsing preserves limits and narrowed environment',()=>{
 const r=run(fixture,['expand',process.env.DAILY_AGENDA_CHILD||bin,'2026-10-06','2026-10-07','UTC'],ics([timed()]));assert.equal(r.status,0,r.stderr);assert.equal(JSON.parse(r.stdout)[0].summary,'Example');
 const limits=process.env.DAILY_AGENDA_LIMITS!;assert.ok(limits,'Set DAILY_AGENDA_LIMITS to the normal manual probe');const p=run(fixture,['expand',limits,'2026-10-06','2026-10-07','UTC'],'',{AGENDA_PARENT_SECRET:'not inherited',LD_PRELOAD:''});assert.equal(p.status,0,p.stderr);assert.deepEqual(JSON.parse(p.stdout),[{cpu:30,as:1073741824,file:2000000,core:0,secret:false,preload:false,lang:'C.UTF-8',tz:'UTC'}]);
});
test('ISO week/basic dates and rejected noncanonical date syntax',()=>{
 for(const date of ['20261006','2026-W41-2','2026W412'])parity(ics([timed()]),['--parse-feed',date,'UTC']);
 for(const date of ['2026-1-6','2026-10-6','2026--W41-2','2026-W54-2'])parity(ics([timed()]),['--parse-feed',date,'UTC'],1);
});
test('NUL/invalid UTF-8 text and category-C characters remain untrusted data',()=>{
 parity(ics([timed().replace('Example','A\u0000B\u001b[31m\u200d\ue000')]));
 const raw=Buffer.from(ics([timed().replace('Example','PLACEHOLDER')]));raw[raw.indexOf('PLACEHOLDER')]=0xff;parity(raw);
});
test('long durations and mixed end timezones preserve construction',()=>{
 parity(ics(['UID:long\nDTSTART:19000101T090000Z\nDTEND:22000101T100000Z']));
 parity(ics(['UID:mixed\nDTSTART;TZID=America/New_York:20261006T090000\nDTEND:20261006T140000Z']));
});
test('override sequences, active overrides of cancellation and future shifts',()=>{
 const master=timed('RRULE:FREQ=DAILY;COUNT=8\nSEQUENCE:3','20261004T090000Z').replace('DTEND:20261006T100000Z','DURATION:PT1H');
 parity(ics([master,'UID:event\nRECURRENCE-ID:20261006T090000Z\nDTSTART:20261006T150000Z\nSEQUENCE:2\nSUMMARY:Obsolete']));
 parity(ics([master+'\nSTATUS:CANCELLED','UID:event\nRECURRENCE-ID;RANGE=THISANDFUTURE:20261005T090000Z\nDTSTART:20261005T130000Z\nSEQUENCE:4\nSUMMARY:Active']));
 parity(ics(['UID:future\nDTSTART:21261005T090000Z\nRRULE:FREQ=DAILY;COUNT=3','UID:future\nRECURRENCE-ID;RANGE=THISANDFUTURE:21261005T090000Z\nDTSTART:20261005T090000Z\nSUMMARY:Earlier']));
});
test('cache schema, embedded NULs and duplicate key semantics match legacy',()=>{scratch(root=>{
 const e=env(root),today=new Date().toISOString().slice(0,10);mkdirSync(join(root,'cache','daily-agenda'),{recursive:true});const path=join(root,'cache','daily-agenda','calendar.json');
 const events=[{summary:'A\u0000B',calendar:'W\u0000ork',start:{date:today}},{summary:'x',start:{dateTime:today+'T09:00:00Z'},end:{dateTime:today+'T08:00:00Z'}},{summary:'x',start:{date:today},end:{date:today}}, {summary:'x',start:{dateTime:today+'T09:00:00'}},{summary:5,start:{date:today}}];
 for(const event of events){writeFileSync(path,JSON.stringify({day:today,fetched_at:new Date().toISOString(),events:[event]}));const n=run(bin,[],'',e);if(legacy){const old=run(legacy,[],'',e);assert.equal(n.stdout,old.stdout);}}
 writeFileSync(path,`{"day":"${today}","fetched_at":"${new Date().toISOString()}","events":[{"summary":"Key","start\\u0000evil":{"date":"${today}"}}]}`);const n=run(bin,[],'',e);assert.ok(n.stdout.includes('Calendar cache unavailable'));if(legacy)assert.equal(n.stdout,run(legacy,[],'',e).stdout);
});});
test('unrecognised TZ falls back to the system timezone',()=>{scratch(root=>{const e={...env(root),TZ:'Not/AZone'};const n=run(bin,['--status'],'',e);assert.equal(n.status,0);if(legacy)assert.equal(n.stdout,run(legacy,['--status'],'',e).stdout);});});
test('pretty terminal output matches legacy without live calendar access',()=>{scratch(root=>{const pty=process.env.DAILY_AGENDA_PTY!;assert.ok(pty,'Set DAILY_AGENDA_PTY');const e=env(root),today=new Date().toISOString().slice(0,10);mkdirSync(join(root,'Notes',today),{recursive:true});writeFileSync(join(root,'Notes',today,'today.md'),'# Reminders\n- Pretty résumé with a hyphenated-word\n');const n=run(pty,[bin],'',e);assert.equal(n.status,0,n.stderr);assert.ok(n.stdout.includes('\u001b[1;36mTODAY'));if(legacy)assert.equal(n.stdout,run(pty,[legacy],'',e).stdout);});});

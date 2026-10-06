import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {runInNewContext} from 'node:vm';
import test from 'node:test';
const fill=readFileSync(new URL('../src/fill.js',import.meta.url),'utf8');
for(const mode of ['native','wrapped','missing-field','missing-button','wrong-origin']) {
  test(`credential fill: ${mode}`,()=>{
    const events=[];
    class Input {
      set value(value){assert(this instanceof Input);this.currentValue=value;}
      matches(selector){return selector==='input';}
      dispatchEvent(event){events.push(event.type);}
    }
    class Event {constructor(type){this.type=type;}}
    const input=new Input(),button={matches:()=>true,click:()=>events.push('submit')};
    const wrapper=child=>({matches:()=>false,querySelector:()=>child});
    const fields={field:mode==='native'?input:wrapper(input),button:mode==='native'?button:wrapper(button)};
    if(mode==='missing-field')fields.field=wrapper(null);
    if(mode==='missing-button')fields.button=null;
    const result=runInNewContext(`${fill}('field','button','synthetic-input')`,{
      document:{querySelector:selector=>fields[selector]},
      location:{origin:mode==='wrong-origin'?'https://unrelated.invalid':'https://eu-west-2.signin.aws'},
      HTMLInputElement:Input,InputEvent:Event,Event,
    });
    const expected=mode==='native'||mode==='wrapped';
    assert.equal(result,expected);
    assert.deepEqual(events,expected?['input','change','submit']:[]);
    assert.equal(input.currentValue,expected?'synthetic-input':undefined);
  });
}

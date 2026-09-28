"""Run the embedded phone page logic against a minimal DOM and mocked fetch."""
import pathlib
import shutil
import subprocess
import unittest


class OrbitWifiPageTests(unittest.TestCase):
    def test_actual_phone_flow(self):
        node = shutil.which('node')
        if not node:
            self.skipTest('Node is needed for phone page tests')
        root = pathlib.Path(__file__).resolve().parents[2]
        page = (root / 'components/esp-wifi-connect/orbit_wifi_portal.cc').read_text()
        script = page.split('<script>', 1)[1].split('</script>', 1)[0]
        harness = r'''
const vm=require('node:vm'),assert=require('node:assert/strict');
const nodes={};let storage={},writes=[],requests=[],reply={success:true},fail=false;
class Element {
 constructor(){this.value='';this.hidden=false;this.disabled=false;this.type='password';this.options=[];this.selectedIndex=0;}
 replaceChildren(...o){this.options=o;this.selectedIndex=0;} add(o){this.options.push(o);}
 focus(){this.focused=true;}setAttribute(k,v){this[k]=v;}
}
const context={document:{getElementById:id=>nodes[id]??=new Element()},Option:function(text,value){this.text=text;this.value=value;},
 sessionStorage:{getItem:k=>storage[k],setItem:(k,v)=>{storage[k]=v;writes.push([k,v]);},removeItem:k=>delete storage[k]},
 fetch:async(url,options)=>{requests.push([url,options]);if(fail)throw Error();return {ok:true,json:async()=>url==='/scan'?{aps:[{ssid:'Kitchen'},{ssid:'Kitchen'},{ssid:'Guest'}]}:reply};}};
vm.createContext(context);vm.runInContext(SOURCE,context);
const tick=()=>new Promise(r=>setImmediate(r));
(async()=>{
 await tick();assert.equal(nodes.net.options.length,4); // duplicate access points collapsed
 nodes.net.selectedIndex=1;nodes.net.value='Kitchen';nodes.net.onchange();
 assert.equal(nodes.ssid.value,'Kitchen');assert.equal(storage['orbit-network'],'Kitchen');
 nodes.pass.value='private-test-password';nodes.show.onclick();assert.equal(nodes.pass.type,'text');nodes.show.onclick();assert.equal(nodes.pass.type,'password');
 reply={success:false,error:'Wrong password'};await nodes.f.onsubmit({preventDefault(){}});
 assert.equal(nodes.status.textContent,'Wrong password');assert.equal(nodes.pass.value,'private-test-password');assert.equal(nodes.f.hidden,false);assert.equal(nodes.save.disabled,false);
 fail=true;await nodes.cancel.onclick();assert.equal(nodes.f.hidden,false);assert.match(nodes.status.textContent,/Press blue/);
 await nodes.f.onsubmit({preventDefault(){}});assert.match(nodes.status.textContent,/check whether it is connected/);assert.equal(nodes.f.hidden,false);
 fail=false;reply={success:true};await nodes.f.onsubmit({preventDefault(){}});
 assert.equal(nodes.f.hidden,true);assert.equal(nodes.pass.value,'');assert.equal(storage['orbit-network'],undefined);assert.match(nodes.status.textContent,/reconnecting/);
 assert(writes.every(([k,v])=>k==='orbit-network'&&v!=='private-test-password'));
 const count=requests.length;await nodes.f.onsubmit({preventDefault(){}});assert.equal(requests.length,count);
})().catch(e=>{console.error(e);process.exitCode=1;});
'''
        import json
        subprocess.run([node, '-e', 'const SOURCE=' + json.dumps(script) + ';\n' + harness], check=True)

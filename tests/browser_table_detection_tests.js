// Execute the exact script used by both native WebViews with document fixtures.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const source = fs.readFileSync(path.join(__dirname, '../src/downloads/BrowserPolicy.cpp'), 'utf8');
const match = source.match(/R"bmstable\(([\s\S]*?)\)bmstable"/);
assert(match, 'The shared browser table detector must be available');
function detect({url = 'https://tables.example/insane/index.html', base = url, metas = [], body = ''} = {}) {
    return JSON.parse(JSON.stringify(vm.runInNewContext(match[1], {
        URL, location: {href: url},
        document: {baseURI: base, body: {textContent: body},
            querySelectorAll: () => metas.map(attributes => ({getAttribute: name => attributes[name] ?? null}))}
    })));
}
assert.deepEqual(detect({metas: [{name: 'bmstable', content: 'header.json'}]}), {
    pageUrl: 'https://tables.example/insane/index.html', tableUrl: 'https://tables.example/insane/header.json'
});
assert.equal(detect({base: 'https://cdn.example/tables/', metas: [{name: 'BmsTable', content: '../header.json?x=1&y=2'}]}).tableUrl,
    'https://cdn.example/header.json?x=1&y=2');
assert.equal(detect({metas: [{name: 'bmstable', content: '//cdn.example/header.json'}]}).tableUrl,
    'https://cdn.example/header.json');
assert.equal(detect({url: 'http://tables.example/header', body: JSON.stringify({name: 'Event', symbol: '', data_url: './data.json'})}).tableUrl,
    'http://tables.example/header');
for (const body of ['', 'ordinary page', '{}', '[]', '{bad', '{"name":"Table","symbol":"x"}',
    '{"name":"Table","symbol":"x","data_url":""}', '{"name":"Table","symbol":"x","data_url":"file:///data"}']) {
    assert.equal(detect({url: 'https://tables.example/header.json', body}).tableUrl, '', body);
}
for (const content of ['', 'javascript:alert(1)', 'file:///tmp/header.json', 'https://user:secret@tables.example/header.json']) {
    assert.equal(detect({metas: [{name: 'bmstable', content}]}).tableUrl, '', content);
}
assert.equal(detect({metas: [{name: 'description', content: 'header.json'}]}).tableUrl, '');
assert.equal(detect({url: 'https://tables.example/next-page'}).tableUrl, '');
console.log('Browser table detection passed');

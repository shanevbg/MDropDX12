const net = require('net');
const preset = 'c:/Code/Entertainment/MDropDX12/log/TestMW/resources/presets/Milkdrop2077/presets3/MilkDrop2077.R260.milk2';

function pipeCmd(pipePath, label) {
  return new Promise((resolve) => {
    const buf = [];
    const c = net.connect(pipePath, () => {
      console.log(`[${label}] Connected, loading preset...`);
      c.write('LOAD_PRESET=' + preset + '\n');
    });
    c.on('data', d => {
      const s = d.toString().trim();
      if (s) { buf.push(s); console.log(`[${label}] <<`, s.substring(0, 200)); }
    });
    c.on('error', e => { console.log(`[${label}] ERR:`, e.message); resolve(null); });
    c.on('end', () => { console.log(`[${label}] Disconnected`); resolve(buf.join('\n')); });
    // After 5s, request state + capture, then close
    setTimeout(() => {
      console.log(`[${label}] Requesting STATE...`);
      c.write('STATE\n');
    }, 5000);
    setTimeout(() => {
      console.log(`[${label}] Requesting CAPTURE...`);
      c.write('CAPTURE\n');
    }, 7000);
    setTimeout(() => c.end(), 9000);
  });
}

async function main() {
  const [mdrop, md3] = await Promise.all([
    pipeCmd('//./pipe/Milkwave_69512', 'MDropDX12'),
    pipeCmd('//./pipe/Milkwave_15264', 'MilkDrop3'),
  ]);
  console.log('\n=== Done ===');
}
main();

const net = require('net');
const pipePath = '//./pipe/Milkwave_45020';
const c = net.connect(pipePath, () => {
  console.log('Connected');
  c.write('LOAD_PRESET=c:/Code/Entertainment/MDropDX12/log/TestMW/resources/presets/Milkdrop2077/presets3/MilkDrop2077.R260.milk2\n');
  setTimeout(() => {
    c.write('CAPTURE\n');
    setTimeout(() => c.end(), 2000);
  }, 3000);
});
c.on('data', d => console.log(d.toString()));
c.on('error', e => console.log('ERR:', e.message));

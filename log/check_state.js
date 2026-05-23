const net = require('net');
const pipePath = '//./pipe/Milkwave_69512';
const c = net.connect(pipePath, () => {
  c.write('STATE\n');
  setTimeout(() => c.end(), 2000);
});
c.on('data', d => console.log(d.toString().trim()));
c.on('error', e => console.log('ERR:', e.message));

// Private test Chrome profiles only. No connection to an existing user browser.
import readline from 'node:readline';
const socket = new WebSocket(process.argv[2]);
await new Promise((resolve,reject)=>{socket.addEventListener('open',resolve,{once:true});socket.addEventListener('error',reject,{once:true});});
socket.addEventListener('message',event=>{const message=JSON.parse(event.data);if(message.id)process.stdout.write(JSON.stringify(message)+'\n');});
socket.addEventListener('error',()=>process.exit(2));
const lines=readline.createInterface({input:process.stdin});
lines.on('line',line=>socket.send(line));
lines.on('close',()=>socket.close());

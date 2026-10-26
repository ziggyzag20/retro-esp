const fs = require('fs');
const path = require('path');
const out = process.argv[2];
fs.mkdirSync(out, {recursive:true});
function writeBmp(file) {
  const w=32,h=32,rowBytes=(w*3+3)&~3,dataBytes=rowBytes*h;
  const b=Buffer.alloc(54+dataBytes,0);
  b.write('BM',0,'ascii'); b.writeUInt32LE(b.length,2); b.writeUInt32LE(54,10);
  b.writeUInt32LE(40,14); b.writeInt32LE(w,18); b.writeInt32LE(h,22);
  b.writeUInt16LE(1,26); b.writeUInt16LE(24,28); b.writeUInt32LE(dataBytes,34);
  b.writeInt32LE(2835,38); b.writeInt32LE(2835,42);
  const bg=[8,13,24];
  const set=(x,y,c)=>{ if(x<0||x>=w||y<0||y>=h)return; const off=54+(h-1-y)*rowBytes+x*3; b[off]=c[2];b[off+1]=c[1];b[off+2]=c[0]; };
  for(let y=0;y<h;y++) for(let x=0;x<w;x++) set(x,y,bg);
  for(let y=2;y<=27;y++) { const half=y<18?2+Math.floor((y-2)*.48):Math.max(4,10-Math.floor((y-18)*.52)); for(let x=16-half;x<=16+half;x++) set(x,y,[34,190,220]); }
  for(let y=12;y<=22;y++) for(let x=2;x<30;x++) if(Math.abs(x-16)<=(y<16?14:Math.max(8,14-(y-16)))) set(x,y,[36,118,156]);
  for(let y=8;y<=14;y++) for(let x=13;x<=18;x++) set(x,y,y<10?[245,247,255]:[243,86,184]);
  for(let y=20;y<=24;y++) for(let x=7;x<=9;x++) set(x,y,[185,246,106]);
  for(let y=20;y<=24;y++) for(let x=23;x<=25;x++) set(x,y,[185,246,106]);
  for(let y=27;y<=30;y++) for(let x=14;x<=17;x++) set(x,y,y<30?[255,200,87]:[243,86,184]);
  for(let x=4;x<=5;x++) { set(x,15,[255,200,87]); set(31-x,15,[255,200,87]); }
  fs.writeFileSync(file,b);
}
function writeWav(file, samples) {
  const rate=44100, dataBytes=samples.length*2, b=Buffer.alloc(44+dataBytes);
  b.write('RIFF',0,'ascii'); b.writeUInt32LE(36+dataBytes,4); b.write('WAVE',8,'ascii');
  b.write('fmt ',12,'ascii'); b.writeUInt32LE(16,16); b.writeUInt16LE(1,20); b.writeUInt16LE(1,22);
  b.writeUInt32LE(rate,24); b.writeUInt32LE(rate*2,28); b.writeUInt16LE(2,32); b.writeUInt16LE(16,34);
  b.write('data',36,'ascii'); b.writeUInt32LE(dataBytes,40);
  samples.forEach((v,i)=>b.writeInt16LE(Math.max(-32768,Math.min(32767,Math.round(v))),44+i*2));
  fs.writeFileSync(file,b);
}
const rate=44100, notes=[523.25,659.25,783.99,659.25,587.33,698.46,880,698.46], theme=[];
for(let n=0;n<notes.length;n++) for(let j=0;j<rate/2;j++) { const t=j/rate, phase=2*Math.PI*notes[n]*t, env=Math.max(0,Math.min(1,j/(rate*.018),(rate*.5-j)/(rate*.07))); theme.push((Math.sin(phase)*.72+Math.sin(phase*2)*.18+Math.sin(phase*3)*.10)*env*7200); }
const impact=[];
for(let j=0;j<rate*.18;j++) { const t=j/rate, env=Math.exp(-t*24), f=980-650*(j/(rate*.18)); impact.push(Math.sin(2*Math.PI*f*t)*env*11000); }
writeBmp(path.join(out,'courier.bmp'));
writeWav(path.join(out,'theme.wav'),theme);
writeWav(path.join(out,'impact.wav'),impact);

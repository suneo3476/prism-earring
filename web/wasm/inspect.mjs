// inspect.mjs — prism.wasm の import / export 一覧とサイズを表示する開発用ツール。
//   node web/wasm/inspect.mjs [path/to/prism.wasm]
// 既定は web/prism.wasm。ビルド成果物の依存面(import ゼロ)を目視確認するために使う。
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const target = process.argv[2] || path.join(here, '..', 'prism.wasm');
const bytes = fs.readFileSync(target);
const module = new WebAssembly.Module(bytes);

console.log(`file    : ${target}`);
console.log(`size    : ${bytes.length} bytes`);
console.log(`imports : ${JSON.stringify(WebAssembly.Module.imports(module))}`);
console.log(`exports : ${JSON.stringify(WebAssembly.Module.exports(module))}`);

// verify-shape.mjs — prism.wasm の「形」を検査する(build.sh から呼ばれる)。
//
//   node web/wasm/verify-shape.mjs [path/to/prism.wasm]
//
// 検査内容:
//   1. import が 0 本(Worklet 側で WebAssembly.instantiate(bytes, {}) だけで動く)
//   2. 契約 2 の 7 関数 + 検証用 4 関数が素の名前でエクスポートされている
//   3. memory がエクスポートされている(HEAPF32 共有の前提)
//   4. 静的コンストラクタ起動口(_initialize または __wasm_call_ctors)がある
// いずれか欠けると非ゼロ終了する。
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const target = process.argv[2] || path.join(here, '..', 'prism.wasm');

const REQUIRED_FUNCTIONS = [
    'ps_create',
    'ps_destroy',
    'ps_prepare',
    'ps_reset',
    'ps_io_ptr',
    'ps_process',
    'ps_set_param',
    'ps_latency_ms',
    'ps_latency_samples',
    'ps_window_samples',
    'ps_sweep_samples'
];

const problems = [];
const module = new WebAssembly.Module(fs.readFileSync(target));
const imports = WebAssembly.Module.imports(module);
const exports = WebAssembly.Module.exports(module);
const exportNames = new Set(exports.map((e) => e.name));

if (imports.length > 0) {
    problems.push(`import が ${imports.length} 本ある: ${JSON.stringify(imports)}`);
}
for (const name of REQUIRED_FUNCTIONS) {
    const found = exports.find((e) => e.name === name);
    if (!found) {
        problems.push(`export が無い: ${name}`);
    } else if (found.kind !== 'function') {
        problems.push(`export の種別が関数でない: ${name} (${found.kind})`);
    }
}
if (!exports.some((e) => e.name === 'memory' && e.kind === 'memory')) {
    problems.push('memory がエクスポートされていない');
}
if (!exportNames.has('_initialize') && !exportNames.has('__wasm_call_ctors')) {
    problems.push('静的コンストラクタ起動口(_initialize / __wasm_call_ctors)が無い');
}

if (problems.length > 0) {
    console.error('FAIL prism.wasm の形が想定と違います:');
    for (const p of problems) {
        console.error(`  - ${p}`);
    }
    process.exit(1);
}
console.log('OK   import 0 本 / 契約 2 の export 一式 / memory / 初期化口 を確認');
